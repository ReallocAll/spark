#include "native/alloc/linux_allocation_gateway_client.h"

#include <cstring>
#include <thread>
#include <vector>

#include "native/alloc/linux_elf_admission.h"
#include "native/alloc/linux_gateway_identity.h"
#include "spark_gateway_compatibility.h"

namespace spark {
namespace {

const char Anchor = 0;

const SparkGatewayV1 *load(std::string &error)
{
    std::string root;
    gateway::Identity owner;
    if (!gateway::installationRoot(&Anchor, root, owner)) {
        error = "cannot locate Spark's loaded installation identity";
        return nullptr;
    }
    const auto *api = spark_allocation_gateway_v1();
    std::array<char, 256> detail{};
    if (api->bootstrap(root.c_str(), &Anchor, detail.data(), detail.size()) == 0) {
        error = detail.data();
        return nullptr;
    }
    return api;
}

}  // namespace

bool LinuxAllocationGateway::reserve(const std::array<void *, 7> &originals, std::string &error)
{
    if (api_ != nullptr) {
        if (!retired_) {
            return true;
        }
        api_ = nullptr;
        binding_ = {.size = sizeof(SparkGatewayBindingV1), .group = 0, .entries = {}, .tls_entry = nullptr};
        retired_ = false;
    }
    try {
        const auto *api = load(error);
        if (api == nullptr) {
            return false;
        }
        std::array<char, 256> detail{};
        if (api->reserve(originals.data(), &Anchor, &binding_, detail.data(), detail.size()) == 0) {
            error = detail.data();
            return false;
        }
        api_ = api;
        return true;
    }
    catch (const std::exception &exception) {
        error = "cannot initialize the Linux allocation gateway: " + std::string(exception.what());
        return false;
    }
    catch (...) {
        error = "cannot initialize the Linux allocation gateway";
        return false;
    }
}

bool LinuxAllocationGateway::open(const SparkGatewayCallbacksV1 &callbacks, void *context, bool tls, std::string &error)
{
    if (api_ != nullptr && api_->open(binding_.group, &callbacks, context, tls ? 1 : 0) != 0) {
        return true;
    }
    error = "Linux allocation gateway cannot admit callbacks";
    return false;
}

void LinuxAllocationGateway::publish()
{
    if (api_ != nullptr) {
        api_->publish(binding_.group);
    }
}

void LinuxAllocationGateway::close(bool final)
{
    if (api_ != nullptr) {
        api_->close(binding_.group, final ? 1 : 0);
    }
}

bool LinuxAllocationGateway::waitUntil(std::chrono::steady_clock::time_point deadline, bool tls,
                                       std::string &error) const
{
    while (api_ != nullptr && (api_->active(binding_.group, 0) != 0 || (tls && api_->active(binding_.group, 1) != 0))) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "timed out waiting for resident Linux allocation callbacks";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

bool LinuxAllocationGateway::clear(bool tls, std::string &error)
{
    if (api_ == nullptr || api_->clear(binding_.group, tls ? 1 : 0) != 0) {
        return true;
    }
    error = "Linux allocation gateway callbacks are not quiescent";
    return false;
}

bool LinuxAllocationGateway::retire(std::string &error)
{
    if (api_ == nullptr || retired_) {
        return true;
    }
    if (api_->retire(binding_.group) == 0) {
        error = "Linux allocation gateway retirement is incomplete";
        return false;
    }
    retired_ = true;
    return true;
}

bool LinuxAllocationGateway::cancel() noexcept
{
    if (api_ == nullptr) {
        return true;
    }
    try {
        if (api_->cancel(binding_.group) == 0) {
            return false;
        }
    }
    catch (...) {
        return false;
    }
    api_ = nullptr;
    binding_ = {.size = sizeof(SparkGatewayBindingV1), .group = 0, .entries = {}, .tls_entry = nullptr};
    retired_ = false;
    return true;
}

}  // namespace spark

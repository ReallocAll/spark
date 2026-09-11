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

const Elf64_Sym *querySymbol(const gateway::elf::Object &object)
{
    const Elf64_Sym *result = nullptr;
    for (const auto &symbol : object.symbols) {
        if (symbol.st_shndx != SHN_UNDEF && object.string(symbol.st_name) == SPARK_GATEWAY_SYMBOL) {
            gateway::elf::require(result == nullptr && ELF64_ST_TYPE(symbol.st_info) == STT_FUNC &&
                                      ELF64_ST_BIND(symbol.st_info) == STB_GLOBAL &&
                                      ELF64_ST_VISIBILITY(symbol.st_other) == STV_DEFAULT,
                                  "gateway query is not an ordinary unique export");
            result = &symbol;
        }
    }
    return result;
}

bool compatible(const SparkGatewayV1 *api)
{
    return api != nullptr && api->size == sizeof(SparkGatewayV1) && api->version == SPARK_GATEWAY_ABI_VERSION &&
           api->family != nullptr && std::strcmp(api->family, SPARK_GATEWAY_FAMILY) == 0 &&
           api->compatibility != nullptr && std::strcmp(api->compatibility, SPARK_GATEWAY_COMPATIBILITY) == 0 &&
           api->capacity == SPARK_GATEWAY_EXPECTED_CAPACITY &&
           api->provider_capacity == SPARK_GATEWAY_EXPECTED_CAPACITY * 7 && api->bootstrap != nullptr &&
           api->installation != nullptr && api->reserve != nullptr && api->open != nullptr && api->close != nullptr &&
           api->active != nullptr && api->clear != nullptr && api->retire != nullptr && api->publish != nullptr &&
           api->cancel != nullptr;
}

const SparkGatewayV1 *load(std::string &error)
{
    std::string root;
    gateway::Identity owner;
    if (!gateway::installationRoot(&Anchor, root, owner)) {
        error = "cannot locate the installed Linux allocation runtime from Spark's loaded identity";
        return nullptr;
    }
    const auto expected = std::filesystem::path(root) / ".spark-native" / SPARK_GATEWAY_FILENAME;
    std::error_code path_error;
    const auto canonical = std::filesystem::canonical(expected, path_error);
    if (path_error || canonical != expected || !std::filesystem::is_regular_file(canonical)) {
        error = "installed Linux allocation gateway is missing or escapes its runtime layout";
        return nullptr;
    }
    gateway::elf::Admission before(&Anchor);
    const gateway::elf::Object *existing = nullptr;
    for (const auto &object : before.snapshot.objects) {
        if (querySymbol(object) != nullptr) {
            gateway::elf::require(existing == nullptr && object.identity.path == canonical,
                                  "resident gateway family is ambiguous or outside installation");
            existing = &object;
        }
    }
    gateway::elf::Object preflight;
    preflight.read(canonical, true);
    before.preflight(preflight);
    gateway::LoaderHandle handle;
    if (existing != nullptr) {
        handle.reset(gateway::lease(existing->identity));
        gateway::loaderFault(1);
    }
    else {
        handle.reset(::dlopen(canonical.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND));
        gateway::loaderFault(2);
    }
    gateway::elf::require(static_cast<bool>(handle), "cannot retain installed allocation gateway");
    link_map *map = nullptr;
    Lmid_t namespace_id = -1;
    gateway::elf::require(::dlinfo(handle.get(), RTLD_DI_LINKMAP, static_cast<void *>(&map)) == 0 && map != nullptr &&
                              ::dlinfo(handle.get(), RTLD_DI_LMID, &namespace_id) == 0 && namespace_id == LM_ID_BASE,
                          "gateway requires main loader namespace");
    gateway::elf::Admission after(&Anchor);
    gateway::elf::require(after.snapshot.subs == before.snapshot.subs &&
                              after.snapshot.adds == before.snapshot.adds + (existing == nullptr ? 1 : 0) &&
                              after.snapshot.objects.size() ==
                                  before.snapshot.objects.size() + (existing == nullptr ? 1 : 0),
                          "loader changed during helper acquisition");
    for (const auto &previous : before.snapshot.objects) {
        gateway::elf::require(std::ranges::any_of(after.snapshot.objects,
                                                  [&](const auto &current) {
                                                      return previous.bias == current.bias &&
                                                             previous.identity.device == current.identity.device &&
                                                             previous.identity.inode == current.identity.inode &&
                                                             previous.loader_name == current.loader_name;
                                                  }),
                              "loaded identity changed during helper acquisition");
    }
    const gateway::elf::Object *loaded = nullptr;
    for (const auto &object : after.snapshot.objects) {
        if (object.bias == static_cast<std::uintptr_t>(map->l_addr) && object.identity.path == canonical) {
            loaded = &object;
        }
    }
    gateway::elf::require(loaded != nullptr, "installed gateway identity changed during load");
    gateway::elf::require(loaded->identity.device == preflight.identity.device &&
                              loaded->identity.inode == preflight.identity.inode,
                          "helper file changed after preflight");
    auto verified = *loaded;
    verified.dynamic.clear();
    verified.needed.clear();
    verified.version_names.clear();
    verified.read(canonical, true);
    after.preflight(verified);
    after.bindings(verified);
    gateway::loaderFault(3);
    const auto *symbol = querySymbol(verified);
    gateway::elf::require(symbol != nullptr, "gateway ordinary query export is absent");
    auto query = reinterpret_cast<SparkGatewayQueryV1>(::dlsym(handle.get(), SPARK_GATEWAY_SYMBOL));
    gateway::elf::require(reinterpret_cast<std::uintptr_t>(query) == gateway::elf::add(verified.bias, symbol->st_value),
                          "gateway query does not match approved ordinary export");
    const auto *api = query();
    if (!compatible(api)) {
        error = "resident allocation gateway is incompatible or belongs to another installation; restart required";
        return nullptr;
    }
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
    if (api_->cancel(binding_.group) == 0) {
        return false;
    }
    api_ = nullptr;
    binding_ = {.size = sizeof(SparkGatewayBindingV1), .group = 0, .entries = {}, .tls_entry = nullptr};
    retired_ = false;
    return true;
}

}  // namespace spark

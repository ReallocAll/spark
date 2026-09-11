#ifndef ENDSTONE_SPARK_LINUX_ALLOCATION_GATEWAY_CLIENT_H
#define ENDSTONE_SPARK_LINUX_ALLOCATION_GATEWAY_CLIENT_H

#include <array>
#include <chrono>
#include <string>

#include "native/alloc/linux_allocation_gateway_abi.h"

namespace spark {

class LinuxAllocationGateway {
public:
    LinuxAllocationGateway() = default;
    LinuxAllocationGateway(const LinuxAllocationGateway &) = delete;
    LinuxAllocationGateway &operator=(const LinuxAllocationGateway &) = delete;

    bool reserve(const std::array<void *, 7> &originals, std::string &error);
    bool open(const SparkGatewayCallbacksV1 &callbacks, void *context, bool tls, std::string &error);
    void publish();
    void close(bool final);
    bool waitUntil(std::chrono::steady_clock::time_point deadline, bool tls, std::string &error) const;
    bool clear(bool tls, std::string &error);
    bool retire(std::string &error);
    bool cancel() noexcept;
    bool reserved() const noexcept { return api_ != nullptr; }
    bool retired() const noexcept { return retired_; }
    const SparkGatewayBindingV1 &binding() const noexcept { return binding_; }

private:
    const SparkGatewayV1 *api_ = nullptr;
    SparkGatewayBindingV1 binding_{sizeof(SparkGatewayBindingV1), 0, {}, nullptr};
    bool retired_ = false;
};

}  // namespace spark

#endif

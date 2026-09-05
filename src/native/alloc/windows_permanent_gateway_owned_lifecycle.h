#pragma once

#include <cstdint>
#include <string>

#include "native/alloc/windows_permanent_gateway_experiment.h"

namespace spark::stable_entry_experiment {

#ifdef _WIN32

struct PermanentGatewayOwnerTicket {
    std::uint64_t value = 0;
    std::uint64_t generation = 0;
};

bool bindOwnedPermanentGateway(PermanentGatewayHandle &handle, void *handler, std::uint64_t timeout_ms,
                               PermanentGatewayOwnerTicket &ticket, std::string &error);

bool detachOwnedPermanentGateway(PermanentGatewayHandle &handle, PermanentGatewayOwnerTicket &ticket,
                                 std::uint64_t timeout_ms, std::string &error);

#endif

}  // namespace spark::stable_entry_experiment

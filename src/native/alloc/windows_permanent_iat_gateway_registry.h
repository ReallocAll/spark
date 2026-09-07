#pragma once

#include <cstdint>
#include <string>

#include "native/alloc/windows_permanent_iat_gateway_experiment.h"

namespace spark::permanent_iat_gateway_experiment {

#ifdef _WIN32

// Acquire the unique process-lifetime gateway for an allocator original.
// The first caller constructs and publishes one gateway; later hot-reloaded
// Spark images recover that same gateway without retaining plugin-owned state.
bool acquirePermanentIatGateway(void *original, std::uint32_t stack_argument_count, PermanentIatGatewayHandle &handle,
                                std::string &error);

#endif

}  // namespace spark::permanent_iat_gateway_experiment

#pragma once

#include "native/alloc/windows_permanent_gateway_experiment.h"

#include <cstdint>
#include <string>

namespace spark::stable_entry_experiment {

#ifdef _WIN32

// Acquire the one process-lifetime gateway assigned to this allocator entry.
// The first caller installs and publishes it into a named pagefile-backed
// registry. Later Spark DLL images reuse the same gateway. If the registry says
// a gateway already exists but the public entry is no longer Spark-owned, the
// acquisition fails closed and never installs a second code island.
bool acquirePermanentGateway(void *entry, std::uint32_t stack_argument_count,
                             PermanentGatewayHandle &handle, std::string &error);

#endif

}  // namespace spark::stable_entry_experiment

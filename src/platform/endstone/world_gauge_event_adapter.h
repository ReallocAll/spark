#ifndef SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_EVENT_ADAPTER_H
#define SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_EVENT_ADAPTER_H

#include "platform/endstone/world_gauge_state.h"

namespace spark::endstone_adapter {

// Semantic production name for the state machine driven by Endstone event callbacks.
// Deterministic tests exercise EndstoneWorldGaugeState directly, so the callbacks and
// tests use the exact same implementation rather than a separate forwarding wrapper.
using EndstoneWorldGaugeEventAdapter = EndstoneWorldGaugeState;

}  // namespace spark::endstone_adapter

#endif  // SPARK_PLATFORM_ENDSTONE_WORLD_GAUGE_EVENT_ADAPTER_H

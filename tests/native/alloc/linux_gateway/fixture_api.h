#ifndef SPARK_LINUX_GATEWAY_FIXTURE_API_H
#define SPARK_LINUX_GATEWAY_FIXTURE_API_H

#include <pthread.h>

#include <atomic>

#include "native/alloc/linux_allocation_gateway_abi.h"

struct GatewayFixtureState {
    std::atomic<unsigned> callbacks[8]{};
    std::atomic<unsigned> unwind_mask{0};
    std::atomic<bool> block_callback{false};
    std::atomic<bool> callback_entered{false};
    std::atomic<bool> release_callback{false};
    std::atomic<bool> fail_key_delete{false};
    std::atomic<bool> fail_after_key_create{false};
    std::atomic<bool> nested{false};
};

struct GatewayFixtureInfo {
    SparkGatewayBindingV1 binding{};
    pthread_key_t key{};
};

#endif

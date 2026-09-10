#ifndef ENDSTONE_SPARK_ALLOCATION_LIFECYCLE_TEST_ACCESS_H
#define ENDSTONE_SPARK_ALLOCATION_LIFECYCLE_TEST_ACCESS_H

#include <atomic>

#include "native/alloc/allocation_sampler.h"

namespace spark::test {

struct TrackingGate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> exited{false};
};

struct StartFailureGate {
    std::atomic<bool> before_thread_creation{false};
    std::atomic<bool> fail_now{false};
};

struct AllocationLifecycleTestAccess {
    static bool holdTrackingCall(AllocationSampler &, TrackingGate &) noexcept;
    static void armThreadCreationFailure(AllocationSampler &, StartFailureGate &) noexcept;
    static void disarmThreadCreationFailure(AllocationSampler &) noexcept;
};

}  // namespace spark::test

#endif  // ENDSTONE_SPARK_ALLOCATION_LIFECYCLE_TEST_ACCESS_H

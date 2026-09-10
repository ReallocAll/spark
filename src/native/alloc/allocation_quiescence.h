#ifndef ENDSTONE_SPARK_ALLOCATION_QUIESCENCE_H
#define ENDSTONE_SPARK_ALLOCATION_QUIESCENCE_H

#include <chrono>
#include <functional>

namespace spark::detail {

template <typename Clock, typename IsActive, typename Sleep>
bool waitForQuiescence(typename Clock::duration timeout, IsActive &&is_active, Sleep &&sleep)
{
    const auto deadline = Clock::now() + timeout;
    while (true) {
        if (!std::invoke(is_active)) {
            return true;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            return false;
        }
        std::invoke(sleep, deadline - now);
    }
}

}  // namespace spark::detail

#endif  // ENDSTONE_SPARK_ALLOCATION_QUIESCENCE_H

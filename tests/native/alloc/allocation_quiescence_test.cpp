#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>

#include "native/alloc/allocation_quiescence.h"

namespace {

using Duration = std::chrono::milliseconds;

struct FakeClock {
    // NOLINTBEGIN(readability-identifier-naming)
    using duration = Duration;
    using time_point = std::chrono::time_point<FakeClock, duration>;

    static inline time_point current{};

    static time_point now() noexcept { return current; }
    // NOLINTEND(readability-identifier-naming)
};

void immediateQuiet()
{
    FakeClock::current = {};
    std::size_t sleeps = 0;
    const bool quiesced = spark::detail::waitForQuiescence<FakeClock>(
        std::chrono::seconds(5), [] { return false; }, [&](Duration) { ++sleeps; });
    assert(quiesced);
    assert(sleeps == 0);
}

void permanentlyActive()
{
    FakeClock::current = {};
    std::size_t sleeps = 0;
    const bool quiesced = spark::detail::waitForQuiescence<FakeClock>(
        std::chrono::seconds(5), [] { return true; },
        [&](Duration remaining) {
            ++sleeps;
            FakeClock::current += remaining;
        });
    assert(!quiesced);
    assert(sleeps == 1);
    assert(FakeClock::current == FakeClock::time_point{} + std::chrono::seconds(5));
}

void oversleep()
{
    FakeClock::current = {};
    std::size_t sleeps = 0;
    const bool quiesced = spark::detail::waitForQuiescence<FakeClock>(
        std::chrono::seconds(5), [] { return true; },
        [&](Duration remaining) {
            ++sleeps;
            FakeClock::current += remaining + Duration(1);
        });
    assert(!quiesced);
    assert(sleeps == 1);
    assert(FakeClock::current > FakeClock::time_point{} + std::chrono::seconds(5));
}

void interruptedShortSleep()
{
    FakeClock::current = {};
    std::size_t sleeps = 0;
    const bool quiesced = spark::detail::waitForQuiescence<FakeClock>(
        std::chrono::seconds(5), [] { return true; },
        [&](Duration remaining) {
            ++sleeps;
            FakeClock::current += (std::min)(remaining, Duration(1));
        });
    assert(!quiesced);
    assert(sleeps == 5000);
    assert(FakeClock::current == FakeClock::time_point{} + std::chrono::seconds(5));
}

}  // namespace

int main()
{
    immediateQuiet();
    permanentlyActive();
    oversleep();
    interruptedShortSleep();
    return 0;
}

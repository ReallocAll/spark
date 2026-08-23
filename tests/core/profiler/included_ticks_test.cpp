#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "core/profiler/profiler.h"

namespace spark {

struct ProfilerTestAccess {
    static std::int32_t includedTicks(const Profiler &profiler)
    {
        return profiler.included_ticks_.load(std::memory_order_relaxed);
    }
};

}  // namespace spark

namespace {

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

void stop(spark::Profiler &profiler)
{
    std::string error;
    assert(profiler.cancel(error));
}

}  // namespace

int main()
{
    spark::Profiler profiler;
    spark::ProfilerOptions options;
    options.interval_ms = 1;
    options.only_ticks_over_ms = 10;

    std::string error;
    assert(profiler.start(options, currentThreadId(), error));
    profiler.onTick(10.0);
    profiler.onTick(10.001);
    profiler.onTick(std::numeric_limits<double>::quiet_NaN());
    profiler.onTick(std::numeric_limits<double>::infinity());
    profiler.onTick(-std::numeric_limits<double>::infinity());
    assert(spark::ProfilerTestAccess::includedTicks(profiler) == 1);
    stop(profiler);

    assert(profiler.start(options, currentThreadId(), error));
    assert(spark::ProfilerTestAccess::includedTicks(profiler) == 0);
    stop(profiler);

    options.only_ticks_over_ms = -1;
    assert(profiler.start(options, currentThreadId(), error));
    profiler.onTick(100.0);
    assert(spark::ProfilerTestAccess::includedTicks(profiler) == 0);
    stop(profiler);

    options.only_ticks_over_ms = std::numeric_limits<std::int64_t>::max();
    assert(!profiler.start(options, currentThreadId(), error));
    assert(error == "tick threshold is too large");
    return 0;
}

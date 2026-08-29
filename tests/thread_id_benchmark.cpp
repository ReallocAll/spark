#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "native/sampler/thread_info.h"

namespace {

using Clock = std::chrono::steady_clock;

#if defined(_MSC_VER)
#define SPARK_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define SPARK_NOINLINE __attribute__((noinline))
#else
#define SPARK_NOINLINE
#endif

SPARK_NOINLINE std::uint64_t directThreadId() noexcept
{
    return spark::currentNativeThreadId();
}

SPARK_NOINLINE std::uint64_t cachedThreadId() noexcept
{
    thread_local const std::uint64_t tid = spark::currentNativeThreadId();
    return tid;
}

template <typename Function>
void exercise(Function function, std::size_t iterations, volatile std::uint64_t &sink)
{
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        sum += function();
    }
    sink = sum;
}

template <typename Function>
double measure(Function function, std::size_t threads, std::size_t iterations_per_thread)
{
    volatile std::uint64_t sink = 0;
    const auto start = Clock::now();
    if (threads == 1) {
        exercise(function, iterations_per_thread, sink);
    }
    else {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        std::vector<std::uint64_t> sinks(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers.emplace_back([&, i] {
                volatile std::uint64_t local_sink = 0;
                exercise(function, iterations_per_thread, local_sink);
                sinks[i] = local_sink;
            });
        }
        for (std::thread &worker : workers) {
            worker.join();
        }
        for (std::uint64_t value : sinks) {
            sink = sink + value;
        }
    }
    if (sink == 0) {
        std::fprintf(stderr, "unexpected zero native thread id sum\n");
    }
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

template <typename Function>
double medianNsPerCall(Function function, std::size_t threads, std::size_t iterations_per_thread)
{
    constexpr int kTrials = 7;
    std::vector<double> values;
    values.reserve(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        const double elapsed = measure(function, threads, iterations_per_thread);
        values.push_back(elapsed / static_cast<double>(threads * iterations_per_thread));
    }
    std::ranges::sort(values);
    return values[values.size() / 2];
}

void printCase(const char *name, std::size_t threads, std::size_t iterations_per_thread, double ns_per_call)
{
    std::printf("%s,%zu,%zu,%.3f\n", name, threads, iterations_per_thread, ns_per_call);
}

std::size_t configuredIterations() noexcept
{
    constexpr std::size_t kDefaultIterations = 2000000;
    const char *value = std::getenv("SPARK_THREAD_ID_BENCH_ITERATIONS");
    if (value == nullptr || value[0] == '\0') {
        return kDefaultIterations;
    }
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value && *end == '\0' && parsed != 0 ? static_cast<std::size_t>(parsed) : kDefaultIterations;
}

}  // namespace

int main()
{
    const std::size_t iterations_per_thread = configuredIterations();

    // Warm both paths before measuring so TLS initialization is not charged to
    // the steady-state callback fast path.
    volatile std::uint64_t warmup = directThreadId() + cachedThreadId();
    (void)warmup;

    std::printf("case,threads,iterations_per_thread,median_ns_per_call\n");
    printCase("direct", 1, iterations_per_thread, medianNsPerCall(directThreadId, 1, iterations_per_thread));
    printCase("cached", 1, iterations_per_thread, medianNsPerCall(cachedThreadId, 1, iterations_per_thread));
    printCase("direct", 4, iterations_per_thread, medianNsPerCall(directThreadId, 4, iterations_per_thread));
    printCase("cached", 4, iterations_per_thread, medianNsPerCall(cachedThreadId, 4, iterations_per_thread));
    return 0;
}

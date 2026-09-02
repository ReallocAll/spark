#ifndef _WIN32
#error "windows_etw_heap_workload.cpp is Windows-only"
#endif

#include <windows.h>

#include <malloc.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr std::size_t kIterations = 20000;
std::atomic<std::uint64_t> g_checksum{0};

__declspec(noinline) void spark_etw_probe_malloc_free(std::size_t iteration)
{
    const std::size_t size = 48 + (iteration & 511U);
    void *memory = std::malloc(size);
    if (memory == nullptr) {
        std::abort();
    }
    std::memset(memory, static_cast<int>(iteration & 0x7fU), size);
    g_checksum.fetch_add(static_cast<unsigned char *>(memory)[0], std::memory_order_relaxed);
    std::free(memory);
}

__declspec(noinline) void spark_etw_probe_calloc_free(std::size_t iteration)
{
    const std::size_t count = 4 + (iteration & 15U);
    auto *memory = static_cast<unsigned char *>(std::calloc(count, 32));
    if (memory == nullptr) {
        std::abort();
    }
    g_checksum.fetch_add(memory[count - 1], std::memory_order_relaxed);
    std::free(memory);
}

__declspec(noinline) void spark_etw_probe_realloc(std::size_t iteration)
{
    void *memory = std::malloc(64 + (iteration & 63U));
    if (memory == nullptr) {
        std::abort();
    }
    memory = std::realloc(memory, 1024 + (iteration & 1023U));
    if (memory == nullptr) {
        std::abort();
    }
    std::free(memory);
}

__declspec(noinline) void spark_etw_probe_aligned(std::size_t iteration)
{
    void *memory = _aligned_malloc(256 + (iteration & 255U), 64);
    if (memory == nullptr) {
        std::abort();
    }
    std::memset(memory, 0x5a, 64);
    g_checksum.fetch_add(static_cast<unsigned char *>(memory)[0], std::memory_order_relaxed);
    _aligned_free(memory);
}

__declspec(noinline) void spark_etw_probe_heapapi(std::size_t iteration)
{
    HANDLE heap = ::GetProcessHeap();
    void *memory = ::HeapAlloc(heap, 0, 128 + (iteration & 255U));
    if (memory == nullptr) {
        std::abort();
    }
    memory = ::HeapReAlloc(heap, 0, memory, 2048 + (iteration & 511U));
    if (memory == nullptr) {
        std::abort();
    }
    if (::HeapFree(heap, 0, memory) == FALSE) {
        std::abort();
    }
}

__declspec(noinline) void *spark_etw_probe_live_malloc(std::size_t iteration)
{
    void *memory = std::malloc(4096 + (iteration & 1023U));
    if (memory != nullptr) {
        std::memset(memory, 0x33, 64);
    }
    return memory;
}

}  // namespace

int main()
{
    std::fprintf(stderr, "spark-etw-workload pid=%lu begin iterations=%llu\n",
                 static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long long>(kIterations));

    std::vector<void *> live;
    live.reserve(256);
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        spark_etw_probe_malloc_free(iteration);
        spark_etw_probe_calloc_free(iteration);
        spark_etw_probe_realloc(iteration);
        spark_etw_probe_aligned(iteration);
        spark_etw_probe_heapapi(iteration);
        if ((iteration % 100) == 0) {
            void *memory = spark_etw_probe_live_malloc(iteration);
            if (memory == nullptr) {
                std::abort();
            }
            live.push_back(memory);
        }
    }

    // Keep a deterministic subset live long enough for the trace to observe a
    // genuine outstanding-allocation set. Free half before exit so the analysis
    // can distinguish live reconstruction from total allocation accounting.
    for (std::size_t index = 0; index < live.size() / 2; ++index) {
        std::free(live[index]);
        live[index] = nullptr;
    }

    std::fprintf(stderr, "spark-etw-workload ready live=%llu expected_outstanding=%llu checksum=%llu\n",
                 static_cast<unsigned long long>(live.size()),
                 static_cast<unsigned long long>(live.size() - live.size() / 2),
                 static_cast<unsigned long long>(g_checksum.load(std::memory_order_relaxed)));
    std::fflush(stderr);

    // xperf -PidNewProcess returns while the child continues. Leave a bounded
    // collection window after all allocation families have executed.
    ::Sleep(3000);
    std::fprintf(stderr, "spark-etw-workload exit\n");
    return 0;
}

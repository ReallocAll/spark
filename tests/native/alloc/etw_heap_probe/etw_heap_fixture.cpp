#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>

namespace {

volatile std::uintptr_t g_sink = 0;

void record(const char *kind, const char *phase, void *address, std::size_t size) {
    std::printf("fixture kind=%s phase=%s address=%p size=%zu\n", kind, phase, address, size);
    std::fflush(stdout);
    g_sink ^= reinterpret_cast<std::uintptr_t>(address);
}

__declspec(noinline) void *fixtureMalloc(std::size_t size) {
    return std::malloc(size);
}

__declspec(noinline) void *fixtureCalloc(std::size_t count, std::size_t size) {
    return std::calloc(count, size);
}

__declspec(noinline) void *fixtureRealloc(void *address, std::size_t size) {
    return std::realloc(address, size);
}

__declspec(noinline) void fixtureFree(void *address) {
    std::free(address);
}

__declspec(noinline) void *fixtureAlignedMalloc(std::size_t size, std::size_t alignment) {
    return _aligned_malloc(size, alignment);
}

__declspec(noinline) void *fixtureAlignedRealloc(void *address, std::size_t size, std::size_t alignment) {
    return _aligned_realloc(address, size, alignment);
}

__declspec(noinline) void fixtureAlignedFree(void *address) {
    _aligned_free(address);
}

__declspec(noinline) void *fixtureHeapAlloc(HANDLE heap, std::size_t size) {
    return HeapAlloc(heap, 0, size);
}

__declspec(noinline) void *fixtureHeapReAlloc(HANDLE heap, void *address, std::size_t size) {
    return HeapReAlloc(heap, 0, address, size);
}

__declspec(noinline) void fixtureHeapFree(HANDLE heap, void *address) {
    if (address != nullptr) {
        (void)HeapFree(heap, 0, address);
    }
}

bool exerciseRepresentatives() {
    HANDLE heap = GetProcessHeap();
    if (heap == nullptr) {
        return false;
    }

    void *malloc_ptr = fixtureMalloc(8192);
    void *calloc_ptr = fixtureCalloc(32, 257);
    void *aligned_ptr = fixtureAlignedMalloc(12288, 64);
    void *heap_ptr = fixtureHeapAlloc(heap, 16384);
    if (malloc_ptr == nullptr || calloc_ptr == nullptr || aligned_ptr == nullptr || heap_ptr == nullptr) {
        fixtureFree(malloc_ptr);
        fixtureFree(calloc_ptr);
        fixtureAlignedFree(aligned_ptr);
        fixtureHeapFree(heap, heap_ptr);
        return false;
    }

    record("malloc", "live", malloc_ptr, 8192);
    record("calloc", "live", calloc_ptr, 32 * 257);
    record("aligned", "live", aligned_ptr, 12288);
    record("HeapAlloc", "live", heap_ptr, 16384);

    void *malloc_realloc = fixtureRealloc(malloc_ptr, 24576);
    if (malloc_realloc == nullptr) {
        fixtureFree(malloc_ptr);
        fixtureFree(calloc_ptr);
        fixtureAlignedFree(aligned_ptr);
        fixtureHeapFree(heap, heap_ptr);
        return false;
    }
    malloc_ptr = malloc_realloc;
    record("realloc", "live", malloc_ptr, 24576);

    void *aligned_realloc = fixtureAlignedRealloc(aligned_ptr, 20480, 64);
    if (aligned_realloc == nullptr) {
        fixtureFree(malloc_ptr);
        fixtureFree(calloc_ptr);
        fixtureAlignedFree(aligned_ptr);
        fixtureHeapFree(heap, heap_ptr);
        return false;
    }
    aligned_ptr = aligned_realloc;
    record("aligned_realloc", "live", aligned_ptr, 20480);

    void *heap_realloc = fixtureHeapReAlloc(heap, heap_ptr, 32768);
    if (heap_realloc == nullptr) {
        fixtureFree(malloc_ptr);
        fixtureFree(calloc_ptr);
        fixtureAlignedFree(aligned_ptr);
        fixtureHeapFree(heap, heap_ptr);
        return false;
    }
    heap_ptr = heap_realloc;
    record("HeapReAlloc", "live", heap_ptr, 32768);

    Sleep(50);

    record("realloc", "free", malloc_ptr, 24576);
    fixtureFree(malloc_ptr);
    record("calloc", "free", calloc_ptr, 32 * 257);
    fixtureFree(calloc_ptr);
    record("aligned_realloc", "free", aligned_ptr, 20480);
    fixtureAlignedFree(aligned_ptr);
    record("HeapReAlloc", "free", heap_ptr, 32768);
    fixtureHeapFree(heap, heap_ptr);
    return true;
}

bool allocationStorm() {
    HANDLE heap = GetProcessHeap();
    if (heap == nullptr) {
        return false;
    }

    constexpr std::size_t kIterations = 20000;
    for (std::size_t i = 0; i < kIterations; ++i) {
        const std::size_t base = 48 + (i % 977);

        void *malloc_ptr = fixtureMalloc(base);
        if (malloc_ptr == nullptr) {
            return false;
        }
        void *realloc_ptr = fixtureRealloc(malloc_ptr, base + 137);
        if (realloc_ptr == nullptr) {
            fixtureFree(malloc_ptr);
            return false;
        }
        fixtureFree(realloc_ptr);

        void *calloc_ptr = fixtureCalloc(3, base);
        if (calloc_ptr == nullptr) {
            return false;
        }
        fixtureFree(calloc_ptr);

        void *aligned_ptr = fixtureAlignedMalloc(base + 64, 64);
        if (aligned_ptr == nullptr) {
            return false;
        }
        fixtureAlignedFree(aligned_ptr);

        void *heap_ptr = fixtureHeapAlloc(heap, base + 31);
        if (heap_ptr == nullptr) {
            return false;
        }
        void *heap_realloc = fixtureHeapReAlloc(heap, heap_ptr, base + 193);
        if (heap_realloc == nullptr) {
            fixtureHeapFree(heap, heap_ptr);
            return false;
        }
        fixtureHeapFree(heap, heap_realloc);
    }

    std::printf("fixture storm_iterations=%zu sink=0x%llx\n", kIterations,
                static_cast<unsigned long long>(g_sink));
    return true;
}

} // namespace

int main() {
    std::printf("windows-etw-heap-fixture begin pid=%lu\n", GetCurrentProcessId());
    std::fflush(stdout);

    if (!exerciseRepresentatives()) {
        std::fprintf(stderr, "representative allocation exercise failed\n");
        return 1;
    }
    if (!allocationStorm()) {
        std::fprintf(stderr, "allocation storm failed\n");
        return 2;
    }

    std::printf("windows-etw-heap-fixture PASS\n");
    return 0;
}

#ifndef _WIN32
#error "windows_etw_heap_workload.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr std::size_t kOperations = 50000;
constexpr std::size_t kOutstanding = 512;

}  // namespace

int main()
{
    std::cout << "pid=" << ::GetCurrentProcessId() << '\n';
    std::cout.flush();

    HANDLE heap = ::GetProcessHeap();
    if (heap == nullptr) {
        std::cerr << "heap-workload GetProcessHeap failed error=" << ::GetLastError() << '\n';
        return 2;
    }

    std::vector<void *> outstanding;
    outstanding.reserve(kOutstanding);

    for (std::size_t index = 0; index < kOperations; ++index) {
        const SIZE_T size = 32 + (index & 0x3ffU);
        void *block = ::HeapAlloc(heap, 0, size);
        if (block == nullptr) {
            std::cerr << "heap-workload HeapAlloc failed index=" << index << '\n';
            return 3;
        }

        if ((index & 1U) == 0) {
            void *grown = ::HeapReAlloc(heap, 0, block, size + 128);
            if (grown == nullptr) {
                (void)::HeapFree(heap, 0, block);
                std::cerr << "heap-workload HeapReAlloc failed index=" << index << '\n';
                return 4;
            }
            block = grown;
        }

        if ((index % 97U) == 0 && outstanding.size() < kOutstanding) {
            outstanding.push_back(block);
        } else if (::HeapFree(heap, 0, block) == FALSE) {
            std::cerr << "heap-workload HeapFree failed index=" << index << '\n';
            return 5;
        }

        void *crt = std::malloc(48 + (index & 0x1ffU));
        if (crt == nullptr) {
            std::cerr << "heap-workload malloc failed index=" << index << '\n';
            return 6;
        }
        void *resized = std::realloc(crt, 96 + (index & 0x1ffU));
        if (resized == nullptr) {
            std::free(crt);
            std::cerr << "heap-workload realloc failed index=" << index << '\n';
            return 7;
        }
        std::free(resized);
    }

    for (void *block : outstanding) {
        if (::HeapFree(heap, 0, block) == FALSE) {
            std::cerr << "heap-workload outstanding HeapFree failed\n";
            return 8;
        }
    }

    std::cout << "result=workload-pass operations=" << kOperations
              << " delayed_frees=" << outstanding.size() << '\n';
    return 0;
}

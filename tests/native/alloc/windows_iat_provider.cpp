#include <cstddef>
#include <cstdlib>

extern "C" __declspec(dllexport) void *sparkIatProviderAlloc(std::size_t size)
{
    return std::malloc(size);
}

extern "C" __declspec(dllexport) void sparkIatProviderFree(void *pointer)
{
    std::free(pointer);
}

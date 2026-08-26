#include <cstddef>

extern "C" __declspec(dllimport) void *sparkIatProviderAlloc(std::size_t size);
extern "C" __declspec(dllimport) void sparkIatProviderFree(void *pointer);

extern "C" __declspec(dllexport) void *sparkIatConsumerAlloc(std::size_t size)
{
    return sparkIatProviderAlloc(size);
}

extern "C" __declspec(dllexport) void sparkIatConsumerFree(void *pointer)
{
    sparkIatProviderFree(pointer);
}

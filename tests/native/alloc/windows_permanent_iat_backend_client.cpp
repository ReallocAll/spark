#ifndef _WIN32
#error "windows_permanent_iat_backend_client.cpp is Windows-only"
#endif

#include <cstddef>
#include <cstdlib>

extern "C" __declspec(dllexport) void *__cdecl windowsPermanentIatClientMalloc(std::size_t size) noexcept
{
    return std::malloc(size);
}

extern "C" __declspec(dllexport) void __cdecl windowsPermanentIatClientFree(void *pointer) noexcept
{
    std::free(pointer);
}

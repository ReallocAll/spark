#include <cstddef>
#include <vector>

#include "selftest_allocation_internal.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#endif

namespace spark::selftest {

bool setCurrentThreadName(const char *name)
{
#ifdef _WIN32
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (length <= 1) {
        return false;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(length));
    if (::MultiByteToWideChar(CP_UTF8, 0, name, -1, wide.data(), length) == 0) {
        return false;
    }
    return SUCCEEDED(::SetThreadDescription(::GetCurrentThread(), wide.data()));
#elif defined(__linux__)
    return ::pthread_setname_np(::pthread_self(), name) == 0;
#else
    (void)name;
    return false;
#endif
}

}  // namespace spark::selftest

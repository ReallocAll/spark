#ifndef ENDSTONE_SPARK_LINUX_LOADER_HANDLE_H
#define ENDSTONE_SPARK_LINUX_LOADER_HANDLE_H

#include <dlfcn.h>

#include <utility>
#if defined(SPARK_GATEWAY_HANDLE_TESTING)
#include <cstdlib>
#include <new>
#endif

namespace spark::gateway {

inline void loaderFault(unsigned point)
{
#if defined(SPARK_GATEWAY_HANDLE_TESTING)
    const char *value = std::getenv("SPARK_GATEWAY_HANDLE_FAULT");
    if (value != nullptr && std::strtoul(value, nullptr, 10) == point) {
        throw std::bad_alloc();
    }
#else
    (void)point;
#endif
}

class LoaderHandle {
public:
    explicit LoaderHandle(void *handle = nullptr) noexcept : handle_(handle) {}
    ~LoaderHandle() noexcept { reset(); }
    LoaderHandle(const LoaderHandle &) = delete;
    LoaderHandle &operator=(const LoaderHandle &) = delete;
    LoaderHandle(LoaderHandle &&other) noexcept : handle_(other.release()) {}
    LoaderHandle &operator=(LoaderHandle &&other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    void *get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    void *release() noexcept { return std::exchange(handle_, nullptr); }
    void reset(void *handle = nullptr) noexcept
    {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
        }
        handle_ = handle;
    }

private:
    void *handle_ = nullptr;
};

}  // namespace spark::gateway

#endif

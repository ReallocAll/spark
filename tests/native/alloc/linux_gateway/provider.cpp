#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
std::atomic<unsigned> *Counters = nullptr;
void count(std::size_t api)
{
    if (Counters != nullptr) {
        Counters[api].fetch_add(1);
    }
}
}  // namespace

extern "C" void provider_counters(std::atomic<unsigned> *counters)
{
    Counters = counters;
}

extern "C" void *provider_malloc(std::size_t size)
{
    count(0);
    if (size == 0x12345678) {
        errno = ENOMEM;
        return nullptr;
    }
    auto *result = static_cast<unsigned char *>(std::malloc(size + 16));
    if (result != nullptr) {
        result[0] = PROVIDER_DOMAIN;
        return result + 16;
    }
    return nullptr;
}

extern "C" void provider_free(void *pointer)
{
    count(3);
    if (pointer != nullptr) {
        auto *base = static_cast<unsigned char *>(pointer) - 16;
        if (base[0] != PROVIDER_DOMAIN) {
            std::abort();
        }
        std::free(base);
    }
}

extern "C" void *provider_calloc(std::size_t count_value, std::size_t size)
{
    count(1);
    if (size != 0 && count_value > std::numeric_limits<std::size_t>::max() / size) {
        errno = ENOMEM;
        return nullptr;
    }
    void *result = provider_malloc(count_value * size);
    if (result != nullptr) {
        std::memset(result, 0, count_value * size);
    }
    return result;
}

extern "C" void *provider_realloc(void *pointer, std::size_t size)
{
    count(2);
    if (pointer == nullptr) {
        return provider_malloc(size);
    }
    auto *base = static_cast<unsigned char *>(pointer) - 16;
    if (base[0] != PROVIDER_DOMAIN) {
        std::abort();
    }
    auto *result = static_cast<unsigned char *>(std::realloc(base, size + 16));
    return result != nullptr ? result + 16 : nullptr;
}

extern "C" void *provider_reallocarray(void *pointer, std::size_t count_value, std::size_t size)
{
    count(4);
    if (size != 0 && count_value > std::numeric_limits<std::size_t>::max() / size) {
        errno = ENOMEM;
        return nullptr;
    }
    return provider_realloc(pointer, count_value * size);
}

extern "C" void *provider_aligned_alloc(std::size_t alignment, std::size_t size)
{
    count(5);
    if (alignment != 16 || size % alignment != 0) {
        errno = EINVAL;
        return nullptr;
    }
    return provider_malloc(size);
}

extern "C" int provider_posix_memalign(void **result, std::size_t alignment, std::size_t size)
{
    count(6);
    if (alignment != 16) {
        return EINVAL;
    }
    *result = provider_malloc(size);
    return *result != nullptr ? 0 : ENOMEM;
}

#include <cstddef>

extern "C" void *provider_malloc(std::size_t);
extern "C" void provider_free(void *);

namespace {
void *(*volatile Allocate)(std::size_t) = &provider_malloc;
void (*volatile Release)(void *) = &provider_free;
}  // namespace

extern "C" void *consumer_malloc(std::size_t size)
{
    return Allocate(size);
}

extern "C" void consumer_free(void *pointer)
{
    Release(pointer);
}

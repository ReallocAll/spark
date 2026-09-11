#include <cstdlib>

extern "C" __attribute__((used, retain)) void sparkLegacyAllocationImports()
{
    void *(*volatile zero)(std::size_t, std::size_t) = &std::calloc;
    void *(*volatile resize)(void *, std::size_t) = &std::realloc;
    void (*volatile release)(void *) = &std::free;
    void *pointer = zero(1, 32);
    pointer = resize(pointer, 64);
    release(pointer);
}

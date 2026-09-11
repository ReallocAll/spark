#include <sys/types.h>

extern "C" pid_t getpid() noexcept
{
    return 314159;
}

#include <dlfcn.h>

#include <cstdint>

#include "native/sampler/capture_linux.cpp"  // NOLINT(bugprone-suspicious-include)

namespace {

bool GFailNamedPin = false;
int GNamedCalls = 0;
int GNullCalls = 0;
int GLastFlags = 0;

}  // namespace

extern "C" void *realDlopen(const char *name, int flags) asm("__real_dlopen");
extern "C" void *wrapDlopen(const char *name, int flags) asm("__wrap_dlopen");

extern "C" void *wrapDlopen(const char *name, int flags)
{
    GLastFlags = flags;
    if (name == nullptr) {
        ++GNullCalls;
    }
    else {
        ++GNamedCalls;
        if (GFailNamedPin) {
            return nullptr;
        }
    }
    return realDlopen(name, flags);
}

extern "C" __attribute__((visibility("default"))) bool sparkCaptureFixtureArm(bool fail_named_pin)
{
    GFailNamedPin = fail_named_pin;
    return spark::Capture::arm();
}

extern "C" __attribute__((visibility("default"))) bool sparkCaptureFixtureDisarm()
{
    return spark::Capture::disarm();
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t sparkCaptureFixtureHandler()
{
    return reinterpret_cast<std::uintptr_t>(&spark::handler);
}

extern "C" __attribute__((visibility("default"))) int sparkCaptureFixtureNamedCalls()
{
    return GNamedCalls;
}

extern "C" __attribute__((visibility("default"))) int sparkCaptureFixtureNullCalls()
{
    return GNullCalls;
}

extern "C" __attribute__((visibility("default"))) int sparkCaptureFixtureFlags()
{
    return GLastFlags;
}

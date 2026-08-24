#ifdef __linux__
#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <span>
#include <string>

#include <sys/types.h>

#include "native/alloc/elf_import_hooks.h"
#include "selftest_allocation_internal.h"

namespace spark::selftest {

pid_t linuxHookProbe() noexcept
{
    return static_cast<pid_t>(-12345);
}

pid_t (*volatile LinuxGetpidCall)() = &::getpid;

bool verifyLinuxImportHooks()
{
    const pid_t expected = ::getpid();
    spark::ElfImportHooks hooks;
    const spark::ElfImportHookSpec spec{
        .name = "getpid", .replacement = reinterpret_cast<void *>(&linuxHookProbe), .required = true};
    std::string error;
    if (!hooks.prepare(std::span<const spark::ElfImportHookSpec>(&spec, 1), error) || hooks.targetCount() == 0 ||
        !hooks.install(error)) {
        std::fprintf(stderr, "linux import hooks: setup failed: %s\n", error.c_str());
        return false;
    }
    if (LinuxGetpidCall() != static_cast<pid_t>(-12345)) {
        std::fprintf(stderr, "linux import hooks: replacement was not observed\n");
        return false;
    }

    void *fixture = ::dlopen(SPARK_ELF_HOOK_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    auto fixture_getpid =
        fixture == nullptr ? nullptr : reinterpret_cast<pid_t (*)()>(::dlsym(fixture, "sparkElfHookFixtureGetpid"));
    if (fixture_getpid == nullptr || fixture_getpid() != expected || !hooks.rescan(error) ||
        fixture_getpid() != static_cast<pid_t>(-12345) || hooks.hookedModuleCount() < 2) {
        std::fprintf(stderr, "linux import hooks: loaded-module rescan failed: %s\n", error.c_str());
        if (fixture != nullptr) {
            ::dlclose(fixture);
        }
        return false;
    }
    ::dlclose(fixture);

    if (!hooks.uninstall(error) || LinuxGetpidCall() != expected) {
        std::fprintf(stderr, "linux import hooks: restoration failed: %s\n", error.c_str());
        return false;
    }
    return true;
}
}  // namespace spark::selftest
#endif

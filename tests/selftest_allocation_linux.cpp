#ifdef __linux__
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <sys/types.h>

#include "native/alloc/elf_import_hooks.h"
#include "selftest_allocation_internal.h"

namespace spark::selftest {

namespace {

std::atomic_bool RejectFixtureScan{false};
std::atomic<std::uintptr_t> AllocationTrafficSink{0};

pid_t linuxHookProbe() noexcept
{
    return static_cast<pid_t>(-12345);
}

bool linuxScanModuleGate(std::string_view name) noexcept
{
    return !RejectFixtureScan.load(std::memory_order_acquire) ||
           name.find("spark_elf_hook_fixture") == std::string_view::npos;
}

void allocationTrafficStep()
{
    void *allocation = std::malloc(256);
    if (allocation == nullptr) {
        return;
    }
    AllocationTrafficSink.fetch_xor(reinterpret_cast<std::uintptr_t>(allocation), std::memory_order_relaxed);
    std::free(allocation);
}

pid_t (*volatile LinuxGetpidCall)() = &::getpid;

}  // namespace

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

    std::atomic_bool stop_traffic{false};
    std::atomic<std::uint64_t> traffic_count{0};
    std::barrier traffic_started(2);
    std::thread traffic([&] {
        traffic_started.arrive_and_wait();
        while (!stop_traffic.load(std::memory_order_acquire)) {
            allocationTrafficStep();
            traffic_count.fetch_add(1, std::memory_order_relaxed);
        }
    });
    traffic_started.arrive_and_wait();

    hooks.setScanModuleGateForTesting(&linuxScanModuleGate);
    bool success = hooks.rescan(error);
    const std::size_t baseline_targets = hooks.targetCount();
    const std::size_t baseline_pages = hooks.pageCount();
    const std::size_t baseline_modules = hooks.hookedModuleCount();
    if (!success) {
        std::fprintf(stderr, "linux import hooks: baseline rescan failed: %s\n", error.c_str());
    }

    for (int iteration = 0; success && iteration < 64; ++iteration) {
        void *fixture = ::dlopen(SPARK_ELF_HOOK_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
        auto fixture_getpid =
            fixture == nullptr ? nullptr : reinterpret_cast<pid_t (*)()>(::dlsym(fixture, "sparkElfHookFixtureGetpid"));
        if (fixture_getpid == nullptr || fixture_getpid() != expected || !hooks.rescan(error) ||
            fixture_getpid() != static_cast<pid_t>(-12345) || hooks.hookedModuleCount() <= baseline_modules) {
            std::fprintf(stderr, "linux import hooks: load/rescan failed at %d: %s\n", iteration, error.c_str());
            if (fixture != nullptr) {
                ::dlclose(fixture);
            }
            success = false;
            break;
        }

        if (iteration == 0) {
            RejectFixtureScan.store(true, std::memory_order_release);
            const bool partial_ok = hooks.rescan(error);
            RejectFixtureScan.store(false, std::memory_order_release);
            if (!partial_ok || hooks.failedModuleCount() == 0 || fixture_getpid() != static_cast<pid_t>(-12345) ||
                !hooks.rescan(error)) {
                std::fprintf(stderr, "linux import hooks: partial scan recovery failed: %s\n", error.c_str());
                ::dlclose(fixture);
                success = false;
                break;
            }
        }

        ::dlclose(fixture);
        void *stale = ::dlopen(SPARK_ELF_HOOK_FIXTURE_PATH, RTLD_NOW | RTLD_NOLOAD);
        if (stale != nullptr) {
            ::dlclose(stale);
            std::fprintf(stderr, "linux import hooks: fixture remained pinned after dlclose at %d\n", iteration);
            success = false;
            break;
        }
        if (!hooks.rescan(error) || hooks.targetCount() != baseline_targets || hooks.pageCount() != baseline_pages ||
            hooks.hookedModuleCount() != baseline_modules) {
            std::fprintf(stderr, "linux import hooks: stale state survived rescan at %d: %s\n", iteration,
                         error.c_str());
            success = false;
            break;
        }
    }

    stop_traffic.store(true, std::memory_order_release);
    traffic.join();
    hooks.setScanModuleGateForTesting(nullptr);
    if (traffic_count.load(std::memory_order_relaxed) == 0) {
        std::fprintf(stderr, "linux import hooks: allocation traffic did not run\n");
        success = false;
    }

    if (!hooks.uninstall(error) || LinuxGetpidCall() != expected) {
        std::fprintf(stderr, "linux import hooks: restoration failed: %s\n", error.c_str());
        return false;
    }
    return success;
}
}  // namespace spark::selftest
#endif

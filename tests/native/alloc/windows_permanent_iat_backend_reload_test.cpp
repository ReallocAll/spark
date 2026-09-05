#ifndef _WIN32
#error "windows_permanent_iat_backend_reload_test.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

using ClientMallocFn = void *(__cdecl *)(std::size_t);
using ClientFreeFn = void(__cdecl *)(void *);
using InstallFn = int(__cdecl *)();
using UninstallFn = int(__cdecl *)();
using ErrorFn = const char *(__cdecl *)();
using CallsFn = std::uint64_t(__cdecl *)();
using SetHoldFn = void(__cdecl *)(int);
using ResetEnteredFn = void(__cdecl *)();
using EnteredFn = int(__cdecl *)();

constexpr std::size_t kReloadCycles = 1000;
constexpr std::size_t kWorkers = 4;
constexpr std::uint64_t kTimeoutMs = 5000;
constexpr wchar_t kClientName[] = L"windows_permanent_iat_backend_client.dll";
constexpr wchar_t kPluginName[] = L"windows_permanent_iat_backend_plugin.dll";

std::atomic<std::size_t> g_cycle{0};
std::atomic<unsigned> g_phase{0};

[[noreturn]] void fail(const char *reason)
{
    std::fprintf(stderr, "stage=permanent-iat-backend-reload failure=%s cycle=%zu phase=%u\n", reason,
                 g_cycle.load(std::memory_order_relaxed), g_phase.load(std::memory_order_relaxed));
    std::fflush(stderr);
    std::abort();
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exception) noexcept
{
    const EXCEPTION_RECORD *record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    const DWORD code = record != nullptr ? record->ExceptionCode : 0;
    const void *address = record != nullptr ? record->ExceptionAddress : nullptr;
    std::uintptr_t rip = 0;
#if defined(_M_X64)
    if (exception != nullptr && exception->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exception->ContextRecord->Rip);
    }
#endif
    std::fprintf(stderr,
                 "stage=permanent-iat-backend-reload exception=0x%08lx address=%p rip=0x%llx cycle=%zu phase=%u\n",
                 static_cast<unsigned long>(code), address, static_cast<unsigned long long>(rip),
                 g_cycle.load(std::memory_order_relaxed), g_phase.load(std::memory_order_relaxed));
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

[[nodiscard]] std::wstring siblingPath(const wchar_t *name)
{
    wchar_t buffer[32768]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        fail("GetModuleFileNameW");
    }
    std::wstring path(buffer, length);
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        fail("sibling-path");
    }
    path.resize(slash + 1);
    path.append(name);
    return path;
}

template <typename Function>
[[nodiscard]] Function requiredExport(HMODULE module, const char *name)
{
    FARPROC proc = ::GetProcAddress(module, name);
    if (proc == nullptr) {
        fail("GetProcAddress");
    }
    return reinterpret_cast<Function>(proc);
}

void clientRoundTrip(ClientMallocFn client_malloc, ClientFreeFn client_free, std::size_t size)
{
    void *pointer = client_malloc(size);
    if (pointer == nullptr) {
        fail("client-malloc-null");
    }
    client_free(pointer);
}

}  // namespace

int main()
{
    ::SetUnhandledExceptionFilter(&unhandledExceptionFilter);
    std::fprintf(stderr, "stage=permanent-iat-backend-reload begin cycles=%zu workers=%zu\n", kReloadCycles, kWorkers);

    const std::wstring client_path = siblingPath(kClientName);
    const std::wstring plugin_path = siblingPath(kPluginName);
    HMODULE client = ::LoadLibraryW(client_path.c_str());
    if (client == nullptr) {
        std::fprintf(stderr, "stage=permanent-iat-backend-reload client-load-failure error=%lu\n",
                     static_cast<unsigned long>(::GetLastError()));
        return 2;
    }
    ClientMallocFn client_malloc = requiredExport<ClientMallocFn>(client, "windowsPermanentIatClientMalloc");
    ClientFreeFn client_free = requiredExport<ClientFreeFn>(client, "windowsPermanentIatClientFree");
    clientRoundTrip(client_malloc, client_free, 64);

    std::uint64_t total_worker_calls = 0;
    for (std::size_t cycle = 0; cycle < kReloadCycles; ++cycle) {
        g_cycle.store(cycle, std::memory_order_release);
        g_phase.store(1, std::memory_order_release);

        HMODULE plugin = ::LoadLibraryW(plugin_path.c_str());
        if (plugin == nullptr) {
            std::fprintf(stderr, "stage=permanent-iat-backend-reload plugin-load-failure cycle=%zu error=%lu\n", cycle,
                         static_cast<unsigned long>(::GetLastError()));
            std::abort();
        }
        InstallFn install = requiredExport<InstallFn>(plugin, "windowsPermanentIatBackendInstall");
        UninstallFn uninstall = requiredExport<UninstallFn>(plugin, "windowsPermanentIatBackendUninstall");
        ErrorFn backend_error = requiredExport<ErrorFn>(plugin, "windowsPermanentIatBackendError");
        CallsFn calls = requiredExport<CallsFn>(plugin, "windowsPermanentIatBackendCalls");
        SetHoldFn set_hold = requiredExport<SetHoldFn>(plugin, "windowsPermanentIatBackendSetHold");
        ResetEnteredFn reset_entered = requiredExport<ResetEnteredFn>(plugin, "windowsPermanentIatBackendResetEntered");
        EnteredFn entered = requiredExport<EnteredFn>(plugin, "windowsPermanentIatBackendEntered");

        g_phase.store(2, std::memory_order_release);
        if (install() == 0) {
            std::fprintf(stderr, "stage=permanent-iat-backend-reload install-failure cycle=%zu error=%s\n", cycle,
                         backend_error());
            std::abort();
        }
        const std::uint64_t before_direct = calls();
        clientRoundTrip(client_malloc, client_free, 96 + cycle % 31);
        if (calls() <= before_direct) {
            fail("client-IAT-not-routed-through-handler");
        }

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> worker_calls{0};
        std::vector<std::thread> workers;
        workers.reserve(kWorkers);
        for (std::size_t worker = 0; worker < kWorkers; ++worker) {
            workers.emplace_back([&, worker] {
                std::size_t size = 32 + worker;
                while (!stop.load(std::memory_order_acquire)) {
                    clientRoundTrip(client_malloc, client_free, size);
                    size = 32 + ((size + 1) & 127U);
                    worker_calls.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        const std::uint64_t worker_deadline = ::GetTickCount64() + kTimeoutMs;
        while (worker_calls.load(std::memory_order_acquire) < 1000) {
            if (::GetTickCount64() >= worker_deadline) {
                fail("worker-start-timeout");
            }
            std::this_thread::yield();
        }

        // Construct the uninstall thread while allocation handlers are still
        // free-running. The host executable is itself part of the IAT scan, so
        // constructing std::thread after hold=true could block in its allocator
        // before the thread that performs detach even exists.
        std::atomic<bool> begin_uninstall{false};
        std::atomic<bool> uninstall_finished{false};
        int uninstall_result = 0;
        std::thread uninstaller([&] {
            while (!begin_uninstall.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            uninstall_result = uninstall();
            uninstall_finished.store(true, std::memory_order_release);
        });

        g_phase.store(3, std::memory_order_release);
        reset_entered();
        set_hold(1);
        const std::uint64_t entered_deadline = ::GetTickCount64() + kTimeoutMs;
        while (entered() == 0) {
            if (::GetTickCount64() >= entered_deadline) {
                fail("held-handler-entry-timeout");
            }
            std::this_thread::yield();
        }

        begin_uninstall.store(true, std::memory_order_release);
        ::Sleep(1);
        if (uninstall_finished.load(std::memory_order_acquire)) {
            fail("uninstall-finished-before-held-handler-return");
        }
        set_hold(0);
        uninstaller.join();
        if (uninstall_result == 0) {
            std::fprintf(stderr, "stage=permanent-iat-backend-reload uninstall-failure cycle=%zu error=%s\n", cycle,
                         backend_error());
            std::abort();
        }

        g_phase.store(4, std::memory_order_release);
        if (::FreeLibrary(plugin) == FALSE) {
            fail("plugin-FreeLibrary");
        }
        if (::GetModuleHandleW(kPluginName) != nullptr) {
            fail("plugin-remained-loaded");
        }

        // Workers continue through the exact plugin unload boundary. Any IAT
        // slot or cached gateway that can still reach unloaded plugin code will
        // surface as an AV here rather than being hidden by post-unload idleness.
        for (std::size_t probe = 0; probe < 256; ++probe) {
            clientRoundTrip(client_malloc, client_free, 48 + probe % 23);
        }
        ::Sleep(1);

        stop.store(true, std::memory_order_release);
        for (std::thread &worker : workers) {
            worker.join();
        }
        if (worker_calls.load(std::memory_order_relaxed) == 0) {
            fail("workers-made-no-progress");
        }
        total_worker_calls += worker_calls.load(std::memory_order_relaxed);

        if ((cycle + 1) % 25 == 0) {
            std::fprintf(stderr, "stage=permanent-iat-backend-reload progress=%zu/%zu total_worker_calls=%llu\n",
                         cycle + 1, kReloadCycles, static_cast<unsigned long long>(total_worker_calls));
            std::fflush(stderr);
        }
    }

    g_phase.store(5, std::memory_order_release);
    clientRoundTrip(client_malloc, client_free, 128);
    if (::FreeLibrary(client) == FALSE) {
        fail("client-FreeLibrary");
    }

    std::fprintf(stderr, "stage=permanent-iat-backend-reload pass cycles=%zu total_worker_calls=%llu\n", kReloadCycles,
                 static_cast<unsigned long long>(total_worker_calls));
    return 0;
}

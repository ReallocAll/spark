#ifndef _WIN32
#error "stable_entry_probe.cpp is Windows-only"
#endif

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

enum class State {
    Empty,
    Resolved,
    Prepared,
    OwnedOriginal,
    Installing,
    Installed,
    Detaching,
    EntriesRestored,
    Draining,
    QuiescenceProof,
    Detached,
    Destroyed,
    Unsafe,
};

const char *stateName(State state) noexcept
{
    switch (state) {
    case State::Empty: return "Empty";
    case State::Resolved: return "Resolved";
    case State::Prepared: return "Prepared";
    case State::OwnedOriginal: return "OwnedOriginal";
    case State::Installing: return "Installing";
    case State::Installed: return "Installed";
    case State::Detaching: return "Detaching";
    case State::EntriesRestored: return "EntriesRestored";
    case State::Draining: return "Draining";
    case State::QuiescenceProof: return "QuiescenceProof";
    case State::Detached: return "Detached";
    case State::Destroyed: return "Destroyed";
    case State::Unsafe: return "Unsafe";
    }
    return "Unknown";
}

bool transition(State &state, State next) noexcept
{
    const bool allowed =
        (state == State::Empty && next == State::Resolved) ||
        (state == State::Resolved && next == State::Prepared) ||
        (state == State::Prepared && next == State::OwnedOriginal) ||
        (state == State::OwnedOriginal && next == State::Installing) ||
        (state == State::Installing && next == State::Installed) ||
        (state == State::Installed && next == State::Detaching) ||
        (state == State::Detaching && next == State::EntriesRestored) ||
        (state == State::EntriesRestored && next == State::Draining) ||
        (state == State::Draining && next == State::QuiescenceProof) ||
        (state == State::QuiescenceProof && next == State::Detached) ||
        (state == State::Detached && next == State::Destroyed);
    if (!allowed) {
        state = State::Unsafe;
        return false;
    }
    state = next;
    return true;
}

std::uint64_t fnv1a(const std::byte *data, std::size_t size) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint8_t>(data[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string narrow(const wchar_t *text)
{
    if (text == nullptr || *text == L'\0') return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

struct ResolvedTarget {
    const char *logical_name = nullptr;
    const wchar_t *requested_module = nullptr;
    void *address = nullptr;
    std::string owner;
    std::array<std::byte, 16> entry{};
    std::uint64_t hash = 0;
};

bool resolveTarget(ResolvedTarget &target)
{
    HMODULE module = ::GetModuleHandleW(target.requested_module);
    if (module == nullptr) return false;
    target.address = reinterpret_cast<void *>(::GetProcAddress(module, target.logical_name));
    if (target.address == nullptr) return false;
    HMODULE owner = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(target.address), &owner) == FALSE) {
        return false;
    }
    wchar_t path[32768]{};
    const DWORD length = ::GetModuleFileNameW(owner, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) return false;
    target.owner = narrow(path);
    std::memcpy(target.entry.data(), target.address, target.entry.size());
    target.hash = fnv1a(target.entry.data(), target.entry.size());
    return true;
}

struct OwnedPatch {
    std::byte *target = nullptr;
    std::array<std::byte, 8> original{};
    std::array<std::byte, 8> installed{};
};

bool installSynthetic(OwnedPatch &patch)
{
    patch.target = static_cast<std::byte *>(::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (patch.target == nullptr) return false;
    patch.original = {std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0xCC}, std::byte{0xCC}, std::byte{0xCC}};
    patch.installed = {std::byte{0xE9}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0xCC}, std::byte{0xCC}, std::byte{0xCC}};
    std::memcpy(patch.target, patch.original.data(), patch.original.size());
    DWORD old = 0;
    if (::VirtualProtect(patch.target, 4096, PAGE_EXECUTE_READWRITE, &old) == FALSE) return false;
    std::memcpy(patch.target, patch.installed.data(), patch.installed.size());
    if (::FlushInstructionCache(::GetCurrentProcess(), patch.target, patch.installed.size()) == FALSE) return false;
    DWORD ignored = 0;
    if (::VirtualProtect(patch.target, 4096, old, &ignored) == FALSE) return false;
    return true;
}

bool restoreSyntheticOwned(OwnedPatch &patch)
{
    if (std::memcmp(patch.target, patch.installed.data(), patch.installed.size()) != 0) {
        return false;
    }
    DWORD old = 0;
    if (::VirtualProtect(patch.target, 4096, PAGE_EXECUTE_READWRITE, &old) == FALSE) return false;
    std::memcpy(patch.target, patch.original.data(), patch.original.size());
    const bool flushed = ::FlushInstructionCache(::GetCurrentProcess(), patch.target, patch.original.size()) != FALSE;
    DWORD ignored = 0;
    const bool protected_again = ::VirtualProtect(patch.target, 4096, old, &ignored) != FALSE;
    return flushed && protected_again;
}

struct CorridorContext {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<std::uint64_t> active{0};
};

__declspec(noinline) DWORD WINAPI preGuardCorridor(void *opaque)
{
    auto &ctx = *static_cast<CorridorContext *>(opaque);
    ctx.entered.store(true, std::memory_order_release);
    while (!ctx.release.load(std::memory_order_acquire)) {
#if defined(_M_X64) || defined(_M_IX86)
        YieldProcessor();
#else
        ::SwitchToThread();
#endif
    }
    ctx.active.fetch_add(1, std::memory_order_acq_rel);
    ctx.active.fetch_sub(1, std::memory_order_release);
    return 0;
}

bool provePreGuardCounterGap(bool &active_zero, bool &rip_in_corridor, std::uint64_t &rip_value)
{
    CorridorContext ctx;
    HANDLE thread = ::CreateThread(nullptr, 0, &preGuardCorridor, &ctx, 0, nullptr);
    if (thread == nullptr) return false;
    const ULONGLONG deadline = ::GetTickCount64() + 5000;
    while (!ctx.entered.load(std::memory_order_acquire) && ::GetTickCount64() < deadline) {
        ::Sleep(0);
    }
    if (!ctx.entered.load(std::memory_order_acquire)) {
        ::TerminateThread(thread, 1);
        ::CloseHandle(thread);
        return false;
    }
    active_zero = ctx.active.load(std::memory_order_acquire) == 0;
    if (::SuspendThread(thread) == static_cast<DWORD>(-1)) {
        ctx.release.store(true, std::memory_order_release);
        ::WaitForSingleObject(thread, 5000);
        ::CloseHandle(thread);
        return false;
    }
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    const bool got_context = ::GetThreadContext(thread, &context) != FALSE;
#ifdef _M_X64
    rip_value = got_context ? static_cast<std::uint64_t>(context.Rip) : 0;
#else
    rip_value = 0;
#endif
    const auto begin = reinterpret_cast<std::uintptr_t>(&preGuardCorridor);
    const auto rip = static_cast<std::uintptr_t>(rip_value);
    rip_in_corridor = got_context && rip >= begin && rip < begin + 512;
    ::ResumeThread(thread);
    ctx.release.store(true, std::memory_order_release);
    const DWORD waited = ::WaitForSingleObject(thread, 5000);
    ::CloseHandle(thread);
    return got_context && waited == WAIT_OBJECT_0;
}

void printHex(const std::array<std::byte, 16> &bytes, char *out, std::size_t out_size)
{
    static constexpr char digits[] = "0123456789abcdef";
    if (out_size < bytes.size() * 2 + 1) return;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto value = static_cast<unsigned>(std::to_integer<unsigned char>(bytes[i]));
        out[i * 2] = digits[value >> 4];
        out[i * 2 + 1] = digits[value & 0xF];
    }
    out[bytes.size() * 2] = '\0';
}

}  // namespace

int main()
{
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
    if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &policy, sizeof(policy)) == FALSE) {
        std::fprintf(stderr, "GetProcessMitigationPolicy failed: %lu\n", ::GetLastError());
        return 2;
    }
    const bool stable_entry_available = policy.ProhibitDynamicCode == 0;

    std::vector<ResolvedTarget> targets = {
        {"malloc", L"ucrtbase.dll"}, {"calloc", L"ucrtbase.dll"}, {"realloc", L"ucrtbase.dll"},
        {"free", L"ucrtbase.dll"}, {"_malloc_base", L"ucrtbase.dll"}, {"_free_base", L"ucrtbase.dll"},
        {"HeapAlloc", L"kernel32.dll"}, {"HeapReAlloc", L"kernel32.dll"}, {"HeapFree", L"kernel32.dll"},
    };
    for (auto &target : targets) {
        if (!resolveTarget(target)) {
            std::fprintf(stderr, "failed to resolve %ls!%s\n", target.requested_module, target.logical_name);
            return 3;
        }
    }

    State state = State::Empty;
    const std::array<State, 11> path = {State::Resolved, State::Prepared, State::OwnedOriginal, State::Installing,
                                        State::Installed, State::Detaching, State::EntriesRestored, State::Draining,
                                        State::QuiescenceProof, State::Detached, State::Destroyed};
    for (State next : path) {
        if (!transition(state, next)) return 4;
    }
    State illegal = State::Installed;
    const bool illegal_failed_closed = !transition(illegal, State::Destroyed) && illegal == State::Unsafe;
    if (!illegal_failed_closed) return 5;

    bool ownership_conflict_failed_closed = true;
    bool exact_restore_succeeded = true;
    for (int cycle = 0; cycle < 1000; ++cycle) {
        OwnedPatch patch;
        if (!installSynthetic(patch)) return 6;
        if (cycle == 0) {
            patch.target[2] = std::byte{0x7F};
            if (restoreSyntheticOwned(patch)) ownership_conflict_failed_closed = false;
            patch.target[2] = patch.installed[2];
        }
        if (!restoreSyntheticOwned(patch)) exact_restore_succeeded = false;
        ::VirtualFree(patch.target, 0, MEM_RELEASE);
        if (!ownership_conflict_failed_closed || !exact_restore_succeeded) return 7;
    }

    bool active_zero = false;
    bool rip_in_corridor = false;
    std::uint64_t rip = 0;
    if (!provePreGuardCounterGap(active_zero, rip_in_corridor, rip)) return 8;
    if (!active_zero || !rip_in_corridor) return 9;

    FILE *file = nullptr;
    if (fopen_s(&file, "stable-entry-probe.json", "wb") != 0 || file == nullptr) return 10;
    std::fprintf(file,
        "{\n  \"status\": \"PASS\",\n  \"backend\": \"experimental-stable-entry-safety-model\",\n"
        "  \"production_backend_changed\": false,\n  \"dynamic_code_prohibited\": %s,\n  \"stable_entry_available\": %s,\n"
        "  \"state_machine_terminal\": \"%s\",\n  \"illegal_transition_failed_closed\": true,\n"
        "  \"ownership_cycles\": 1000,\n  \"ownership_conflict_failed_closed\": true,\n  \"exact_restore_succeeded\": true,\n"
        "  \"active_zero_pre_guard\": true,\n  \"pre_guard_rip_detected\": true,\n  \"pre_guard_rip\": \"0x%llx\",\n"
        "  \"promotion_gate\": \"NOT_MET\",\n  \"recommendation\": \"POSTPONE\",\n"
        "  \"reason\": \"safety primitives validated only in a synthetic ownership model; no allocator entry patch transaction, 1000 hot-unload cycles, or real-BDS stable-entry backend has been proven\",\n"
        "  \"targets\": [\n",
        policy.ProhibitDynamicCode ? "true" : "false",
        stable_entry_available ? "true" : "false",
        stateName(state),
        static_cast<unsigned long long>(rip));
    for (std::size_t i = 0; i < targets.size(); ++i) {
        char bytes[33]{};
        printHex(targets[i].entry, bytes, sizeof(bytes));
        std::fprintf(file,
            "    {\"logical_name\":\"%s\",\"requested_module\":\"%ls\",\"resolved_address\":\"%p\",\"resolved_owner\":\"%s\",\"entry_bytes\":\"%s\",\"entry_hash_fnv1a64\":\"0x%llx\"}%s\n",
            targets[i].logical_name,
            targets[i].requested_module,
            targets[i].address,
            targets[i].owner.c_str(),
            bytes,
            static_cast<unsigned long long>(targets[i].hash),
            i + 1 == targets.size() ? "" : ",");
    }
    std::fprintf(file, "  ]\n}\n");
    std::fclose(file);
    std::puts("stable-entry safety model PASS; promotion gate NOT_MET; recommendation POSTPONE");
    return 0;
}

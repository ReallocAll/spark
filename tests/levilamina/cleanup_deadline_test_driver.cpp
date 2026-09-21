#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error "The cleanup deadline fixture is Windows-only"
#endif

namespace {

constexpr DWORD kTimeoutExit = ERROR_TIMEOUT;
constexpr std::wstring_view kChildName = L"spark_levilamina_cleanup_deadline_test_child.exe";
constexpr std::wstring_view kSentinel = L"--spark-ll-guard-child";

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(UniqueHandle const &) = delete;
    UniqueHandle &operator=(UniqueHandle const &) = delete;

    UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.release()) {}
    UniqueHandle &operator=(UniqueHandle &&other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void reset(HANDLE handle = nullptr)
    {
        if (*this) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

    HANDLE release()
    {
        HANDLE result = handle_;
        handle_ = nullptr;
        return result;
    }

private:
    HANDLE handle_ = nullptr;
};

DWORD remainingMilliseconds(std::chrono::steady_clock::time_point deadline);

class ChildHandles final {
public:
    ChildHandles() = default;
    ~ChildHandles() { cleanup(); }

    ChildHandles(ChildHandles const &) = delete;
    ChildHandles &operator=(ChildHandles const &) = delete;

    UniqueHandle process;
    UniqueHandle thread;
    UniqueHandle job;
    bool cleanup_forced = false;
    bool cleanup_failed = false;
    bool cleanup_unknown = false;
    bool cleanup_proven = false;
    bool cleanup_query_attempted = false;
    bool cleanup_query_succeeded = false;
    bool cleanup_query_failed = false;
    DWORD cleanup_query_exit = STILL_ACTIVE;
    bool cleanup_terminate_attempted = false;
    bool cleanup_terminate_succeeded = false;
    DWORD cleanup_wait_result = WAIT_FAILED;
    DWORD cleanup_fallback_wait_result = WAIT_FAILED;
    bool cleanup_complete = false;

    void cleanup() noexcept
    {
        if (cleanup_complete) {
            return;
        }
        cleanup_complete = true;

        if (!process) {
            job.reset();
            thread.reset();
            return;
        }

        cleanup_query_attempted = true;
        DWORD exit_code = STILL_ACTIVE;
        if (::GetExitCodeProcess(process.get(), &exit_code) != FALSE) {
            cleanup_query_succeeded = true;
            cleanup_query_exit = exit_code;
            if (exit_code == STILL_ACTIVE) {
                cleanup_forced = true;
                cleanup_terminate_attempted = true;
                cleanup_terminate_succeeded = ::TerminateProcess(process.get(), ERROR_PROCESS_ABORTED) != FALSE;
                if (!cleanup_terminate_succeeded) {
                    cleanup_failed = true;
                    cleanup_unknown = true;
                }
            }
        }
        else {
            cleanup_query_failed = true;
            cleanup_failed = true;
            cleanup_unknown = true;
            cleanup_forced = true;
            cleanup_terminate_attempted = true;
            cleanup_terminate_succeeded = ::TerminateProcess(process.get(), ERROR_PROCESS_ABORTED) != FALSE;
            if (!cleanup_terminate_succeeded) {
                cleanup_failed = true;
                cleanup_unknown = true;
            }
        }

        // A failed query or termination attempt still gets the exact owned job as a bounded fallback.
        if (job && (cleanup_query_failed || (cleanup_terminate_attempted && !cleanup_terminate_succeeded))) {
            job.reset();
        }

        const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1500};
        cleanup_wait_result = ::WaitForSingleObject(process.get(), remainingMilliseconds(wait_deadline));
        if (cleanup_wait_result != WAIT_OBJECT_0) {
            cleanup_failed = true;
            cleanup_unknown = true;
            if (job) {
                job.reset();
                const auto fallback_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1500};
                cleanup_fallback_wait_result =
                    ::WaitForSingleObject(process.get(), remainingMilliseconds(fallback_deadline));
            }
        }

        if (job) {
            job.reset();
        }
        if (cleanup_wait_result == WAIT_OBJECT_0 || cleanup_fallback_wait_result == WAIT_OBJECT_0) {
            cleanup_proven = true;
        }
        else {
            cleanup_failed = true;
            cleanup_unknown = true;
        }
        thread.reset();
        process.reset();
    }
};

struct CaseResult {
    bool passed = false;
    bool forced_cleanup = false;
    bool cleanup_failed = false;
    bool cleanup_unknown = false;
    bool cleanup_proven = false;
    bool cleanup_query_attempted = false;
    bool cleanup_query_succeeded = false;
    bool cleanup_query_failed = false;
    DWORD cleanup_query_exit = STILL_ACTIVE;
    bool cleanup_terminate_attempted = false;
    bool cleanup_terminate_succeeded = false;
    DWORD cleanup_wait_result = WAIT_FAILED;
    DWORD cleanup_fallback_wait_result = WAIT_FAILED;
    DWORD observed_exit = STILL_ACTIVE;
};

std::wstring currentExecutablePath()
{
    std::wstring path(32768, L'\0');
    const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    path.resize(length);
    return path;
}

std::wstring childExecutablePath(std::wstring const &driver_path)
{
    const auto separator = driver_path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    return driver_path.substr(0, separator + 1) + std::wstring{kChildName};
}

std::wstring mutexNameFor(std::wstring const &exact_path)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const wchar_t value : exact_path) {
        for (unsigned shift = 0; shift < sizeof(wchar_t) * 8; shift += 8) {
            hash ^= static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) >> shift) & 0xffU);
            hash *= 1099511628211ULL;
        }
    }
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring suffix(16, L'0');
    for (unsigned index = 0; index != suffix.size(); ++index) {
        suffix[suffix.size() - index - 1] = digits[hash & 0xfU];
        hash >>= 4;
    }
    return L"Local\\spark_ll_cleanup_driver_" + suffix;
}

DWORD remainingMilliseconds(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    const auto rounded =
        remaining + (deadline - now > remaining ? std::chrono::milliseconds{1} : std::chrono::milliseconds{0});
    const auto count = rounded.count();
    return static_cast<DWORD>(std::min<std::int64_t>(count, (std::numeric_limits<DWORD>::max)() - 1));
}

CaseResult runCase(std::wstring const &child_path, std::wstring_view mode, DWORD expected_exit, bool release_child)
{
    CaseResult result;
    const auto outer_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    SECURITY_ATTRIBUTES inherit_attributes{};
    inherit_attributes.nLength = sizeof(inherit_attributes);
    inherit_attributes.bInheritHandle = TRUE;

    UniqueHandle entered(::CreateEventW(&inherit_attributes, TRUE, FALSE, nullptr));
    UniqueHandle release(::CreateEventW(&inherit_attributes, TRUE, FALSE, nullptr));
    if (!entered || !release ||
        ::SetHandleInformation(entered.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == FALSE ||
        ::SetHandleInformation(release.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) == FALSE) {
        return result;
    }

    ChildHandles child;
    const auto record_cleanup = [&]() {
        child.cleanup();
        result.forced_cleanup = child.cleanup_forced;
        result.cleanup_failed = child.cleanup_failed;
        result.cleanup_unknown = child.cleanup_unknown;
        result.cleanup_proven = child.cleanup_proven;
        result.cleanup_query_attempted = child.cleanup_query_attempted;
        result.cleanup_query_succeeded = child.cleanup_query_succeeded;
        result.cleanup_query_failed = child.cleanup_query_failed;
        result.cleanup_query_exit = child.cleanup_query_exit;
        result.cleanup_terminate_attempted = child.cleanup_terminate_attempted;
        result.cleanup_terminate_succeeded = child.cleanup_terminate_succeeded;
        result.cleanup_wait_result = child.cleanup_wait_result;
        result.cleanup_fallback_wait_result = child.cleanup_fallback_wait_result;
    };
    child.job.reset(::CreateJobObjectW(nullptr, nullptr));
    if (!child.job || ::SetHandleInformation(child.job.get(), HANDLE_FLAG_INHERIT, 0) == FALSE) {
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (::SetInformationJobObject(child.job.get(), JobObjectExtendedLimitInformation, &limits,
                                  static_cast<DWORD>(sizeof(limits))) == FALSE) {
        return result;
    }

    std::wstring command = L"\"" + child_path + L"\" " + std::wstring{kSentinel} + L" " + std::wstring{mode} + L" " +
                           std::to_wstring(reinterpret_cast<std::uintptr_t>(entered.get())) + L" " +
                           std::to_wstring(reinterpret_cast<std::uintptr_t>(release.get()));
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_info{};
    if (::CreateProcessW(child_path.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup,
                         &process_info) == FALSE) {
        return result;
    }
    child.process.reset(process_info.hProcess);
    child.thread.reset(process_info.hThread);
    std::fprintf(
        stderr, "deadline-launch: mode=%ls pid=%lu process=%p thread=%p job=%p entered=%p release=%p active-limit=1\n",
        mode.data(), static_cast<unsigned long>(process_info.dwProcessId), static_cast<void *>(child.process.get()),
        static_cast<void *>(child.thread.get()), static_cast<void *>(child.job.get()),
        static_cast<void *>(entered.get()), static_cast<void *>(release.get()));

    if (::AssignProcessToJobObject(child.job.get(), child.process.get()) == FALSE) {
        record_cleanup();
        return result;
    }
    if (::ResumeThread(child.thread.get()) == static_cast<DWORD>(-1)) {
        record_cleanup();
        return result;
    }

    const DWORD entered_wait = ::WaitForSingleObject(entered.get(), remainingMilliseconds(outer_deadline));
    if (entered_wait != WAIT_OBJECT_0) {
        record_cleanup();
        return result;
    }
    if (release_child) {
        if (::SetEvent(release.get()) == FALSE) {
            record_cleanup();
            return result;
        }
    }
    const DWORD process_wait = ::WaitForSingleObject(child.process.get(), remainingMilliseconds(outer_deadline));
    if (process_wait != WAIT_OBJECT_0 || ::GetExitCodeProcess(child.process.get(), &result.observed_exit) == FALSE) {
        record_cleanup();
        return result;
    }
    record_cleanup();
    result.passed = result.observed_exit == expected_exit && result.cleanup_proven && !result.cleanup_failed &&
                    !result.cleanup_unknown && !result.forced_cleanup;
    return result;
}

}  // namespace

int wmain(int argc, wchar_t **)
{
    if (argc != 1) {
        return 64;
    }
    const auto driver_path = currentExecutablePath();
    const auto child_path = childExecutablePath(driver_path);
    if (driver_path.empty() || child_path.empty()) {
        return 1;
    }
    UniqueHandle instance_mutex(::CreateMutexW(nullptr, TRUE, mutexNameFor(driver_path).c_str()));
    if (!instance_mutex) {
        return 1;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 2;
    }
    if (::GetFileAttributesW(child_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    try {
        const auto timeout = runCase(child_path, L"timeout", kTimeoutExit, false);
        std::fprintf(stderr,
                     "deadline-timeout: exit=%lu pass=%d forced=%d cleanup-failed=%d unknown=%d query=%d/%d/%d "
                     "query-exit=%lu terminate=%d/%d wait=%lu fallback-wait=%lu\n",
                     static_cast<unsigned long>(timeout.observed_exit), timeout.passed ? 1 : 0,
                     timeout.forced_cleanup ? 1 : 0, timeout.cleanup_failed ? 1 : 0, timeout.cleanup_unknown ? 1 : 0,
                     timeout.cleanup_query_attempted ? 1 : 0, timeout.cleanup_query_succeeded ? 1 : 0,
                     timeout.cleanup_query_failed ? 1 : 0, static_cast<unsigned long>(timeout.cleanup_query_exit),
                     timeout.cleanup_terminate_attempted ? 1 : 0, timeout.cleanup_terminate_succeeded ? 1 : 0,
                     static_cast<unsigned long>(timeout.cleanup_wait_result),
                     static_cast<unsigned long>(timeout.cleanup_fallback_wait_result));
        if (!timeout.passed) {
            return 1;
        }
        const auto normal = runCase(child_path, L"normal", 0, true);
        std::fprintf(stderr,
                     "deadline-normal: exit=%lu pass=%d forced=%d cleanup-failed=%d unknown=%d query=%d/%d/%d "
                     "query-exit=%lu terminate=%d/%d wait=%lu fallback-wait=%lu\n",
                     static_cast<unsigned long>(normal.observed_exit), normal.passed ? 1 : 0,
                     normal.forced_cleanup ? 1 : 0, normal.cleanup_failed ? 1 : 0, normal.cleanup_unknown ? 1 : 0,
                     normal.cleanup_query_attempted ? 1 : 0, normal.cleanup_query_succeeded ? 1 : 0,
                     normal.cleanup_query_failed ? 1 : 0, static_cast<unsigned long>(normal.cleanup_query_exit),
                     normal.cleanup_terminate_attempted ? 1 : 0, normal.cleanup_terminate_succeeded ? 1 : 0,
                     static_cast<unsigned long>(normal.cleanup_wait_result),
                     static_cast<unsigned long>(normal.cleanup_fallback_wait_result));
        return normal.passed ? 0 : 1;
    }
    catch (...) {
        return 1;
    }
}

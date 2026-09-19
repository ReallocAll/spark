#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "platform/levilamina/callback_state.h"
#include "platform/levilamina/cleanup_deadline_guard.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#error "The cleanup deadline fixture is Windows-only"
#endif

namespace {

using spark::levilamina::CallbackState;
using spark::levilamina::CleanupDeadlineGuard;

constexpr std::wstring_view kSentinel = L"--spark-ll-guard-child";
constexpr std::wstring_view kTimeoutMode = L"timeout";
constexpr std::wstring_view kNormalMode = L"normal";
constexpr auto kGuardTimeout = std::chrono::milliseconds{250};
constexpr auto kNormalCompletionDelay = std::chrono::milliseconds{350};

bool parseInheritedHandle(wchar_t const *text, HANDLE &handle)
{
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    errno = 0;
    wchar_t *end = nullptr;
    const auto value = std::wcstoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == nullptr || *end != L'\0' || value == 0 ||
        value > (std::numeric_limits<std::uintptr_t>::max)()) {
        return false;
    }
    handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
    DWORD flags = 0;
    return ::GetHandleInformation(handle, &flags) != FALSE && (flags & HANDLE_FLAG_INHERIT) != 0;
}

struct BlockingCapture {
    HANDLE entered;
    HANDLE release;

    BlockingCapture(HANDLE entered_handle, HANDLE release_handle) : entered(entered_handle), release(release_handle) {}

    ~BlockingCapture() noexcept
    {
        ::SetEvent(entered);
        ::WaitForSingleObject(release, INFINITE);
    }
};

void releasePending(std::vector<std::function<void()>> &pending, std::shared_ptr<CallbackState> const &state,
                    HANDLE release)
{
    ::SetEvent(release);
    pending.clear();
    state->releasePendingPayloads(1);
}

int runTimeout(HANDLE entered, HANDLE release)
{
    auto state = std::make_shared<CallbackState>();
    std::vector<std::function<void()>> queue;
    state->setSubmitter([&](std::function<void()> work) { queue.push_back(std::move(work)); });
    if (!state->post([capture = std::make_shared<BlockingCapture>(entered, release)] {})) {
        return 1;
    }
    if (state->beginClosing([] {}) != CallbackState::CloseClaim::Owner) {
        auto pending = state->takePendingPayloads();
        releasePending(pending, state, release);
        return 1;
    }
    auto pending = state->takePendingPayloads();
    if (pending.size() != 1) {
        releasePending(pending, state, release);
        return 1;
    }

    CleanupDeadlineGuard guard;
    guard.arm(kGuardTimeout);
    const auto armed_deadline = guard.deadline();
    std::atomic<bool> arm_started = false;
    std::thread repeated_arm;
    try {
        repeated_arm = std::thread([&guard, &arm_started] {
            arm_started.store(true, std::memory_order_release);
            for (int attempt = 0; attempt != 64; ++attempt) {
                guard.arm(std::chrono::seconds{5});
                if ((attempt & 7) == 0) {
                    std::this_thread::yield();
                }
            }
        });
        while (!arm_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const auto deadline_during_rearm = guard.deadline();
        repeated_arm.join();
        if (deadline_during_rearm != armed_deadline || guard.deadline() != armed_deadline) {
            releasePending(pending, state, release);
            guard.completeAndJoin();
            return 1;
        }
    }
    catch (...) {
        if (repeated_arm.joinable()) {
            repeated_arm.join();
        }
        releasePending(pending, state, release);
        guard.completeAndJoin();
        return 1;
    }
    pending.clear();
    state->releasePendingPayloads(1);
    return 1;
}

int runNormal(HANDLE entered, HANDLE release)
{
    auto state = std::make_shared<CallbackState>();
    std::vector<std::function<void()>> queue;
    state->setSubmitter([&](std::function<void()> work) { queue.push_back(std::move(work)); });
    if (!state->post([capture = std::make_shared<BlockingCapture>(entered, release)] {})) {
        return 1;
    }
    if (state->beginClosing([] {}) != CallbackState::CloseClaim::Owner) {
        auto pending = state->takePendingPayloads();
        releasePending(pending, state, release);
        return 1;
    }
    auto pending = state->takePendingPayloads();
    if (pending.size() != 1) {
        releasePending(pending, state, release);
        return 1;
    }

    CleanupDeadlineGuard guard;
    guard.arm(kGuardTimeout);
    const auto guard_deadline = guard.deadline();
    std::thread cleanup([state, pending = std::move(pending)]() mutable {
        pending.clear();
        state->releasePendingPayloads(1);
    });

    const auto entered_result = ::WaitForSingleObject(entered, 5000);
    if (entered_result != WAIT_OBJECT_0 || !::SetEvent(release)) {
        ::SetEvent(release);
        cleanup.join();
        guard.completeAndJoin();
        return 1;
    }
    cleanup.join();
    if (!state->waitQuiescent(std::chrono::steady_clock::now() + std::chrono::seconds{2})) {
        guard.completeAndJoin();
        return 1;
    }
    guard.completeAndJoin();
    const auto normal_completion_deadline = guard_deadline + kNormalCompletionDelay;
    std::this_thread::sleep_until(normal_completion_deadline);
    const bool completed_after_deadline = CleanupDeadlineGuard::Clock::now() >= normal_completion_deadline;
    std::fprintf(stderr, "cleanup-child-normal: completed-after-deadline=%d\n", completed_after_deadline ? 1 : 0);
    return completed_after_deadline ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 5 || argv == nullptr || argv[1] == nullptr || argv[2] == nullptr) {
        return 64;
    }
    const std::wstring_view sentinel{argv[1]};
    const std::wstring_view mode{argv[2]};
    if (sentinel != kSentinel || (mode != kTimeoutMode && mode != kNormalMode)) {
        return 64;
    }
    if (argv[3] == nullptr || argv[4] == nullptr) {
        return 64;
    }
    HANDLE entered = nullptr;
    HANDLE release = nullptr;
    if (!parseInheritedHandle(argv[3], entered) || !parseInheritedHandle(argv[4], release)) {
        return 64;
    }
    try {
        return mode == kTimeoutMode ? runTimeout(entered, release) : runNormal(entered, release);
    }
    catch (std::exception const &error) {
        std::fprintf(stderr, "cleanup-child-fail: %s\n", error.what());
        ::SetEvent(release);
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "cleanup-child-fail: unknown exception\n");
        ::SetEvent(release);
        return 1;
    }
}

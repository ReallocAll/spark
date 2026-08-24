#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <mutex>

#include <cpptrace/cpptrace.hpp>
#include <sys/eventfd.h>
#include <sys/syscall.h>

#include "native/sampler/capture.h"

namespace spark {

namespace {

constexpr int KSignal = SIGPROF;
constexpr unsigned KPhaseBits = 2;
constexpr std::uint64_t KPhaseMask = (std::uint64_t{1} << KPhaseBits) - 1;
constexpr std::uint64_t KRequested = 1;
constexpr std::uint64_t KCapturing = 2;
constexpr std::uint64_t KComplete = 3;

using Token = std::uintptr_t;
constexpr Token KMaxToken = static_cast<Token>(std::numeric_limits<std::uint64_t>::max() >> KPhaseBits);

static_assert(sizeof(Token) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<Token>::digits >= std::numeric_limits<std::uint64_t>::digits);
static_assert(KMaxToken <= static_cast<Token>(std::numeric_limits<std::uint64_t>::max() >> KPhaseBits));

std::atomic<std::uint64_t> GState{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
std::atomic<Token> GNextToken{1};
static_assert(std::atomic<Token>::is_always_lock_free);
std::atomic<bool> GArmed{false};
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> GCaptureAdmitted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<std::uint64_t> GHandlerUseCount{0};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

CaptureBuffer GResult;
int GEventFd = -1;
bool GEventFdInitAttempted = false;
void *GModulePin = nullptr;
bool GModulePinAttempted = false;

std::mutex GControlMutex;
bool GHandlerInstalled = false;
bool GAcceptingCaptures = false;
bool GCleanupPending = false;
struct sigaction GPreviousAction{};

std::atomic<std::atomic<bool> *> GTestHandlerEntered{nullptr};
std::atomic<std::atomic<bool> *> GTestHandlerRelease{nullptr};
std::atomic<std::atomic<bool> *> GTestHandlerWakeEntered{nullptr};
std::atomic<std::atomic<bool> *> GTestHandlerWakeRelease{nullptr};
static_assert(std::atomic<std::atomic<bool> *>::is_always_lock_free);

void handler(int, siginfo_t *, void *);

std::uint64_t captureState(Token token, std::uint64_t phase)
{
    return (static_cast<std::uint64_t>(token) << KPhaseBits) | phase;
}

std::uint64_t statePhase(std::uint64_t state)
{
    return state & KPhaseMask;
}

bool monotonicNow(timespec &now)
{
    return ::clock_gettime(CLOCK_MONOTONIC, &now) == 0;
}

timespec addSeconds(timespec value, std::time_t seconds)
{
    value.tv_sec += seconds;
    return value;
}

bool remainingUntil(const timespec &deadline, timespec &remaining)
{
    timespec now{};
    if (!monotonicNow(now)) {
        return false;
    }
    if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        remaining = {};
        return true;
    }
    remaining.tv_sec = deadline.tv_sec - now.tv_sec;
    if (now.tv_nsec > deadline.tv_nsec) {
        --remaining.tv_sec;
        remaining.tv_nsec = (1'000'000'000L - now.tv_nsec) + deadline.tv_nsec;
    }
    else {
        remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
    }
    return true;
}

void drainEventFd()
{
    if (GEventFd < 0) {
        return;
    }
    std::uint64_t value = 0;
    while (::read(GEventFd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
    }
}

bool waitForState(std::uint64_t expected_state, const timespec &deadline)
{
    for (;;) {
        if (GState.load(std::memory_order_acquire) == expected_state) {
            return true;
        }

        timespec timeout{};
        if (!remainingUntil(deadline, timeout)) {
            return GState.load(std::memory_order_acquire) == expected_state;
        }
        if (timeout.tv_sec == 0 && timeout.tv_nsec == 0) {
            return GState.load(std::memory_order_acquire) == expected_state;
        }

        pollfd event{};
        event.fd = GEventFd;
        event.events = POLLIN;
        const int result = ::ppoll(&event, 1, &timeout, nullptr);
        const int error = errno;
        const bool complete = GState.load(std::memory_order_acquire) == expected_state;
        if (complete) {
            return true;
        }
        if (result < 0) {
            if (error == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        drainEventFd();
    }
}

bool waitForQuiescence(const timespec &deadline)
{
    for (;;) {
        const std::uint64_t state = GState.load(std::memory_order_acquire);
        const bool capturing = statePhase(state) == KCapturing;
        const bool in_handler = GHandlerUseCount.load(std::memory_order_acquire) != 0;
        if (!capturing && !in_handler) {
            return true;
        }

        timespec timeout{};
        if (!remainingUntil(deadline, timeout)) {
            return false;
        }
        if (timeout.tv_sec == 0 && timeout.tv_nsec == 0) {
            const std::uint64_t final_state = GState.load(std::memory_order_acquire);
            return statePhase(final_state) != KCapturing && GHandlerUseCount.load(std::memory_order_acquire) == 0;
        }

        pollfd event{};
        event.fd = GEventFd;
        event.events = POLLIN;
        const int result = ::ppoll(&event, 1, &timeout, nullptr);
        const int error = errno;
        const std::uint64_t after_state = GState.load(std::memory_order_acquire);
        const bool after_handler = GHandlerUseCount.load(std::memory_order_acquire) != 0;
        if (result < 0 && error != EINTR) {
            return statePhase(after_state) != KCapturing && !after_handler;
        }
        if (result == 0) {
            return statePhase(after_state) != KCapturing && !after_handler;
        }
        drainEventFd();
    }
}

bool samePathAsExecutable(const char *path)
{
    if (path == nullptr) {
        return false;
    }
    char executable[PATH_MAX]{};
    const ssize_t length = ::readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(executable)) {
        return false;
    }
    executable[length] = '\0';
    return std::strcmp(path, executable) == 0;
}

bool pinHandlerModule()
{
    if (GModulePin != nullptr) {
        return true;
    }
    if (GModulePinAttempted) {
        return false;
    }
    GModulePinAttempted = true;

#ifndef RTLD_NODELETE
    return false;
#else
    Dl_info module{};
    if (::dladdr(reinterpret_cast<void *>(&handler), &module) == 0 || module.dli_fname == nullptr) {
        return false;
    }
    constexpr int flags = RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE;
    GModulePin = ::dlopen(module.dli_fname, flags);
    if (GModulePin == nullptr && samePathAsExecutable(module.dli_fname)) {
        GModulePin = ::dlopen(nullptr, flags);
    }
    return GModulePin != nullptr;
#endif
}

bool initializeEventFd()
{
    if (GEventFd >= 0) {
        return true;
    }
    if (GEventFdInitAttempted) {
        return false;
    }
    GEventFdInitAttempted = true;
    GEventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return GEventFd >= 0;
}

bool installIgnoredAction()
{
    struct sigaction ignored{};
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    return ::sigaction(KSignal, &ignored, nullptr) == 0;
}

Token nextToken()
{
    Token token = GNextToken.load(std::memory_order_relaxed);
    for (;;) {
        if (token == 0 || token > KMaxToken) {
            return 0;
        }
        const Token next = token == KMaxToken ? 0 : token + 1;
        if (GNextToken.compare_exchange_weak(token, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return token;
        }
    }
}

void handler(int, siginfo_t *info, void *)
{
    GHandlerUseCount.fetch_add(1, std::memory_order_relaxed);
    struct HandlerUseGuard {
        ~HandlerUseGuard() { GHandlerUseCount.fetch_sub(1, std::memory_order_release); }
    } handler_use_guard;

    if (info == nullptr || info->si_code != SI_QUEUE) {
        return;
    }
    const Token token = reinterpret_cast<Token>(info->si_value.sival_ptr);
    if (token == 0 || token > KMaxToken) {
        return;
    }
    const std::uint64_t requested_state = captureState(token, KRequested);
    const std::uint64_t capturing_state = captureState(token, KCapturing);
    std::uint64_t expected = requested_state;
    if (!GState.compare_exchange_strong(expected, capturing_state, std::memory_order_acq_rel)) {
        return;
    }
    if (auto *entered = GTestHandlerEntered.load(std::memory_order_acquire)) {
        entered->store(true, std::memory_order_release);
        auto *release = GTestHandlerRelease.load(std::memory_order_acquire);
        while (release != nullptr && !release->load(std::memory_order_acquire)) {
        }
    }
    GResult.count = cpptrace::safe_generate_raw_trace(GResult.ips, CaptureBuffer::kMax, 0);
    GState.store(captureState(token, KComplete), std::memory_order_release);
    if (auto *entered = GTestHandlerWakeEntered.load(std::memory_order_acquire)) {
        entered->store(true, std::memory_order_release);
        auto *release = GTestHandlerWakeRelease.load(std::memory_order_acquire);
        while (release != nullptr && !release->load(std::memory_order_acquire)) {
        }
    }
    const std::uint64_t wake = 1;
    if (GEventFd >= 0) {
        (void)::write(GEventFd, &wake, sizeof(wake));
    }
}

}  // namespace

bool Capture::arm()
{
    std::scoped_lock lock(GControlMutex);
    if (GCleanupPending) {
        return false;
    }
    if (GHandlerInstalled) {
        return GAcceptingCaptures && GArmed.load(std::memory_order_acquire);
    }
    if (GState.load(std::memory_order_acquire) != 0) {
        return false;
    }
    if (!cpptrace::can_signal_safe_unwind() || !pinHandlerModule() || !initializeEventFd()) {
        return false;
    }

    // Warm the safe path before the first signal-handler capture.
    cpptrace::frame_ptr warm[10];
    const std::size_t n = cpptrace::safe_generate_raw_trace(warm, 10);
    if (n == 0) {
        return false;
    }
    cpptrace::safe_object_frame frame;
    cpptrace::get_safe_object_frame(warm[0], &frame);

    struct sigaction action{};
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (::sigaction(KSignal, &action, &GPreviousAction) != 0) {
        return false;
    }

    GHandlerInstalled = true;
    GAcceptingCaptures = true;
    GArmed.store(true, std::memory_order_release);
    return true;
}

bool Capture::disarm()
{
    std::scoped_lock lock(GControlMutex);
    if (!GHandlerInstalled && !GCleanupPending) {
        return true;
    }

    GAcceptingCaptures = false;
    GArmed.store(false, std::memory_order_release);
    if (GCaptureAdmitted.load(std::memory_order_acquire)) {
        GCleanupPending = true;
        return false;
    }
    const std::uint64_t initial_state = GState.load(std::memory_order_acquire);
    if (statePhase(initial_state) == KRequested) {
        std::uint64_t expected = initial_state;
        GState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    if (!installIgnoredAction()) {
        GCleanupPending = true;
        return false;
    }

    timespec now{};
    if (!monotonicNow(now)) {
        GCleanupPending = true;
        return false;
    }
    const timespec deadline = addSeconds(now, 1);
    if (!waitForQuiescence(deadline)) {
        GCleanupPending = true;
        return false;
    }

    const std::uint64_t state = GState.load(std::memory_order_acquire);
    if (statePhase(state) == KComplete) {
        std::uint64_t expected = state;
        GState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
    if (GState.load(std::memory_order_acquire) != 0) {
        GCleanupPending = true;
        return false;
    }
    if (::sigaction(KSignal, &GPreviousAction, nullptr) != 0) {
        GCleanupPending = true;
        return false;
    }
    GHandlerInstalled = false;
    GCleanupPending = false;
    return true;
}

void Capture::setHandlerGateForTesting(std::atomic<bool> *entered, std::atomic<bool> *release)
{
    GTestHandlerRelease.store(release, std::memory_order_release);
    GTestHandlerEntered.store(entered, std::memory_order_release);
}

void Capture::setHandlerWakeGateForTesting(std::atomic<bool> *entered, std::atomic<bool> *release)
{
    GTestHandlerWakeRelease.store(release, std::memory_order_release);
    GTestHandlerWakeEntered.store(entered, std::memory_order_release);
}

void Capture::setNextTokenForTesting(std::uintptr_t next_token)
{
    std::scoped_lock lock(GControlMutex);
    GNextToken.store(next_token, std::memory_order_relaxed);
}

bool Capture::captureThread(std::uint64_t tid, CaptureBuffer &out)
{
    out.count = 0;
    {
        std::scoped_lock lock(GControlMutex);
        if (!GAcceptingCaptures || !GHandlerInstalled || GCleanupPending) {
            return false;
        }
        bool expected = false;
        if (!GCaptureAdmitted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
    }
    struct AdmissionGuard {
        ~AdmissionGuard() { GCaptureAdmitted.store(false, std::memory_order_release); }
    } admission_guard;

    timespec start{};
    if (!monotonicNow(start)) {
        return false;
    }
    const Token token = nextToken();
    if (token == 0) {
        return false;
    }
    const std::uint64_t requested_state = captureState(token, KRequested);
    const std::uint64_t capturing_state = captureState(token, KCapturing);
    const std::uint64_t complete_state = captureState(token, KComplete);
    drainEventFd();
    GResult.count = 0;
    std::uint64_t expected_state = 0;
    if (!GState.compare_exchange_strong(expected_state, requested_state, std::memory_order_acq_rel)) {
        return false;
    }

    siginfo_t info{};
    info.si_signo = KSignal;
    info.si_code = SI_QUEUE;
    info.si_pid = getpid();
    info.si_uid = getuid();
    info.si_value.sival_ptr = reinterpret_cast<void *>(token);
    if (::syscall(SYS_rt_tgsigqueueinfo, getpid(), static_cast<pid_t>(tid), KSignal, &info) != 0) {
        std::uint64_t expected = requested_state;
        GState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        return false;
    }

    const timespec deadline_one = addSeconds(start, 1);
    const timespec deadline_two = addSeconds(start, 2);
    if (!waitForState(complete_state, deadline_one)) {
        std::uint64_t state = GState.load(std::memory_order_acquire);
        if (state == requested_state && GState.compare_exchange_strong(state, 0, std::memory_order_acq_rel)) {
            return false;
        }
        if (state == capturing_state) {
            if (!waitForState(complete_state, deadline_two)) {
                return false;
            }
            state = complete_state;
        }
        if (state == complete_state) {
            std::uint64_t expected = complete_state;
            GState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        }
        return false;
    }

    out = GResult;
    std::uint64_t expected = complete_state;
    if (!GState.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
        return false;
    }
    return out.count > 0;
}

bool Capture::isThreadRunning(std::uint64_t tid)
{
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%llu/stat", static_cast<unsigned long long>(tid));
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return true;
    }
    char buf[256];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) {
        return true;
    }
    buf[n] = '\0';
    // stat layout: "pid (comm) STATE ..." — comm may contain spaces/parens, so scan
    // from the last ')'.
    char *p = std::strrchr(buf, ')');
    if (p == nullptr || p[1] == '\0' || p[2] == '\0') {
        return true;
    }
    char state = p[2];
    return state == 'R';
}

}  // namespace spark

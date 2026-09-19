#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/levilamina/callback_state.h"

namespace {

using spark::levilamina::CallbackState;

constexpr auto kWait = std::chrono::seconds{2};
constexpr auto kShortWait = std::chrono::milliseconds{100};

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

struct Harness {
    std::shared_ptr<CallbackState> state = std::make_shared<CallbackState>();
    std::vector<std::function<void()>> queue;

    Harness()
    {
        state->setSubmitter([this](std::function<void()> work) { queue.push_back(std::move(work)); });
    }

    std::function<void()> take()
    {
        require(!queue.empty(), "expected queued work");
        auto work = std::move(queue.back());
        queue.pop_back();
        return work;
    }
};

struct DestructionProbe {
    std::shared_ptr<std::atomic_bool> destroyed;

    explicit DestructionProbe(std::shared_ptr<std::atomic_bool> value) : destroyed(std::move(value)) {}

    ~DestructionProbe() { destroyed->store(true, std::memory_order_release); }
};

struct SelfWaitPayload {
    std::shared_ptr<CallbackState> state;
    std::shared_ptr<std::atomic_int> result;

    SelfWaitPayload(std::shared_ptr<CallbackState> value, std::shared_ptr<std::atomic_int> output)
        : state(std::move(value)), result(std::move(output))
    {
    }

    ~SelfWaitPayload() noexcept { result->store(static_cast<int>(state->beginClosing()), std::memory_order_release); }
};

template <typename Predicate>
bool waitUntil(std::condition_variable &condition, std::mutex &mutex, Predicate predicate,
               std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex);
    return condition.wait_until(lock, deadline, std::move(predicate));
}

void testInlineAdmissionAndCleanup()
{
    auto state = std::make_shared<CallbackState>();
    std::atomic_int runs{0};
    std::atomic_int errors{0};
    state->setErrorCallback([&](std::string const &) { ++errors; });

    require(state->invokeInline([&] {
                require(state->isInBodyOnCurrentThread(), "inline body did not install TLS state");
                ++runs;
            }),
            "inline body was not admitted");
    require(runs.load() == 1, "admitted inline body did not run");
    require(state->activeBodies() == 0, "admitted inline body remained active");

    auto accepted_result = std::make_shared<std::atomic_int>(-1);
    auto accepted_payload = std::make_shared<SelfWaitPayload>(state, accepted_result);
    require(state->invokeInline([accepted_payload = std::move(accepted_payload)] {}),
            "inline payload body was not admitted");
    require(accepted_result->load(std::memory_order_acquire) ==
                static_cast<int>(CallbackState::CloseClaim::SelfWaitRejected),
            "admitted inline payload destructor did not reject self-wait");

    require(state->invokeInline([] { throw std::runtime_error{"inline body"}; }),
            "throwing inline body was not admitted");
    require(errors.load() == 1, "inline body exception was not reported");
    require(state->activeBodies() == 0, "throwing inline body remained active");
    require(state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "inline body cleanup did not quiesce");

    require(state->beginClosing() == CallbackState::CloseClaim::Owner, "inline close was not claimed");
    auto result = std::make_shared<std::atomic_int>(-1);
    auto payload = std::make_shared<SelfWaitPayload>(state, result);
    require(!state->invokeInline([payload = std::move(payload)] {}), "inline body was admitted after closing");
    require(result->load(std::memory_order_acquire) ==
                static_cast<int>(CallbackState::CloseClaim::SelfWaitRejected),
            "rejected inline payload destructor did not reject self-wait");
    require(runs.load() == 1, "rejected inline body ran");
    auto diagnostics = state->takeDiagnosticCallbacks();
    state->destroyDiagnosticCallbacks(diagnostics);
    require(state->activeBodies() == 0, "rejected inline body remained active");
    require(state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "inline diagnostic cleanup did not quiesce");
    state->markClosed();
}

void testInlineBlockedBodyPreventsQuiescence()
{
    auto state = std::make_shared<CallbackState>();
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    bool admitted = false;
    std::thread runner([&] {
        admitted = state->invokeInline([&] {
            {
                std::lock_guard lock(mutex);
                started = true;
            }
            condition.notify_all();
            std::unique_lock lock(mutex);
            condition.wait_until(lock, std::chrono::steady_clock::now() + kWait, [&] { return release; });
        });
    });

    require(waitUntil(
                condition, mutex, [&] { return started; }, std::chrono::steady_clock::now() + kWait),
            "inline body did not start");
    require(state->activeBodies() == 1, "blocked inline body was not accounted");
    require(!state->waitQuiescent(std::chrono::steady_clock::now() + kShortWait),
            "blocked inline body did not hold quiescence");
    require(state->beginClosing() == CallbackState::CloseClaim::Owner, "close with blocked inline body failed");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    runner.join();
    require(admitted, "blocked inline body was not admitted");
    require(state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "inline body did not quiesce after release");
    state->markClosed();
}

void testPostCloseExactlyOnceAndLateWrapper()
{
    Harness harness;
    std::atomic_int runs{0};
    require(harness.state->post([&] { ++runs; }), "post failed");
    auto wrapper = harness.take();
    require(harness.state->beginClosing() == CallbackState::CloseClaim::Owner, "close was not claimed");
    auto pending = harness.state->takePendingPayloads();
    require(pending.size() == 1, "expected one cancelled payload");
    harness.state->destroyPendingPayloads(pending);
    require(pending.empty(), "cancelled payloads were not destroyed");
    wrapper();
    require(runs.load() == 0, "cancelled wrapper executed its body");
    require(harness.state->pendingWorkSlots() == 0, "cancelled slot remained pending");
    require(harness.state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "cancelled payload cleanup did not quiesce");
    harness.state->markClosed();
    require(harness.state->phase() == CallbackState::Phase::Closed, "state did not close");
    require(!harness.state->post([&] { ++runs; }), "post succeeded after close");
}

void testRunningBodyAndPayloadDestruction()
{
    Harness harness;
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool release = false;
    std::atomic_int runs{0};
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    auto payload = std::make_shared<DestructionProbe>(destroyed);
    require(harness.state->post([&, payload] {
        ++runs;
        {
            std::lock_guard lock(mutex);
            started = true;
        }
        condition.notify_all();
        std::unique_lock lock(mutex);
        condition.wait_until(lock, std::chrono::steady_clock::now() + kWait, [&] { return release; });
    }),
            "post failed");
    auto wrapper = harness.take();
    payload.reset();
    std::thread runner([&] { wrapper(); });
    require(waitUntil(
                condition, mutex, [&] { return started; }, std::chrono::steady_clock::now() + kWait),
            "body did not start");
    require(harness.state->activeBodies() == 1, "running body was not accounted");
    require(harness.state->beginClosing() == CallbackState::CloseClaim::Owner, "close was not claimed");
    auto pending = harness.state->takePendingPayloads();
    require(pending.empty(), "running body unexpectedly had pending payloads");
    harness.state->destroyPendingPayloads(pending);
    require(!harness.state->waitQuiescent(std::chrono::steady_clock::now() + kShortWait),
            "blocked body did not hold quiescence");
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    condition.notify_all();
    runner.join();
    require(harness.state->waitQuiescent(std::chrono::steady_clock::now() + kWait), "running body did not quiesce");
    require(runs.load() == 1, "running body count was incorrect");
    require(destroyed->load(std::memory_order_acquire), "executed payload was not destroyed");
    harness.state->markClosed();
}

void testThrowingBodyAndConcurrentClose()
{
    Harness harness;
    std::atomic_int errors{0};
    harness.state->setErrorCallback([&](std::string const &) { ++errors; });
    require(harness.state->post([] { throw std::runtime_error{"body"}; }), "post failed");
    auto wrapper = harness.take();
    wrapper();
    require(errors.load() == 1, "body exception was not reported");
    require(harness.state->activeBodies() == 0, "throwing body remained active");
    require(harness.state->beginClosing() == CallbackState::CloseClaim::Owner, "close was not claimed");
    auto pending = harness.state->takePendingPayloads();
    harness.state->destroyPendingPayloads(pending);
    auto diagnostics = harness.state->takeDiagnosticCallbacks();
    require(diagnostics.info == nullptr && diagnostics.error != nullptr, "error callback was not detached");
    harness.state->destroyDiagnosticCallbacks(diagnostics);
    require(harness.state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "throwing body cleanup did not quiesce");
    harness.state->markClosed();

    Harness concurrent;
    std::mutex barrier_mutex;
    std::condition_variable barrier;
    bool owner_entered = false;
    bool waiter_claimed = false;
    bool allow_owner_finish = false;
    CallbackState::CloseClaim waiter_claim = CallbackState::CloseClaim::AlreadyClosed;
    bool waiter_closed = false;
    bool owner_timed_out = false;
    std::thread owner([&] {
        const auto claim = concurrent.state->beginClosing();
        {
            std::lock_guard lock(barrier_mutex);
            owner_entered = claim == CallbackState::CloseClaim::Owner;
        }
        barrier.notify_all();
        owner_timed_out = !waitUntil(
            barrier, barrier_mutex, [&] { return allow_owner_finish; }, std::chrono::steady_clock::now() + kWait);
        concurrent.state->markClosed();
    });
    require(waitUntil(
                barrier, barrier_mutex, [&] { return owner_entered; }, std::chrono::steady_clock::now() + kWait),
            "closing owner did not reach the barrier");
    std::thread waiter([&] {
        waiter_claim = concurrent.state->beginClosing();
        {
            std::lock_guard lock(barrier_mutex);
            waiter_claimed = true;
        }
        barrier.notify_all();
        waiter_closed = concurrent.state->waitClosed(std::chrono::steady_clock::now() + kWait);
    });
    require(waitUntil(
                barrier, barrier_mutex, [&] { return waiter_claimed; }, std::chrono::steady_clock::now() + kWait),
            "closing waiter did not reach the barrier");
    {
        std::lock_guard lock(barrier_mutex);
        allow_owner_finish = true;
    }
    barrier.notify_all();
    owner.join();
    waiter.join();
    require(!owner_timed_out, "closing owner barrier timed out");
    require(waiter_claim == CallbackState::CloseClaim::AlreadyClosing, "closing waiter claimed ownership");
    require(waiter_closed, "closing waiter did not observe closed state");
    require(concurrent.state->phase() == CallbackState::Phase::Closed, "concurrent close did not finish");
}

void testProducerSelfWaitRejection()
{
    auto state = std::make_shared<CallbackState>();
    state->reserveProducer();
    state->setProducerThreadIdentity();
    require(state->beginClosing() == CallbackState::CloseClaim::SelfWaitRejected,
            "producer self-close was not rejected");
    state->rollbackProducerReservation();
    require(state->beginClosing() == CallbackState::CloseClaim::Owner, "close after producer rollback failed");
    state->markClosed();

    auto producer_state = std::make_shared<CallbackState>();
    producer_state->reserveProducer();
    std::atomic_int producer_claim{static_cast<int>(CallbackState::CloseClaim::AlreadyClosed)};
    std::thread producer([producer_state, &producer_claim] {
        producer_state->setProducerThreadIdentity();
        producer_claim.store(static_cast<int>(producer_state->beginClosing()), std::memory_order_release);
        producer_state->markProducerDone();
    });
    require(producer_state->waitProducer(std::chrono::steady_clock::now() + kWait),
            "producer completion was not observed");
    producer.join();
    require(producer_claim.load(std::memory_order_acquire) ==
                static_cast<int>(CallbackState::CloseClaim::SelfWaitRejected),
            "producer-thread close was not rejected");
    require(producer_state->beginClosing() == CallbackState::CloseClaim::Owner,
            "close after producer completion failed");
    producer_state->markClosed();
}

void testPayloadDestructorSelfWait()
{
    {
        Harness harness;
        auto result = std::make_shared<std::atomic_int>(-1);
        auto payload = std::make_shared<SelfWaitPayload>(harness.state, result);
        require(harness.state->post([payload] {}), "executed payload post failed");
        payload.reset();
        auto wrapper = harness.take();
        wrapper();
        require(result->load(std::memory_order_acquire) ==
                    static_cast<int>(CallbackState::CloseClaim::SelfWaitRejected),
                "executed payload destructor did not reject self-wait");
        require(harness.state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
                "executed payload cleanup did not quiesce");
        require(harness.state->beginClosing() == CallbackState::CloseClaim::Owner,
                "close after executed payload failed");
        harness.state->markClosed();
    }

    {
        Harness harness;
        auto result = std::make_shared<std::atomic_int>(-1);
        auto payload = std::make_shared<SelfWaitPayload>(harness.state, result);
        require(harness.state->post([payload] {}), "cancelled payload post failed");
        payload.reset();
        auto wrapper = harness.take();
        require(harness.state->beginClosing() == CallbackState::CloseClaim::Owner,
                "close before cancelled payload failed");
        auto pending = harness.state->takePendingPayloads();
        require(pending.size() == 1, "expected one cancelled payload");
        harness.state->destroyPendingPayloads(pending);
        require(result->load(std::memory_order_acquire) ==
                    static_cast<int>(CallbackState::CloseClaim::SelfWaitRejected),
                "cancelled payload destructor did not reject self-wait");
        wrapper();
        require(harness.state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
                "cancelled payload cleanup did not quiesce");
        harness.state->markClosed();
    }
}

void testBlockedReportAndDetachedDiagnostics()
{
    auto state = std::make_shared<CallbackState>();
    std::mutex mutex;
    std::condition_variable condition;
    bool report_started = false;
    bool release_report = false;
    state->setInfoCallback([&](std::string const &) {
        {
            std::lock_guard lock(mutex);
            report_started = true;
        }
        condition.notify_all();
        std::unique_lock lock(mutex);
        condition.wait_until(lock, std::chrono::steady_clock::now() + kWait, [&] { return release_report; });
    });

    std::thread reporter([&] { state->reportInfo("blocked"); });
    const bool started =
        waitUntil(condition, mutex, [&] { return report_started; }, std::chrono::steady_clock::now() + kWait);
    if (!started) {
        {
            std::lock_guard lock(mutex);
            release_report = true;
        }
        condition.notify_all();
        reporter.join();
        throw std::runtime_error{"diagnostic report did not start"};
    }

    bool setup_ok = state->beginClosing() == CallbackState::CloseClaim::Owner;
    auto callbacks = state->takeDiagnosticCallbacks();
    setup_ok = setup_ok && callbacks.info != nullptr && callbacks.error == nullptr;
    state->destroyDiagnosticCallbacks(callbacks);
    setup_ok = setup_ok && callbacks.info == nullptr && callbacks.error == nullptr;
    const bool blocked = !state->waitQuiescent(std::chrono::steady_clock::now() + kShortWait);
    {
        std::lock_guard lock(mutex);
        release_report = true;
    }
    condition.notify_all();
    reporter.join();
    require(setup_ok, "detached diagnostic callbacks were not transferred and destroyed");
    require(blocked, "blocked diagnostic report did not hold quiescence");
    require(state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "diagnostic report did not quiesce after release");
    state->markClosed();
}

void testSubmissionFailureCleanup()
{
    auto state = std::make_shared<CallbackState>();
    state->setSubmitter([](std::function<void()>) { throw std::runtime_error{"submit"}; });
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    auto payload = std::make_shared<DestructionProbe>(destroyed);
    require(!state->post([payload] {}), "throwing submitter reported success");
    payload.reset();
    require(destroyed->load(std::memory_order_acquire), "failed submission payload was not destroyed");
    require(state->pendingWorkSlots() == 0, "failed submission left a pending slot");
    require(state->activeBodies() == 0, "failed submission left active accounting");
    auto pending = state->takePendingPayloads();
    require(pending.empty(), "failed submission left payloads to cancel");
    state->destroyPendingPayloads(pending);
    require(state->waitQuiescent(std::chrono::steady_clock::now() + kWait),
            "failed submission cleanup did not quiesce");
    require(state->beginClosing() == CallbackState::CloseClaim::Owner, "close after failed submission failed");
    state->markClosed();
}

void testCleanupScopeSelfWaitOracle()
{
    auto state = std::make_shared<CallbackState>();
    std::atomic_int fatal_calls{0};
    state->setFatalHandler([&](CallbackState::FatalReason) { ++fatal_calls; });
    {
        auto cleanup_scope = state->enterCleanupScope();
        require(state->isInBodyOnCurrentThread(), "cleanup scope did not set TLS state");
        require(state->activeBodies() == 0, "cleanup scope changed active accounting");
    }
    require(!state->isInBodyOnCurrentThread(), "cleanup scope TLS state remained installed");

    CallbackState::CloseClaim nested_claim = CallbackState::CloseClaim::AlreadyClosed;
    bool cleanup_seen = false;
    require(state->beginClosing([&] {
        cleanup_seen = state->isInBodyOnCurrentThread();
        nested_claim = state->beginClosing();
    }) == CallbackState::CloseClaim::Owner,
            "cleanup close was not claimed");
    require(cleanup_seen, "detach callback did not run in cleanup scope");
    require(nested_claim == CallbackState::CloseClaim::SelfWaitRejected, "cleanup-scope self-close was not rejected");
    require(fatal_calls.load() == 0, "self-wait rejection invoked the fatal handler");
    state->markClosed();
}

}  // namespace

int wmain(int argc, wchar_t **)
{
    if (argc != 1) {
        return 64;
    }
    try {
        std::fprintf(stderr, "protocol-test-start\n");
        testInlineAdmissionAndCleanup();
        testInlineBlockedBodyPreventsQuiescence();
        testPostCloseExactlyOnceAndLateWrapper();
        testRunningBodyAndPayloadDestruction();
        testThrowingBodyAndConcurrentClose();
        testProducerSelfWaitRejection();
        testPayloadDestructorSelfWait();
        testBlockedReportAndDetachedDiagnostics();
        testSubmissionFailureCleanup();
        testCleanupScopeSelfWaitOracle();
        std::fprintf(stderr, "protocol-test-pass\n");
        return 0;
    }
    catch (std::exception const &error) {
        std::fprintf(stderr, "protocol-test-fail: %s\n", error.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "protocol-test-fail: unknown exception\n");
        return 1;
    }
}

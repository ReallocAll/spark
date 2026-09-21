#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

#include "platform/levilamina/world_gauge_provider.h"

namespace spark::levilamina {

LeviLaminaWorldGaugeProvider::LeviLaminaWorldGaugeProvider(std::shared_ptr<CallbackState>) {}
LeviLaminaWorldGaugeProvider::~LeviLaminaWorldGaugeProvider() = default;

}  // namespace spark::levilamina

namespace Bedrock::PubSub::Detail {

class SubscriptionBodyBase {};

}  // namespace Bedrock::PubSub::Detail

namespace {

using spark::levilamina::WorldCallbackAdmission;
using spark::levilamina::WorldCallbackControl;
using spark::levilamina::WorldCallbackFunction;
using spark::levilamina::CallbackState;
using spark::levilamina::LeviLaminaWorldGaugeProvider;

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void testAlreadyDispatchedClosureBlocksClose()
{
    WorldCallbackAdmission admission;
    auto lease = admission.tryEnter();
    require(static_cast<bool>(lease), "admitted callback was rejected");
    admission.closeAdmission();
    require(!admission.accepting(), "closed callback admission reopened");
    require(admission.activeCallbacks() == 1, "closure ownership was not retained");
    require(!admission.waitQuiescent(std::chrono::steady_clock::now()),
            "self close was admitted while a closure was active");
    require(!static_cast<bool>(admission.tryEnter()), "new callback entered after close");

    lease.release();
    require(admission.activeCallbacks() == 0, "closure ownership was not released");
    require(admission.waitQuiescent(std::chrono::steady_clock::now() + std::chrono::seconds{1}),
            "closure quiescence was not established after release");
}

void testWaiterSeesClosureRelease()
{
    WorldCallbackAdmission admission;
    auto lease = admission.tryEnter();
    require(static_cast<bool>(lease), "waiter setup callback was rejected");
    admission.closeAdmission();

    std::atomic_bool quiescent{false};
    std::thread waiter([&] {
        quiescent = admission.waitQuiescent(std::chrono::steady_clock::now() + std::chrono::seconds{1});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    require(!quiescent.load(), "waiter ignored an active callback closure");
    lease.release();
    waiter.join();
    require(quiescent.load(), "waiter did not observe callback closure release");
}

std::atomic_int *g_calls = nullptr;
std::atomic_bool g_throw = false;
std::atomic_bool g_throw_publication = false;

void countCallback(LeviLaminaWorldGaugeProvider &, int amount)
{
    if (g_throw.exchange(false, std::memory_order_acq_rel)) {
        throw std::runtime_error{"test callback failure"};
    }
    g_calls->fetch_add(amount, std::memory_order_relaxed);
}

struct ThrowingPublication final {
    using Callback = WorldCallbackFunction<int>;

    Callback callback;

    ThrowingPublication(std::shared_ptr<WorldCallbackControl> control, Callback::BodyFunction body)
        : callback(std::move(control), body)
    {
    }

    ThrowingPublication(ThrowingPublication const &other) : callback(other.callback)
    {
        if (g_throw_publication.load(std::memory_order_acquire)) {
            throw std::runtime_error{"synthetic publication failure"};
        }
    }

    ThrowingPublication(ThrowingPublication &&) noexcept = default;
    ThrowingPublication &operator=(ThrowingPublication const &) = delete;
    ThrowingPublication &operator=(ThrowingPublication &&) noexcept = default;

    void operator()(int amount) const noexcept { callback(amount); }
};

void testCallableCopiesAreOwnedUntilDestroyed()
{
    auto callback_state = std::make_shared<CallbackState>();
    LeviLaminaWorldGaugeProvider provider{callback_state};
    auto control = std::make_shared<WorldCallbackControl>(&provider, callback_state);
    std::atomic_int calls{0};
    g_calls = &calls;

    using Callback = WorldCallbackFunction<int>;
    Callback::BodyFunction body = &countCallback;
    {
        Callback direct{control, body};
        const auto direct_count = control->liveWrappers();
        direct = direct;
        direct = std::move(direct);
        require(control->liveWrappers() == direct_count, "wrapper self-assignment changed ownership count");
    }

    std::function<void(int)> published{Callback{control, body}};
    require(control->liveWrappers() == 1, "initial published callback was not counted");

    auto copied = published;
    require(control->liveWrappers() == 2, "std::function copy was not counted");
    std::function<void(int)> moved = std::move(copied);
    require(control->liveWrappers() == 2, "std::function move changed ownership count");

    published(3);
    require(calls.load(std::memory_order_relaxed) == 3, "admitted callback body did not execute");
    g_throw = true;
    published(7);
    require(calls.load(std::memory_order_relaxed) == 3, "exceptional callback body unexpectedly committed");
    require(control->activeBodies() == 0, "exceptional callback body leaked its lease");
    published(2);
    require(calls.load(std::memory_order_relaxed) == 5, "callback did not recover after exception");

    std::function<void(int)> partial_publication;
    partial_publication = moved;
    require(control->liveWrappers() == 3, "partial callback publication was not counted");

    control->closeAdmission();
    require(!control->waitQuiescent(std::chrono::steady_clock::now()),
            "close ignored host-held callback copies");
    published(4);
    require(calls.load(std::memory_order_relaxed) == 5, "closed callback executed provider body");

    published = {};
    partial_publication = {};
    moved = {};
    require(control->liveWrappers() == 0, "callback copies were not released");
    require(control->waitQuiescent(std::chrono::steady_clock::now() + std::chrono::seconds{1}),
            "callback wrapper quiescence was not established");
    g_calls = nullptr;
}

void testDistinctWrapperAssignmentsTransferOwnership()
{
    auto callback_state = std::make_shared<CallbackState>();
    LeviLaminaWorldGaugeProvider provider{callback_state};
    auto old_control = std::make_shared<WorldCallbackControl>(&provider, callback_state);
    auto new_control = std::make_shared<WorldCallbackControl>(&provider, callback_state);
    std::atomic_int calls{0};
    g_calls = &calls;
    g_throw = false;

    using Callback = WorldCallbackFunction<int>;
    Callback::BodyFunction body = &countCallback;
    Callback first{old_control, body};
    Callback second{new_control, body};
    require(old_control->liveWrappers() == 1 && new_control->liveWrappers() == 1,
            "distinct wrapper setup was not counted");

    first = second;
    require(old_control->liveWrappers() == 0, "copy assignment retained old control ownership");
    require(new_control->liveWrappers() == 2, "copy assignment did not retain new control ownership");

    old_control->closeAdmission();
    require(old_control->waitQuiescent(std::chrono::steady_clock::now()),
            "old control remained owned after copy assignment");

    second = std::move(first);
    require(new_control->liveWrappers() == 1, "move assignment duplicated new control ownership");
    const auto before_inert_call = calls.load(std::memory_order_relaxed);
    first(7);
    require(calls.load(std::memory_order_relaxed) == before_inert_call,
            "moved-from wrapper still invoked its callback");
    second(2);
    require(calls.load(std::memory_order_relaxed) == before_inert_call + 2,
            "moved-to wrapper did not retain callback behavior");

    new_control->closeAdmission();
    require(!new_control->waitQuiescent(std::chrono::steady_clock::now()),
            "live moved-to wrapper did not block close");
    second = Callback{};
    require(new_control->liveWrappers() == 0, "moved-to wrapper ownership was not released");
    require(new_control->waitQuiescent(std::chrono::steady_clock::now()),
            "new control did not quiesce after moved-to wrapper release");
    g_calls = nullptr;
}

void testPublicationExceptionUnwindsTemporaryWrapper()
{
    auto callback_state = std::make_shared<CallbackState>();
    LeviLaminaWorldGaugeProvider provider{callback_state};
    auto control = std::make_shared<WorldCallbackControl>(&provider, callback_state);
    std::atomic_int calls{0};
    g_calls = &calls;
    g_throw = false;
    g_throw_publication = false;

    using Callback = WorldCallbackFunction<int>;
    ThrowingPublication source{control, &countCallback};
    std::function<void(int)> first_published{std::move(source)};
    require(control->liveWrappers() == 1, "first publication was not counted");
    std::function<void(int)> retained_copy = first_published;
    require(control->liveWrappers() == 2, "retained publication copy was not counted");

    g_throw_publication = true;
    bool threw = false;
    try {
        std::function<void(int)> second_published = first_published;
        static_cast<void>(second_published);
    }
    catch (std::runtime_error const &) {
        threw = true;
    }
    g_throw_publication = false;
    require(threw, "second publication did not throw during callable publication");
    require(control->liveWrappers() == 2, "failed publication leaked or released retained ownership");

    control->closeAdmission();
    require(!control->waitQuiescent(std::chrono::steady_clock::now()),
            "retained publication copies did not block close");
    retained_copy = {};
    require(control->liveWrappers() == 1, "retained publication copy was not released");
    require(!control->waitQuiescent(std::chrono::steady_clock::now()),
            "first publication was released unexpectedly");
    first_published = {};
    require(control->liveWrappers() == 0, "first publication ownership was not released");
    require(control->waitQuiescent(std::chrono::steady_clock::now()),
            "control did not quiesce after publication copies were released");
    g_calls = nullptr;
}

void testWorldCapturePolicies()
{
    int chunk = 0;
    require(!spark::levilamina::bds::detail::floorChunkCoordinate(
                std::numeric_limits<float>::quiet_NaN(), chunk
            ),
            "NaN chunk coordinate was accepted");
    require(!spark::levilamina::bds::detail::floorChunkCoordinate(
                std::numeric_limits<float>::infinity(), chunk
            ),
            "+inf chunk coordinate was accepted");
    require(!spark::levilamina::bds::detail::floorChunkCoordinate(
                -std::numeric_limits<float>::infinity(), chunk
            ),
            "-inf chunk coordinate was accepted");
    require(spark::levilamina::bds::detail::floorChunkCoordinate(-0.1F, chunk) && chunk == -1,
            "finite chunk coordinate floor changed");

    require(!spark::levilamina::bds::detail::isLoadedChunkState(::ChunkState::Generating),
            "pre-loaded chunk state was accepted");
    require(spark::levilamina::bds::detail::isLoadedChunkState(::ChunkState::Loaded),
            "loaded chunk state was rejected");
    require(!spark::levilamina::bds::detail::dimensionCaptureInvalidates(
                spark::levilamina::bds::DimensionRetention::ForeignLevel
            ),
            "foreign dimension was treated as capture failure");
    require(spark::levilamina::bds::detail::dimensionCaptureInvalidates(
                spark::levilamina::bds::DimensionRetention::Failed
            ),
            "failed dimension capture did not invalidate");
}

void testSubscriptionBodyTransfer()
{
    using spark::levilamina::bds::pubsub::moveSubscriptionBody;
    using ::Bedrock::PubSub::SubscriptionBase;
    using ::Bedrock::PubSub::Detail::SubscriptionBodyBase;

    SubscriptionBase source;
    SubscriptionBase destination;
    auto body = std::make_shared<SubscriptionBodyBase>();
    source.mBody = body;
    moveSubscriptionBody(destination, source);
    require(destination.mBody.lock() == body, "subscription body was not transferred");
    require(source.mBody.expired() && source.mBody.use_count() == 0, "subscription source was not emptied");

    SubscriptionBase empty_source;
    SubscriptionBase empty_destination;
    moveSubscriptionBody(empty_destination, empty_source);
    require(empty_destination.mBody.expired() && empty_source.mBody.expired(),
            "empty subscription body transfer changed state");
}

}  // namespace

int main()
{
    try {
        testAlreadyDispatchedClosureBlocksClose();
        testWaiterSeesClosureRelease();
        testCallableCopiesAreOwnedUntilDestroyed();
        testDistinctWrapperAssignmentsTransferOwnership();
        testPublicationExceptionUnwindsTemporaryWrapper();
        testWorldCapturePolicies();
        testSubscriptionBodyTransfer();
        return 0;
    }
    catch (std::exception const &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    catch (...) {
        std::fprintf(stderr, "unknown callback admission test failure\n");
        return 1;
    }
}

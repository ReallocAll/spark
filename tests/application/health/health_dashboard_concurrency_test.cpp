#include <cassert>
#include <chrono>

#include "../../net/native_exit_gate.h"
#include "health_dashboard_test_support.h"

using namespace spark::health_dashboard_test;  // NOLINT(google-build-using-namespace)

namespace {

void testNativeExitRetainsDashboard()
{
    Probe probe;
    spark::test::NativeExitGate gate;
    spark::HealthDashboard *owner = nullptr;
    auto dashboard = makeDashboard(probe, [&](const auto &) {
        spark::test::holdNativeThreadExit(gate);
        owner->requestStop();
    });
    owner = dashboard.get();
    assert(dashboard->open(dataAt(0), "NativeExit").accepted);
    gate.waitEntered();
    const auto begin = std::chrono::steady_clock::now();
    assert(!dashboard->shutdownWithin(std::chrono::milliseconds(20)));
    assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));
    assert(probe.destroyed_count.load() == 0);
    assert(!dashboard->open(dataAt(1), "Replacement").accepted);
    assert(probe.factory_count.load() == 1);
    gate.unblock();
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(probe.destroyed_count.load() == 1);
}

void testRetainedConnectionRetryCompletesOnce()
{
    Probe probe;
    probe.next_open_success = false;
    probe.allow_close.store(false);
    auto dashboard = makeDashboard(probe);
    assert(dashboard->open(dataAt(0), "First").accepted);
    assert(waitFor(probe, [&] { return probe.completions.size() == 1; }));
    assert(!probe.completions[0].ok);
    assert(probe.factory_count.load() == 1);
    assert(probe.destroyed_count.load() == 0);

    assert(dashboard->open(dataAt(1), "Second").accepted);
    assert(waitFor(probe, [&] { return probe.completions.size() == 2; }));
    assert(!probe.completions[1].ok);
    assert(probe.completions[1].completed);
    assert(probe.completions[1].error == "previous health dashboard still closing; retry");
    assert(!dashboard->openPending());
    assert(spark::HealthDashboardTestAccess::idle(*dashboard));
    assert(probe.factory_count.load() == 1);
    assert(probe.destroyed_count.load() == 0);

    probe.allow_close.store(true);
    {
        std::scoped_lock lock(probe.mutex);
        probe.next_open_success = true;
    }
    assert(dashboard->open(dataAt(2), "Third").accepted);
    assert(waitFor(probe, [&] { return probe.completions.size() == 3; }));
    assert(probe.completions[2].ok);
    assert(probe.factory_count.load() == 2);
    assert(probe.destroyed_count.load() == 1);
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(probe.destroyed_count.load() == 2);
}

void testCompletionCanAdmitRetryWithoutLosingResult()
{
    Probe probe;
    probe.next_open_success = false;
    spark::HealthDashboard *dashboard_ptr = nullptr;
    auto dashboard = makeDashboard(probe, [&](spark::HealthDashboard::OpenResult result) {
        const bool retry = result.sender_name == "First";
        if (retry) {
            {
                std::scoped_lock lock(probe.mutex);
                probe.next_open_success = true;
            }
            assert(dashboard_ptr->open(dataAt(1), "Retry").accepted);
        }
        {
            std::scoped_lock lock(probe.mutex);
            probe.completions.push_back(std::move(result));
        }
        probe.cv.notify_all();
        if (retry) {
            throw std::runtime_error("notifier failed after retry admission");
        }
    });
    dashboard_ptr = dashboard.get();
    assert(dashboard->open(dataAt(0), "First").accepted);
    assert(waitFor(probe, [&] { return probe.completions.size() == 2; }));
    assert(!probe.completions[0].ok);
    assert(probe.completions[1].ok);
    assert(!dashboard->openPending());
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
}

void testBlockedConnectionFactoryIsBoundedAndReapable()
{
    Probe probe;
    probe.configureFactory(true);
    auto dashboard = makeDashboard(probe);

    const auto queued = dashboard->open(dataAt(0), "Factory");
    assert(queued.accepted);
    assert(probe.waitFactoryEntered());

    const auto begin = std::chrono::steady_clock::now();
    assert(!dashboard->shutdownWithin(std::chrono::milliseconds::zero()));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    assert(elapsed < std::chrono::milliseconds(250));
    assert(spark::HealthDashboardTestAccess::stopping(*dashboard));

    probe.releaseFactory();
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(spark::HealthDashboardTestAccess::idle(*dashboard));
    assert(!probe.close_during_work.load(std::memory_order_acquire));
}

void testBlockedWebSocketOpenCancels()
{
    Probe probe;
    probe.next_open_block = true;
    auto dashboard = makeDashboard(probe);

    assert(dashboard->open(dataAt(0), "Open").accepted);
    assert(probe.waitConnectionReady());
    assert(probe.latest->waitOpenEntered());
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(!probe.close_during_work.load(std::memory_order_acquire));
    assert(spark::HealthDashboardTestAccess::idle(*dashboard));
}

void testBlockedBytebinUploadCancels()
{
    Probe probe;
    probe.configureUpload(true);
    auto dashboard = makeDashboard(probe);

    assert(dashboard->open(dataAt(0), "Upload").accepted);
    assert(probe.waitUploadEntered());
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(probe.upload_cancelled.load(std::memory_order_acquire));
    assert(!probe.close_during_work.load(std::memory_order_acquire));
}

void testBlockedSendStatisticsCancels()
{
    Probe probe;
    auto dashboard = makeDashboard(probe);

    assert(dashboard->open(dataAt(0), "Send").accepted);
    assert(waitFor(probe, [&] { return probe.completions.size() == 1; }));
    assert(probe.completions.front().ok);
    assert(probe.waitConnectionReady());
    probe.latest->setClient(true);
    probe.latest->configureSend(false, true);
    assert(dashboard->enqueueUpdate(dataAt(10000), 10000));
    assert(probe.latest->waitSendEntered());

    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(!probe.close_during_work.load(std::memory_order_acquire));
    assert(spark::HealthDashboardTestAccess::idle(*dashboard));
}

void testRepeatedShutdownIsSafe()
{
    Probe probe;
    auto dashboard = makeDashboard(probe);
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    dashboard->shutdown();
}

void testPostShutdownOperationsAreRejected()
{
    Probe probe;
    auto dashboard = makeDashboard(probe);
    assert(dashboard->open(dataAt(0), "Initial").accepted);
    assert(waitFor(probe, [&] { return probe.completions.size() == 1; }));
    assert(probe.completions.front().ok);
    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));

    const int trusted_before = probe.trusted_send_count.load(std::memory_order_acquire);
    const auto reopen = dashboard->open(dataAt(0), "Rejected");
    assert(!reopen.accepted);
    assert(!reopen.error.empty());
    assert(!dashboard->enqueueUpdate(dataAt(10000), 10000));
    dashboard->sendClientTrusted("pending");
    assert(probe.trusted_send_count.load(std::memory_order_acquire) == trusted_before);
}

}  // namespace

int main()
{
    testNativeExitRetainsDashboard();
    testRetainedConnectionRetryCompletesOnce();
    testCompletionCanAdmitRetryWithoutLosingResult();
    testBlockedConnectionFactoryIsBoundedAndReapable();
    testBlockedWebSocketOpenCancels();
    testBlockedBytebinUploadCancels();
    testBlockedSendStatisticsCancels();
    testRepeatedShutdownIsSafe();
    testPostShutdownOperationsAreRejected();
    return 0;
}

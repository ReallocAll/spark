#include <cassert>
#include <chrono>

#include "health_dashboard_test_support.h"

using namespace spark::health_dashboard_test;  // NOLINT(google-build-using-namespace)

namespace {

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
    testBlockedConnectionFactoryIsBoundedAndReapable();
    testBlockedWebSocketOpenCancels();
    testBlockedBytebinUploadCancels();
    testBlockedSendStatisticsCancels();
    testRepeatedShutdownIsSafe();
    testPostShutdownOperationsAreRejected();
    return 0;
}

#include <cassert>
#include <chrono>
#include <vector>

#include "health_dashboard_test_support.h"

using namespace spark::health_dashboard_test;  // NOLINT(google-build-using-namespace)

int main()
{
    Probe probe;
    auto dashboard = makeDashboard(probe);

    const auto queued = dashboard->open(dataAt(0), "Alice");
    assert(queued.accepted);
    assert(dashboard->openPending());
    assert(probe.waitConnectionReady());
    assert(probe.latest->waitOpenEntered());
    assert(!dashboard->open(dataAt(0), "Second").accepted);
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == 1; }));
    assert(probe.completions[0].ok);
    assert(probe.completions[0].completed);
    assert(probe.completions[0].url == "https://viewer/initial-key");
    assert(probe.completions[0].payload_key == "initial-key");
    const auto uploaded_channel = probe.uploaded_channel;
    assert(uploaded_channel && uploaded_channel->channel_id == "fake-channel");
    assert(uploaded_channel->public_key == std::vector<std::uint8_t>({1, 2, 3}));
    assert(dashboard->isOpen());

    probe.latest->setClient(true);
    assert(dashboard->updateDue(10000));
    assert(dashboard->enqueueUpdate(dataAt(10000), 10000));
    assert(waitFor(probe, [&] { return probe.latest->sendCount() == 1; }));

    assert(dashboard->shutdownWithin(std::chrono::seconds(2)));
    assert(!dashboard->isOpen());
    assert(!dashboard->openPending());
    assert(spark::HealthDashboardTestAccess::idle(*dashboard));
    return 0;
}

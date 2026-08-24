#include <cassert>
#include <thread>
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
    if (!uploaded_channel) {
        return 1;
    }
    assert(uploaded_channel->public_key == std::vector<std::uint8_t>({1, 2, 3}));
    assert(dashboard->isOpen());

    probe.latest->setClient(false);
    assert(!dashboard->updateDue(10000));
    probe.latest->setClient(true);
    assert(!dashboard->updateDue(9999));
    assert(dashboard->updateDue(10000));
    assert(dashboard->enqueueUpdate(dataAt(10000), 10000));
    assert(!dashboard->enqueueUpdate(dataAt(10000), 20000));
    assert(waitFor(probe, [&] { return probe.latest->sendCount() == 1; }));

    probe.latest->configureSend(false, true);
    assert(dashboard->enqueueUpdate(dataAt(20000), 20000));
    assert(probe.latest->waitSendEntered());
    assert(!dashboard->enqueueUpdate(dataAt(30000), 30000));
    probe.latest->releaseSend();
    assert(waitFor(probe, [&] { return probe.latest->sendCount() == 2; }));

    probe.latest->configureSend(true, false);
    assert(dashboard->enqueueUpdate(dataAt(30000), 30000));
    assert(waitFor(probe, [&] { return dashboard->consumeFailure(); }));
    assert(!dashboard->isOpen());

    dashboard->shutdown();
    probe.next_open_success = true;
    const auto restarted = dashboard->open(dataAt(0), "Restart");
    assert(restarted.accepted);
    assert(probe.latest->waitOpenEntered());
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == 2; }));
    assert(probe.completions.back().ok);
    probe.latest->setClient(true);

    probe.latest->configureSend(false, true);
    assert(dashboard->enqueueUpdate(dataAt(10000), 10000));
    assert(probe.latest->waitSendEntered());
    const std::size_t completions_before_shutdown = probe.completions.size();
    std::thread shutdown_thread([&] { dashboard->shutdown(); });
    while (!spark::HealthDashboardTestAccess::stopping(*dashboard)) {
        std::this_thread::yield();
    }
    probe.latest->releaseSend();
    shutdown_thread.join();
    assert(probe.completions.size() == completions_before_shutdown);
    assert(!probe.close_during_work.load(std::memory_order_acquire));

    probe.next_open_success = false;
    const auto failed = dashboard->open(dataAt(0), "Failure");
    assert(failed.accepted);
    assert(probe.latest->waitOpenEntered());
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == completions_before_shutdown + 1; }));
    assert(!probe.completions.back().ok);
    dashboard->shutdown();

    probe.next_open_success = true;
    const auto trusted = dashboard->open(dataAt(0), "Trust");
    assert(trusted.accepted);
    assert(probe.latest->waitOpenEntered());
    probe.latest->releaseOpen();
    assert(waitFor(probe, [&] { return probe.completions.size() == completions_before_shutdown + 2; }));
    assert(dashboard->pendingKey("pending") == std::vector<std::uint8_t>({9, 8, 7}));
    dashboard->sendClientTrusted("pending");
    dashboard->shutdown();
    return 0;
}

#include <cassert>
#include <cmath>
#include <functional>
#include <map>
#include <string>

#include "core/stats/network_monitor.h"
#include "network_monitor_test_support.h"

using namespace spark;        // NOLINT(google-build-using-namespace)
using namespace spark::test;  // NOLINT(google-build-using-namespace)

namespace {

void testSubtract()
{
    NetworkInterfaceInfo a = makeInfo("eth0", 1000, 100, 2000, 200);
    NetworkInterfaceInfo b = makeInfo("eth0", 600, 60, 1200, 120);
    NetworkInterfaceInfo diff = a.subtract(b);
    assert(diff.rx_bytes == 400);
    assert(diff.rx_packets == 40);
    assert(diff.tx_bytes == 800);
    assert(diff.tx_packets == 80);

    // Subtracting zero returns the original.
    NetworkInterfaceInfo zero;
    NetworkInterfaceInfo diff2 = a.subtract(zero);
    assert(diff2.rx_bytes == 1000);
    assert(diff2.tx_bytes == 2000);

    // isZero
    assert(zero.isZero());
    assert(!a.isZero());
}

void testNetworkMonitorFirstPoll()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 10, 2000, 20)}});

    NetworkMonitor monitor(std::ref(poller));
    bool result = monitor.poll();
    assert(!result);  // first poll returns false

    auto totals = monitor.systemTotals();
    assert(totals.size() == 1);
    assert(totals.count("eth0") == 1);
}

void testNetworkMonitorSecondPoll()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 10, 2000, 20)}});
    poller.addReading({{"eth0", makeInfo("eth0", 7000, 70, 8000, 80)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    monitor.poll();  // first poll, returns false
    clock.advance(std::chrono::seconds(60));
    bool result = monitor.poll();
    assert(result);  // second poll returns true

    auto snap = monitor.snapshot();
    assert(snap.count("eth0") == 1);
    const auto &s = snap["eth0"];
    assert(s.rx_bytes_per_second.present);
    // diff = 6000 bytes over 60s = 100 bytes/s
    assert(std::abs(s.rx_bytes_per_second.mean - 100.0) < 0.001);
    assert(std::abs(s.tx_bytes_per_second.mean - 100.0) < 0.001);
    assert(std::abs(s.rx_packets_per_second.mean - 1.0) < 0.001);
    assert(std::abs(s.tx_packets_per_second.mean - 1.0) < 0.001);
}

void testNetworkMonitorIgnoresVethAndBr()
{
    MockPoller poller;
    poller.addReading({
        {"eth0", makeInfo("eth0", 1000, 10, 2000, 20)},
        {"veth1234", makeInfo("veth1234", 500, 5, 500, 5)},
        {"br-abc", makeInfo("br-abc", 300, 3, 300, 3)},
    });
    poller.addReading({
        {"eth0", makeInfo("eth0", 7000, 70, 8000, 80)},
        {"veth1234", makeInfo("veth1234", 1000, 10, 1000, 10)},
        {"br-abc", makeInfo("br-abc", 600, 6, 600, 6)},
    });

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    monitor.poll();
    clock.advance(std::chrono::seconds(60));
    monitor.poll();

    auto snap = monitor.snapshot();
    assert(snap.count("eth0") == 1);
    assert(!snap.contains("veth1234"));
    assert(!snap.contains("br-abc"));
}

void testNetworkMonitorEmptyPolls()
{
    MockPoller poller;
    NetworkMonitor monitor(std::ref(poller));
    assert(!monitor.poll());
    assert(!monitor.poll());
    assert(monitor.snapshot().empty());
}

}  // namespace

int main()
{
    testSubtract();
    testNetworkMonitorFirstPoll();
    testNetworkMonitorSecondPoll();
    testNetworkMonitorIgnoresVethAndBr();
    testNetworkMonitorEmptyPolls();

    std::printf("All network_monitor tests passed.\n");
    return 0;
}

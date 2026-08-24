#include <cassert>
#include <chrono>
#include <cmath>
#include <functional>

#include "network_monitor_test_support.h"

using namespace spark;        // NOLINT(google-build-using-namespace)
using namespace spark::test;  // NOLINT(google-build-using-namespace)

namespace {

void testResetAndRebaseline()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 100, 2000, 200)}});
    poller.addReading({{"eth0", makeInfo("eth0", 2000, 200, 3000, 300)}});
    poller.addReading({{"eth0", makeInfo("eth0", 100, 10, 200, 20)}});
    poller.addReading({{"eth0", makeInfo("eth0", 1100, 110, 1200, 120)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    require(!monitor.poll(), "initial reset baseline was accepted");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "normal reset prelude delta was rejected");
    clock.advance(std::chrono::seconds(10));
    require(!monitor.poll(), "counter reset was accepted as a delta");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "post-reset delta was rejected");
    const auto snapshot = monitor.snapshot();
    require(snapshot.at("eth0").rx_bytes_per_second.min >= 0.0, "reset produced a negative rate");
    require(std::abs(snapshot.at("eth0").rx_bytes_per_second.mean - 100.0) < 0.001,
            "post-reset rate used the absolute counter");
}

void testInterfaceLifecycle()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 1000, 10, 1000, 10)}});
    poller.addReading(
        {{"eth0", makeInfo("eth0", 1100, 11, 1100, 11)}, {"wlan0", makeInfo("wlan0", 1000000, 1000, 1000000, 1000)}});
    poller.addReading({});
    for (int i = 0; i < NetworkMonitor::kWindowSize - 1; ++i) {
        poller.addReading({});
    }
    poller.addReading({{"wlan0", makeInfo("wlan0", 2000000, 2000, 2000000, 2000)}});
    poller.addReading({{"wlan0", makeInfo("wlan0", 2000100, 2001, 2000100, 2001)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    require(!monitor.poll(), "initial interface baseline was accepted");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "existing interface delta was rejected");
    require(!monitor.snapshot().contains("wlan0"), "new interface absolute counter was accepted");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "disappeared interface zero sample was rejected");
    require(std::abs(monitor.snapshot().at("eth0").rx_bytes_per_second.mean - 5.0) < 0.001,
            "disappeared interface did not retain its prior rate");
    for (int i = 0; i < NetworkMonitor::kWindowSize - 1; ++i) {
        clock.advance(std::chrono::seconds(10));
        require(monitor.poll(), "repeated interface absence was rejected");
    }
    require(std::abs(monitor.snapshot().at("eth0").rx_bytes_per_second.mean) < 0.001,
            "repeated interface absence did not roll toward zero");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "reappeared interface baseline was not accompanied by the absent zero sample");
    require(std::abs(monitor.snapshot().at("eth0").rx_bytes_per_second.mean) < 0.001,
            "reappeared interface changed the absent zero history");
    clock.advance(std::chrono::seconds(10));
    require(monitor.poll(), "reappeared interface delta was rejected");
    require(std::abs(monitor.snapshot().at("wlan0").rx_bytes_per_second.mean - 10.0) < 0.001,
            "reappeared interface rate was incorrect");
}

void testElapsedGuard()
{
    MockPoller poller;
    poller.addReading({{"eth0", makeInfo("eth0", 100, 10, 100, 10)}});
    poller.addReading({{"eth0", makeInfo("eth0", 200, 20, 200, 20)}});
    poller.addReading({{"eth0", makeInfo("eth0", 201, 21, 201, 21)}});

    MockClock clock;
    NetworkMonitor monitor(std::ref(poller), std::ref(clock));
    require(!monitor.poll(), "initial elapsed baseline was accepted");
    require(!monitor.poll(), "zero elapsed interval was accepted");
    clock.advance(std::chrono::microseconds(1));
    require(monitor.poll(), "small positive elapsed interval was rejected");
    const double rate = monitor.snapshot().at("eth0").rx_bytes_per_second.mean;
    require(std::isfinite(rate), "small elapsed interval produced a non-finite rate");
    require(rate >= 0.0, "small elapsed interval produced a negative rate");
}

}  // namespace

int main()
{
    testResetAndRebaseline();
    testInterfaceLifecycle();
    testElapsedGuard();
    return 0;
}

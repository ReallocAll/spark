#ifndef ENDSTONE_SPARK_NETWORK_MONITOR_TEST_SUPPORT_H
#define ENDSTONE_SPARK_NETWORK_MONITOR_TEST_SUPPORT_H

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "core/stats/network_monitor.h"

namespace spark::test {

inline void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "network monitor: %s\n", message);
        std::abort();
    }
}

class MockPoller {
public:
    void addReading(const std::map<std::string, NetworkInterfaceInfo> &reading) { readings_.push_back(reading); }

    std::map<std::string, NetworkInterfaceInfo> operator()()
    {
        if (index_ >= readings_.size()) {
            return {};
        }
        return readings_[index_++];
    }

private:
    std::vector<std::map<std::string, NetworkInterfaceInfo>> readings_;
    std::size_t index_ = 0;
};

class MockClock {
public:
    NetworkMonitor::Clock::time_point operator()() const { return now_; }
    void advance(std::chrono::nanoseconds duration) { now_ += duration; }

private:
    NetworkMonitor::Clock::time_point now_;
};

inline NetworkInterfaceInfo makeInfo(const std::string &name, std::uint64_t rx_bytes, std::uint64_t rx_packets,
                                     std::uint64_t tx_bytes, std::uint64_t tx_packets)
{
    return {
        .name = name, .rx_bytes = rx_bytes, .rx_packets = rx_packets, .tx_bytes = tx_bytes, .tx_packets = tx_packets};
}

}  // namespace spark::test

#endif

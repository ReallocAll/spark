#include "core/stats/network_monitor.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace spark {

// ---- NetworkInterfaceInfo ----

bool NetworkInterfaceInfo::isZero() const
{
    return rx_bytes == 0 && rx_packets == 0 && rx_errors == 0 && tx_bytes == 0 && tx_packets == 0 && tx_errors == 0;
}

NetworkInterfaceInfo NetworkInterfaceInfo::subtract(const NetworkInterfaceInfo &other) const
{
    if (other.isZero()) {
        return *this;
    }
    NetworkInterfaceInfo diff;
    diff.name = name;
    diff.rx_bytes = rx_bytes - other.rx_bytes;
    diff.rx_packets = rx_packets - other.rx_packets;
    diff.rx_errors = rx_errors - other.rx_errors;
    diff.tx_bytes = tx_bytes - other.tx_bytes;
    diff.tx_packets = tx_packets - other.tx_packets;
    diff.tx_errors = tx_errors - other.tx_errors;
    return diff;
}

// ---- DoubleRollingAverage ----

DoubleRollingAverage::DoubleRollingAverage(std::size_t window_size) : capacity_(window_size)
{
    samples_.reserve(window_size);
}

void DoubleRollingAverage::add(double value)
{
    if (count_ < capacity_) {
        samples_.push_back(value);
        ++count_;
    }
    else {
        total_ -= samples_[head_];
        samples_[head_] = value;
        head_ = (head_ + 1) % capacity_;
    }
    total_ += value;
}

double DoubleRollingAverage::mean() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return total_ / static_cast<double>(count_);
}

double DoubleRollingAverage::max() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return *std::max_element(samples_.begin(), std::next(samples_.begin(), static_cast<std::ptrdiff_t>(count_)));
}

double DoubleRollingAverage::min() const
{
    if (count_ == 0) {
        return 0.0;
    }
    return *std::min_element(samples_.begin(), std::next(samples_.begin(), static_cast<std::ptrdiff_t>(count_)));
}

std::vector<double> DoubleRollingAverage::sortedCopy() const
{
    std::vector<double> s(samples_.begin(), std::next(samples_.begin(), static_cast<std::ptrdiff_t>(count_)));
    std::ranges::sort(s);
    return s;
}

double DoubleRollingAverage::percentile(double p) const
{
    if (count_ == 0) {
        return 0.0;
    }
    auto s = sortedCopy();
    int rank = static_cast<int>(std::ceil(p * static_cast<double>(s.size() - 1)));
    return s[static_cast<std::size_t>(rank)];
}

double DoubleRollingAverage::median() const
{
    return percentile(0.5);
}

double DoubleRollingAverage::percentile95() const
{
    return percentile(0.95);
}

// ---- NetworkInterfaceAverages ----

NetworkInterfaceAverages::NetworkInterfaceAverages(std::size_t window_size)
    : rx_bytes_per_second(window_size), tx_bytes_per_second(window_size), rx_packets_per_second(window_size),
      tx_packets_per_second(window_size)
{
}

void NetworkInterfaceAverages::accept(const NetworkInterfaceInfo &info, double poll_interval_seconds)
{
    if (poll_interval_seconds <= 0.0) {
        return;
    }
    double inv = 1.0 / poll_interval_seconds;
    rx_bytes_per_second.add(static_cast<double>(info.rx_bytes) * inv);
    tx_bytes_per_second.add(static_cast<double>(info.tx_bytes) * inv);
    rx_packets_per_second.add(static_cast<double>(info.rx_packets) * inv);
    tx_packets_per_second.add(static_cast<double>(info.tx_packets) * inv);
}

// ---- NetworkMonitor ----

NetworkMonitor::NetworkMonitor() : poll_fn_(pollNetworkInterfaces), now_fn_(Clock::now) {}

NetworkMonitor::NetworkMonitor(PollFn poll_fn, NowFn now_fn) : poll_fn_(std::move(poll_fn)), now_fn_(std::move(now_fn))
{
}

bool NetworkMonitor::shouldIgnore(const std::string &name)
{
    // Match upstream spark: ignore virtual eth adapters and container bridge networks.
    if (name.starts_with("veth")) {
        return true;
    }
    if (name.starts_with("br-")) {
        return true;
    }
    return false;
}

bool NetworkMonitor::poll()
{
    std::map<std::string, NetworkInterfaceInfo> current = poll_fn_();
    const Clock::time_point now = now_fn_();

    if (first_poll_) {
        previous_ = current;
        previous_poll_time_ = now;
        first_poll_ = false;
        return false;
    }

    const double elapsed_seconds = std::chrono::duration<double>(now - previous_poll_time_).count();
    previous_poll_time_ = now;
    if (elapsed_seconds <= 0.0 || !std::isfinite(elapsed_seconds)) {
        previous_ = current;
        return false;
    }

    bool accepted = false;
    for (auto &[name, averages] : averages_) {
        if (!current.contains(name)) {
            averages.accept(NetworkInterfaceInfo{}, elapsed_seconds);
            accepted = true;
        }
    }
    for (const auto &[name, info] : current) {
        if (shouldIgnore(name)) {
            continue;
        }
        auto prev_it = previous_.find(name);
        if (prev_it == previous_.end()) {
            continue;
        }
        const NetworkInterfaceInfo &previous = prev_it->second;
        if (info.rx_bytes < previous.rx_bytes || info.rx_packets < previous.rx_packets ||
            info.rx_errors < previous.rx_errors || info.tx_bytes < previous.tx_bytes ||
            info.tx_packets < previous.tx_packets || info.tx_errors < previous.tx_errors) {
            continue;
        }
        auto it = averages_.find(name);
        if (it == averages_.end()) {
            it = averages_.emplace(name, NetworkInterfaceAverages(kWindowSize)).first;
        }
        it->second.accept(info.subtract(previous), elapsed_seconds);
        accepted = true;
    }

    previous_ = current;
    return accepted;
}

std::map<std::string, NetworkInterfaceSnapshot> NetworkMonitor::snapshot() const
{
    std::map<std::string, NetworkInterfaceSnapshot> out;
    for (const auto &[name, avg] : averages_) {
        if (avg.rx_bytes_per_second.samples() == 0) {
            continue;
        }
        NetworkInterfaceSnapshot s;
        s.rx_bytes_per_second.present = true;
        s.rx_bytes_per_second.mean = avg.rx_bytes_per_second.mean();
        s.rx_bytes_per_second.max = avg.rx_bytes_per_second.max();
        s.rx_bytes_per_second.min = avg.rx_bytes_per_second.min();
        s.rx_bytes_per_second.median = avg.rx_bytes_per_second.median();
        s.rx_bytes_per_second.percentile95 = avg.rx_bytes_per_second.percentile95();

        s.tx_bytes_per_second.present = true;
        s.tx_bytes_per_second.mean = avg.tx_bytes_per_second.mean();
        s.tx_bytes_per_second.max = avg.tx_bytes_per_second.max();
        s.tx_bytes_per_second.min = avg.tx_bytes_per_second.min();
        s.tx_bytes_per_second.median = avg.tx_bytes_per_second.median();
        s.tx_bytes_per_second.percentile95 = avg.tx_bytes_per_second.percentile95();

        s.rx_packets_per_second.present = true;
        s.rx_packets_per_second.mean = avg.rx_packets_per_second.mean();
        s.rx_packets_per_second.max = avg.rx_packets_per_second.max();
        s.rx_packets_per_second.min = avg.rx_packets_per_second.min();
        s.rx_packets_per_second.median = avg.rx_packets_per_second.median();
        s.rx_packets_per_second.percentile95 = avg.rx_packets_per_second.percentile95();

        s.tx_packets_per_second.present = true;
        s.tx_packets_per_second.mean = avg.tx_packets_per_second.mean();
        s.tx_packets_per_second.max = avg.tx_packets_per_second.max();
        s.tx_packets_per_second.min = avg.tx_packets_per_second.min();
        s.tx_packets_per_second.median = avg.tx_packets_per_second.median();
        s.tx_packets_per_second.percentile95 = avg.tx_packets_per_second.percentile95();

        out[name] = s;
    }
    return out;
}

std::map<std::string, NetworkInterfaceInfo> NetworkMonitor::systemTotals() const
{
    return previous_;
}

}  // namespace spark

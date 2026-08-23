#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "application/health/health_report.h"

namespace {

class Sender final : public spark::CommandSender {
public:
    std::string getName() const override { return "Console"; }
    bool isPlayer() const override { return false; }

    bool contains(const std::string &text) const
    {
        for (const std::string &message : messages) {
            if (message.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> messages;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { messages.push_back(message); }
};

class Metadata final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ExportContext &, std::int64_t) override {}
    void gatherWorldMetadata(spark::ExportContext &) override {}
    std::int64_t serverUptimeSeconds() override { return 1; }
    std::int64_t playerCount() override { return 2; }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }
};

spark::NetworkRateValues rate(double mean)
{
    spark::NetworkRateValues value;
    value.present = true;
    value.mean = mean;
    return value;
}

}  // namespace

int main()
{
    spark::StatisticsService statistics;
    Metadata metadata;
    spark::NetworkInterfaceSnapshot active;
    active.rx_bytes_per_second = rate(2048.0);
    active.rx_packets_per_second = rate(12.0);
    active.tx_bytes_per_second = rate(0.0);
    active.tx_packets_per_second = rate(0.0);
    spark::NetworkInterfaceSnapshot idle;
    idle.rx_bytes_per_second = rate(0.0);
    idle.rx_packets_per_second = rate(0.0);
    idle.tx_bytes_per_second = rate(0.0);
    idle.tx_packets_per_second = rate(0.0);

    const std::map<std::string, spark::NetworkInterfaceSnapshot> snapshots{{"active", active}, {"idle", idle}};
    Sender summary;
    spark::showHealthReport(summary, statistics, metadata, snapshots, false, false);
    assert(summary.contains("2.00 KiB/s"));
    assert(summary.contains("12 pps"));
    assert(summary.contains("active RX"));
    assert(!summary.contains("active TX"));
    assert(!summary.contains("idle RX"));

    Sender detailed;
    spark::showHealthReport(detailed, statistics, metadata, snapshots, true, true);
    assert(detailed.contains("active TX"));
    assert(detailed.contains("idle RX"));
    assert(detailed.contains("idle TX"));
    return 0;
}

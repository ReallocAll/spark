#include <fstream>
#include <sstream>

#include "core/stats/network_monitor.h"

namespace spark {

namespace {

std::map<std::string, NetworkInterfaceInfo> readProcNetDev(const std::vector<std::string> &lines)
{
    if (lines.size() < 3) {
        return {};
    }

    const std::string &header = lines[1];
    const std::size_t bar1 = header.find('|');
    const std::size_t bar2 = header.find('|', bar1 == std::string::npos ? 0 : bar1 + 1);
    if (bar1 == std::string::npos || bar2 == std::string::npos) {
        return {};
    }

    const auto split_fields = [](const std::string &value) {
        std::vector<std::string> fields;
        std::istringstream stream(value);
        std::string field;
        while (stream >> field) {
            fields.push_back(field);
        }
        return fields;
    };

    const std::vector<std::string> rx_fields = split_fields(header.substr(bar1 + 1, bar2 - bar1 - 1));
    const std::vector<std::string> tx_fields = split_fields(header.substr(bar2 + 1));
    const std::size_t rx_count = rx_fields.size();
    const std::size_t expected = rx_count + tx_fields.size();

    const auto index_of = [](const std::vector<std::string> &fields, const std::string &name) {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i] == name) {
                return i;
            }
        }
        return std::string::npos;
    };

    const std::size_t rx_bytes = index_of(rx_fields, "bytes");
    const std::size_t rx_packets = index_of(rx_fields, "packets");
    const std::size_t rx_errors = index_of(rx_fields, "errs");
    const std::size_t tx_bytes = index_of(tx_fields, "bytes");
    const std::size_t tx_packets = index_of(tx_fields, "packets");
    const std::size_t tx_errors = index_of(tx_fields, "errs");
    if (rx_bytes == std::string::npos || rx_packets == std::string::npos || rx_errors == std::string::npos ||
        tx_bytes == std::string::npos || tx_packets == std::string::npos || tx_errors == std::string::npos) {
        return {};
    }

    const std::size_t tx_bytes_index = rx_count + tx_bytes;
    const std::size_t tx_packets_index = rx_count + tx_packets;
    const std::size_t tx_errors_index = rx_count + tx_errors;
    std::map<std::string, NetworkInterfaceInfo> result;
    for (std::size_t i = 2; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        const std::size_t start = line.find_first_not_of(" \t");
        const std::size_t end = line.find_last_not_of(" \t", colon - 1);
        if (start == std::string::npos || end == std::string::npos) {
            continue;
        }

        std::istringstream stream(line.substr(colon + 1));
        std::vector<std::uint64_t> values;
        std::uint64_t value = 0;
        while (stream >> value) {
            values.push_back(value);
        }
        if (values.size() != expected) {
            continue;
        }

        NetworkInterfaceInfo info;
        info.name = line.substr(start, end - start + 1);
        info.rx_bytes = values[rx_bytes];
        info.rx_packets = values[rx_packets];
        info.rx_errors = values[rx_errors];
        info.tx_bytes = values[tx_bytes_index];
        info.tx_packets = values[tx_packets_index];
        info.tx_errors = values[tx_errors_index];
        result[info.name] = info;
    }
    return result;
}

}  // namespace

std::map<std::string, NetworkInterfaceInfo> pollNetworkInterfaces()
{
    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) {
        return {};
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    return readProcNetDev(lines);
}

}  // namespace spark

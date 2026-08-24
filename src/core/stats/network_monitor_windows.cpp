#include "core/stats/network_monitor.h"

// clang-format off: iphlpapi.h requires windows.h types
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on

#include <utility>

namespace spark {

namespace {

bool isUsableWindowsInterface(const MIB_IF_ROW2 &row)
{
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp) {
        return false;
    }

    const auto &flags = row.InterfaceAndOperStatusFlags;
    return !flags.FilterInterface && !flags.NotMediaConnected && !flags.Paused && !flags.EndPointInterface;
}

std::string wideToUtf8(const wchar_t *value)
{
    if (value == nullptr || *value == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr) <= 0) {
        return {};
    }
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

std::string windowsInterfaceDisplayName(const MIB_IF_ROW2 &row)
{
    std::string name = wideToUtf8(row.Alias);
    if (!name.empty()) {
        return name;
    }

    char interface_name[IF_MAX_STRING_SIZE + 1]{};
    if (ConvertInterfaceLuidToNameA(&row.InterfaceLuid, interface_name, sizeof(interface_name)) == NO_ERROR) {
        return interface_name;
    }
    return {};
}

}  // namespace

std::map<std::string, NetworkInterfaceInfo> pollNetworkInterfaces()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return {};
    }

    std::map<std::string, NetworkInterfaceInfo> result;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2 &row = table->Table[i];
        if (!isUsableWindowsInterface(row)) {
            continue;
        }

        std::string interface_name = windowsInterfaceDisplayName(row);
        if (interface_name.empty()) {
            continue;
        }

        NetworkInterfaceInfo info;
        info.name = std::move(interface_name);
        info.rx_bytes = row.InOctets;
        info.tx_bytes = row.OutOctets;
        info.rx_packets = row.InUcastPkts + row.InNUcastPkts;
        info.tx_packets = row.OutUcastPkts + row.OutNUcastPkts;
        info.rx_errors = row.InErrors;
        info.tx_errors = row.OutErrors;

        if (result.contains(info.name)) {
            info.name += " (" + std::to_string(row.InterfaceIndex) + ")";
        }
        result[info.name] = std::move(info);
    }
    FreeMibTable(table);
    return result;
}

}  // namespace spark

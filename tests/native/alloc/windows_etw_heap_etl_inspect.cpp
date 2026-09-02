#ifndef _WIN32
#error "windows_etw_heap_etl_inspect.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#pragma comment(lib, "advapi32.lib")

namespace {

constexpr GUID kHeapGuid = {
    0x222962ab, 0x6180, 0x4b88, {0xa8, 0x25, 0x34, 0x6b, 0x75, 0xf2, 0xa2, 0x4a}};
constexpr GUID kStackWalkGuid = {
    0xdef2fe46, 0x7bd6, 0x4b80, {0xbd, 0x94, 0xf5, 0x7f, 0xe2, 0x0d, 0x0c, 0xe3}};

struct ProbeStats {
    DWORD target_pid = 0;
    std::uint64_t total_records = 0;
    std::uint64_t target_records = 0;
    std::uint64_t heap_records = 0;
    std::uint64_t target_heap_records = 0;
    std::uint64_t stackwalk_records = 0;
    std::uint64_t target_stackwalk_header_records = 0;
    std::uint64_t target_extended_stack32 = 0;
    std::uint64_t target_extended_stack64 = 0;
    std::array<std::uint64_t, 256> heap_ids{};
    std::array<std::uint64_t, 256> heap_opcodes{};
};

ProbeStats *g_stats = nullptr;

void WINAPI onEvent(PEVENT_RECORD event) noexcept
{
    ProbeStats *stats = g_stats;
    if (stats == nullptr || event == nullptr) {
        return;
    }

    ++stats->total_records;
    const bool target = event->EventHeader.ProcessId == stats->target_pid;
    if (target) {
        ++stats->target_records;
        for (USHORT index = 0; index < event->ExtendedDataCount; ++index) {
            const USHORT type = event->ExtendedData[index].ExtType;
            if (type == EVENT_HEADER_EXT_TYPE_STACK_TRACE32) {
                ++stats->target_extended_stack32;
            } else if (type == EVENT_HEADER_EXT_TYPE_STACK_TRACE64) {
                ++stats->target_extended_stack64;
            }
        }
    }

    if (::IsEqualGUID(event->EventHeader.ProviderId, kHeapGuid)) {
        ++stats->heap_records;
        if (target) {
            ++stats->target_heap_records;
            const USHORT id = event->EventHeader.EventDescriptor.Id;
            const UCHAR opcode = event->EventHeader.EventDescriptor.Opcode;
            if (id < stats->heap_ids.size()) {
                ++stats->heap_ids[id];
            }
            ++stats->heap_opcodes[opcode];
        }
    }

    if (::IsEqualGUID(event->EventHeader.ProviderId, kStackWalkGuid)) {
        ++stats->stackwalk_records;
        if (target) {
            ++stats->target_stackwalk_header_records;
        }
    }
}

void printHistogram(const char *label, const std::array<std::uint64_t, 256> &values)
{
    std::cerr << label << '=';
    bool first = true;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index] == 0) {
            continue;
        }
        if (!first) {
            std::cerr << ',';
        }
        std::cerr << index << ':' << values[index];
        first = false;
    }
    if (first) {
        std::cerr << "none";
    }
    std::cerr << '\n';
}

[[nodiscard]] bool parsePid(const wchar_t *text, DWORD &pid)
{
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    errno = 0;
    wchar_t *end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (errno != 0 || end == text || *end != L'\0' || value == 0 ||
        value > std::numeric_limits<DWORD>::max()) {
        return false;
    }
    pid = static_cast<DWORD>(value);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3) {
        std::wcerr << L"usage: windows_etw_heap_etl_inspect.exe <trace.etl> <pid>\n";
        return 2;
    }

    ProbeStats stats;
    if (!parsePid(argv[2], stats.target_pid)) {
        std::wcerr << L"invalid target pid: " << argv[2] << L'\n';
        return 3;
    }
    g_stats = &stats;

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LogFileName = argv[1];
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = &onEvent;

    TRACEHANDLE trace = ::OpenTraceW(&logfile);
    if (trace == INVALID_PROCESSTRACE_HANDLE) {
        std::cerr << "etl-inspect OpenTrace failed error=" << ::GetLastError() << '\n';
        g_stats = nullptr;
        return 4;
    }

    TRACEHANDLE handle = trace;
    const ULONG process_status = ::ProcessTrace(&handle, 1, nullptr, nullptr);
    const ULONG close_status = ::CloseTrace(trace);
    g_stats = nullptr;

    std::cerr << "stage=etl-inspect process_status=" << process_status
              << " close_status=" << close_status
              << " events_lost=" << logfile.EventsLost
              << " target_pid=" << stats.target_pid
              << " total_records=" << stats.total_records
              << " target_records=" << stats.target_records
              << " heap_records=" << stats.heap_records
              << " target_heap_records=" << stats.target_heap_records
              << " stackwalk_records=" << stats.stackwalk_records
              << " target_stackwalk_header_records=" << stats.target_stackwalk_header_records
              << " target_extended_stack32=" << stats.target_extended_stack32
              << " target_extended_stack64=" << stats.target_extended_stack64 << '\n';
    printHistogram("target_heap_ids", stats.heap_ids);
    printHistogram("target_heap_opcodes", stats.heap_opcodes);

    if (process_status != ERROR_SUCCESS) {
        std::cerr << "result=probe-error process_trace_status=" << process_status << '\n';
        return 5;
    }
    if (logfile.EventsLost != 0) {
        std::cerr << "result=incomplete events_lost=" << logfile.EventsLost << '\n';
        return 6;
    }
    if (stats.target_heap_records == 0) {
        std::cerr << "result=no-target-heap-events\n";
        return 7;
    }

    const std::uint64_t target_extended_stacks =
        stats.target_extended_stack32 + stats.target_extended_stack64;
    std::cerr << "result=heap-events-observed target_heap_events=" << stats.target_heap_records
              << " target_extended_stack_events=" << target_extended_stacks
              << " total_stackwalk_events=" << stats.stackwalk_records << '\n';
    return 0;
}

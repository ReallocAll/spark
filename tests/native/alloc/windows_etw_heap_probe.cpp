#ifndef _WIN32
#error "windows_etw_heap_probe.cpp is Windows-only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <evntcons.h>
#include <evntrace.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace {

// SystemMemoryProviderGuid documented by Microsoft for the post-SDK-20348
// System Provider mapping. Keep a local definition so this bounded probe does
// not depend on the build SDK exporting a named GUID object.
constexpr GUID kSystemMemoryProviderGuid = {
    0x82958ca9, 0xb6cd, 0x47f8, {0xa3, 0xa8, 0x03, 0xae, 0x85, 0xa4, 0xbc, 0x24}};
constexpr ULONGLONG kSystemMemoryHeapKeyword = 0x80ULL;
constexpr std::size_t kOperations = 20000;

struct ProbeStats {
    DWORD process_id = 0;
    std::atomic<std::uint64_t> provider_events{0};
    std::atomic<std::uint64_t> process_events{0};
    std::atomic<std::uint64_t> stack32_events{0};
    std::atomic<std::uint64_t> stack64_events{0};
    std::array<std::atomic<std::uint64_t>, 256> event_ids{};
    std::array<std::atomic<std::uint64_t>, 256> opcodes{};
};

ProbeStats *g_stats = nullptr;

void WINAPI onEvent(PEVENT_RECORD event) noexcept
{
    ProbeStats *stats = g_stats;
    if (stats == nullptr || event == nullptr ||
        !::IsEqualGUID(event->EventHeader.ProviderId, kSystemMemoryProviderGuid)) {
        return;
    }

    stats->provider_events.fetch_add(1, std::memory_order_relaxed);
    if (event->EventHeader.ProcessId != stats->process_id) {
        return;
    }

    stats->process_events.fetch_add(1, std::memory_order_relaxed);
    const USHORT id = event->EventHeader.EventDescriptor.Id;
    const UCHAR opcode = event->EventHeader.EventDescriptor.Opcode;
    if (id < stats->event_ids.size()) {
        stats->event_ids[id].fetch_add(1, std::memory_order_relaxed);
    }
    stats->opcodes[opcode].fetch_add(1, std::memory_order_relaxed);

    for (USHORT index = 0; index < event->ExtendedDataCount; ++index) {
        const USHORT type = event->ExtendedData[index].ExtType;
        if (type == EVENT_HEADER_EXT_TYPE_STACK_TRACE32) {
            stats->stack32_events.fetch_add(1, std::memory_order_relaxed);
        }
        else if (type == EVENT_HEADER_EXT_TYPE_STACK_TRACE64) {
            stats->stack64_events.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

[[nodiscard]] std::vector<std::byte> makeProperties(const std::wstring &session_name)
{
    const std::size_t name_bytes = (session_name.size() + 1) * sizeof(wchar_t);
    std::vector<std::byte> storage(sizeof(EVENT_TRACE_PROPERTIES) + name_bytes);
    std::memset(storage.data(), 0, storage.size());

    auto *properties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(storage.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = 1;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 32;
    properties->FlushTimer = 1;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    auto *name = reinterpret_cast<wchar_t *>(storage.data() + properties->LoggerNameOffset);
    std::memcpy(name, session_name.c_str(), name_bytes);
    return storage;
}

void runHeapWorkload()
{
    HANDLE heap = ::GetProcessHeap();
    if (heap == nullptr) {
        std::cerr << "etw-heap-probe GetProcessHeap failed error=" << ::GetLastError() << '\n';
        std::abort();
    }

    for (std::size_t index = 0; index < kOperations; ++index) {
        const SIZE_T initial = 32 + (index & 0xffU);
        void *block = ::HeapAlloc(heap, 0, initial);
        if (block == nullptr) {
            std::abort();
        }
        if ((index & 1U) == 0) {
            void *grown = ::HeapReAlloc(heap, 0, block, initial + 64);
            if (grown == nullptr) {
                ::HeapFree(heap, 0, block);
                std::abort();
            }
            block = grown;
        }
        if (::HeapFree(heap, 0, block) == FALSE) {
            std::abort();
        }

        // Exercise the CRT path as well. On current Windows CRTs this normally
        // reaches the process heap, but the probe records observed ETW facts
        // rather than assuming that implementation detail.
        void *crt = std::malloc(48 + (index & 0x7fU));
        if (crt == nullptr) {
            std::abort();
        }
        void *resized = std::realloc(crt, 96 + (index & 0x7fU));
        if (resized == nullptr) {
            std::free(crt);
            std::abort();
        }
        std::free(resized);
    }
}

void printHistogram(const char *label, const std::array<std::atomic<std::uint64_t>, 256> &values)
{
    std::cerr << label << '=';
    bool first = true;
    for (std::size_t index = 0; index < values.size(); ++index) {
        const std::uint64_t count = values[index].load(std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        if (!first) {
            std::cerr << ',';
        }
        std::cerr << index << ':' << count;
        first = false;
    }
    if (first) {
        std::cerr << "none";
    }
    std::cerr << '\n';
}

}  // namespace

int main()
{
    ProbeStats stats;
    stats.process_id = ::GetCurrentProcessId();
    g_stats = &stats;

    const std::wstring session_name =
        L"SparkEtwHeapProbe-" + std::to_wstring(stats.process_id) + L"-" + std::to_wstring(::GetTickCount64());
    std::vector<std::byte> properties_storage = makeProperties(session_name);
    auto *properties = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(properties_storage.data());

    TRACEHANDLE session = 0;
    const ULONG start_status = ::StartTraceW(&session, session_name.c_str(), properties);
    std::cerr << "stage=etw-heap-probe start status=" << start_status << " pid=" << stats.process_id << '\n';
    if (start_status != ERROR_SUCCESS) {
        // System logger sessions may be unavailable due to permissions or host
        // policy. That is bounded feasibility evidence, not a reason to mutate
        // security policy or bypass the restriction.
        std::cerr << "result=unsupported start_status=" << start_status << '\n';
        return 0;
    }

    bool session_started = true;
    auto stopSession = [&] {
        if (!session_started) {
            return;
        }
        const ULONG stop_status = ::ControlTraceW(session, session_name.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
        std::cerr << "stage=etw-heap-probe stop status=" << stop_status << " events_lost=" << properties->EventsLost
                  << " buffers_written=" << properties->BuffersWritten
                  << " log_buffers_lost=" << properties->LogBuffersLost
                  << " realtime_buffers_lost=" << properties->RealTimeBuffersLost << '\n';
        session_started = false;
    };

    const ULONG enable_status =
        ::EnableTraceEx2(session, &kSystemMemoryProviderGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE,
                         kSystemMemoryHeapKeyword, 0, 1000, nullptr);
    std::cerr << "stage=etw-heap-probe enable-system-memory-heap status=" << enable_status << '\n';
    if (enable_status != ERROR_SUCCESS) {
        stopSession();
        std::cerr << "result=unsupported enable_status=" << enable_status << '\n';
        return 0;
    }

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<LPWSTR>(session_name.c_str());
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = &onEvent;
    TRACEHANDLE consumer = ::OpenTraceW(&logfile);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD failure = ::GetLastError();
        (void)::EnableTraceEx2(session, &kSystemMemoryProviderGuid, EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                               TRACE_LEVEL_NONE, 0, 0, 1000, nullptr);
        stopSession();
        std::cerr << "result=probe-error open_trace_status=" << failure << '\n';
        return 2;
    }

    std::atomic<ULONG> process_status{ERROR_SUCCESS};
    std::thread consumer_thread([&] {
        TRACEHANDLE handle = consumer;
        process_status.store(::ProcessTrace(&handle, 1, nullptr, nullptr), std::memory_order_release);
    });

    ::Sleep(100);
    std::cerr << "stage=etw-heap-probe workload operations=" << kOperations << '\n';
    runHeapWorkload();
    ::Sleep(500);

    const ULONG disable_status =
        ::EnableTraceEx2(session, &kSystemMemoryProviderGuid, EVENT_CONTROL_CODE_DISABLE_PROVIDER, TRACE_LEVEL_NONE, 0,
                         0, 1000, nullptr);
    std::cerr << "stage=etw-heap-probe disable status=" << disable_status << '\n';
    stopSession();
    consumer_thread.join();
    (void)::CloseTrace(consumer);
    g_stats = nullptr;

    const ULONG consumed_status = process_status.load(std::memory_order_acquire);
    const std::uint64_t provider_events = stats.provider_events.load(std::memory_order_relaxed);
    const std::uint64_t process_events = stats.process_events.load(std::memory_order_relaxed);
    const std::uint64_t stack32_events = stats.stack32_events.load(std::memory_order_relaxed);
    const std::uint64_t stack64_events = stats.stack64_events.load(std::memory_order_relaxed);

    std::cerr << "stage=etw-heap-probe consume status=" << consumed_status << " provider_events=" << provider_events
              << " process_events=" << process_events << " stack32_events=" << stack32_events
              << " stack64_events=" << stack64_events << " logfile_events_lost=" << logfile.EventsLost << '\n';
    printHistogram("event_ids", stats.event_ids);
    printHistogram("opcodes", stats.opcodes);

    if (consumed_status != ERROR_SUCCESS && consumed_status != ERROR_CANCELLED) {
        std::cerr << "result=probe-error process_trace_status=" << consumed_status << '\n';
        return 3;
    }
    if (provider_events == 0) {
        std::cerr << "result=no-provider-events raw_system_heap_keyword_insufficient_or_process_not_heap-enabled\n";
        return 0;
    }
    if (process_events == 0) {
        std::cerr << "result=no-process-events provider_events=" << provider_events
                  << " raw_system_heap_keyword_not_observing_probe_pid\n";
        return 0;
    }

    std::cerr << "result=events-observed process_events=" << process_events
              << " stack_events=" << (stack32_events + stack64_events) << '\n';
    return 0;
}

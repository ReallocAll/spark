#ifndef ENDSTONE_SPARK_CI_DIAGNOSTICS_H
#define ENDSTONE_SPARK_CI_DIAGNOSTICS_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace spark {

inline constexpr std::uint64_t kCiDiagnosticsMagic = 0x454e4453544f4e45ULL;
inline constexpr std::uint64_t kCiDiagnosticsVersion = 2;
inline constexpr std::uint64_t kCiDiagnosticsReady = 1;
inline constexpr std::string_view kCiDiagnosticsEnvironment = "ENDSTONE_SPARK_CI_DIAGNOSTICS";

using CiDiagnosticsAtomic = std::atomic<std::uint64_t>;
static_assert(CiDiagnosticsAtomic::is_always_lock_free);
static_assert(sizeof(CiDiagnosticsAtomic) == sizeof(std::uint64_t));
static_assert(alignof(CiDiagnosticsAtomic) >= alignof(std::uint64_t));

enum class CiDiagnosticContext : std::uint64_t {
    ApplicationTick = 0,
    PluginTick = 1,
    ApplicationCommand = 2,
    PluginCommand = 3,
    Timeout = 4,
    Profiler = 5,
    Completion = 6,
    Notification = 7,
    SamplerLifecycleMain = 8,
    SamplerLifecycleLiveExport = 9,
    TimeoutWorker = 10,
    Export = 11,
    SamplerWorker = 12,
    Capture = 13,
    AggregatorWorker = 14,
    Count = 15,
};

enum class CiDiagnosticPhase : std::uint64_t {
    Unknown = 0,

    ApplicationTickEnter = 4,
    ApplicationTickExit = 5,
    ApplicationTickExceptionalExit = 6,
    PluginTickEnter = 7,
    PluginTickExit = 8,
    PluginTickExceptionalExit = 9,
    ApplicationCommandEnter = 13,
    ApplicationCommandExit = 14,
    ApplicationCommandExceptionalExit = 15,
    PluginCommandEnter = 16,
    PluginCommandExit = 17,
    PluginCommandExceptionalExit = 18,

    TimeoutArm = 32,
    TimeoutCancel = 33,
    TimeoutCompletion = 34,
    TimeoutFired = 35,
    ProfilerStart = 48,
    ProfilerStartFailed = 49,
    ProfilerStopSamplingEnter = 50,
    ProfilerStopSamplingExit = 51,
    ProfilerStopSamplingExceptionalExit = 52,
    ProfilerShutdownEnter = 53,
    ProfilerShutdownExit = 54,
    ProfilerShutdownExceptionalExit = 55,

    CompletionEnter = 68,
    CompletionExit = 69,
    CompletionExceptionalExit = 70,

    NotificationEnter = 80,
    NotificationExit = 81,
    NotificationExceptionalExit = 82,

    SamplerStart = 96,
    SamplerStartFailed = 97,
    SamplerDbgHelpAcquired = 98,
    SamplerStopRequested = 99,
    SamplerCancel = 100,
    SamplerJoin = 101,
    SamplerJoinComplete = 102,
    SamplerAggregatorJoin = 103,
    SamplerAggregatorJoinComplete = 104,
    SamplerDisarm = 105,
    SamplerDisarmComplete = 106,
    SamplerPause = 107,
    SamplerPauseJoin = 108,
    SamplerPauseJoinComplete = 109,
    SamplerResume = 110,
    SamplerWorkerStart = 111,
    SamplerWorkerRunning = 112,
    SamplerWorkerExit = 113,
    AggregatorWorkerStart = 114,
    AggregatorWorkerExit = 115,

    CaptureEnter = 128,
    CaptureOpenFailed = 129,
    CaptureSuspendAttempt = 130,
    CaptureSuspended = 131,
    CaptureSuspendFailed = 132,
    CaptureContextCall = 133,
    CaptureContextReturn = 134,
    CaptureWalkCall = 135,
    CaptureWalkReturn = 136,
    CaptureResumeAttempt = 137,
    CaptureResumed = 138,
    CaptureResumeFailed = 139,
    CaptureTargetExited = 140,
    CaptureComplete = 141,
    CaptureFailed = 142,

    ExportEnter = 161,
    ExportComplete = 162,
    ExportFailed = 163,
    ExportCompletionQueued = 164,
    ExportCompletionFallback = 165,
};

enum class CiDiagnosticCounter : std::uint64_t {
    None = 0,
    SuspendSuccess = 1,
    ResumeSuccess = 2,
    WalkCalls = 3,
};

struct alignas(8) CiDiagnosticRecord {
    CiDiagnosticsAtomic sequence{0};
    CiDiagnosticsAtomic generation{0};
    CiDiagnosticsAtomic phase_and_transition{0};
    CiDiagnosticsAtomic worker_tid{0};
    CiDiagnosticsAtomic target_tid{0};
    CiDiagnosticsAtomic suspend_success_count{0};
    CiDiagnosticsAtomic resume_success_count{0};
    CiDiagnosticsAtomic walk_call_count{0};
};

inline constexpr std::size_t kCiDiagnosticsRecordCount = static_cast<std::size_t>(CiDiagnosticContext::Count);

struct alignas(8) CiDiagnosticsRegion {
    CiDiagnosticsAtomic magic{0};
    CiDiagnosticsAtomic version{0};
    CiDiagnosticsAtomic size{0};
    // Identifies the shared mapping lifetime.
    CiDiagnosticsAtomic generation{0};
    CiDiagnosticsAtomic ready{0};
    CiDiagnosticRecord records[kCiDiagnosticsRecordCount];
};

static_assert(sizeof(CiDiagnosticRecord) == 8 * sizeof(std::uint64_t));
static_assert(sizeof(CiDiagnosticsRegion) == (5 + kCiDiagnosticsRecordCount * 8) * sizeof(std::uint64_t));

struct CiDiagnosticSnapshot {
    bool consistent = false;
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    CiDiagnosticPhase phase = CiDiagnosticPhase::Unknown;
    std::uint64_t transition = 0;
    std::uint64_t worker_tid = 0;
    std::uint64_t target_tid = 0;
    std::uint64_t suspend_success_count = 0;
    std::uint64_t resume_success_count = 0;
    std::uint64_t walk_call_count = 0;
};

// A false result means every field other than consistent is unknown.
CiDiagnosticSnapshot readCiDiagnosticSnapshot(const CiDiagnosticRecord &record) noexcept;

class CiDiagnostics final {
public:
    class Scope final {
    public:
        Scope(CiDiagnostics *diagnostics, CiDiagnosticContext context, CiDiagnosticPhase entry,
              CiDiagnosticPhase normal_exit, CiDiagnosticPhase exceptional_exit, std::uint64_t worker_tid = 0,
              std::uint64_t target_tid = 0) noexcept;
        ~Scope() noexcept;

        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        CiDiagnostics *diagnostics_;
        CiDiagnosticContext context_;
        CiDiagnosticPhase normal_exit_;
        CiDiagnosticPhase exceptional_exit_;
        std::uint64_t worker_tid_;
        std::uint64_t target_tid_;
        int uncaught_on_entry_;
    };

    class LiveExportScope final {
    public:
        LiveExportScope() noexcept;
        ~LiveExportScope() noexcept;

        LiveExportScope(const LiveExportScope &) = delete;
        LiveExportScope &operator=(const LiveExportScope &) = delete;

    private:
        bool previous_;
    };

    CiDiagnostics() noexcept;
    ~CiDiagnostics();

    CiDiagnostics(const CiDiagnostics &) = delete;
    CiDiagnostics &operator=(const CiDiagnostics &) = delete;

    // Opens the mapping only when ENDSTONE_SPARK_CI_DIAGNOSTICS=1.
    bool open() noexcept;
    void close() noexcept;

    // Uses an in-process region for focused tests; it never creates a mapping.
    bool openForTesting(bool enabled) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] std::string_view mappingName() const noexcept { return mapping_name_; }
    [[nodiscard]] CiDiagnosticsRegion *regionForTesting() noexcept { return region_; }
    [[nodiscard]] const CiDiagnosticsRegion *regionForTesting() const noexcept { return region_; }

    static bool environmentEnabled(const char *value) noexcept;
    static std::string mappingNameForPid(std::uint64_t pid);

    std::uint64_t beginGeneration() noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

    // Each context is written by one owning thread. The transaction is
    // lock-free and safe on the suspended-target path.
    void publish(CiDiagnosticContext context, CiDiagnosticPhase phase, std::uint64_t worker_tid = 0,
                 std::uint64_t target_tid = 0, CiDiagnosticCounter counter = CiDiagnosticCounter::None,
                 std::uint64_t counter_delta = 0) noexcept;

private:
    static void initializeRegion(CiDiagnosticsRegion &region, std::uint64_t mapping_lifetime_generation) noexcept;
    bool openMapping() noexcept;
    CiDiagnosticRecord *record(CiDiagnosticContext context) noexcept;

    CiDiagnosticsRegion local_region_{};
    CiDiagnosticsRegion *region_ = &local_region_;
    // Profiler sessions are independent from mapping lifetimes.
    CiDiagnosticsAtomic profiler_generation_{0};
    bool enabled_ = false;
    bool registered_owner_ = false;
    bool mapping_owner_ = false;
    std::string mapping_name_;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    void *mapping_handle_ = nullptr;
    void *mapping_view_ = nullptr;
#endif
};

CiDiagnostics *globalCiDiagnostics() noexcept;
bool registerGlobalCiDiagnostics(CiDiagnostics *diagnostics) noexcept;
void unregisterGlobalCiDiagnostics(CiDiagnostics *diagnostics) noexcept;
std::uint64_t ciDiagnosticCurrentThreadId() noexcept;
CiDiagnosticContext samplerLifecycleDiagnosticContext() noexcept;

}  // namespace spark

#endif  // ENDSTONE_SPARK_CI_DIAGNOSTICS_H

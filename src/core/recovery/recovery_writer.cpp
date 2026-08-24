#include "core/recovery/recovery_writer.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace spark {

namespace {

constexpr std::size_t KMaxQueueReservationAttempts = 64;

std::uint64_t monotonicNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool syncFileImpl(std::FILE *f)
{
#ifdef _WIN32
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)))) != 0;
#else
    return fdatasync(fileno(f)) == 0;
#endif
}

}  // namespace

RecoveryWriter::RecoveryWriter(Config config) : config_(std::move(config)), queue_(config_.queue_capacity) {}

RecoveryWriter::~RecoveryWriter()
{
    if (!stop()) {
        std::terminate();
    }
}

bool RecoveryWriter::start()
{
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (stop_requested_.load(std::memory_order_acquire) || thread_.joinable()) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
    worker_exited_.store(false, std::memory_order_release);

    std::error_code ec;
    std::filesystem::create_directories(config_.directory, ec);
    if (ec) {
        enabled_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        worker_exited_.store(true, std::memory_order_release);
        return false;
    }

    for (const auto &entry : std::filesystem::directory_iterator(config_.directory, ec)) {
        if (ec) {
            enabled_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            worker_exited_.store(true, std::memory_order_release);
            return false;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        const bool is_segment = name.size() > 12 && name.starts_with("segment-") && name.ends_with(".jnl");
        const bool is_snapshot = name == "metadata.snapshot" || name == "metadata.snapshot.tmp";
        if (!is_segment && !is_snapshot) {
            continue;
        }
        if (is_segment) {
            const std::string_view number(name.data() + 8, name.size() - 12);
            std::uint32_t parsed = 0;
            const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), parsed);
            if (error != std::errc{} || end != number.data() + number.size()) {
                continue;
            }
        }
        std::filesystem::remove(entry.path(), ec);
        if (ec) {
            enabled_.store(false, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            worker_exited_.store(true, std::memory_order_release);
            return false;
        }
    }

    if (!openSegment(0)) {
        enabled_.store(false, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        worker_exited_.store(true, std::memory_order_release);
        return false;
    }

    enabled_.store(true, std::memory_order_release);
    last_sync_ = std::chrono::steady_clock::now();
    try {
        thread_ = std::thread([this] {
            try {
                writerLoop();
            }
            catch (...) {
                enabled_.store(false, std::memory_order_release);
                if (file_) {
                    closeSegment();
                }
            }
            running_.store(false, std::memory_order_release);
            accepting_.store(false, std::memory_order_release);
            enabled_.store(false, std::memory_order_release);
            markWorkerExited();
        });
    }
    catch (...) {
        running_.store(false, std::memory_order_release);
        enabled_.store(false, std::memory_order_release);
        accepting_.store(false, std::memory_order_release);
        closeSegment();
        worker_exited_.store(true, std::memory_order_release);
        return false;
    }
    accepting_.store(true, std::memory_order_release);
    return true;
}

void RecoveryWriter::requestStop() noexcept
{
    accepting_.store(false, std::memory_order_release);
    stop_requested_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
}

bool RecoveryWriter::stop()
{
    const int timeout_ms = std::max(config_.shutdown_timeout_ms, 0);
    return stop(std::chrono::milliseconds(timeout_ms));
}

bool RecoveryWriter::stop(std::chrono::milliseconds timeout)
{
    requestStop();
    if (!thread_.joinable()) {
        enabled_.store(false, std::memory_order_release);
        return true;
    }

    if (!worker_exited_.load(std::memory_order_acquire)) {
        std::unique_lock lock(exit_mutex_);
        if (!exit_cv_.wait_for(lock, timeout,
                               [this] { return worker_exited_.load(std::memory_order_acquire); })) {
            return false;
        }
    }
    return tryReap();
}

bool RecoveryWriter::tryReap()
{
    std::scoped_lock lock(reap_mutex_);
    if (!thread_.joinable()) {
        enabled_.store(false, std::memory_order_release);
        return true;
    }
    if (!worker_exited_.load(std::memory_order_acquire)) {
        return false;
    }
    thread_.join();
    enabled_.store(false, std::memory_order_release);
    return true;
}

void RecoveryWriter::producerDone() noexcept
{
    active_producers_.fetch_sub(1, std::memory_order_acq_rel);
    cv_.notify_all();
}

void RecoveryWriter::enqueue(RecordType type, const JournalBuffer &payload)
{
    if (!enabled_.load(std::memory_order_acquire) || !accepting_.load(std::memory_order_acquire)) {
        return;
    }

    active_producers_.fetch_add(1, std::memory_order_acq_rel);
    if (!accepting_.load(std::memory_order_acquire)) {
        producerDone();
        return;
    }

    std::size_t current = queue_size_.load(std::memory_order_relaxed);
    bool reserved = false;
    for (std::size_t attempt = 0; attempt < KMaxQueueReservationAttempts; ++attempt) {
        if (current >= config_.queue_capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            producerDone();
            return;
        }
        if (queue_size_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
            reserved = true;
            break;
        }
    }
    if (!reserved) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        producerDone();
        return;
    }

    try {
        const std::uint32_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
        auto record = serializeRecord(type, seq, payload);
        if (!queue_.enqueue(std::move(record))) {
            queue_size_.fetch_sub(1, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    catch (...) {
        queue_size_.fetch_sub(1, std::memory_order_relaxed);
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    producerDone();
}

void RecoveryWriter::journalModuleDef(std::uint32_t module_id, std::string_view path)
{
    cacheModuleDef(module_id, path);
    enqueue(RecordType::ModuleDef, buildModuleDefPayload(module_id, path));
}

void RecoveryWriter::journalThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id, std::string_view name)
{
    cacheThreadDef(thread_id, os_thread_id, name);
    enqueue(RecordType::ThreadDef, buildThreadDefPayload(thread_id, os_thread_id, name));
}

void RecoveryWriter::journalSample(const Sample &sample)
{
    enqueue(RecordType::Sample, buildSamplePayload(sample));
}

void RecoveryWriter::journalTickEvent(std::uint64_t tick_id, double mspt)
{
    enqueue(RecordType::TickEvent, buildTickEventPayload(tick_id, mspt));
}

void RecoveryWriter::journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns)
{
    enqueue(RecordType::StallBegin, buildStallBeginPayload(detected_ns, last_tick_ns));
}

void RecoveryWriter::journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns)
{
    enqueue(RecordType::StallEnd, buildStallEndPayload(detected_ns, recovered_ns));
}

void RecoveryWriter::journalCleanEnd()
{
    enqueue(RecordType::CleanEnd, buildCleanEndPayload(monotonicNowNs()));
    requestFlush();
}

void RecoveryWriter::journalSessionConfig(std::uint32_t interval_us, std::int32_t only_ticks_over_ms, bool all_threads,
                                          bool regex_threads, bool ignore_sleeping, std::uint8_t thread_grouper,
                                          std::uint8_t profile_type, bool live_only, std::string_view creator_name,
                                          bool creator_is_player, std::string_view comment,
                                          const std::vector<std::string> &thread_patterns,
                                          std::int32_t window_adjustment_ms)
{
    JournalBuffer payload = buildSessionConfigPayload(
        interval_us, only_ticks_over_ms, all_threads, regex_threads, ignore_sleeping, thread_grouper, profile_type,
        live_only, creator_name, creator_is_player, comment, thread_patterns, window_adjustment_ms);
    {
        std::scoped_lock lock(metadata_mutex_);
        cached_session_config_.assign(payload.data(), payload.data() + payload.size());
    }
    enqueue(RecordType::SessionConfig, payload);
}

void RecoveryWriter::cacheModuleDef(std::uint32_t module_id, std::string_view path)
{
    std::scoped_lock lock(metadata_mutex_);
    cached_modules_[module_id].assign(path.data(), path.size());
}

void RecoveryWriter::cacheThreadDef(std::uint64_t thread_id, std::uint64_t os_thread_id, std::string_view name)
{
    std::scoped_lock lock(metadata_mutex_);
    SnapshotThreadDef def;
    def.thread_id = thread_id;
    def.os_thread_id = os_thread_id;
    def.name.assign(name.data(), name.size());
    cached_threads_[thread_id] = std::move(def);
}

void RecoveryWriter::requestFlush()
{
    flush_requested_.store(true, std::memory_order_release);
    cv_.notify_one();
}

bool RecoveryWriter::allowIo(IoOperation operation) noexcept
{
    if (!config_.io_hook) {
        return true;
    }
    try {
        return config_.io_hook(operation);
    }
    catch (...) {
        return false;
    }
}

bool RecoveryWriter::writeFile(std::FILE *file, const void *data, std::size_t size)
{
    if (!allowIo(IoOperation::Write)) {
        return false;
    }
    return std::fwrite(data, 1, size, file) == size;
}

bool RecoveryWriter::syncFile(std::FILE *file)
{
    return allowIo(IoOperation::Sync) && syncFileImpl(file);
}

bool RecoveryWriter::closeFile(std::FILE *file)
{
    const bool allowed = allowIo(IoOperation::Close);
    const bool closed = std::fclose(file) == 0;
    return allowed && closed;
}

bool RecoveryWriter::renameFile(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &ec)
{
    if (!allowIo(IoOperation::Rename)) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    std::filesystem::rename(from, to, ec);
    return !ec;
}

bool RecoveryWriter::openSegment(std::uint32_t segment_number)
{
    segment_path_ = config_.directory / ("segment-" + std::to_string(segment_number) + ".jnl");
    file_ = std::fopen(segment_path_.string().c_str(), "wb");
    if (!file_) {
        return false;
    }

    auto header = serializeFileHeader(config_.session_id, monotonicNowNs(), segment_number);
    if (!writeFile(file_, header.data(), header.size())) {
        closeFile(file_);
        file_ = nullptr;
        return false;
    }

    segment_number_ = segment_number;
    segment_bytes_ = header.size();
    total_bytes_ += header.size();
    return true;
}

bool RecoveryWriter::closeSegment()
{
    if (!file_) {
        return true;
    }
    std::FILE *file = file_;
    file_ = nullptr;
    return closeFile(file);
}

bool RecoveryWriter::syncFile()
{
    if (!file_) {
        return false;
    }
    if (!syncFile(file_)) {
        enabled_.store(false, std::memory_order_release);
        return false;
    }
    last_sync_ = std::chrono::steady_clock::now();
    return true;
}

void RecoveryWriter::rotateIfNeeded()
{
    if (total_bytes_ <= config_.max_total_bytes) {
        return;
    }

    if (!writeMetadataSnapshot()) {
        return;
    }

    while (first_retained_segment_ < segment_number_ && total_bytes_ > config_.max_total_bytes) {
        auto path = config_.directory / ("segment-" + std::to_string(first_retained_segment_) + ".jnl");
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        if (!ec && std::filesystem::remove(path, ec)) {
            total_bytes_ -= (size > 0 ? static_cast<std::size_t>(size) : 0);
            ++first_retained_segment_;
        }
        else {
            break;
        }
    }
}

bool RecoveryWriter::writeMetadataSnapshot()
{
    std::vector<std::uint8_t> session_config;
    std::vector<SnapshotModuleDef> modules;
    std::vector<SnapshotThreadDef> threads;
    {
        std::scoped_lock lock(metadata_mutex_);
        session_config = cached_session_config_;
        modules.reserve(cached_modules_.size());
        for (const auto &[id, path] : cached_modules_) {
            modules.push_back({.module_id = id, .path = path});
        }
        threads.reserve(cached_threads_.size());
        for (const auto &[tid, def] : cached_threads_) {
            threads.push_back(def);
        }
    }

    auto buf = serializeMetadataSnapshot(config_.session_id, monotonicNowNs(), session_config, modules, threads);

    const auto tmp_path = config_.directory / "metadata.snapshot.tmp";
    const auto final_path = config_.directory / "metadata.snapshot";

    std::FILE *f = std::fopen(tmp_path.string().c_str(), "wb");
    if (!f) {
        return false;
    }
    if (!writeFile(f, buf.data(), buf.size())) {
        closeFile(f);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    if (!syncFile(f)) {
        closeFile(f);
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    if (!closeFile(f)) {
        std::error_code ec;
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::error_code ec;
    if (!renameFile(tmp_path, final_path, ec)) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

void RecoveryWriter::markWorkerExited() noexcept
{
    worker_exited_.store(true, std::memory_order_release);
    exit_cv_.notify_all();
}

void RecoveryWriter::writerLoop()
{
    const auto flush_interval = std::chrono::milliseconds(config_.flush_interval_ms);
    const auto sync_interval = std::chrono::milliseconds(config_.sync_interval_ms);
    std::vector<std::uint8_t> record;

    const auto drain_record = [this, &record]() -> bool {
        if (!queue_.try_dequeue(record)) {
            return false;
        }
        queue_size_.fetch_sub(1, std::memory_order_relaxed);
        if (!file_) {
            return true;
        }
        if (!writeFile(file_, record.data(), record.size())) {
            enabled_.store(false, std::memory_order_release);
            return true;
        }
        segment_bytes_ += record.size();
        total_bytes_ += record.size();
        written_.fetch_add(1, std::memory_order_relaxed);

        if (segment_bytes_ >= config_.max_segment_bytes) {
            if (!syncFile()) {
                return true;
            }
            if (!closeSegment()) {
                enabled_.store(false, std::memory_order_release);
                return true;
            }
            if (!openSegment(segment_number_ + 1)) {
                enabled_.store(false, std::memory_order_release);
                return true;
            }
            rotateIfNeeded();
        }
        return true;
    };

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, flush_interval, [this] {
                return !running_.load(std::memory_order_acquire) || flush_requested_.load(std::memory_order_acquire) ||
                       queue_size_.load(std::memory_order_relaxed) != 0;
            });
        }
        flush_requested_.store(false, std::memory_order_release);

        if (!enabled_.load(std::memory_order_acquire)) {
            break;
        }

        bool wrote_any = false;
        while (queue_size_.load(std::memory_order_relaxed) != 0 && drain_record()) {
            wrote_any = true;
            if (!enabled_.load(std::memory_order_acquire)) {
                break;
            }
        }

        if (wrote_any && enabled_.load(std::memory_order_acquire)) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_sync_ >= sync_interval) {
                syncFile();
            }
        }
    }

    while (enabled_.load(std::memory_order_acquire)) {
        bool drained_any = false;
        while (queue_size_.load(std::memory_order_relaxed) != 0 && drain_record()) {
            drained_any = true;
            if (!enabled_.load(std::memory_order_acquire)) {
                break;
            }
        }
        if (!enabled_.load(std::memory_order_acquire)) {
            break;
        }
        if (active_producers_.load(std::memory_order_acquire) == 0 &&
            queue_size_.load(std::memory_order_acquire) == 0) {
            break;
        }
        if (!drained_any) {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(1));
        }
    }

    if (file_) {
        if (enabled_.load(std::memory_order_acquire)) {
            syncFile();
        }
        if (!closeSegment()) {
            enabled_.store(false, std::memory_order_release);
        }
    }
}

}  // namespace spark

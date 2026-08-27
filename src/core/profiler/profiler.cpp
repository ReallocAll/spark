#include "core/profiler/profiler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <regex>
#include <stdexcept>

#include "core/util/monotonic_time.h"
#include "profiling_window.h"
#include "spark_constants.h"

namespace spark {
namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

}  // namespace

std::uint64_t Profiler::sampleCount() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.sampleCount() : sampler_.sampleCount();
}

std::uint64_t Profiler::sampledAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.sampledBytes() : 0;
}

std::uint64_t Profiler::observedAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.observedBytes() : 0;
}

std::uint64_t Profiler::droppedSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.droppedSamples() : sampler_.droppedSamples();
}

std::uint64_t Profiler::filteredAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.filteredSamples() : 0;
}

std::uint64_t Profiler::allocationThreadNameFailures() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.threadNameFailures() : 0;
}

std::uint64_t Profiler::freedAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.freedSamples() : 0;
}

std::uint64_t Profiler::liveAllocationSamples() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.liveSamples() : 0;
}

std::uint64_t Profiler::liveAllocationBytes() const
{
    return mode_ == ProfileMode::Allocation ? allocation_sampler_.liveBytes() : 0;
}

bool Profiler::backendFailure(std::string &error) const
{
    if (mode_ == ProfileMode::Execution) {
        return sampler_.failure(error);
    }
    return allocation_sampler_.failure(error);
}

const std::vector<AllocationHookCapability> &Profiler::allocationHookCapabilities() const
{
    return allocation_sampler_.hookCapabilities();
}

bool Profiler::setCurrentThreadAllocationTrackingSuppressed(bool suppressed) noexcept
{
    return allocation_sampler_.setCurrentThreadTrackingSuppressed(suppressed);
}

std::size_t Profiler::allocationHookTargetCount() const
{
    return allocation_sampler_.hookTargetCount();
}

void Profiler::requestStop() noexcept
{
    sampling_stop_requested_.store(true, std::memory_order_release);
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    if (mode_ == ProfileMode::Allocation) {
        allocation_sampler_.requestStop();
    }
    else {
        sampler_.requestStop();
    }
}

bool Profiler::start(const ProfilerOptions &options, std::uint64_t main_tid, std::string &error)
{
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (running_.load()) {
        error = "profiler is already running";
        return false;
    }
    if (!reapRecoveryWriter()) {
        error = "previous recovery writer is still stopping";
        return false;
    }
    sampling_stop_requested_.store(false, std::memory_order_release);
    included_ticks_.store(0, std::memory_order_relaxed);

    if (options.regex && options.threads.empty()) {
        error = "--regex requires at least one --thread pattern";
        return false;
    }
    if (std::ranges::find(options.threads, "*") != options.threads.end() &&
        (options.regex || options.threads.size() != 1)) {
        error = "--thread * cannot be combined with other thread selectors or --regex";
        return false;
    }

    options_ = options;
    mode_ = options.alloc ? ProfileMode::Allocation : ProfileMode::Execution;

    const std::int64_t session_start_ms = nowMs();
    if (options.timeout_seconds > 0 &&
        options.timeout_seconds > (std::numeric_limits<std::int64_t>::max() - session_start_ms) / 1000) {
        error = "profiling timeout is too large";
        return false;
    }
    if (options.only_ticks_over_ms > std::numeric_limits<std::int32_t>::max()) {
        error = "tick threshold is too large";
        return false;
    }

    bool started = false;
    if (mode_ == ProfileMode::Allocation) {
        if (options.allocation_interval_bytes <= 0) {
            error = "allocation sampling interval must be greater than zero";
            return false;
        }
        interval_ = options.allocation_interval_bytes;
        AllocationSamplerConfig config;
        config.interval_bytes = options.allocation_interval_bytes;
        config.session_seed = main_tid;
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        config.all_threads = options.threads.empty() || (options.threads.size() == 1 && options.threads.front() == "*");
        config.regex_threads = options.regex;
        if (!config.all_threads) {
            config.thread_patterns = options.threads;
        }
        config.live_only = options.alloc_live_only;
        config.fail_aggregator_for_testing = options.fail_allocation_aggregator_for_testing;
        if (!recovery_dir_.empty()) {
            RecoveryWriter::Config wc;
            wc.directory = recovery_dir_;
            wc.session_id = static_cast<std::uint64_t>(session_start_ms);
            auto writer = std::make_unique<RecoveryWriter>(std::move(wc));
            if (writer->start()) {
                writer->journalSessionConfig(
                    static_cast<std::uint32_t>(interval_),
                    options.only_ticks_over_ms > 0 ? static_cast<std::int32_t>(options.only_ticks_over_ms) : 0,
                    config.all_threads, config.regex_threads, false, static_cast<std::uint8_t>(options.thread_grouper),
                    1, config.live_only, options.creator_name, options.creator_is_player, options.comment,
                    options.threads, profiling_window::windowAdjustmentMs(), options.creator_unique_id);
                writer->requestFlush();
                std::scoped_lock lock(recovery_mutex_);
                recovery_writer_ = std::move(writer);
                allocation_sampler_.setRecoverySink(recovery_writer_.get());
            }
            else {
                allocation_sampler_.setRecoverySink(nullptr);
            }
        }
        started = allocation_sampler_.start(config, error);
    }
    else {
        const int interval_ms = options.interval_ms > 0 ? options.interval_ms : 4;
        if (interval_ms > kMaxSamplingIntervalMs) {
            error = "sampling interval must not exceed 1000 milliseconds";
            return false;
        }
        interval_ = interval_ms * 1000;

        SamplerConfig config;
        config.interval_us = interval_;
        config.ignore_sleeping = options.ignore_sleeping;
        config.all_threads = options.threads.size() == 1 && options.threads.front() == "*";
        config.regex_threads = options.regex;
        if (!config.all_threads) {
            config.thread_patterns = options.threads;
        }
        config.only_ticks_over_ms = options.only_ticks_over_ms > 0 ? options.only_ticks_over_ms : 0;
        sampler_.setTarget(main_tid);
        if (!recovery_dir_.empty()) {
            RecoveryWriter::Config wc;
            wc.directory = recovery_dir_;
            wc.session_id = static_cast<std::uint64_t>(session_start_ms);
            auto writer = std::make_unique<RecoveryWriter>(std::move(wc));
            if (writer->start()) {
                writer->journalSessionConfig(
                    static_cast<std::uint32_t>(interval_),
                    options.only_ticks_over_ms > 0 ? static_cast<std::int32_t>(options.only_ticks_over_ms) : 0,
                    config.all_threads, config.regex_threads, config.ignore_sleeping,
                    static_cast<std::uint8_t>(options.thread_grouper), 0, false, options.creator_name,
                    options.creator_is_player, options.comment, options.threads, profiling_window::windowAdjustmentMs(),
                    options.creator_unique_id);
                // Journal the bounded execution module sentinel before samples.
                writer->journalModuleDef(0, kOtherModulesSentinel);
                writer->requestFlush();
                std::scoped_lock lock(recovery_mutex_);
                recovery_writer_ = std::move(writer);
                sampler_.setRecoverySink(recovery_writer_.get());
            }
            else {
                sampler_.setRecoverySink(nullptr);
            }
        }
        started = sampler_.start(config);
        if (!started) {
            error = sampler_.lastError().empty() ? "the platform stack-capture backend could not be initialized"
                                                 : sampler_.lastError();
        }
    }

    if (!started) {
        stopRecoveryWriter();
        return false;
    }

    running_.store(true);
    start_time_ms_ = session_start_ms;
    end_time_ms_ = 0;
    auto_end_time_ms_ =
        options.timeout_seconds > 0 ? start_time_ms_ + static_cast<std::int64_t>(options.timeout_seconds) * 1000 : -1;
    return true;
}

void Profiler::onTick(double mspt_ms)
{
    if (!running_.load()) {
        return;
    }
    if (options_.only_ticks_over_ms > 0 && std::isfinite(mspt_ms) &&
        mspt_ms > static_cast<double>(options_.only_ticks_over_ms)) {
        std::int32_t current = included_ticks_.load(std::memory_order_relaxed);
        while (current < std::numeric_limits<std::int32_t>::max() &&
               !included_ticks_.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) {
        }
    }
    if (mode_ == ProfileMode::Allocation) {
        allocation_sampler_.onTick(mspt_ms);
    }
    else {
        sampler_.onTick(mspt_ms);
    }
}

bool Profiler::stopSampling(std::string &error)
{
    sampling_stop_requested_.store(true, std::memory_order_release);
    if (stop_requested_hook_) {
        stop_requested_hook_();
    }
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (!running_.load()) {
        return true;
    }
    const std::int64_t requested_end_time_ms = nowMs();
    if (mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.stop(error)) {
            if (!allocation_sampler_.running()) {
                running_.store(false);
            }
            return false;
        }
        stopRecoveryWriter();
    }
    else {
        if (!sampler_.stop()) {
            error = sampler_.lastError();
            return false;
        }
        stopRecoveryWriter();
        if (sampler_.failure(error)) {
            running_.store(false);
            end_time_ms_ = requested_end_time_ms;
            return false;
        }
    }
    running_.store(false);
    end_time_ms_ = requested_end_time_ms;
    return true;
}

void Profiler::stopSampling()
{
    std::string ignored;
    stopSampling(ignored);
}

std::string Profiler::stop(const ExportContext &ctx)
{
    if (!running_.load()) {
        return {};
    }
    std::string error;
    if (!stopSampling(error)) {
        return {};
    }
    return exportData(ctx);
}

std::string Profiler::liveExport(const ExportContext &ctx)
{
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (!running_.load() || sampling_stop_requested_.load(std::memory_order_acquire)) {
        return {};
    }
    if (mode_ == ProfileMode::Allocation) {
        const bool tracking_was_suppressed = allocation_sampler_.setCurrentThreadTrackingSuppressed(true);
        try {
            AllocationSnapshot snapshot;
            std::string snapshot_error;
            if (!allocation_sampler_.snapshot(snapshot, snapshot_error)) {
                allocation_sampler_.setCurrentThreadTrackingSuppressed(tracking_was_suppressed);
                if (!snapshot_error.empty()) {
                    throw std::runtime_error(snapshot_error);
                }
                return {};
            }
            if (live_export_paused_hook_) {
                live_export_paused_hook_();
            }
            std::string data = exportData(ctx, &snapshot);
            allocation_sampler_.setCurrentThreadTrackingSuppressed(tracking_was_suppressed);
            return data;
        }
        catch (...) {
            allocation_sampler_.setCurrentThreadTrackingSuppressed(tracking_was_suppressed);
            throw;
        }
    }
    sampler_.pauseForExport();
    std::string sampler_error;
    if (sampler_.failure(sampler_error)) {
        if (!sampler_.stop()) {
            throw std::runtime_error(sampler_.lastError());
        }
        running_.store(false);
        stopRecoveryWriter();
        throw std::runtime_error(sampler_error);
    }
    std::string data;
    try {
        if (live_export_paused_hook_) {
            live_export_paused_hook_();
        }
        data = exportData(ctx);
    }
    catch (...) {
        if (sampling_stop_requested_.load(std::memory_order_acquire)) {
            sampler_.stop();
        }
        else if (!sampler_.resumeAfterExport()) {
            sampler_.stop();
            running_.store(false);
            stopRecoveryWriter();
        }
        throw;
    }
    if (sampling_stop_requested_.load(std::memory_order_acquire)) {
        if (!sampler_.stop()) {
            throw std::runtime_error(sampler_.lastError());
        }
        return data;
    }
    if (!sampler_.resumeAfterExport()) {
        sampler_.stop();
        running_.store(false);
        stopRecoveryWriter();
        throw std::runtime_error("the sampler service threads could not be resumed after live export");
    }
    return data;
}

bool Profiler::cancel(std::string &error)
{
    std::string stop_error;
    if (stopSampling(stop_error)) {
        error.clear();
        discardRecoveryJournal();
        return true;
    }

    // A failed aggregator invalidates the data; completed cleanup is a successful cancel.
    std::string backend_error;
    if (!running_.load() && backendFailure(backend_error)) {
        error.clear();
        discardRecoveryJournal();
        return true;
    }

    error = std::move(stop_error);
    return false;
}

void Profiler::cancel()
{
    std::string ignored;
    cancel(ignored);
}

bool Profiler::shutdown(std::string &error)
{
    sampling_stop_requested_.store(true, std::memory_order_release);
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    error.clear();
    if (running_.load() && mode_ == ProfileMode::Allocation) {
        if (!allocation_sampler_.shutdown(error)) {
            return false;
        }
        stopRecoveryWriter();
        running_.store(false);
        discardRecoveryJournal();
        if (hasPendingRecoveryWriter()) {
            error = "recovery writer shutdown timed out";
            return false;
        }
        return true;
    }
    if (running_.load()) {
        if (!sampler_.stop()) {
            error = sampler_.lastError();
            return false;
        }
        stopRecoveryWriter();
        running_.store(false);
    }
    // Clean shutdown: discard the journal so the next startup does not treat
    // it as a crash.  This is safe even if no journal exists.
    discardRecoveryJournal();
    if (hasPendingRecoveryWriter()) {
        error = "recovery writer shutdown timed out";
        return false;
    }
    return allocation_sampler_.shutdown(error);
}

}  // namespace spark

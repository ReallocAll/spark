#include <utility>

#include "core/profiler/profiler.h"

namespace spark {

void Profiler::stopRecoveryWriter()
{
    // A pending backend may still journal: keep the writer and both sinks intact.
    if (allocation_sampler_.backendCleanupPending()) {
        return;
    }
    RecoveryWriter *writer = nullptr;
    {
        std::scoped_lock lock(recovery_mutex_);
        sampler_.setRecoverySink(nullptr);
        allocation_sampler_.setRecoverySink(nullptr);
        writer = recovery_writer_.get();
    }
    if (!writer) {
        return;
    }

    // Do NOT journal CleanEnd here. CleanEnd before export would make a crash during
    // export look clean on the next startup. The journal is deleted only after a
    // successful export, cancel, or clean shutdown.
    writer->requestFlush();
    if (!writer->stop()) {
        // Keep ownership: the worker may still be executing Spark code or file I/O.
        return;
    }

    std::scoped_lock lock(recovery_mutex_);
    if (recovery_writer_.get() == writer) {
        recovery_writer_.reset();
    }
}

bool Profiler::reapRecoveryWriter()
{
    if (allocation_sampler_.backendCleanupPending()) {
        return false;
    }
    std::scoped_lock lock(recovery_mutex_);
    if (!recovery_writer_) {
        return true;
    }
    if (!recovery_writer_->tryReap()) {
        return false;
    }
    recovery_writer_.reset();
    return true;
}

bool Profiler::hasPendingRecoveryWriter() const
{
    std::scoped_lock lock(recovery_mutex_);
    return recovery_writer_ != nullptr;
}

RecoveryDiscardResult Profiler::discardRecoveryJournal()
{
    if (allocation_sampler_.backendCleanupPending()) {
        retain_recovery_journal_on_shutdown_.store(true, std::memory_order_release);
        return {.status = RecoveryDiscardStatus::BackendCleanupPending,
                .message = "allocation backend cleanup is still pending"};
    }
    stopRecoveryWriter();
    if (hasPendingRecoveryWriter()) {
        retain_recovery_journal_on_shutdown_.store(true, std::memory_order_release);
        return {.status = RecoveryDiscardStatus::WriterPending, .message = "recovery writer shutdown timed out"};
    }
    if (recovery_dir_.empty()) {
        retain_recovery_journal_on_shutdown_.store(false, std::memory_order_release);
        return {};
    }

    std::error_code ec;
#if defined(SPARK_ALLOCATION_LIFECYCLE_TESTING)
    if (recovery_remove_function_) {
        recovery_remove_function_(recovery_dir_, ec);
    }
    else {
        std::filesystem::remove_all(recovery_dir_, ec);
    }
#else
    std::filesystem::remove_all(recovery_dir_, ec);
#endif
    if (ec) {
        retain_recovery_journal_on_shutdown_.store(true, std::memory_order_release);
        return {.status = RecoveryDiscardStatus::FilesystemError,
                .message = "recovery journal cleanup failed: " + ec.message()};
    }
    retain_recovery_journal_on_shutdown_.store(false, std::memory_order_release);
    return {};
}

void Profiler::journalStallBegin(std::uint64_t detected_ns, std::uint64_t last_tick_ns)
{
    std::scoped_lock lock(recovery_mutex_);
    if (recovery_writer_) {
        recovery_writer_->journalStallBegin(detected_ns, last_tick_ns);
        recovery_writer_->requestFlush();
    }
}

void Profiler::journalStallEnd(std::uint64_t detected_ns, std::uint64_t recovered_ns)
{
    std::scoped_lock lock(recovery_mutex_);
    if (recovery_writer_) {
        recovery_writer_->journalStallEnd(detected_ns, recovered_ns);
        recovery_writer_->requestFlush();
    }
}

}  // namespace spark

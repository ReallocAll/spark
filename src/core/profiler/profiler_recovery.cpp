#include <utility>

#include "core/profiler/profiler.h"

namespace spark {

void Profiler::stopRecoveryWriter()
{
    std::unique_ptr<RecoveryWriter> writer;
    {
        std::scoped_lock lock(recovery_mutex_);
        sampler_.setRecoverySink(nullptr);
        allocation_sampler_.setRecoverySink(nullptr);
        writer = std::move(recovery_writer_);
    }
    if (!writer) {
        return;
    }
    // Do NOT journal CleanEnd here.  CleanEnd was previously written before
    // export, which caused crash-during-export profiles to be silently
    // discarded on the next startup.  The journal is now deleted only after a
    // successful export (announceResult), cancel, or clean shutdown.
    writer->requestFlush();
    writer->stop();
}

void Profiler::discardRecoveryJournal()
{
    std::unique_ptr<RecoveryWriter> writer;
    {
        std::scoped_lock lock(recovery_mutex_);
        sampler_.setRecoverySink(nullptr);
        allocation_sampler_.setRecoverySink(nullptr);
        writer = std::move(recovery_writer_);
    }
    if (writer) {
        writer->requestFlush();
        writer->stop();
    }
    if (!recovery_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(recovery_dir_, ec);
    }
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

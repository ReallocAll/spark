#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <thread>

#include "native/sampler/capture.h"
#include "native/sampler/capture_windows_backend.h"
#include "native/sampler/sampler.h"
#include "native/sampler/windows_stack_snapshot.h"

namespace spark {

struct CaptureTestAccess {
    static void setWindowsBackend(WindowsCaptureBackend *backend) { Capture::setWindowsBackendForTesting(backend); }
    static std::uint64_t cancellationGeneration() { return Capture::cancellationGenerationForTesting(); }
};

}  // namespace spark

namespace {

using namespace std::chrono_literals;
constexpr std::uint64_t KFakeThreadId = 0x5A17;

class FakeWindowsCaptureBackend final : public spark::WindowsCaptureBackend {
public:
    void configureSuccess()
    {
        std::scoped_lock lock(mutex_);
        open_ok_ = true;
        suspend_ok_ = true;
        context_ok_ = true;
        snapshot_ok_ = true;
        unwind_failure_at_ = -1;
        unwind_mode_ = UnwindMode::Normal;
        cancel_on_suspend_ = false;
        resume_failures_remaining_ = 0;
        block_context_ = false;
        block_unwind_ = false;
        context_entered_ = false;
        release_context_ = false;
        unwind_index_ = 0;
        successful_suspends_ = 0;
        resume_calls_ = 0;
        successful_resumes_ = 0;
        close_calls_ = 0;
        context_calls_ = 0;
        snapshot_calls_ = 0;
        unwind_calls_ = 0;
        closed_while_suspended_ = false;
        resume_without_suspend_ = false;
        snapshot_while_resumed_ = false;
        unwind_while_suspended_ = false;
        suspend_count_at_unwind_ = suspend_count_;
    }

    void setInitialSuspendCount(DWORD count)
    {
        std::scoped_lock lock(mutex_);
        baseline_suspend_count_ = count;
        suspend_count_ = count;
    }

    void failOpen() { setFlag(open_ok_, false); }
    void failSuspend() { setFlag(suspend_ok_, false); }
    void failContext() { setFlag(context_ok_, false); }
    void failSnapshot() { setFlag(snapshot_ok_, false); }
    void failUnwindAt(int index)
    {
        std::scoped_lock lock(mutex_);
        unwind_failure_at_ = index;
    }
    void unwindForever() { setUnwindMode(UnwindMode::Forever); }
    void unwindCycle() { setUnwindMode(UnwindMode::Cycle); }
    void cancelOnSuspend() { setFlag(cancel_on_suspend_, true); }

    void failResumeTimes(int count)
    {
        std::scoped_lock lock(mutex_);
        resume_failures_remaining_ = count;
    }

    void blockContext()
    {
        std::scoped_lock lock(mutex_);
        block_context_ = true;
        context_entered_ = false;
        release_context_ = false;
    }

    bool waitForContext(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return context_entered_; });
    }

    void releaseContext()
    {
        {
            std::scoped_lock lock(mutex_);
            release_context_ = true;
        }
        condition_.notify_all();
    }

    void blockUnwind()
    {
        std::scoped_lock lock(mutex_);
        block_unwind_ = true;
        unwind_entered_ = false;
        release_unwind_ = false;
    }

    bool waitForUnwind(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return unwind_entered_; });
    }

    void releaseUnwind()
    {
        {
            std::scoped_lock lock(mutex_);
            release_unwind_ = true;
        }
        condition_.notify_all();
    }

    bool waitForSuccessfulResume(std::size_t count, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count] { return successful_resumes_ >= count; });
    }

    HANDLE openThread(DWORD) noexcept override
    {
        std::scoped_lock lock(mutex_);
        return open_ok_ ? static_cast<HANDLE>(&fake_thread_) : nullptr;
    }

    DWORD suspendThread(HANDLE) noexcept override
    {
        bool cancel = false;
        DWORD previous = std::numeric_limits<DWORD>::max();
        {
            std::scoped_lock lock(mutex_);
            if (suspend_ok_) {
                previous = suspend_count_;
                ++suspend_count_;
                ++successful_suspends_;
                cancel = cancel_on_suspend_;
            }
        }
        if (cancel) {
            spark::Capture::cancelPending();
        }
        return previous;
    }

    bool getThreadContext(HANDLE, CONTEXT &context) noexcept override
    {
        std::unique_lock lock(mutex_);
        ++context_calls_;
        context_entered_ = true;
        condition_.notify_all();
        if (block_context_) {
            condition_.wait(lock, [this] { return release_context_; });
        }
        if (!context_ok_) {
            return false;
        }
        context.Rip = 0x1000;
        context.Rbp = 0x2000;
        context.Rsp = 0x3000;
        return true;
    }

    bool captureStackSnapshot(HANDLE, const CONTEXT &context, spark::WindowsStackSnapshot &snapshot) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++snapshot_calls_;
        if (suspend_count_ <= baseline_suspend_count_) {
            snapshot_while_resumed_ = true;
        }
        if (!snapshot_ok_) {
            return false;
        }
        if (!snapshot.setRange(static_cast<std::uintptr_t>(context.Rsp), sizeof(std::uint64_t))) {
            return false;
        }
        const std::uint64_t return_address = 0x2000;
        std::memcpy(snapshot.data(), &return_address, sizeof(return_address));
        return true;
    }

    spark::WindowsWalkStatus unwindNext(const spark::WindowsStackSnapshot &, CONTEXT &context,
                                        std::uintptr_t &instruction_pointer) noexcept override
    {
        std::unique_lock lock(mutex_);
        ++unwind_calls_;
        suspend_count_at_unwind_ = suspend_count_;
        if (suspend_count_ > baseline_suspend_count_) {
            unwind_while_suspended_ = true;
        }
        unwind_entered_ = true;
        condition_.notify_all();
        if (block_unwind_) {
            condition_.wait(lock, [this] { return release_unwind_; });
        }
        const int index = unwind_index_++;
        if (unwind_failure_at_ == index) {
            return spark::WindowsWalkStatus::Failure;
        }
        if (unwind_mode_ == UnwindMode::Cycle) {
            instruction_pointer = 0x1000;
            return spark::WindowsWalkStatus::Frame;
        }
        if (unwind_mode_ == UnwindMode::Forever) {
            instruction_pointer = std::uintptr_t{0x2000} + static_cast<std::uintptr_t>(index) * 8U;
            context.Rip = instruction_pointer;
            context.Rsp = 0x3008 + static_cast<DWORD64>(index) * 8;
            return spark::WindowsWalkStatus::Frame;
        }
        if (index == 0) {
            instruction_pointer = 0x2000;
            context.Rip = instruction_pointer;
            context.Rsp += sizeof(std::uint64_t);
            return spark::WindowsWalkStatus::Frame;
        }
        return spark::WindowsWalkStatus::Complete;
    }

    DWORD resumeThread(HANDLE) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++resume_calls_;
        if (resume_failures_remaining_ > 0) {
            --resume_failures_remaining_;
            return std::numeric_limits<DWORD>::max();
        }
        if (suspend_count_ <= baseline_suspend_count_) {
            resume_without_suspend_ = true;
            return std::numeric_limits<DWORD>::max();
        }
        const DWORD previous = suspend_count_;
        --suspend_count_;
        ++successful_resumes_;
        condition_.notify_all();
        return previous;
    }

    bool threadExited(HANDLE) noexcept override { return false; }

    void closeThread(HANDLE) noexcept override
    {
        std::scoped_lock lock(mutex_);
        ++close_calls_;
        if (suspend_count_ > baseline_suspend_count_) {
            closed_while_suspended_ = true;
        }
    }

    std::size_t successfulSuspends() const
    {
        std::scoped_lock lock(mutex_);
        return successful_suspends_;
    }
    std::size_t resumeCalls() const
    {
        std::scoped_lock lock(mutex_);
        return resume_calls_;
    }
    std::size_t successfulResumes() const
    {
        std::scoped_lock lock(mutex_);
        return successful_resumes_;
    }
    std::size_t closeCalls() const
    {
        std::scoped_lock lock(mutex_);
        return close_calls_;
    }
    std::size_t contextCalls() const
    {
        std::scoped_lock lock(mutex_);
        return context_calls_;
    }
    std::size_t snapshotCalls() const
    {
        std::scoped_lock lock(mutex_);
        return snapshot_calls_;
    }
    std::size_t unwindCalls() const
    {
        std::scoped_lock lock(mutex_);
        return unwind_calls_;
    }

    DWORD suspendCountAtUnwind() const
    {
        std::scoped_lock lock(mutex_);
        return suspend_count_at_unwind_;
    }
    bool snapshotWhileResumed() const
    {
        std::scoped_lock lock(mutex_);
        return snapshot_while_resumed_;
    }
    bool unwindWhileSuspended() const
    {
        std::scoped_lock lock(mutex_);
        return unwind_while_suspended_;
    }

    bool balanced() const
    {
        std::scoped_lock lock(mutex_);
        return suspend_count_ == baseline_suspend_count_ && successful_suspends_ == successful_resumes_ &&
               !closed_while_suspended_ && !resume_without_suspend_;
    }

private:
    enum class UnwindMode {
        Normal,
        Forever,
        Cycle,
    };

    void setUnwindMode(UnwindMode mode)
    {
        std::scoped_lock lock(mutex_);
        unwind_mode_ = mode;
    }

    void setFlag(bool &flag, bool value)
    {
        std::scoped_lock lock(mutex_);
        flag = value;
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ok_ = true;
    bool suspend_ok_ = true;
    bool context_ok_ = true;
    bool snapshot_ok_ = true;
    int unwind_failure_at_ = -1;
    UnwindMode unwind_mode_ = UnwindMode::Normal;
    bool cancel_on_suspend_ = false;
    bool block_context_ = false;
    bool block_unwind_ = false;
    bool context_entered_ = false;
    bool release_context_ = false;
    bool unwind_entered_ = false;
    bool release_unwind_ = false;
    int resume_failures_remaining_ = 0;
    int unwind_index_ = 0;
    DWORD baseline_suspend_count_ = 0;
    DWORD suspend_count_ = 0;
    int fake_thread_ = 0;
    std::size_t successful_suspends_ = 0;
    std::size_t resume_calls_ = 0;
    std::size_t successful_resumes_ = 0;
    std::size_t close_calls_ = 0;
    std::size_t context_calls_ = 0;
    std::size_t snapshot_calls_ = 0;
    std::size_t unwind_calls_ = 0;
    bool closed_while_suspended_ = false;
    bool resume_without_suspend_ = false;
    bool snapshot_while_resumed_ = false;
    bool unwind_while_suspended_ = false;
    DWORD suspend_count_at_unwind_ = 0;
};

class CaptureSession {
public:
    explicit CaptureSession(FakeWindowsCaptureBackend &backend) : backend_(backend)
    {
        spark::CaptureTestAccess::setWindowsBackend(&backend_);
        armed_ = spark::Capture::arm();
    }

    ~CaptureSession()
    {
        if (armed_) {
            spark::Capture::disarm();
        }
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
    }

    [[nodiscard]] bool armed() const { return armed_; }

private:
    FakeWindowsCaptureBackend &backend_;
    bool armed_ = false;
};

bool require(bool condition, const char *message)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "windows capture lifecycle: %s\n", message);
    return false;
}

class SyntheticUnwindFixture {
public:
    static constexpr std::uintptr_t KFunctionBegin = 0x400000;
    static constexpr std::uintptr_t KFunctionEnd = 0x400100;
    static constexpr std::uintptr_t KMetadataAddress = 0x500000;
    static constexpr std::uintptr_t KImageBase = 0x900000;
    static constexpr std::uintptr_t KChainAddress = KImageBase + 0x100;
    static constexpr std::uintptr_t KStackBase = 0x800000;

    SyntheticUnwindFixture() { reset(); }

    void reset()
    {
        primary_.fill(0);
        chain_.fill(0);
        instructions_.fill(0x90);
        function_ = {.begin = KFunctionBegin, .end = KFunctionEnd, .unwind_info = KMetadataAddress, .image_base = 0};
        context_ = CONTEXT{};
        snapshot_.clear();
        last_instruction_pointer_ = 0;
        setStackSize(0x200);
    }

    void setStackSize(std::size_t size)
    {
        (void)snapshot_.setRange(KStackBase, size);
        std::memset(snapshot_.data(), 0, size);
    }

    void setContext(std::uintptr_t instruction_offset = 0x80)
    {
        context_ = CONTEXT{};
        context_.Rip = KFunctionBegin + instruction_offset;
        context_.Rsp = KStackBase;
    }

    void setRbp(std::uint64_t value) { context_.Rbp = value; }
    void setR13(std::uint64_t value) { context_.R13 = value; }
    void setRsp(std::uint64_t value) { context_.Rsp = value; }
    void setPrimaryByte(std::size_t offset, std::uint8_t value) { primary_[offset] = value; }

    void setQword(std::uintptr_t stack_location, std::uint64_t stored_value)
    {
        std::memcpy(snapshot_.data() + (stack_location - KStackBase), &stored_value, sizeof(stored_value));
    }

    void setXmm(std::uintptr_t address, const M128A &value)
    {
        std::memcpy(snapshot_.data() + (address - KStackBase), &value, sizeof(value));
    }

    static void setInfo(std::array<std::uint8_t, 1024> &storage, std::uint8_t flags, std::uint8_t prolog,
                        std::uint8_t frame_register, std::uint8_t frame_offset,
                        std::initializer_list<std::uint16_t> slots)
    {
        storage.fill(0);
        storage[0] = static_cast<std::uint8_t>(1U | (flags << 3U));
        storage[1] = prolog;
        storage[2] = static_cast<std::uint8_t>(slots.size());
        storage[3] = static_cast<std::uint8_t>((frame_offset << 4U) | frame_register);
        std::size_t index = 0;
        for (const auto slot : slots) {
            storage[4U + index * 2U] = static_cast<std::uint8_t>(slot & 0xffU);
            storage[5U + index * 2U] = static_cast<std::uint8_t>(slot >> 8U);
            ++index;
        }
    }

    void setPrimaryInfo(std::uint8_t flags, std::uint8_t prolog, std::uint8_t frame_register, std::uint8_t frame_offset,
                        std::initializer_list<std::uint16_t> slots)
    {
        setInfo(primary_, flags, prolog, frame_register, frame_offset, slots);
    }

    void setChainInfo(std::uint8_t flags, std::uint8_t prolog, std::uint8_t frame_register, std::uint8_t frame_offset,
                      std::initializer_list<std::uint16_t> slots)
    {
        setInfo(chain_, flags, prolog, frame_register, frame_offset, slots);
    }

    void setChainEntry(std::uint32_t begin_rva, std::uint32_t end_rva, std::uint32_t unwind_rva)
    {
        const auto offset = (static_cast<std::size_t>(primary_[2]) + 1U & ~1U) * 2U + 4U;
        writeDword(primary_, offset, begin_rva);
        writeDword(primary_, offset + 4U, end_rva);
        writeDword(primary_, offset + 8U, unwind_rva);
    }

    void setChainMode(bool enabled)
    {
        function_.image_base = enabled ? KImageBase : 0;
        if (enabled) {
            function_.unwind_info = KMetadataAddress;
        }
    }

    void setInstructionBytes(std::initializer_list<std::uint8_t> bytes, std::size_t offset = 0)
    {
        instructions_.fill(0x90);
        std::size_t index = 0;
        for (const auto byte : bytes) {
            if (offset + index >= instructions_.size()) {
                break;
            }
            instructions_[offset + index++] = byte;
        }
    }

    spark::WindowsWalkStatus step()
    {
        return spark::windowsUnwindNext(snapshot_, context_, last_instruction_pointer_, lookup, this, read, this);
    }

    [[nodiscard]] const CONTEXT &context() const { return context_; }
    [[nodiscard]] std::uintptr_t lastInstructionPointer() const { return last_instruction_pointer_; }
    [[nodiscard]] std::uint8_t primaryByte(std::size_t offset) const { return primary_[offset]; }

private:
    static void writeDword(std::array<std::uint8_t, 1024> &storage, std::size_t offset, std::uint32_t value)
    {
        storage[offset] = static_cast<std::uint8_t>(value & 0xffU);
        storage[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        storage[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        storage[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    }

    static bool copyRange(std::uintptr_t address, void *destination, std::size_t bytes, std::uintptr_t base,
                          const std::uint8_t *source, std::size_t source_size) noexcept
    {
        return address >= base && bytes <= source_size && address - base <= source_size - bytes &&
               (std::memcpy(destination, source + (address - base), bytes), true);
    }

    static spark::WindowsFunctionLookupStatus lookup(std::uintptr_t control_pc, spark::WindowsRuntimeFunction &function,
                                                     void *opaque) noexcept
    {
        auto &fixture = *static_cast<SyntheticUnwindFixture *>(opaque);
        if (control_pc < fixture.function_.begin || control_pc >= fixture.function_.end) {
            return spark::WindowsFunctionLookupStatus::Leaf;
        }
        function = fixture.function_;
        return spark::WindowsFunctionLookupStatus::Function;
    }

    static bool read(std::uintptr_t address, void *destination, std::size_t bytes, void *opaque) noexcept
    {
        auto &fixture = *static_cast<SyntheticUnwindFixture *>(opaque);
        if (copyRange(address, destination, bytes, KMetadataAddress, fixture.primary_.data(),
                      fixture.primary_.size()) ||
            copyRange(address, destination, bytes, KChainAddress, fixture.chain_.data(), fixture.chain_.size()) ||
            copyRange(address, destination, bytes, KFunctionBegin, fixture.instructions_.data(),
                      fixture.instructions_.size())) {
            return true;
        }
        return false;
    }

    std::array<std::uint8_t, 1024> primary_{};
    std::array<std::uint8_t, 1024> chain_{};
    std::array<std::uint8_t, 1024> instructions_{};
    spark::WindowsRuntimeFunction function_{};
    CONTEXT context_{};
    spark::WindowsStackSnapshot snapshot_{};
    std::uintptr_t last_instruction_pointer_ = 0;
};

template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool testSyntheticUnwindOperations()
{
    SyntheticUnwindFixture fixture;
    std::uint64_t return_address = 0x700000;
    const auto slot = [](std::uint8_t code_offset, std::uint8_t operation, std::uint8_t operation_info = 0) {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>((operation_info << 4U) | operation) << 8U) |
                                          code_offset);
    };

    fixture.setContext();
    fixture.setPrimaryInfo(0, 0, 0, 0, {});
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.lastInstructionPointer() == return_address &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + sizeof(std::uint64_t),
                 "leaf unwind did not pop the return address")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(5, 2, 3)});
    fixture.setStackSize(0x40);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 32U, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 40U,
                 "ALLOC_SMALL unwind used the wrong stack size")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(5, 1), 0x0008U});
    fixture.setStackSize(0x50);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 64U, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 72U,
                 "ALLOC_LARGE unwind used the wrong stack size")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(5, 1, 1), 0x1000U, 0x0000U});
    fixture.setStackSize(0x1010);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 0x1000U, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 0x1008U,
                 "large-form ALLOC_LARGE unwind used the wrong stack size")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setRbp(0);
    fixture.setPrimaryInfo(0, 7, 5, 2, {slot(6, 3), slot(5, 2, 7)});
    fixture.setRbp(SyntheticUnwindFixture::KStackBase + 32U);
    fixture.setStackSize(0x60);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 64U, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 72U,
                 "frame-register unwind did not restore RSP")) {
        return false;
    }

    fixture.reset();
    fixture.setContext(1);
    fixture.setPrimaryInfo(0, 7, 5, 2, {slot(6, 3), slot(5, 2, 7)});
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 8U,
                 "pre-frame-pointer prolog unwind used an unavailable frame register")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 7, 13, 1, {slot(6, 4, 13), 0x0002U, slot(5, 3)});
    fixture.setR13(SyntheticUnwindFixture::KStackBase + 16U);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 16U, 0x12345678U);
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    const auto restored_status = fixture.step();
    if (!require(restored_status == spark::WindowsWalkStatus::Frame && fixture.context().R13 == 0x12345678U,
                 "non-RBP frame register was not restored")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(6, 8, 6), 0x0001U});
    M128A xmm_value{.Low = static_cast<LONGLONG>(0x1122334455667788ULL),
                    .High = static_cast<LONGLONG>(0x8877665544332211ULL)};
    fixture.setXmm(SyntheticUnwindFixture::KStackBase + 16U, xmm_value);
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame && fixture.context().Xmm6.Low == xmm_value.Low &&
                     fixture.context().Xmm6.High == xmm_value.High,
                 "SAVE_XMM128 did not consume its slot")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(6, 5, 13), 0x0010U, 0x0000U});
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 16U, 0x87654321U);
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame && fixture.context().R13 == 0x87654321U,
                 "SAVE_NONVOL_FAR did not consume its slots")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 0, 0, 0, {slot(0, 10)});
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 40U,
                 "PUSH_MACHFRAME without an error code was not unwound")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 0, 0, 0, {slot(0, 10, 1)});
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 8U, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 48U,
                 "PUSH_MACHFRAME with an error code was not unwound")) {
        return false;
    }

    return true;
}

bool testSyntheticUnwindRejectionAndChains()
{
    SyntheticUnwindFixture fixture;
    const std::uint64_t return_address = 0x700000;
    const auto slot = [](std::uint8_t code_offset, std::uint8_t operation, std::uint8_t operation_info = 0) {
        return static_cast<std::uint16_t>((static_cast<std::uint16_t>((operation_info << 4U) | operation) << 8U) |
                                          code_offset);
    };

    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(6, 4, 13), 0x0020U});
    fixture.setStackSize(0x10);
    fixture.setQword(SyntheticUnwindFixture::KStackBase, return_address);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Complete,
                 "out-of-range saved register was not truncated")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setPrimaryInfo(0, 6, 0, 0, {slot(6, 6)});
    fixture.setPrimaryByte(0, 2);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Complete, "invalid unwind version was not truncated")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setChainMode(true);
    fixture.setPrimaryInfo(4, 0, 0, 0, {});
    fixture.setChainInfo(0, 1, 0, 0, {slot(1, 0, 5)});
    fixture.setChainEntry(0x1000U, 0x1100U, 0x100U);
    fixture.setQword(SyntheticUnwindFixture::KStackBase, 0x11111111U);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 8U, return_address);
    const auto chain_status = fixture.step();
    if (!require(chain_status == spark::WindowsWalkStatus::Frame &&
                     fixture.context().Rsp == SyntheticUnwindFixture::KStackBase + 16U,
                 "chained unwind metadata was not followed")) {
        return false;
    }

    fixture.reset();
    fixture.setContext();
    fixture.setChainMode(true);
    fixture.setPrimaryInfo(4, 0, 0, 0, {});
    fixture.setChainInfo(4, 0, 0, 0, {});
    fixture.setChainEntry(0x1000U, 0x1100U, 0x100U);
    fixture.setChainEntry(0x1000U, 0x1100U, 0x100U);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Complete, "cyclic chained metadata was not truncated")) {
        return false;
    }

    fixture.reset();
    fixture.setContext(0xc0);
    fixture.setPrimaryInfo(0, 0, 0, 0, {});
    fixture.setInstructionBytes({0x48U, 0x83U, 0xc4U, 0x08U, 0xc3U}, 0xc0U);
    fixture.setQword(SyntheticUnwindFixture::KStackBase + 8U, return_address);
    fixture.setRsp(SyntheticUnwindFixture::KStackBase);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Frame &&
                     fixture.lastInstructionPointer() == return_address,
                 "basic epilogue was not unwound")) {
        return false;
    }

    fixture.reset();
    fixture.setContext(0xc0);
    fixture.setPrimaryInfo(0, 0, 0, 0, {});
    fixture.setInstructionBytes({0x48U, 0x83U, 0xc4U, 0x08U, 0x90U}, 0xc0U);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Complete, "ambiguous epilogue was not truncated")) {
        return false;
    }

    fixture.reset();
    fixture.setContext(0xc0);
    fixture.setPrimaryInfo(0, 0, 0, 0, {});
    fixture.setInstructionBytes({0x49U, 0x83U, 0xc4U, 0x08U, 0xc3U}, 0xc0U);
    if (!require(fixture.step() == spark::WindowsWalkStatus::Complete,
                 "non-RSP REX epilogue was incorrectly unwound")) {
        return false;
    }

    if (!require(spark::windowsCanonicalAddress(0x00007fff00000000ULL) &&
                     spark::windowsCanonicalAddress(0xffff800000000000ULL) &&
                     !spark::windowsCanonicalAddress(0x0001000000000000ULL) &&
                     !spark::windowsCanonicalAddress(0xffff000000000000ULL),
                 "noncanonical Windows addresses were accepted")) {
        return false;
    }
    return true;
}

bool testFailureStages()
{
    FakeWindowsCaptureBackend backend;
    CaptureSession session(backend);
    if (!require(session.armed(), "arm failed")) {
        return false;
    }

    spark::CaptureBuffer buffer{};

    backend.configureSuccess();
    backend.failOpen();
    buffer.count = 99;
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "OpenThread failure did not fail cleanly") ||
        !require(backend.successfulSuspends() == 0 && backend.resumeCalls() == 0 && backend.closeCalls() == 0,
                 "OpenThread failure touched suspension state")) {
        return false;
    }

    backend.configureSuccess();
    backend.failSuspend();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "SuspendThread failure did not fail cleanly") ||
        !require(backend.resumeCalls() == 0 && backend.closeCalls() == 1 && backend.balanced(),
                 "SuspendThread failure resumed an unsuspended thread")) {
        return false;
    }

    backend.configureSuccess();
    backend.cancelOnSuspend();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer), "post-suspend cancellation was ignored") ||
        !require(backend.contextCalls() == 0 && backend.successfulResumes() == 1 && backend.balanced(),
                 "post-suspend cancellation did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failContext();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "GetThreadContext failure leaked a partial capture") ||
        !require(backend.contextCalls() == 1 && backend.snapshotCalls() == 0 && backend.successfulResumes() == 1 &&
                     backend.balanced(),
                 "GetThreadContext failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failSnapshot();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "stack snapshot failure leaked a partial capture") ||
        !require(backend.snapshotCalls() == 1 && backend.unwindCalls() == 0 && backend.successfulResumes() == 1 &&
                     backend.balanced(),
                 "stack snapshot failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failUnwindAt(0);
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "first unwind failure published a partial capture") ||
        !require(backend.unwindCalls() == 1 && backend.successfulResumes() == 1 && backend.balanced(),
                 "first unwind failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    backend.failUnwindAt(1);
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "mid unwind failure published a partial capture") ||
        !require(backend.unwindCalls() == 2 && backend.successfulResumes() == 1 && backend.balanced(),
                 "mid unwind failure did not restore exactly once")) {
        return false;
    }

    backend.configureSuccess();
    if (!require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 2,
                 "successful capture did not publish expected frames") ||
        !require(backend.successfulResumes() == 1 && backend.suspendCountAtUnwind() == 0 &&
                     !backend.snapshotWhileResumed() && !backend.unwindWhileSuspended() && backend.balanced(),
                 "successful capture did not restore target before unwind")) {
        return false;
    }

    backend.configureSuccess();
    backend.failContext();
    if (!require(!spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 0,
                 "failed capture retained frames from the previous capture") ||
        !require(backend.balanced(), "failed reuse left the target suspended")) {
        return false;
    }

    backend.configureSuccess();
    return require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 2,
                   "capture backend did not recover after a failure") &&
           require(backend.balanced(), "recovered capture left suspension state changed");
}

bool testUnwindBoundsAndCycles()
{
    FakeWindowsCaptureBackend backend;
    CaptureSession session(backend);
    if (!require(session.armed(), "arm failed for unwind bound test")) {
        return false;
    }

    spark::CaptureBuffer buffer{};
    backend.configureSuccess();
    backend.unwindForever();
    if (!require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == spark::CaptureBuffer::kMax &&
                     backend.unwindCalls() == spark::CaptureBuffer::kMax - 1U,
                 "unwind exceeded the fixed frame bound") ||
        !require(backend.balanced(), "bounded unwind changed suspension state")) {
        return false;
    }

    backend.configureSuccess();
    backend.unwindCycle();
    if (!require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 1 &&
                     backend.unwindCalls() == 1,
                 "cyclic unwind did not truncate at the repeated frame") ||
        !require(backend.balanced(), "cyclic unwind changed suspension state")) {
        return false;
    }
    return true;
}

bool testResumeRetryAndPriorSuspendCount()
{
    FakeWindowsCaptureBackend backend;
    backend.setInitialSuspendCount(2);
    CaptureSession session(backend);
    if (!require(session.armed(), "arm failed for resume retry")) {
        return false;
    }

    backend.configureSuccess();
    backend.failResumeTimes(1);
    spark::CaptureBuffer buffer{};
    return require(spark::Capture::captureThread(KFakeThreadId, buffer) && buffer.count == 2,
                   "transient ResumeThread failure prevented capture recovery") &&
           require(backend.resumeCalls() == 2 && backend.successfulResumes() == 1,
                   "ResumeThread retry produced the wrong successful resume count") &&
           require(backend.balanced(), "pre-existing suspend count was not restored");
}

bool testDisarmCancellationAndRearm()
{
    FakeWindowsCaptureBackend backend;
    spark::CaptureTestAccess::setWindowsBackend(&backend);
    if (!require(spark::Capture::arm(), "arm failed for disarm cancellation")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    backend.configureSuccess();
    backend.blockUnwind();
    spark::CaptureBuffer buffer{};
    std::atomic<bool> capture_result{true};
    std::thread capture(
        [&] { capture_result.store(spark::Capture::captureThread(KFakeThreadId, buffer), std::memory_order_release); });
    if (!require(backend.waitForUnwind(2s), "capture did not reach the blocked unwind stage")) {
        backend.releaseUnwind();
        capture.join();
        spark::Capture::disarm();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    const std::uint64_t generation = spark::CaptureTestAccess::cancellationGeneration();
    std::atomic<bool> disarm_result{false};
    std::thread disarm([&] { disarm_result.store(spark::Capture::disarm(), std::memory_order_release); });
    if (!require(waitFor([&] { return spark::CaptureTestAccess::cancellationGeneration() != generation; }, 2s),
                 "disarm did not cancel the in-flight capture")) {
        backend.releaseUnwind();
        capture.join();
        disarm.join();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    if (!require(backend.successfulResumes() == backend.successfulSuspends() && backend.suspendCountAtUnwind() == 0 &&
                     !backend.unwindWhileSuspended(),
                 "unwind entered before the target was resumed")) {
        backend.releaseUnwind();
        capture.join();
        disarm.join();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    backend.releaseUnwind();
    capture.join();
    disarm.join();

    if (!require(!capture_result.load(std::memory_order_acquire) && disarm_result.load(std::memory_order_acquire),
                 "disarm cancellation did not complete cleanly") ||
        !require(backend.successfulResumes() == 1 && backend.closeCalls() == 1 && backend.balanced(),
                 "disarm closed the target before restoring it")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    backend.configureSuccess();
    if (!require(spark::Capture::arm(), "rearm failed after cancelled capture")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    const bool restarted = spark::Capture::captureThread(KFakeThreadId, buffer);
    const bool final_disarm = spark::Capture::disarm();
    spark::CaptureTestAccess::setWindowsBackend(nullptr);
    return require(restarted && buffer.count == 2, "capture did not recover after disarm/rearm") &&
           require(final_disarm && backend.balanced(), "final disarm did not restore lifecycle state");
}

bool testSamplerStopAndRestart()
{
    FakeWindowsCaptureBackend backend;
    spark::CaptureTestAccess::setWindowsBackend(&backend);
    backend.configureSuccess();
    backend.blockUnwind();

    spark::Sampler sampler;
    sampler.setTarget(KFakeThreadId, "Fake server thread");
    spark::SamplerConfig config;
    config.interval_us = 1000;
    if (!require(sampler.start(config), "sampler start failed")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    if (!require(backend.waitForUnwind(2s), "sampler capture did not reach blocked unwind")) {
        backend.releaseUnwind();
        sampler.stop();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    const std::uint64_t generation = spark::CaptureTestAccess::cancellationGeneration();
    std::atomic<bool> stop_result{false};
    std::thread stopper([&] { stop_result.store(sampler.stop(), std::memory_order_release); });
    if (!require(waitFor([&] { return spark::CaptureTestAccess::cancellationGeneration() != generation; }, 2s),
                 "sampler stop did not cancel the in-flight capture")) {
        backend.releaseUnwind();
        stopper.join();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    if (!require(backend.successfulResumes() == backend.successfulSuspends() && backend.suspendCountAtUnwind() == 0 &&
                     !backend.unwindWhileSuspended(),
                 "sampler unwind entered before the target was resumed")) {
        backend.releaseUnwind();
        stopper.join();
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    backend.releaseUnwind();
    stopper.join();

    if (!require(stop_result.load(std::memory_order_acquire) && backend.balanced(),
                 "sampler stop did not restore the suspended target")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }

    backend.configureSuccess();
    if (!require(sampler.start(config), "sampler did not restart after cancelled capture")) {
        spark::CaptureTestAccess::setWindowsBackend(nullptr);
        return false;
    }
    const bool captured_again = backend.waitForSuccessfulResume(1, 2s);
    const bool stopped_again = sampler.stop();
    spark::CaptureTestAccess::setWindowsBackend(nullptr);
    return require(captured_again && backend.unwindCalls() >= 2, "sampler did not capture again after restart") &&
           require(stopped_again && backend.balanced(), "restarted sampler did not shut down cleanly");
}

}  // namespace

int main()
{
    if (!testSyntheticUnwindOperations() || !testSyntheticUnwindRejectionAndChains() || !testFailureStages() ||
        !testUnwindBoundsAndCycles() || !testResumeRetryAndPriorSuspendCount() || !testDisarmCancellationAndRearm() ||
        !testSamplerStopAndRestart()) {
        return 1;
    }
    return 0;
}

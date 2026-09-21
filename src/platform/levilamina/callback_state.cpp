#include "platform/levilamina/callback_state.h"

#include <utility>

namespace spark::levilamina {

namespace {

struct WorkSlotState {
    enum class Status { Pending, Claimed, Cancelled };
};

}  // namespace

struct CallbackState::WorkSlot {
    using Status = WorkSlotState::Status;

    explicit WorkSlot(std::function<void()> body) : body(std::move(body)) {}

    Status status = Status::Pending;
    std::function<void()> body;
};

CallbackState::DiagnosticCallbacks::DiagnosticCallbacks(DiagnosticCallbacks&& other) noexcept
    : info(std::move(other.info)),
      error(std::move(other.error)),
      owner_(std::move(other.owner_)),
      reserved_activity_(std::exchange(other.reserved_activity_, 0))
{}

CallbackState::DiagnosticCallbacks& CallbackState::DiagnosticCallbacks::operator=(DiagnosticCallbacks&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    if (owner_) {
        owner_->destroyDiagnosticCallbacks(*this);
    }
    info = std::move(other.info);
    error = std::move(other.error);
    owner_ = std::move(other.owner_);
    reserved_activity_ = std::exchange(other.reserved_activity_, 0);
    return *this;
}

CallbackState::DiagnosticCallbacks::~DiagnosticCallbacks() noexcept
{
    if (owner_) {
        owner_->destroyDiagnosticCallbacks(*this);
    }
}

CallbackState::CleanupScope::CleanupScope(CallbackState* state) noexcept : state_(state)
{
    if (state_ != nullptr) {
        frame_.state = state_;
        state_->pushTls(frame_);
    }
}

CallbackState::CleanupScope::~CleanupScope() noexcept
{
    if (state_ != nullptr) {
        state_->popTls(frame_);
    }
}

CallbackState::ActivityScope::ActivityScope(CallbackState* state, std::size_t count) noexcept
    : state_(state), count_(count)
{
    frame_.state = state;
    state_->pushTls(frame_);
}

CallbackState::ActivityScope::~ActivityScope() noexcept
{
    release();
}

void CallbackState::ActivityScope::release() noexcept
{
    if (released_) {
        return;
    }
    released_ = true;
    state_->popTls(frame_);
    state_->releaseActive(count_);
}

void CallbackState::setSubmitter(Submitter submitter)
{
    std::shared_ptr<Submitter> replacement;
    if (submitter) {
        replacement = std::make_shared<Submitter>(std::move(submitter));
    }

    std::shared_ptr<Submitter> previous;
    bool discard_replacement = false;
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Open) {
            discard_replacement = true;
        }
        else {
            previous = std::move(submitter_);
            if (previous) {
                reserveActiveLocked(1);
            }
            submitter_ = std::move(replacement);
        }
    }

    if (previous) {
        destroySubmitter(previous, true);
    }
    if (discard_replacement && replacement) {
        destroySubmitter(replacement, false);
    }
}

void CallbackState::setInfoCallback(Diagnostic callback)
{
    std::shared_ptr<Diagnostic> replacement;
    if (callback) {
        replacement = std::make_shared<Diagnostic>(std::move(callback));
    }

    std::shared_ptr<Diagnostic> previous;
    bool discard_replacement = false;
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Open) {
            discard_replacement = true;
        }
        else {
            previous = std::move(info_callback_);
            if (previous) {
                reserveActiveLocked(1);
            }
            info_callback_ = std::move(replacement);
        }
    }

    if (previous) {
        destroyDiagnostic(previous, true);
    }
    if (discard_replacement && replacement) {
        destroyDiagnostic(replacement, false);
    }
}

void CallbackState::setErrorCallback(Diagnostic callback)
{
    std::shared_ptr<Diagnostic> replacement;
    if (callback) {
        replacement = std::make_shared<Diagnostic>(std::move(callback));
    }

    std::shared_ptr<Diagnostic> previous;
    bool discard_replacement = false;
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Open) {
            discard_replacement = true;
        }
        else {
            previous = std::move(error_callback_);
            if (previous) {
                reserveActiveLocked(1);
            }
            error_callback_ = std::move(replacement);
        }
    }

    if (previous) {
        destroyDiagnostic(previous, true);
    }
    if (discard_replacement && replacement) {
        destroyDiagnostic(replacement, false);
    }
}

void CallbackState::setFatalHandler(FatalHandler handler)
{
    std::shared_ptr<FatalHandler> replacement;
    if (handler) {
        replacement = std::make_shared<FatalHandler>(std::move(handler));
    }

    std::shared_ptr<FatalHandler> previous;
    bool discard_replacement = false;
    {
        std::lock_guard lock(mutex_);
        if (phase_ != Phase::Open) {
            discard_replacement = true;
        }
        else {
            previous = std::move(fatal_handler_);
            if (previous) {
                reserveActiveLocked(1);
            }
            fatal_handler_ = std::move(replacement);
        }
    }

    if (previous) {
        destroyFatalHandler(previous, true);
    }
    if (discard_replacement && replacement) {
        destroyFatalHandler(replacement, false);
    }
}

bool CallbackState::post(std::function<void()> body)
{
    auto slot = std::make_shared<WorkSlot>(std::move(body));
    auto self = shared_from_this();
    std::shared_ptr<Submitter> submitter;
    bool accepted = false;

    {
        std::lock_guard lock(mutex_);
        if (phase_ == Phase::Open && submitter_ && *submitter_) {
            pending_.push_back(slot);
            ++pending_work_slots_;
            reserveActiveLocked(1);
            submitter = submitter_;
            accepted = true;
        }
        else {
            reserveActiveLocked(1);
        }
    }

    auto submit_scope = adoptActive(1);
    if (!accepted) {
        slot.reset();
        submit_scope.release();
        return false;
    }

    try {
        (*submitter)([self, slot] { self->invokeSlot(slot); });
    }
    catch (...) {
        std::function<void()> abandoned;
        bool abandoned_cancelled = false;
        {
            std::lock_guard lock(mutex_);
            abandoned_cancelled = cancelSlotLocked(slot, abandoned);
        }
        if (abandoned_cancelled) {
            auto payload_scope = adoptActive(1);
            abandoned = {};
            payload_scope.release();
        }
        submitter.reset();
        submit_scope.release();
        return false;
    }

    submitter.reset();
    submit_scope.release();
    return true;
}

bool CallbackState::invokeInline(std::function<void()> body)
{
    auto self = shared_from_this();
    bool admitted = false;
    {
        std::lock_guard lock(mutex_);
        admitted = phase_ == Phase::Open;
        reserveActiveLocked(1);
    }

    auto body_scope = adoptActive(1);
    if (!admitted) {
        body = {};
        body_scope.release();
        static_cast<void>(self);
        return false;
    }

    std::exception_ptr exception;
    try {
        if (body) {
            body();
        }
    }
    catch (...) {
        exception = std::current_exception();
    }
    body = {};
    try {
        if (exception) {
            reportException(exception);
        }
    }
    catch (...) {
        // Keep teardown accounting intact if a diagnostic callback throws.
    }
    exception = {};
    body_scope.release();
    static_cast<void>(self);
    return true;
}

CallbackState::CloseClaim CallbackState::beginClosing()
{
    return beginClosing({});
}

CallbackState::CloseClaim CallbackState::beginClosing(std::function<void()> detach_resources)
{
    std::shared_ptr<Submitter> detached_submitter;
    {
        std::lock_guard lock(mutex_);
        if (isActiveOnCurrentThread() || (producer_started_ && producer_thread_ == std::this_thread::get_id())) {
            return CloseClaim::SelfWaitRejected;
        }
        if (phase_ == Phase::Closed) {
            return CloseClaim::AlreadyClosed;
        }
        if (phase_ == Phase::Closing) {
            return CloseClaim::AlreadyClosing;
        }

        phase_ = Phase::Closing;
        producer_stop_requested_ = true;
        detached_submitter = std::move(submitter_);
        if (detached_submitter) {
            reserveActiveLocked(1);
        }
        detached_info_callback_ = std::move(info_callback_);
        detached_error_callback_ = std::move(error_callback_);
        detached_diagnostic_activity_ = 0;
        if (detached_info_callback_) {
            ++detached_diagnostic_activity_;
        }
        if (detached_error_callback_) {
            ++detached_diagnostic_activity_;
        }
        reserveActiveLocked(detached_diagnostic_activity_);
        condition_.notify_all();
    }

    if (detached_submitter) {
        destroySubmitter(detached_submitter, true);
    }

    if (detach_resources) {
        auto cleanup_scope = enterCleanupScope();
        try {
            detach_resources();
        }
        catch (...) {
            detach_resources = {};
            throw;
        }
        detach_resources = {};
    }
    return CloseClaim::Owner;
}

CallbackState::CleanupScope CallbackState::enterCleanupScope() noexcept
{
    return CleanupScope{this};
}

std::vector<std::function<void()>> CallbackState::takePendingPayloads()
{
    std::vector<std::function<void()>> payloads;
    std::lock_guard lock(mutex_);
    payloads.reserve(static_cast<std::size_t>(pending_work_slots_));
    for (auto const& slot : pending_) {
        if (slot->status == WorkSlot::Status::Pending) {
            static_cast<void>(cancelSlotLocked(slot, payloads.emplace_back()));
        }
    }
    pending_.clear();
    condition_.notify_all();
    return payloads;
}

void CallbackState::destroyPendingPayloads(std::vector<std::function<void()>>& payloads)
{
    const auto count = payloads.size();
    if (count == 0) {
        payloads.clear();
        return;
    }
    auto payload_scope = adoptActive(count);
    payloads.clear();
    payload_scope.release();
}

void CallbackState::releasePendingPayloads(std::size_t count)
{
    releaseActive(count);
}

CallbackState::DiagnosticCallbacks CallbackState::takeDiagnosticCallbacks()
{
    auto owner = shared_from_this();
    std::shared_ptr<Diagnostic> info;
    std::shared_ptr<Diagnostic> error;
    std::size_t count = 0;
    {
        std::lock_guard lock(mutex_);
        info = std::move(detached_info_callback_);
        error = std::move(detached_error_callback_);
        count = std::exchange(detached_diagnostic_activity_, 0);
        if (count == 0) {
            info = std::move(info_callback_);
            error = std::move(error_callback_);
            if (info) {
                ++count;
            }
            if (error) {
                ++count;
            }
            reserveActiveLocked(count);
        }
    }

    DiagnosticCallbacks callbacks;
    callbacks.owner_ = std::move(owner);
    callbacks.reserved_activity_ = count;
    callbacks.info = std::move(info);
    callbacks.error = std::move(error);
    return callbacks;
}

void CallbackState::destroyDiagnosticCallbacks(DiagnosticCallbacks& callbacks) noexcept
{
    auto owner = callbacks.owner_;
    if (!owner) {
        callbacks.info.reset();
        callbacks.error.reset();
        callbacks.reserved_activity_ = 0;
        return;
    }

    const auto count = callbacks.reserved_activity_;
    auto destroy_scope = owner->adoptActive(count);
    callbacks.info.reset();
    callbacks.error.reset();
    callbacks.reserved_activity_ = 0;
    callbacks.owner_.reset();
    destroy_scope.release();
}

void CallbackState::reserveProducer()
{
    std::lock_guard lock(mutex_);
    if (phase_ != Phase::Open) {
        return;
    }
    producer_started_ = true;
    producer_done_ = false;
    producer_thread_ = {};
    condition_.notify_all();
}

void CallbackState::setProducerThreadIdentity()
{
    std::lock_guard lock(mutex_);
    if (!producer_started_ || producer_done_) {
        return;
    }
    producer_thread_ = std::this_thread::get_id();
    condition_.notify_all();
}

void CallbackState::rollbackProducerReservation()
{
    std::lock_guard lock(mutex_);
    producer_started_ = false;
    producer_done_ = true;
    producer_thread_ = {};
    condition_.notify_all();
}

void CallbackState::markProducerDone()
{
    std::lock_guard lock(mutex_);
    producer_done_ = true;
    condition_.notify_all();
}

bool CallbackState::waitProducer(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] { return producer_done_; });
}

bool CallbackState::waitQuiescent(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] {
        return active_bodies_ == 0 && pending_work_slots_ == 0;
    });
}

bool CallbackState::waitClosed(std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] { return phase_ == Phase::Closed; });
}

void CallbackState::invokeSlot(std::shared_ptr<WorkSlot> const& slot)
{
    std::function<void()> body;
    std::function<void()> cancelled_payload;
    bool cancelled = false;
    {
        std::lock_guard lock(mutex_);
        if (slot->status != WorkSlot::Status::Pending) {
            return;
        }
        if (phase_ != Phase::Open) {
            cancelled = cancelSlotLocked(slot, cancelled_payload);
        }
        else {
            slot->status = WorkSlot::Status::Claimed;
            --pending_work_slots_;
            ++active_bodies_;
            body = std::move(slot->body);
            condition_.notify_all();
        }
    }

    if (cancelled) {
        auto payload_scope = adoptActive(1);
        cancelled_payload = {};
        payload_scope.release();
        return;
    }

    auto body_scope = adoptActive(1);
    std::exception_ptr exception;
    try {
        if (body) {
            body();
        }
    }
    catch (...) {
        exception = std::current_exception();
    }
    body = {};
    try {
        if (exception) {
            reportException(exception);
        }
    }
    catch (...) {
        // Keep teardown accounting intact if a diagnostic callback throws.
    }
    exception = {};
    body_scope.release();
}

void CallbackState::observeTick()
{
    std::lock_guard lock(mutex_);
    if (phase_ == Phase::Open) {
        ++raw_tick_observations_;
    }
}

void CallbackState::markClosed()
{
    std::lock_guard lock(mutex_);
    phase_ = Phase::Closed;
    condition_.notify_all();
}

void CallbackState::report(std::shared_ptr<Diagnostic> callback, std::string message) noexcept
{
    if (!callback) {
        return;
    }
    auto report_scope = adoptActive(1);
    try {
        (*callback)(message);
    }
    catch (...) {
    }
    callback.reset();
    report_scope.release();
}

void CallbackState::reportInfo(std::string message) noexcept
{
    std::shared_ptr<Diagnostic> callback;
    {
        std::lock_guard lock(mutex_);
        callback = info_callback_;
        if (callback) {
            reserveActiveLocked(1);
        }
    }
    report(std::move(callback), std::move(message));
}

void CallbackState::reportError(std::string message) noexcept
{
    std::shared_ptr<Diagnostic> callback;
    {
        std::lock_guard lock(mutex_);
        callback = error_callback_;
        if (callback) {
            reserveActiveLocked(1);
        }
    }
    report(std::move(callback), std::move(message));
}

void CallbackState::reportException(std::exception_ptr exception) noexcept
{
    if (!exception) {
        return;
    }
    try {
        std::rethrow_exception(exception);
    }
    catch (std::exception const& error) {
        reportError(std::string{"dispatcher body failed: "} + error.what());
    }
    catch (...) {
        reportError("dispatcher body failed with an unknown exception");
    }
}

void CallbackState::failFatal(FatalReason reason)
{
    std::shared_ptr<FatalHandler> handler;
    {
        std::lock_guard lock(mutex_);
        handler = fatal_handler_;
        if (handler) {
            reserveActiveLocked(1);
        }
    }
    if (!handler) {
        return;
    }

    auto fatal_scope = adoptActive(1);
    try {
        (*handler)(reason);
    }
    catch (...) {
    }
    handler.reset();
    fatal_scope.release();
}

bool CallbackState::producerStopRequested() const
{
    std::lock_guard lock(mutex_);
    return producer_stop_requested_;
}

bool CallbackState::isProducerThread() const
{
    std::lock_guard lock(mutex_);
    return producer_started_ && producer_thread_ == std::this_thread::get_id();
}

bool CallbackState::isInBodyOnCurrentThread() const { return isActiveOnCurrentThread(); }

CallbackState::Phase CallbackState::phase() const
{
    std::lock_guard lock(mutex_);
    return phase_;
}

std::uint64_t CallbackState::rawTickObservations() const
{
    std::lock_guard lock(mutex_);
    return raw_tick_observations_;
}

std::uint64_t CallbackState::activeBodies() const
{
    std::lock_guard lock(mutex_);
    return active_bodies_;
}

std::uint64_t CallbackState::pendingWorkSlots() const
{
    std::lock_guard lock(mutex_);
    return pending_work_slots_;
}

CallbackState::ActivityScope CallbackState::adoptActive(std::size_t count) noexcept
{
    return ActivityScope{this, count};
}

void CallbackState::releaseActive(std::size_t count) noexcept
{
    std::lock_guard lock(mutex_);
    if (count >= active_bodies_) {
        active_bodies_ = 0;
    }
    else {
        active_bodies_ -= count;
    }
    condition_.notify_all();
}

void CallbackState::reserveActiveLocked(std::size_t count) noexcept
{
    active_bodies_ += static_cast<std::uint64_t>(count);
}

void CallbackState::destroySubmitter(std::shared_ptr<Submitter>& submitter, bool reserved) noexcept
{
    if (!submitter) {
        return;
    }
    if (!reserved) {
        std::lock_guard lock(mutex_);
        reserveActiveLocked(1);
    }
    auto destroy_scope = adoptActive(1);
    submitter.reset();
    destroy_scope.release();
}

void CallbackState::destroyDiagnostic(std::shared_ptr<Diagnostic>& callback, bool reserved) noexcept
{
    if (!callback) {
        return;
    }
    if (!reserved) {
        std::lock_guard lock(mutex_);
        reserveActiveLocked(1);
    }
    auto destroy_scope = adoptActive(1);
    callback.reset();
    destroy_scope.release();
}

void CallbackState::destroyFatalHandler(std::shared_ptr<FatalHandler>& handler, bool reserved) noexcept
{
    if (!handler) {
        return;
    }
    if (!reserved) {
        std::lock_guard lock(mutex_);
        reserveActiveLocked(1);
    }
    auto destroy_scope = adoptActive(1);
    handler.reset();
    destroy_scope.release();
}

bool CallbackState::cancelSlotLocked(std::shared_ptr<WorkSlot> const& slot, std::function<void()>& payload)
{
    if (slot->status != WorkSlot::Status::Pending) {
        return false;
    }
    slot->status = WorkSlot::Status::Cancelled;
    --pending_work_slots_;
    ++active_bodies_;
    payload = std::move(slot->body);
    condition_.notify_all();
    return true;
}

void CallbackState::pushTls(TlsFrame& frame) noexcept
{
    frame.previous = tlsTop();
    tlsTop() = &frame;
}

void CallbackState::popTls(TlsFrame& frame) noexcept
{
    if (tlsTop() == &frame) {
        tlsTop() = frame.previous;
    }
    else {
        auto* current = tlsTop();
        while (current != nullptr && current->previous != &frame) {
            current = current->previous;
        }
        if (current != nullptr) {
            current->previous = frame.previous;
        }
    }
    frame.previous = nullptr;
    frame.state = nullptr;
}

bool CallbackState::isActiveOnCurrentThread() const noexcept
{
    for (auto* frame = tlsTop(); frame != nullptr; frame = frame->previous) {
        if (frame->state == this) {
            return true;
        }
    }
    return false;
}

CallbackState::TlsFrame*& CallbackState::tlsTop() noexcept
{
    thread_local TlsFrame* top = nullptr;
    return top;
}

}  // namespace spark::levilamina

#include <algorithm>
#include <chrono>
#include <exception>
#include <initializer_list>
#include <limits>
#include <utility>

#include "core/util/base64.h"
#include "core/util/monotonic_time.h"
#include "core/ws/viewer_socket.h"
#include "proto/sampler_data.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

std::size_t accountedBytes(std::initializer_list<std::size_t> sizes)
{
    std::size_t total = 256;
    for (const std::size_t size : sizes) {
        if (size > std::numeric_limits<std::size_t>::max() - total) {
            return std::numeric_limits<std::size_t>::max();
        }
        total += size;
    }
    return total;
}

constexpr auto KOpenCleanupBudget = std::chrono::milliseconds(500);

}  // namespace

ViewerSocket::ViewerSocket(Config config, Crypto::KeyPair key_pair)
    : config_(std::move(config)), key_pair_(std::move(key_pair))
{
}

ViewerSocket::~ViewerSocket()
{
    if (!closeWithin(std::chrono::seconds(2))) {
        std::terminate();
    }
}

SocketChannelInfo ViewerSocket::channelInfo() const
{
    SocketChannelInfo info;
    info.channel_id = channel_id_;
    info.public_key = key_pair_.public_key_x509;
    return info;
}

std::string ViewerSocket::open(const UploadCallback &upload, const CancellationToken &cancellation)
{
    std::scoped_lock open_lock(open_mutex_);
    if (cancellation.stopRequested()) {
        return {};
    }
    std::uint64_t generation = 0;
    bool generation_started = false;
    try {
        std::string channel_id;
        {
            std::scoped_lock transport_lock(transport_mutex_);
            if (ws_) {
                if (!ws_->closeWithin(KOpenCleanupBudget)) {
                    return {};
                }
                ws_.reset();
            }
            generation = prepareOpen();
            generation_started = true;
            ws_ = std::make_unique<WebSocketClient>();
            if (!ws_->setLocalCloseMessage(encodeServerClose(key_pair_.private_key_pkcs8))) {
                state_.store(ConnectionState::Closed, std::memory_order_release);
                return {};
            }
            if (transport_created_for_testing_) {
                transport_created_for_testing_(*ws_);
            }
            ws_->setMessageCallback([this](const std::string &data) { onMessage(data); });
            ws_->setCloseCallback([this, generation](const WebSocketClient::Termination &termination) {
                onTransportClosed(generation, termination);
            });

            channel_id_ = ws_->connect(config_.bytesocks_host, config_.user_agent, cancellation);
            if (channel_id_.empty()) {
                state_.store(ConnectionState::Closed, std::memory_order_release);
                return {};
            }
            channel_id = channel_id_;
        }

        if (cancellation.stopRequested()) {
            requestStop();
            return {};
        }

        // Build SocketChannelInfo proto.
        SocketChannelInfo info;
        info.channel_id = std::move(channel_id);
        info.public_key = key_pair_.public_key_x509;
        std::string channel_info_proto = encodeSocketChannelInfo(info);

        // Upload initial sampler data.
        std::string bytebin_key = upload(channel_info_proto);

        {
            std::scoped_lock transport_lock(transport_mutex_);
            if (generation != connection_generation_.load(std::memory_order_acquire) || cancellation.stopRequested()) {
                if (ws_) {
                    ws_->requestStop();
                }
                state_.store(ConnectionState::Closed, std::memory_order_release);
                return {};
            }
            if (bytebin_key.empty() || state_.load(std::memory_order_acquire) != ConnectionState::Opening || !ws_ ||
                !ws_->isOpen()) {
                if (ws_) {
                    ws_->closeWithin(KOpenCleanupBudget);
                }
                state_.store(ConnectionState::Closed, std::memory_order_release);
                return {};
            }

            {
                std::scoped_lock lock(payload_mutex_);
                last_payload_id_ = bytebin_key;
            }
            ConnectionState expected = ConnectionState::Opening;
            if (!state_.compare_exchange_strong(expected, ConnectionState::Open, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                ws_->closeWithin(KOpenCleanupBudget);
                state_.store(ConnectionState::Closed, std::memory_order_release);
                return {};
            }
        }
        return config_.viewer_url + bytebin_key;
    }
    catch (...) {
        try {
            std::scoped_lock transport_lock(transport_mutex_);
            if (generation_started && generation == connection_generation_.load(std::memory_order_acquire)) {
                if (ws_) {
                    ws_->closeWithin(KOpenCleanupBudget);
                }
                state_.store(ConnectionState::Closed, std::memory_order_release);
            }
        }
        catch (...) {
            return {};
        }
        return {};
    }
}

std::uint64_t ViewerSocket::prepareOpen()
{
    std::uint64_t generation = 0;
    {
        std::scoped_lock lock(close_mutex_);
        generation = connection_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        state_.store(ConnectionState::Opening, std::memory_order_release);
        close_reason_ = CloseReason::None;
        close_diagnostic_.clear();
    }
    open_time_ms_ = nowMs();
    last_ping_ms_.store(0);
    channel_id_.clear();
    {
        std::scoped_lock lock(payload_mutex_);
        last_payload_id_.clear();
    }
    {
        std::scoped_lock lock(queue_mutex_);
        incoming_queue_.clear();
    }
    incoming_overflow_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(pending_keys_mutex_);
        pending_keys_.clear();
        conflicted_client_ids_.clear();
        all_client_ids_conflicted_ = false;
    }
    return generation;
}

void ViewerSocket::requestStop() noexcept
{
    state_.store(ConnectionState::Closed, std::memory_order_release);
    connection_generation_.fetch_add(1, std::memory_order_acq_rel);
    try {
        std::unique_lock transport_lock(transport_mutex_, std::try_to_lock);
        if (transport_lock.owns_lock() && ws_) {
            ws_->requestStop();
        }
        std::unique_lock lock(close_mutex_, std::try_to_lock);
        if (lock.owns_lock() && close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::LocalClose;
        }
    }
    catch (...) {
        state_.store(ConnectionState::Closed, std::memory_order_release);
    }
}

bool ViewerSocket::closeWithin(std::chrono::milliseconds timeout) noexcept
{
    return closeUntil(std::chrono::steady_clock::now() + std::max(timeout, std::chrono::milliseconds::zero()));
}

bool ViewerSocket::closeUntil(std::chrono::steady_clock::time_point deadline) noexcept
{
    requestStop();
    try {
        std::unique_lock open_lock(open_mutex_, std::defer_lock);
        if (!open_lock.try_lock() && !open_lock.try_lock_until(deadline)) {
            return false;
        }
        std::unique_lock transport_lock(transport_mutex_, std::defer_lock);
        if (!transport_lock.try_lock() && !transport_lock.try_lock_until(deadline)) {
            return false;
        }
        if (!ws_) {
            return true;
        }
        if (!ws_->closeUntil(deadline)) {
            return false;
        }
        ws_.reset();
        return true;
    }
    catch (...) {
        return false;
    }
}

void ViewerSocket::close() noexcept
{
    static_cast<void>(closeWithin(std::chrono::seconds(2)));
}

ViewerSocket::CloseReason ViewerSocket::closeReason() const
{
    std::scoped_lock lock(close_mutex_);
    return close_reason_;
}

std::string ViewerSocket::takeDiagnostic()
{
    std::scoped_lock lock(close_mutex_);
    return std::exchange(close_diagnostic_, {});
}

void ViewerSocket::setCloseState(CloseReason reason, std::string diagnostic)
{
    std::scoped_lock lock(close_mutex_);
    if (close_reason_ != CloseReason::None) {
        return;
    }
    close_reason_ = reason;
    close_diagnostic_ = std::move(diagnostic);
}

void ViewerSocket::onTransportClosed(std::uint64_t generation, const WebSocketClient::Termination &termination)
{
    std::scoped_lock lock(close_mutex_);
    if (generation != connection_generation_.load(std::memory_order_acquire)) {
        return;
    }

    switch (termination.kind) {
    case WebSocketClient::TerminationKind::LocalClose:
        if (close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::LocalClose;
            close_diagnostic_.clear();
        }
        break;
    case WebSocketClient::TerminationKind::RemoteClose:
        if (close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::RemoteClose;
            close_diagnostic_ = "Live viewer closed: remote endpoint closed the connection";
        }
        break;
    case WebSocketClient::TerminationKind::SendError:
        if (close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::SendError;
            close_diagnostic_ = "Live viewer transport failed: send error: " + termination.detail;
        }
        break;
    case WebSocketClient::TerminationKind::ReceiveError:
        if (close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::ReceiveError;
            close_diagnostic_ = "Live viewer transport failed: receive error: " + termination.detail;
        }
        break;
    case WebSocketClient::TerminationKind::WorkerFailure:
        if (close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::WorkerFailure;
            close_diagnostic_ = "Live viewer transport failed: worker failure: " + termination.detail;
        }
        break;
    case WebSocketClient::TerminationKind::None:
        if (close_reason_ == CloseReason::None) {
            close_reason_ = CloseReason::WorkerFailure;
            close_diagnostic_ = "Live viewer transport failed: worker stopped unexpectedly";
        }
        break;
    }
    state_.store(ConnectionState::Closed, std::memory_order_release);
}

}  // namespace spark

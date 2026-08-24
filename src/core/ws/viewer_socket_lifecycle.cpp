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

}  // namespace

ViewerSocket::ViewerSocket(Config config, Crypto::KeyPair key_pair)
    : config_(std::move(config)), key_pair_(std::move(key_pair))
{
}

ViewerSocket::~ViewerSocket()
{
    close();
}

SocketChannelInfo ViewerSocket::channelInfo() const
{
    SocketChannelInfo info;
    info.channel_id = channel_id_;
    info.public_key = key_pair_.public_key_x509;
    return info;
}

std::string ViewerSocket::open(const UploadCallback &upload)
{
    try {
        std::scoped_lock transport_lock(transport_mutex_);
        if (ws_) {
            ws_->close();
            ws_.reset();
        }
        prepareOpen();
        ws_ = std::make_unique<WebSocketClient>();
        ws_->setMessageCallback([this](const std::string &data) { onMessage(data); });
        ws_->setCloseCallback(
            [this](const WebSocketClient::Termination &termination) { onTransportClosed(termination); });

        channel_id_ = ws_->connect(config_.bytesocks_host, config_.user_agent);
        if (channel_id_.empty()) {
            return {};
        }

        // Build SocketChannelInfo proto.
        SocketChannelInfo info;
        info.channel_id = channel_id_;
        info.public_key = key_pair_.public_key_x509;
        std::string channel_info_proto = encodeSocketChannelInfo(info);

        // Upload initial sampler data.
        std::string bytebin_key = upload(channel_info_proto);
        if (bytebin_key.empty()) {
            ws_->close();
            return {};
        }
        if (!ws_->isOpen()) {
            ws_->close();
            return {};
        }

        {
            std::scoped_lock lock(payload_mutex_);
            last_payload_id_ = bytebin_key;
        }
        open_.store(true);
        return config_.viewer_url + bytebin_key;
    }
    catch (...) {
        open_.store(false);
        try {
            std::scoped_lock transport_lock(transport_mutex_);
            if (ws_) {
                ws_->close();
            }
        }
        catch (...) {
            open_.store(false);
        }
        return {};
    }
}

void ViewerSocket::prepareOpen()
{
    open_.store(false);
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
    }
    {
        std::scoped_lock lock(close_mutex_);
        close_reason_ = CloseReason::None;
        close_diagnostic_.clear();
    }
}

void ViewerSocket::close() noexcept
{
    try {
        std::scoped_lock transport_lock(transport_mutex_);
        const bool was_open = open_.exchange(false);
        try {
            setCloseState(CloseReason::LocalClose);
        }
        catch (...) {
            open_.store(false);
        }
        if (ws_) {
            if (was_open && ws_->isOpen()) {
                try {
                    const auto private_key = key_pair_.private_key_pkcs8;
                    WebSocketClient::DeferredEncoder encoder = [private_key]() {
                        return encodeServerClose(private_key);
                    };
                    enqueueDeferredLocked(std::move(encoder), accountedBytes({private_key.size()}));
                }
                catch (...) {
                    setDeferredSendError();
                }
            }
            ws_->close();
        }
    }
    catch (...) {
        open_.store(false);
    }
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

void ViewerSocket::onTransportClosed(const WebSocketClient::Termination &termination)
{
    switch (termination.kind) {
    case WebSocketClient::TerminationKind::LocalClose:
        setCloseState(CloseReason::LocalClose);
        break;
    case WebSocketClient::TerminationKind::RemoteClose:
        setCloseState(CloseReason::RemoteClose, "Live viewer closed: remote endpoint closed the connection");
        break;
    case WebSocketClient::TerminationKind::SendError:
        setCloseState(CloseReason::SendError, "Live viewer transport failed: send error: " + termination.detail);
        break;
    case WebSocketClient::TerminationKind::ReceiveError:
        setCloseState(CloseReason::ReceiveError, "Live viewer transport failed: receive error: " + termination.detail);
        break;
    case WebSocketClient::TerminationKind::WorkerFailure:
        setCloseState(CloseReason::WorkerFailure,
                      "Live viewer transport failed: worker failure: " + termination.detail);
        break;
    case WebSocketClient::TerminationKind::None:
        setCloseState(CloseReason::WorkerFailure, "Live viewer transport failed: worker stopped unexpectedly");
        break;
    }
    open_.store(false);
}

}  // namespace spark

#include "core/ws/viewer_socket.h"

#include <initializer_list>
#include <limits>
#include <utility>

#include "core/util/base64.h"
#include "core/util/monotonic_time.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "proto/sampler_data.h"
#include "spark_constants.h"

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

void ViewerSocket::processWindowRotate(const UploadCallback &upload)
{
    {
        std::scoped_lock transport_lock(transport_mutex_);
        if (!open_.load() || !ws_ || !ws_->isOpen()) {
            return;
        }
    }

    auto time = nowMs();
    if ((time - open_time_ms_) > kInitialTimeoutMs && (time - last_ping_ms_.load()) > kEstablishedTimeoutMs) {
        setCloseState(CloseReason::ClientPingTimeout, "Live viewer closed: client ping timeout");
        close();
        return;
    }

    // No clients connected yet.
    if (last_ping_ms_.load() == 0) {
        return;
    }

    std::string bytebin_key = upload(std::string());
    if (bytebin_key.empty()) {
        return;
    }

    sendUpdate(bytebin_key);
}

void ViewerSocket::sendUpdate(const std::string &bytebin_key)
{
    try {
        std::scoped_lock transport_lock(transport_mutex_);
        if (!open_.load() || !ws_ || !ws_->isOpen()) {
            return;
        }
        {
            std::scoped_lock lock(payload_mutex_);
            last_payload_id_ = bytebin_key;
        }
        const auto private_key = key_pair_.private_key_pkcs8;
        WebSocketClient::DeferredEncoder encoder = [payload_id = bytebin_key, private_key]() {
            return encodeServerUpdateSamplerData(payload_id, private_key);
        };
        enqueueDeferredLocked(std::move(encoder), accountedBytes({bytebin_key.size(), private_key.size()}));
    }
    catch (...) {
        setDeferredSendError();
    }
}

bool ViewerSocket::sendStatistics(const std::string &platform, const std::string &system, const std::string &metrics)
{
    try {
        std::scoped_lock transport_lock(transport_mutex_);
        if (!open_.load(std::memory_order_acquire) || !hasClient() || !ws_ || !ws_->isOpen()) {
            return false;
        }
        const auto private_key = key_pair_.private_key_pkcs8;
        WebSocketClient::DeferredEncoder encoder = [platform, system, metrics, private_key]() {
            return encodeServerUpdateStatistics(platform, system, metrics, private_key);
        };
        return enqueueDeferredLocked(
            std::move(encoder), accountedBytes({platform.size(), system.size(), metrics.size(), private_key.size()}));
    }
    catch (...) {
        setDeferredSendError();
        return false;
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

bool ViewerSocket::tick()
{
    try {
        if (!open_.load()) {
            return false;
        }
        bool transport_closed = false;
        WebSocketClient::Termination termination;
        {
            std::scoped_lock transport_lock(transport_mutex_);
            if (!ws_ || !ws_->isOpen()) {
                transport_closed = true;
                if (ws_) {
                    termination = ws_->termination();
                }
            }
        }
        if (transport_closed) {
            onTransportClosed(termination);
            return false;
        }

        if (incoming_overflow_.exchange(false, std::memory_order_acq_rel)) {
            setCloseState(CloseReason::ReceiveError, "Live viewer closed: incoming message queue exceeded its limit");
            close();
            return false;
        }

        // Process packets parsed and verified by the transport worker.
        std::vector<WsIncomingPacket> packets;
        {
            std::scoped_lock lock(queue_mutex_);
            packets.swap(incoming_queue_);
        }
        for (const auto &packet : packets) {
            switch (packet.type) {
            case WsPacketType::ClientPing:
                last_ping_ms_.store(nowMs());
                {
                    const bool ok = open_.load();
                    const std::int32_t data = packet.ping.data;
                    const auto private_key = key_pair_.private_key_pkcs8;
                    WebSocketClient::DeferredEncoder encoder = [ok, data, private_key]() {
                        return encodeServerPong(ok, data, private_key);
                    };
                    enqueueDeferred(std::move(encoder), accountedBytes({private_key.size()}));
                }
                break;
            case WsPacketType::ClientConnect: {
                last_ping_ms_.store(nowMs());
                const bool trusted = isTrustedClient(packet);
                if (packet.verified && !packet.public_key.empty()) {
                    std::scoped_lock lock(pending_keys_mutex_);
                    const auto existing = pending_keys_.find(packet.connect.client_id);
                    if (existing != pending_keys_.end()) {
                        existing->second = packet.public_key;
                    }
                    else if (pending_keys_.size() < kMaxPendingKeys) {
                        pending_keys_.emplace(packet.connect.client_id, packet.public_key);
                    }
                }
                int state = trusted ? 0 : 1;  // 0=ACCEPTED, 1=UNTRUSTED
                std::string payload_id;
                {
                    std::scoped_lock lock(payload_mutex_);
                    payload_id = last_payload_id_;
                }
                const std::string client_id = packet.connect.client_id;
                const int sampler_interval = config_.sampler_interval;
                const int statistics_interval = config_.statistics_interval;
                const auto private_key = key_pair_.private_key_pkcs8;
                WebSocketClient::DeferredEncoder encoder = [client_id, state, sampler_interval, statistics_interval,
                                                            payload_id, private_key]() {
                    return encodeServerConnectResponse(client_id, state, sampler_interval, statistics_interval,
                                                       payload_id, private_key);
                };
                enqueueDeferred(std::move(encoder),
                                accountedBytes({client_id.size(), payload_id.size(), private_key.size()}));
                break;
            }
            default:
                break;
            }
        }

        // Check timeout.
        auto time = nowMs();
        if ((time - open_time_ms_) > kInitialTimeoutMs && (time - last_ping_ms_.load()) > kEstablishedTimeoutMs) {
            setCloseState(CloseReason::ClientPingTimeout, "Live viewer closed: client ping timeout");
            close();
            return false;
        }

        return true;
    }
    catch (...) {
        setDeferredSendError();
        close();
        return false;
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

bool ViewerSocket::enqueueDeferredLocked(WebSocketClient::DeferredEncoder encoder,
                                         std::size_t accounted_input_bytes) noexcept
{
    try {
        if (!ws_ || !ws_->isOpen()) {
            return false;
        }
        return ws_->sendDeferred(std::move(encoder), accounted_input_bytes);
    }
    catch (...) {
        setDeferredSendError();
        return false;
    }
}

bool ViewerSocket::enqueueDeferred(WebSocketClient::DeferredEncoder encoder, std::size_t accounted_input_bytes) noexcept
{
    try {
        std::scoped_lock transport_lock(transport_mutex_);
        return enqueueDeferredLocked(std::move(encoder), accounted_input_bytes);
    }
    catch (...) {
        setDeferredSendError();
        return false;
    }
}

void ViewerSocket::setDeferredSendError() noexcept
{
    try {
        setCloseState(CloseReason::SendError, "Live viewer transport failed: deferred send enqueue");
    }
    catch (...) {
        open_.store(false);
    }
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

void ViewerSocket::onMessage(const std::string &data)
{
    WsIncomingPacket packet;
    if (!decodeRawPacket(data, packet)) {
        return;
    }
    std::scoped_lock lock(queue_mutex_);
    if (incoming_queue_.size() >= kMaxQueuedPackets) {
        incoming_overflow_.store(true, std::memory_order_release);
        return;
    }
    incoming_queue_.push_back(std::move(packet));
}

bool ViewerSocket::isTrustedClient(const WsIncomingPacket &packet) const
{
    return packet.type == WsPacketType::ClientConnect && packet.verified && !packet.public_key.empty() &&
           is_key_trusted_ && is_key_trusted_(packet.public_key);
}

std::vector<std::uint8_t> ViewerSocket::pendingKey(const std::string &client_id) const
{
    std::scoped_lock lock(pending_keys_mutex_);
    auto it = pending_keys_.find(client_id);
    if (it == pending_keys_.end()) {
        return {};
    }
    return it->second;
}

void ViewerSocket::sendClientTrusted(const std::string &client_id)
{
    try {
        std::vector<std::uint8_t> pending_key;
        {
            std::scoped_lock lock(pending_keys_mutex_);
            const auto pending = pending_keys_.find(client_id);
            if (pending == pending_keys_.end()) {
                return;
            }
            pending_key = pending->second;
        }
        if (!is_key_trusted_ || !is_key_trusted_(pending_key)) {
            return;
        }
        std::string payload_id;
        {
            std::scoped_lock lock(payload_mutex_);
            payload_id = last_payload_id_;
        }
        const int sampler_interval = config_.sampler_interval;
        const int statistics_interval = config_.statistics_interval;
        const auto private_key = key_pair_.private_key_pkcs8;
        WebSocketClient::DeferredEncoder encoder = [client_id, sampler_interval, statistics_interval, payload_id,
                                                    private_key]() {
            return encodeServerConnectResponse(client_id, 0,  // 0=ACCEPTED
                                               sampler_interval, statistics_interval, payload_id, private_key);
        };
        enqueueDeferred(std::move(encoder), accountedBytes({client_id.size(), payload_id.size(), private_key.size()}));
    }
    catch (...) {
        setDeferredSendError();
    }
}

}  // namespace spark

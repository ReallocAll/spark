#include <initializer_list>
#include <limits>
#include <utility>

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

bool ViewerSocket::tick()
{
    try {
        if (!isOpen()) {
            return false;
        }
        bool transport_closed = false;
        std::uint64_t generation = 0;
        WebSocketClient::Termination termination;
        {
            std::scoped_lock transport_lock(transport_mutex_);
            generation = connection_generation_.load(std::memory_order_acquire);
            if (!ws_ || !ws_->isOpen()) {
                transport_closed = true;
                if (ws_) {
                    termination = ws_->termination();
                }
            }
        }
        if (transport_closed) {
            onTransportClosed(generation, termination);
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
                    const bool ok = isOpen();
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
                    const std::string &client_id = packet.connect.client_id;
                    if (all_client_ids_conflicted_ || conflicted_client_ids_.contains(client_id)) {
                        pending_keys_.erase(client_id);
                    }
                    else {
                        const auto existing = pending_keys_.find(client_id);
                        if (existing != pending_keys_.end()) {
                            if (existing->second != packet.public_key) {
                                pending_keys_.erase(existing);
                                if (conflicted_client_ids_.size() < kMaxConflictedClientIds) {
                                    conflicted_client_ids_.insert(client_id);
                                }
                                else {
                                    all_client_ids_conflicted_ = true;
                                    pending_keys_.clear();
                                }
                            }
                        }
                        else if (!all_client_ids_conflicted_ && pending_keys_.size() < kMaxPendingKeys) {
                            pending_keys_.emplace(client_id, packet.public_key);
                        }
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
            if (all_client_ids_conflicted_ || conflicted_client_ids_.contains(client_id)) {
                return;
            }
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

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

void ViewerSocket::processWindowRotate(const UploadCallback &upload)
{
    {
        std::scoped_lock transport_lock(transport_mutex_);
        if (!isOpen() || !ws_ || !ws_->isOpen()) {
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
        if (!isOpen() || !ws_ || !ws_->isOpen()) {
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
        if (!isOpen() || !hasClient() || !ws_ || !ws_->isOpen()) {
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
        state_.store(ConnectionState::Closed, std::memory_order_release);
    }
}

}  // namespace spark

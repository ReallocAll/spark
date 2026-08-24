#ifndef ENDSTONE_SPARK_VIEWER_SOCKET_H
#define ENDSTONE_SPARK_VIEWER_SOCKET_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/ws/crypto.h"
#include "core/ws/ws_proto.h"
#include "net/websocket.h"

namespace spark {

struct ViewerSocketTestAccess;

// Manages a live viewer WebSocket connection and pushes payload IDs on window rotation.
class ViewerSocket {
public:
    enum class CloseReason {
        None,
        LocalClose,
        RemoteClose,
        SendError,
        ReceiveError,
        WorkerFailure,
        ClientPingTimeout
    };

    // Called to produce sampler data and upload it to bytebin.
    // The channel_info_proto is non-empty only for the initial upload;
    // window rotates pass an empty string. Returns the bytebin key.
    using UploadCallback = std::function<std::string(const std::string &channel_info_proto)>;

    struct Config {
        std::string bytesocks_host;
        std::string bytebin_url;
        std::string viewer_url;
        std::string user_agent = "spark-plugin";
        int sampler_interval = 10;
        int statistics_interval = 10;
    };

    ViewerSocket(Config config, Crypto::KeyPair key_pair);
    ~ViewerSocket();

    // Create the WebSocket channel, connect, and upload initial data.
    // Returns the viewer URL on success, or empty string on failure.
    std::string open(const UploadCallback &upload);

    // Uploads on the caller's thread and sends the new payload ID.
    void processWindowRotate(const UploadCallback &upload);

    // Send a pre-uploaded payload ID to all connected viewers.  Thread-safe
    // against tick(); designed to be called from a worker thread after the
    // upload completes.
    void sendUpdate(const std::string &bytebin_key);

    // Send signed, already serialized platform/system/metrics statistics to
    // a connected viewer. Thread-safe against tick().
    bool sendStatistics(const std::string &platform, const std::string &system, const std::string &metrics);

    // Close the socket and signal the viewer.
    void close() noexcept;

    bool isOpen() const { return state_.load(std::memory_order_acquire) == ConnectionState::Open; }
    CloseReason closeReason() const;
    std::string takeDiagnostic();

    // Get the SocketChannelInfo for embedding in the initial sampler data upload.
    SocketChannelInfo channelInfo() const;

    // Called on each tick to check timeouts and process queued messages.
    // Returns false if the socket should be closed due to timeout.
    bool tick();

    // Returns true after the viewer has sent its first ping.
    bool hasClient() const { return last_ping_ms_.load(std::memory_order_acquire) != 0; }

    // Sets the trusted-keys check callback for live viewer authentication.
    using IsKeyTrustedCallback = std::function<bool(const std::vector<std::uint8_t> &)>;
    void setIsKeyTrustedCallback(IsKeyTrustedCallback cb) { is_key_trusted_ = std::move(cb); }

    // Get the pending public key for a client, or empty if not found.
    std::vector<std::uint8_t> pendingKey(const std::string &client_id) const;

    // Re-send a connect response with state=ACCEPTED for a previously-untrusted client.
    void sendClientTrusted(const std::string &client_id);

private:
    friend struct ViewerSocketTestAccess;

    enum class ConnectionState : std::uint8_t {
        Closed,
        Opening,
        Open,
    };

    void onMessage(const std::string &data);
    std::uint64_t prepareOpen();
    void onTransportClosed(std::uint64_t generation, const WebSocketClient::Termination &termination);
    void setCloseState(CloseReason reason, std::string diagnostic = {});
    [[nodiscard]] bool isTrustedClient(const WsIncomingPacket &packet) const;
    bool enqueueDeferredLocked(WebSocketClient::DeferredEncoder encoder, std::size_t accounted_input_bytes) noexcept;
    bool enqueueDeferred(WebSocketClient::DeferredEncoder encoder, std::size_t accounted_input_bytes) noexcept;
    void setDeferredSendError() noexcept;

    Config config_;
    Crypto::KeyPair key_pair_;
    std::mutex open_mutex_;
    mutable std::mutex transport_mutex_;
    std::unique_ptr<WebSocketClient> ws_;

    std::atomic<ConnectionState> state_{ConnectionState::Closed};
    std::atomic<std::uint64_t> connection_generation_{0};
    std::int64_t open_time_ms_ = 0;
    std::atomic<std::int64_t> last_ping_ms_{0};
    std::string last_payload_id_;
    std::mutex payload_mutex_;  // protects last_payload_id_ across threads
    std::string channel_id_;

    IsKeyTrustedCallback is_key_trusted_;

    // Pending client keys awaiting trust approval.
    mutable std::mutex pending_keys_mutex_;
    std::map<std::string, std::vector<std::uint8_t>> pending_keys_;

    // Incoming messages are queued for processing on the main thread.
    std::mutex queue_mutex_;
    std::vector<WsIncomingPacket> incoming_queue_;
    std::atomic<bool> incoming_overflow_{false};

    mutable std::mutex close_mutex_;
    CloseReason close_reason_ = CloseReason::None;
    std::string close_diagnostic_;

    static constexpr std::int64_t kInitialTimeoutMs = 60000;      // 60s
    static constexpr std::int64_t kEstablishedTimeoutMs = 30000;  // 30s
    static constexpr std::size_t kMaxQueuedPackets = 64;
    static constexpr std::size_t kMaxPendingKeys = 64;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_VIEWER_SOCKET_H

#ifndef ENDSTONE_SPARK_WEBSOCKET_LIFECYCLE_TEST_SUPPORT_H
#define ENDSTONE_SPARK_WEBSOCKET_LIFECYCLE_TEST_SUPPORT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "core/ws/crypto.h"
#include "core/ws/viewer_socket.h"
#include "net/websocket.h"

namespace spark {

struct WebSocketClientTestAccess {
    enum class SendStep {
        Idle,
        Progress,
        Retry,
        Fatal
    };

    static void startExitedWorker(WebSocketClient &client, std::mutex &mutex, std::condition_variable &cv, bool &exited)
    {
        client.running_.store(true);
        client.thread_ = std::thread([&client, &mutex, &cv, &exited]() {
            client.running_.store(false);
            {
                std::scoped_lock lock(mutex);
                exited = true;
            }
            cv.notify_one();
        });
    }

    static bool joinable(const WebSocketClient &client) { return client.thread_.joinable(); }

    static bool running(const WebSocketClient &client) { return client.running_.load(); }

    static bool startThrowingCallbackWorker(WebSocketClient &client, std::atomic<std::uint64_t> &cleanup_count)
    {
        client.host_ = "localhost";
        client.channel_id_ = "test";
        client.user_agent_ = "test";
        client.incoming_message_for_testing_ = "message";
        client.resource_cleanup_count_for_testing_ = &cleanup_count;
        client.message_cb_ = [](const std::string &) {
            throw std::runtime_error("injected");
        };
        client.local_close_requested_.store(false);
        client.running_.store(true);
        return client.startReceiveWorker();
    }

    static void startLocalCloseWorker(WebSocketClient &client, std::atomic<bool> &close_attempted)
    {
        client.running_.store(true);
        client.local_close_requested_.store(false);
        client.thread_ = std::thread([&client, &close_attempted] {
            while (client.running_.load()) {
                std::this_thread::yield();
            }
            close_attempted.store(client.local_close_requested_.exchange(false));
        });
    }

    static void enqueue(WebSocketClient &client, const std::string &message)
    {
        client.running_.store(true);
        client.send(message);
    }

    static SendStep processSend(WebSocketClient &client,
                                const std::function<std::pair<int, std::size_t>(std::string_view)> &send_function)
    {
        const auto step = client.processNextSend([&send_function](const char *data, std::size_t size) {
            const auto [code, sent] = send_function(std::string_view(data, size));
            return WebSocketClient::SendAttempt{.code = code, .sent = sent};
        });
        switch (step) {
        case WebSocketClient::SendStep::Idle:
            return SendStep::Idle;
        case WebSocketClient::SendStep::Progress:
            return SendStep::Progress;
        case WebSocketClient::SendStep::Retry:
            return SendStep::Retry;
        case WebSocketClient::SendStep::Fatal:
            return SendStep::Fatal;
        }
        return SendStep::Fatal;
    }

    static std::string pendingSend(const WebSocketClient &client)
    {
        return client.pending_send_.value_or(std::string()).substr(client.pending_send_offset_);
    }

    static std::size_t queued(const WebSocketClient &client) { return client.send_queue_.size(); }

    static void receiveFailure(WebSocketClient &client, int code) { client.handleReceiveFailure(code); }

    static std::size_t maximumQueuedSends() { return WebSocketClient::kMaxQueuedSends; }

    static std::size_t maximumOutgoingMessageBytes() { return WebSocketClient::kMaxOutgoingMessageBytes; }

    static std::size_t maximumQueuedSendBytes() { return WebSocketClient::kMaxQueuedSendBytes; }

    static std::size_t maximumCreateResponseBytes() { return WebSocketClient::kMaxCreateResponseBytes; }

    static std::size_t queuedBytes(const WebSocketClient &client) { return client.queued_send_bytes_; }

    static void setRunning(WebSocketClient &client, bool running) { client.running_.store(running); }

    static bool enqueueDeferred(WebSocketClient &client, WebSocketClient::DeferredEncoder encoder, std::size_t bytes)
    {
        client.running_.store(true);
        return client.sendDeferred(std::move(encoder), bytes);
    }

    static std::size_t writeResponse(char *ptr, std::size_t size, std::size_t nmemb, std::string &response)
    {
        return WebSocketClient::writeCallback(ptr, size, nmemb, &response);
    }

    static bool drainLocalClose(WebSocketClient &client)
    {
        return client.drainLocalClose([](const char *, std::size_t size) {
            return WebSocketClient::SendAttempt{.code = CURLE_OK, .sent = size};
        });
    }

    static void startCloseDrainWorker(WebSocketClient &client)
    {
        client.thread_ = std::thread([&client]() {
            while (client.running_.load()) {
                std::this_thread::yield();
            }
            client.drainLocalClose([](const char *, std::size_t size) {
                return WebSocketClient::SendAttempt{.code = CURLE_OK, .sent = size};
            });
            client.running_.store(false);
        });
    }
};

struct ViewerSocketTestAccess {
    static std::uint64_t beginOpen(ViewerSocket &socket)
    {
        std::scoped_lock transport_lock(socket.transport_mutex_);
        const auto generation = socket.prepareOpen();
        if (!socket.ws_) {
            socket.ws_ = std::make_unique<WebSocketClient>();
        }
        WebSocketClientTestAccess::setRunning(*socket.ws_, true);
        return generation;
    }

    static bool markOpen(ViewerSocket &socket)
    {
        std::scoped_lock transport_lock(socket.transport_mutex_);
        auto expected = ViewerSocket::ConnectionState::Opening;
        return socket.state_.compare_exchange_strong(expected, ViewerSocket::ConnectionState::Open,
                                                     std::memory_order_acq_rel, std::memory_order_acquire);
    }

    static void terminate(ViewerSocket &socket, std::uint64_t generation, WebSocketClient::TerminationKind kind,
                          const std::string &detail = {})
    {
        socket.onTransportClosed(generation, {.kind = kind, .detail = detail});
    }

    static bool trustedClient(const ViewerSocket &socket, const WsIncomingPacket &packet)
    {
        return socket.isTrustedClient(packet);
    }

    static void message(ViewerSocket &socket, const std::string &data) { socket.onMessage(data); }

    static std::size_t queuedMessages(const ViewerSocket &socket)
    {
        std::scoped_lock transport_lock(socket.transport_mutex_);
        return socket.ws_ == nullptr ? 0 : WebSocketClientTestAccess::queued(*socket.ws_);
    }

    static std::unique_lock<std::mutex> lockTransport(ViewerSocket &socket)
    {
        return std::unique_lock<std::mutex>(socket.transport_mutex_);
    }
};

namespace websocket_lifecycle_test {

std::string clientConnectPacket(const Crypto::KeyPair &key_pair, bool valid_signature);
bool waitForExit(const WebSocketClient &client);
void runViewerLifecycleTests();

}  // namespace websocket_lifecycle_test

}  // namespace spark

#endif  // ENDSTONE_SPARK_WEBSOCKET_LIFECYCLE_TEST_SUPPORT_H

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "core/util/base64.h"
#include "core/ws/crypto.h"
#include "core/ws/viewer_socket.h"
#include "net/websocket.h"
#include "proto/proto_writer.h"

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
    static void markOpen(ViewerSocket &socket)
    {
        std::scoped_lock transport_lock(socket.transport_mutex_);
        socket.prepareOpen();
        if (!socket.ws_) {
            socket.ws_ = std::make_unique<WebSocketClient>();
        }
        WebSocketClientTestAccess::setRunning(*socket.ws_, true);
        socket.open_.store(true);
    }

    static void terminate(ViewerSocket &socket, WebSocketClient::TerminationKind kind, const std::string &detail = {})
    {
        socket.onTransportClosed({.kind = kind, .detail = detail});
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

}  // namespace spark

namespace {

std::string clientConnectPacket(const spark::Crypto::KeyPair &key_pair, bool valid_signature)
{
    std::string connect;
    spark::ProtoWriter connect_writer(connect);
    connect_writer.string(1, "client");

    std::string wrapper;
    spark::ProtoWriter wrapper_writer(wrapper);
    wrapper_writer.message(11, connect);

    std::vector<std::uint8_t> signature = spark::Crypto::sign(
        key_pair.private_key_pkcs8, reinterpret_cast<const std::uint8_t *>(wrapper.data()), wrapper.size());
    assert(!signature.empty());
    if (!valid_signature) {
        signature.front() ^= 0xff;
    }

    std::string raw;
    spark::ProtoWriter raw_writer(raw);
    raw_writer.int32(1, spark::Crypto::kVersion);
    raw_writer.string(2, std::string_view(reinterpret_cast<const char *>(key_pair.public_key_x509.data()),
                                          key_pair.public_key_x509.size()));
    raw_writer.string(3, std::string_view(reinterpret_cast<const char *>(signature.data()), signature.size()));
    raw_writer.string(4, wrapper);
    return spark::base64Encode(raw);
}

bool waitForExit(const spark::WebSocketClient &client)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (spark::WebSocketClientTestAccess::running(client) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return !spark::WebSocketClientTestAccess::running(client);
}

}  // namespace

int main()
{
    spark::WebSocketClient client;
    std::mutex mutex;
    std::condition_variable cv;
    bool exited = false;
    spark::WebSocketClientTestAccess::startExitedWorker(client, mutex, cv, exited);

    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(cv.wait_for(lock, std::chrono::seconds(2), [&exited]() { return exited; }));
    }

    assert(spark::WebSocketClientTestAccess::joinable(client));
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    client.close();

    std::atomic<bool> close_attempted{false};
    spark::WebSocketClientTestAccess::startLocalCloseWorker(client, close_attempted);
    client.close();
    assert(close_attempted.load());
    assert(!spark::WebSocketClientTestAccess::joinable(client));
    client.close();

    std::atomic<std::uint64_t> cleanup_count{0};
    assert(spark::WebSocketClientTestAccess::startThrowingCallbackWorker(client, cleanup_count));
    assert(waitForExit(client));
    assert(cleanup_count.load(std::memory_order_acquire) == 2);
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));

    assert(spark::WebSocketClientTestAccess::startThrowingCallbackWorker(client, cleanup_count));
    assert(waitForExit(client));
    assert(cleanup_count.load(std::memory_order_acquire) == 4);
    client.close();
    assert(!spark::WebSocketClientTestAccess::joinable(client));

    {
        spark::WebSocketClient full_send;
        spark::WebSocketClientTestAccess::enqueue(full_send, "alpha");
        assert(spark::WebSocketClientTestAccess::processSend(full_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(full_send).empty());
        assert(spark::WebSocketClientTestAccess::queued(full_send) == 0);
        full_send.close();
    }

    {
        spark::WebSocketClient retry_send;
        spark::WebSocketClientTestAccess::enqueue(retry_send, "retry");
        assert(spark::WebSocketClientTestAccess::processSend(retry_send, [](std::string_view) {
                   return std::pair{CURLE_AGAIN, std::size_t{0}};
               }) == spark::WebSocketClientTestAccess::SendStep::Retry);
        assert(spark::WebSocketClientTestAccess::pendingSend(retry_send) == "retry");
        assert(spark::WebSocketClientTestAccess::processSend(retry_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(retry_send).empty());
        retry_send.close();
    }

    {
        spark::WebSocketClient partial_send;
        spark::WebSocketClientTestAccess::enqueue(partial_send, "partial");
        assert(spark::WebSocketClientTestAccess::processSend(partial_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size() - 3};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(partial_send) == "ial");
        assert(spark::WebSocketClientTestAccess::processSend(partial_send, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        assert(spark::WebSocketClientTestAccess::pendingSend(partial_send).empty());
        partial_send.close();
    }

    {
        spark::WebSocketClient ordered_send;
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "AAAA");
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "BB");
        spark::WebSocketClientTestAccess::enqueue(ordered_send, "C");
        std::vector<std::string> attempts;
        auto send = [&attempts](std::string_view data) {
            attempts.emplace_back(data);
            const std::size_t sent = attempts.size() == 1 ? 2 : data.size();
            return std::pair{CURLE_OK, sent};
        };
        while (spark::WebSocketClientTestAccess::processSend(ordered_send, send) !=
               spark::WebSocketClientTestAccess::SendStep::Idle) {
        }
        assert((attempts == std::vector<std::string>{"AAAA", "AA", "BB", "C"}));
        ordered_send.close();
    }

    {
        spark::WebSocketClient failed_send;
        spark::WebSocketClientTestAccess::enqueue(failed_send, "failure");
        assert(spark::WebSocketClientTestAccess::processSend(failed_send, [](std::string_view) {
                   return std::pair{CURLE_SEND_ERROR, std::size_t{0}};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        const auto termination = failed_send.termination();
        assert(termination.kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!termination.detail.empty());
        failed_send.close();
    }

    {
        spark::WebSocketClient deferred;
        const std::thread::id caller = std::this_thread::get_id();
        std::atomic<bool> ran_on_worker{false};
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            deferred,
            [&ran_on_worker, caller]() {
                ran_on_worker.store(std::this_thread::get_id() != caller);
                return std::string("deferred");
            },
            32));
        std::thread worker([&deferred]() {
            assert(spark::WebSocketClientTestAccess::processSend(deferred, [](std::string_view data) {
                       return std::pair{CURLE_OK, data.size()};
                   }) == spark::WebSocketClientTestAccess::SendStep::Progress);
            spark::WebSocketClientTestAccess::setRunning(deferred, false);
        });
        worker.join();
        assert(ran_on_worker.load());
        deferred.close();
    }

    {
        spark::WebSocketClient throwing;
        const bool accepted = spark::WebSocketClientTestAccess::enqueueDeferred(
            throwing, []() -> std::string { throw std::runtime_error("deferred failure"); }, 1);
        assert(accepted);
        assert(spark::WebSocketClientTestAccess::processSend(throwing, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        assert(throwing.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        throwing.close();
    }

    {
        spark::WebSocketClient missing_encoder;
        const bool accepted = spark::WebSocketClientTestAccess::enqueueDeferred(
            missing_encoder, spark::WebSocketClient::DeferredEncoder{}, 1);
        assert(!accepted);
        assert(missing_encoder.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        missing_encoder.close();
    }

    {
        spark::WebSocketClient exact;
        const std::size_t maximum = spark::WebSocketClientTestAccess::maximumOutgoingMessageBytes();
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            exact, [maximum]() { return std::string(maximum, 'x'); }, 1));
        assert(spark::WebSocketClientTestAccess::processSend(exact, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        exact.close();

        spark::WebSocketClient direct_exact;
        spark::WebSocketClientTestAccess::enqueue(direct_exact, std::string(maximum, 'x'));
        assert(spark::WebSocketClientTestAccess::processSend(direct_exact, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Progress);
        direct_exact.close();

        spark::WebSocketClient direct_oversized;
        spark::WebSocketClientTestAccess::enqueue(direct_oversized, std::string(maximum + 1, 'x'));
        assert(direct_oversized.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        direct_oversized.close();

        spark::WebSocketClient oversized;
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            oversized, [maximum]() { return std::string(maximum + 1, 'x'); }, 1));
        assert(spark::WebSocketClientTestAccess::processSend(oversized, [](std::string_view data) {
                   return std::pair{CURLE_OK, data.size()};
               }) == spark::WebSocketClientTestAccess::SendStep::Fatal);
        assert(oversized.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        oversized.close();
    }

    {
        spark::WebSocketClient bounded_send;
        for (std::size_t i = 0; i < spark::WebSocketClientTestAccess::maximumQueuedSends(); ++i) {
            spark::WebSocketClientTestAccess::enqueue(bounded_send, "queued");
        }
        spark::WebSocketClientTestAccess::enqueue(bounded_send, "overflow");
        assert(bounded_send.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!spark::WebSocketClientTestAccess::running(bounded_send));
        bounded_send.close();
    }

    {
        spark::WebSocketClient bounded_deferred;
        assert(!spark::WebSocketClientTestAccess::enqueueDeferred(
            bounded_deferred, [] { return std::string("not-run"); },
            spark::WebSocketClientTestAccess::maximumQueuedSendBytes() + 1));
        assert(spark::WebSocketClientTestAccess::queued(bounded_deferred) == 0);
        bounded_deferred.close();

        spark::WebSocketClient count_limited;
        for (std::size_t i = 0; i < spark::WebSocketClientTestAccess::maximumQueuedSends(); ++i) {
            assert(spark::WebSocketClientTestAccess::enqueueDeferred(
                count_limited, [] { return std::string("queued"); }, 1));
        }
        assert(spark::WebSocketClientTestAccess::queuedBytes(count_limited) ==
               spark::WebSocketClientTestAccess::maximumQueuedSends());
        assert(!spark::WebSocketClientTestAccess::enqueueDeferred(
            count_limited, [] { return std::string("overflow"); }, 1));
        count_limited.close();
    }

    {
        spark::WebSocketClient bounded_send_bytes;
        const std::string maximum_message(spark::WebSocketClientTestAccess::maximumOutgoingMessageBytes(), 'x');
        const std::size_t accepted = spark::WebSocketClientTestAccess::maximumQueuedSendBytes() /
                                     spark::WebSocketClientTestAccess::maximumOutgoingMessageBytes();
        for (std::size_t i = 0; i < accepted; ++i) {
            spark::WebSocketClientTestAccess::enqueue(bounded_send_bytes, maximum_message);
        }
        spark::WebSocketClientTestAccess::enqueue(bounded_send_bytes, "overflow");
        assert(bounded_send_bytes.termination().kind == spark::WebSocketClient::TerminationKind::SendError);
        assert(!spark::WebSocketClientTestAccess::running(bounded_send_bytes));
        bounded_send_bytes.close();
    }

    {
        spark::WebSocketClient failed_receive;
        spark::WebSocketClientTestAccess::receiveFailure(failed_receive, CURLE_RECV_ERROR);
        const auto termination = failed_receive.termination();
        assert(termination.kind == spark::WebSocketClient::TerminationKind::ReceiveError);
        assert(!termination.detail.empty());
    }

    {
        std::string response;
        const std::size_t maximum = spark::WebSocketClientTestAccess::maximumCreateResponseBytes();
        std::string exact(maximum, 'x');
        assert(spark::WebSocketClientTestAccess::writeResponse(exact.data(), 1, exact.size(), response) == maximum);
        assert(response.size() == maximum);
        assert(spark::WebSocketClientTestAccess::writeResponse(exact.data(), 1, 1, response) == 0);
        assert(spark::WebSocketClientTestAccess::writeResponse(nullptr, std::numeric_limits<std::size_t>::max(), 2,
                                                               response) == 0);
    }

    {
        spark::ViewerSocket auth({}, {});
        bool trusted = true;
        auth.setIsKeyTrustedCallback([&trusted](const std::vector<std::uint8_t> &) { return trusted; });
        spark::ViewerSocketTestAccess::markOpen(auth);
        const spark::Crypto::KeyPair key_pair = spark::Crypto::generateKeyPair();
        spark::ViewerSocketTestAccess::message(auth, clientConnectPacket(key_pair, false));
        assert(auth.tick());
        assert(auth.pendingKey("client").empty());
        const std::size_t invalid_queue_size = spark::ViewerSocketTestAccess::queuedMessages(auth);
        auth.sendClientTrusted("client");
        assert(spark::ViewerSocketTestAccess::queuedMessages(auth) == invalid_queue_size);

        spark::ViewerSocketTestAccess::message(auth, clientConnectPacket(key_pair, true));
        assert(auth.tick());
        assert(auth.pendingKey("client") == key_pair.public_key_x509);
        const std::size_t verified_queue_size = spark::ViewerSocketTestAccess::queuedMessages(auth);
        trusted = false;
        auth.sendClientTrusted("client");
        assert(spark::ViewerSocketTestAccess::queuedMessages(auth) == verified_queue_size);
        trusted = true;
        auth.sendClientTrusted("client");
        assert(spark::ViewerSocketTestAccess::queuedMessages(auth) == verified_queue_size + 1);
        auth.close();
    }

    {
        spark::ViewerSocket concurrent({}, {});
        spark::ViewerSocketTestAccess::markOpen(concurrent);
        auto transport_gate = spark::ViewerSocketTestAccess::lockTransport(concurrent);
        std::atomic<bool> sender_started{false};
        std::atomic<bool> sender_finished{false};
        std::thread sender([&concurrent, &sender_started, &sender_finished]() {
            sender_started.store(true);
            concurrent.sendUpdate("concurrent");
            sender_finished.store(true);
        });
        while (!sender_started.load()) {
            std::this_thread::yield();
        }
        std::thread closer([&concurrent]() { concurrent.close(); });
        transport_gate.unlock();
        sender.join();
        closer.join();
        assert(sender_finished.load());
        assert(!concurrent.isOpen());
    }

    {
        spark::WebSocketClient close_worker;
        const std::thread::id caller = std::this_thread::get_id();
        std::atomic<bool> ran_on_worker{false};
        assert(spark::WebSocketClientTestAccess::enqueueDeferred(
            close_worker,
            [&ran_on_worker, caller]() {
                ran_on_worker.store(std::this_thread::get_id() != caller);
                return std::string("close");
            },
            16));
        spark::WebSocketClientTestAccess::startCloseDrainWorker(close_worker);
        close_worker.close();
        assert(ran_on_worker.load());
    }

    spark::ViewerSocket viewer({}, {});
    viewer.setIsKeyTrustedCallback(
        [](const std::vector<std::uint8_t> &key) { return key == std::vector<std::uint8_t>{1, 2, 3}; });
    spark::WsIncomingPacket connect;
    connect.type = spark::WsPacketType::ClientConnect;
    connect.public_key = {1, 2, 3};
    assert(!spark::ViewerSocketTestAccess::trustedClient(viewer, connect));
    connect.verified = true;
    assert(spark::ViewerSocketTestAccess::trustedClient(viewer, connect));
    connect.public_key = {4, 5, 6};
    assert(!spark::ViewerSocketTestAccess::trustedClient(viewer, connect));

    spark::ViewerSocketTestAccess::markOpen(viewer);
    spark::ViewerSocketTestAccess::terminate(viewer, spark::WebSocketClient::TerminationKind::RemoteClose);
    assert(!viewer.isOpen());
    assert(viewer.closeReason() == spark::ViewerSocket::CloseReason::RemoteClose);
    assert(!viewer.takeDiagnostic().empty());

    spark::ViewerSocketTestAccess::markOpen(viewer);
    assert(viewer.isOpen());
    assert(viewer.closeReason() == spark::ViewerSocket::CloseReason::None);
    viewer.close();
    assert(!viewer.isOpen());
    assert(viewer.closeReason() == spark::ViewerSocket::CloseReason::LocalClose);
    assert(viewer.takeDiagnostic().empty());
    return 0;
}

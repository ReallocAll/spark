#include <cassert>

#include "../core/ws/crypto_test_keys.h"
#include "native_exit_gate.h"
#include "websocket_lifecycle_test_support.h"

namespace spark::websocket_lifecycle_test {

void runViewerLifecycleTests()
{
    {
        spark::test::NativeExitGate gate;
        ViewerSocket socket({}, {});
        ViewerSocketTestAccess::onTransportCreated(socket, [&](WebSocketClient &transport) {
            WebSocketClientTestAccess::prepareIo(
                transport,
                [&] {
                    spark::test::holdNativeThreadExit(gate);
                    throw std::runtime_error("native exit gate");
                },
                [](auto data) { return std::pair{CURLE_OK, data.size()}; });
        });
        std::thread opener([&] { socket.open([](const auto &) { return std::string(); }); });
        gate.waitEntered();
        const auto begin = std::chrono::steady_clock::now();
        assert(!socket.closeWithin(std::chrono::milliseconds(20)));
        assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));
        assert(ViewerSocketTestAccess::hasTransport(socket));
        assert(ViewerSocketTestAccess::transport(socket).connect("localhost", "replacement").empty());
        gate.unblock();
        opener.join();
        assert(socket.closeWithin(std::chrono::seconds(2)));
    }
    for (int failure = 0; failure < 4; ++failure) {
        ViewerSocket socket({}, {});
        ViewerSocketTestAccess::beginOpen(socket);
        assert(ViewerSocketTestAccess::markOpen(socket));
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        WebSocketClientTestAccess::startHeldWorker(ViewerSocketTestAccess::transport(socket), mutex, cv, release);
        if (failure == 0) {
            ViewerSocketTestAccess::overflow(socket);
        }
        else if (failure == 1 || failure == 3) {
            ViewerSocketTestAccess::age(socket);
        }
        else {
            ViewerSocketTestAccess::queueTrustedPacket(socket);
            socket.setIsKeyTrustedCallback([](const auto &) -> bool { throw std::runtime_error("tick gate"); });
        }
        const auto begin = std::chrono::steady_clock::now();
        if (failure == 3) {
            socket.processWindowRotate([](const auto &) -> std::string { std::terminate(); });
        }
        else {
            assert(!socket.tick());
        }
        assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));
        assert(!socket.closeWithin(std::chrono::milliseconds::zero()));
        assert(ViewerSocketTestAccess::hasTransport(socket));
        {
            std::scoped_lock lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(socket.closeWithin(std::chrono::seconds(2)));
    }
    {
        const auto &key = test::testKeyPair();
        const std::string expected = encodeServerClose(key.private_key_pkcs8);
        ViewerSocket socket({}, key);
        CancellationSource cancellation;
        spark::test::NativeExitGate handshake;
        std::vector<std::string> sent;
        WebSocketClient *transport = nullptr;
        std::atomic<bool> open_done{false};
        std::atomic<int> uploads{0};
        ViewerSocketTestAccess::onTransportCreated(socket, [&](WebSocketClient &client) {
            transport = &client;
            assert(WebSocketClientTestAccess::localCloseMessage(client) == expected);
            WebSocketClientTestAccess::prepareIo(
                client,
                [&] {
                    WebSocketClientTestAccess::pendingPrefix(client, "prefix");
                    for (std::size_t i = 0; i < WebSocketClientTestAccess::maximumQueuedSends(); ++i) {
                        client.send(std::to_string(i));
                    }
                    handshake.block();
                },
                [&](std::string_view bytes) {
                    sent.emplace_back(bytes);
                    return std::pair{CURLE_OK, bytes.size()};
                });
        });
        std::thread opener([&] {
            socket.open(
                [&](const auto &) {
                    ++uploads;
                    return std::string("key");
                },
                cancellation.token());
            open_done.store(true);
        });
        handshake.waitEntered();
        assert(sent.empty());
        auto send_lock = WebSocketClientTestAccess::lockSend(*transport);
        cancellation.requestStop();
        assert(!transport->sendDeferred([] { return std::string("late"); }, 4));
        assert(!socket.closeWithin(std::chrono::milliseconds::zero()));
        assert(!socket.closeWithin(std::chrono::milliseconds::zero()));
        send_lock.unlock();
        handshake.unblock();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool closed = false;
        while (!(closed = socket.closeWithin(std::chrono::milliseconds::zero())) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        assert(closed);
        opener.join();
        assert(open_done.load());
        assert(uploads.load() == 0);
        assert(sent.size() == WebSocketClientTestAccess::maximumQueuedSends() + 2);
        assert(sent.front() == "prefix");
        assert(sent.back() == expected);
        for (std::size_t i = 0; i < WebSocketClientTestAccess::maximumQueuedSends(); ++i) {
            assert(sent[i + 1] == std::to_string(i));
        }
        assert(socket.closeWithin(std::chrono::milliseconds::zero()));
    }
    for (const int code : {CURLE_AGAIN, CURLE_SEND_ERROR}) {
        WebSocketClient client;
        assert(client.setLocalCloseMessage("close"));
        WebSocketClientTestAccess::enqueue(client, "prefix");
        client.requestStop();
        int attempts = 0;
        const bool drained = WebSocketClientTestAccess::drainWith(client, [&](auto bytes) {
            ++attempts;
            assert(bytes == "prefix");
            return std::pair{code, std::size_t{2}};
        });
        assert(drained);
        client.requestStop();
        const bool repeated = WebSocketClientTestAccess::drainWith(client, [&](auto bytes) {
            ++attempts;
            return std::pair{CURLE_OK, bytes.size()};
        });
        assert(!repeated);
        assert(attempts == 1);
        assert(WebSocketClientTestAccess::pendingSend(client) == "efix");
    }
    {
        ViewerSocket socket({}, {});
        ViewerSocketTestAccess::beginOpen(socket);
        assert(ViewerSocketTestAccess::markOpen(socket));
        std::mutex mutex;
        std::condition_variable cv;
        bool release = false;
        WebSocketClientTestAccess::startHeldWorker(ViewerSocketTestAccess::transport(socket), mutex, cv, release);
        for (const bool hold_open : {true, false}) {
            auto gate =
                hold_open ? ViewerSocketTestAccess::lockOpen(socket) : ViewerSocketTestAccess::lockTransport(socket);
            bool closed = true;
            const auto begin = std::chrono::steady_clock::now();
            std::thread closer([&] { closed = socket.closeWithin(std::chrono::milliseconds(20)); });
            closer.join();
            assert(!closed);
            assert(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(250));
            assert(ViewerSocketTestAccess::hasTransport(socket));
        }
        assert(!socket.closeWithin(std::chrono::milliseconds(20)));
        assert(!socket.closeWithin(std::chrono::milliseconds::zero()));
        assert(ViewerSocketTestAccess::hasTransport(socket));
        {
            std::scoped_lock lock(mutex);
            release = true;
        }
        cv.notify_all();
        assert(socket.closeWithin(std::chrono::seconds(2)));
        assert(!ViewerSocketTestAccess::hasTransport(socket));
        assert(socket.closeWithin(std::chrono::milliseconds::zero()));
    }

    {
        ViewerSocket socket({}, {});
        CancellationSource cancellation;
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> uploaded{false};
        ViewerSocketTestAccess::onTransportCreated(socket, [&](WebSocketClient &transport) {
            WebSocketClientTestAccess::setCreateChannel(transport, [&](const CancellationToken &token) {
                {
                    std::scoped_lock lock(mutex);
                    entered = true;
                }
                cv.notify_all();
                cancelled.store(token.waitForStop(std::chrono::seconds(2)));
                return std::string();
            });
        });
        std::thread opener([&] {
            assert(socket
                       .open(
                           [&](const std::string &) {
                               uploaded.store(true);
                               return std::string("key");
                           },
                           cancellation.token())
                       .empty());
        });
        {
            std::unique_lock lock(mutex);
            assert(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
        }
        assert(!socket.closeWithin(std::chrono::milliseconds(20)));
        cancellation.requestStop();
        opener.join();
        assert(cancelled.load());
        assert(!uploaded.load());
        assert(socket.closeWithin(std::chrono::milliseconds::zero()));
    }

    {
        ViewerSocket auth({}, {});
        bool trusted = true;
        auth.setIsKeyTrustedCallback([&trusted](const std::vector<std::uint8_t> &) { return trusted; });
        ViewerSocketTestAccess::beginOpen(auth);
        assert(ViewerSocketTestAccess::markOpen(auth));
        const Crypto::KeyPair &key_pair = test::testKeyPair();
        ViewerSocketTestAccess::message(auth, clientConnectPacket(key_pair, false));
        assert(auth.tick());
        assert(auth.pendingKey("client").empty());
        const std::size_t invalid_queue_size = ViewerSocketTestAccess::queuedMessages(auth);
        auth.sendClientTrusted("client");
        assert(ViewerSocketTestAccess::queuedMessages(auth) == invalid_queue_size);

        ViewerSocketTestAccess::message(auth, clientConnectPacket(key_pair, true));
        assert(auth.tick());
        assert(auth.pendingKey("client") == key_pair.public_key_x509);
        const std::size_t verified_queue_size = ViewerSocketTestAccess::queuedMessages(auth);
        trusted = false;
        auth.sendClientTrusted("client");
        assert(ViewerSocketTestAccess::queuedMessages(auth) == verified_queue_size);
        trusted = true;
        auth.sendClientTrusted("client");
        assert(ViewerSocketTestAccess::queuedMessages(auth) == verified_queue_size + 1);
        auth.close();
    }

    {
        ViewerSocket conflict({}, {});
        conflict.setIsKeyTrustedCallback([](const std::vector<std::uint8_t> &) { return false; });
        ViewerSocketTestAccess::beginOpen(conflict);
        assert(ViewerSocketTestAccess::markOpen(conflict));

        const Crypto::KeyPair &first_key = test::testKeyPair();
        const Crypto::KeyPair second_key = Crypto::generateKeyPair();
        assert(!second_key.public_key_x509.empty());

        ViewerSocketTestAccess::message(conflict, clientConnectPacket(first_key, true));
        assert(conflict.tick());
        assert(conflict.pendingKey("client") == first_key.public_key_x509);

        ViewerSocketTestAccess::message(conflict, clientConnectPacket(second_key, true));
        assert(conflict.tick());
        assert(conflict.pendingKey("client").empty());
        const std::size_t conflicted_queue_size = ViewerSocketTestAccess::queuedMessages(conflict);
        conflict.sendClientTrusted("client");
        assert(ViewerSocketTestAccess::queuedMessages(conflict) == conflicted_queue_size);

        ViewerSocketTestAccess::message(conflict, clientConnectPacket(first_key, true));
        assert(conflict.tick());
        assert(conflict.pendingKey("client").empty());
        const std::size_t repeated_queue_size = ViewerSocketTestAccess::queuedMessages(conflict);
        conflict.sendClientTrusted("client");
        assert(ViewerSocketTestAccess::queuedMessages(conflict) == repeated_queue_size);

        conflict.close();
        ViewerSocketTestAccess::beginOpen(conflict);
        assert(ViewerSocketTestAccess::markOpen(conflict));
        ViewerSocketTestAccess::message(conflict, clientConnectPacket(first_key, true));
        assert(conflict.tick());
        assert(conflict.pendingKey("client") == first_key.public_key_x509);
        conflict.close();
    }

    {
        ViewerSocket concurrent({}, {});
        ViewerSocketTestAccess::beginOpen(concurrent);
        assert(ViewerSocketTestAccess::markOpen(concurrent));
        auto transport_gate = ViewerSocketTestAccess::lockTransport(concurrent);
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
        ViewerSocket opening({}, {});
        const auto generation = ViewerSocketTestAccess::beginOpen(opening);
        ViewerSocketTestAccess::terminate(opening, generation, WebSocketClient::TerminationKind::RemoteClose);
        assert(!ViewerSocketTestAccess::markOpen(opening));
        assert(!opening.isOpen());

        const auto older_generation = ViewerSocketTestAccess::beginOpen(opening);
        assert(ViewerSocketTestAccess::markOpen(opening));
        const auto newer_generation = ViewerSocketTestAccess::beginOpen(opening);
        ViewerSocketTestAccess::terminate(opening, older_generation, WebSocketClient::TerminationKind::RemoteClose);
        assert(!opening.isOpen());
        assert(opening.closeReason() == ViewerSocket::CloseReason::None);
        assert(ViewerSocketTestAccess::markOpen(opening));
        assert(opening.isOpen());
        ViewerSocketTestAccess::terminate(opening, newer_generation, WebSocketClient::TerminationKind::RemoteClose);
        assert(!opening.isOpen());
    }

    {
        WebSocketClient close_worker;
        const std::thread::id caller = std::this_thread::get_id();
        std::atomic<bool> ran_on_worker{false};
        assert(WebSocketClientTestAccess::enqueueDeferred(
            close_worker,
            [&ran_on_worker, caller]() {
                ran_on_worker.store(std::this_thread::get_id() != caller);
                return std::string("close");
            },
            16));
        WebSocketClientTestAccess::startCloseDrainWorker(close_worker);
        close_worker.close();
        assert(ran_on_worker.load());
    }

    ViewerSocket viewer({}, {});
    viewer.setIsKeyTrustedCallback(
        [](const std::vector<std::uint8_t> &key) { return key == std::vector<std::uint8_t>{1, 2, 3}; });
    WsIncomingPacket connect;
    connect.type = WsPacketType::ClientConnect;
    connect.public_key = {1, 2, 3};
    assert(!ViewerSocketTestAccess::trustedClient(viewer, connect));
    connect.verified = true;
    assert(ViewerSocketTestAccess::trustedClient(viewer, connect));
    connect.public_key = {4, 5, 6};
    assert(!ViewerSocketTestAccess::trustedClient(viewer, connect));

    const auto generation = ViewerSocketTestAccess::beginOpen(viewer);
    assert(ViewerSocketTestAccess::markOpen(viewer));
    ViewerSocketTestAccess::terminate(viewer, generation, WebSocketClient::TerminationKind::RemoteClose);
    assert(!viewer.isOpen());
    assert(viewer.closeReason() == ViewerSocket::CloseReason::RemoteClose);
    assert(!viewer.takeDiagnostic().empty());

    ViewerSocketTestAccess::beginOpen(viewer);
    assert(ViewerSocketTestAccess::markOpen(viewer));
    assert(viewer.isOpen());
    assert(viewer.closeReason() == ViewerSocket::CloseReason::None);
    viewer.close();
    assert(!viewer.isOpen());
    assert(viewer.closeReason() == ViewerSocket::CloseReason::LocalClose);
    assert(viewer.takeDiagnostic().empty());
}

}  // namespace spark::websocket_lifecycle_test

#include <cassert>

#include "../core/ws/crypto_test_keys.h"
#include "websocket_lifecycle_test_support.h"

namespace spark::websocket_lifecycle_test {

void runViewerLifecycleTests()
{
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

#include "websocket_lifecycle_test_support.h"

#include <cassert>
#include <chrono>

#include "core/util/base64.h"
#include "proto/proto_writer.h"

namespace spark::websocket_lifecycle_test {

std::string clientConnectPacket(const Crypto::KeyPair &key_pair, bool valid_signature)
{
    std::string connect;
    ProtoWriter connect_writer(connect);
    connect_writer.string(1, "client");

    std::string wrapper;
    ProtoWriter wrapper_writer(wrapper);
    wrapper_writer.message(11, connect);

    std::vector<std::uint8_t> signature = Crypto::sign(
        key_pair.private_key_pkcs8, reinterpret_cast<const std::uint8_t *>(wrapper.data()), wrapper.size());
    assert(!signature.empty());
    if (!valid_signature) {
        signature.front() ^= 0xff;
    }

    std::string raw;
    ProtoWriter raw_writer(raw);
    raw_writer.int32(1, Crypto::kVersion);
    raw_writer.string(2, std::string_view(reinterpret_cast<const char *>(key_pair.public_key_x509.data()),
                                          key_pair.public_key_x509.size()));
    raw_writer.string(3, std::string_view(reinterpret_cast<const char *>(signature.data()), signature.size()));
    raw_writer.string(4, wrapper);
    return base64Encode(raw);
}

bool waitForExit(const WebSocketClient &client)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (WebSocketClientTestAccess::running(client) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return !WebSocketClientTestAccess::running(client);
}

}  // namespace spark::websocket_lifecycle_test

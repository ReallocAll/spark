#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

#include "core/util/base64.h"
#include "core/ws/crypto.h"
#include "core/ws/ws_proto.h"
#include "crypto_test_keys.h"
#include "proto/proto_reader.h"
#include "proto/proto_writer.h"

namespace {

std::string rawMessage(const std::string &encoded)
{
    const auto bytes = spark::base64Decode(encoded);
    spark::ProtoReader reader(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
    int field = 0;
    int wire = 0;
    while (reader.nextField(field, wire)) {
        if (field == 4 && wire == 2) {
            return std::string(reader.readString());
        }
        reader.skip(wire);
    }
    return {};
}

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

}  // namespace

int main()
{
    const std::string raw_message = spark::encodeServerUpdateStatistics("", "", {}, {});
    assert(!raw_message.empty());

    const std::string wrapper_bytes = rawMessage(raw_message);
    spark::ProtoReader wrapper(wrapper_bytes);
    int field = 0;
    int wire = 0;
    assert(wrapper.nextField(field, wire));
    assert(field == 4 && wire == 2);

    spark::ProtoReader update = wrapper.readMessage();
    assert(update.nextField(field, wire));
    assert(field == 1 && wire == 2);
    assert(update.readMessage().eof());
    assert(update.nextField(field, wire));
    assert(field == 2 && wire == 2);
    assert(update.readMessage().eof());
    assert(update.nextField(field, wire));
    assert(field == 3 && wire == 2);
    assert(update.readMessage().eof());
    assert(!update.nextField(field, wire));

    const auto &key_pair = spark::test::testKeyPair();
    spark::WsIncomingPacket incoming;
    assert(spark::decodeRawPacket(clientConnectPacket(key_pair, true), incoming));
    assert(incoming.type == spark::WsPacketType::ClientConnect);
    assert(incoming.verified);
    assert(incoming.connect.client_id == "client");

    assert(spark::decodeRawPacket(clientConnectPacket(key_pair, false), incoming));
    assert(!incoming.verified);

    assert(!spark::decodeRawPacket(std::string(spark::kMaxIncomingWsPacketBytes + 1, 'A'), incoming));
    assert(spark::base64Decode("TQ==") == std::vector<std::uint8_t>{'M'});
    assert(spark::base64Decode("TWE=") == (std::vector<std::uint8_t>{'M', 'a'}));
    assert(spark::base64Decode("TWFu") == (std::vector<std::uint8_t>{'M', 'a', 'n'}));
    assert(spark::base64Decode("A===").empty());
    assert(spark::base64Decode("TQ=A").empty());
    assert(spark::base64Decode("TR==").empty());
    assert(spark::base64Decode("TWF=").empty());
    assert(spark::base64Decode("TWE").empty());
    assert(spark::base64Decode("TW$u").empty());

    const std::string overlong_varint(11, static_cast<char>(0x80));
    spark::ProtoReader malformed(overlong_varint);
    assert(!malformed.nextField(field, wire));
    assert(!malformed.valid());

    const std::string oversized_field{"\x80\x80\x80\x80\x10", 5};
    spark::ProtoReader oversized_field_reader(oversized_field);
    assert(!oversized_field_reader.nextField(field, wire));
    assert(!oversized_field_reader.valid());

    for (char tag = 1; tag <= 7; ++tag) {
        const std::string zero_field_bytes(1, tag);
        spark::ProtoReader zero_field(zero_field_bytes);
        assert(!zero_field.nextField(field, wire));
        assert(!zero_field.valid());
    }

    const std::string noncanonical_bytes{"\x88\x01", 2};
    spark::ProtoReader noncanonical(noncanonical_bytes);
    assert(noncanonical.nextField(field, wire));
    assert(field == 17 && wire == 0);
    assert(noncanonical.valid());

    const std::string truncated_length{"\x0a\x7f", 2};
    spark::ProtoReader truncated(truncated_length);
    assert(truncated.nextField(field, wire));
    assert(truncated.readString().empty());
    assert(!truncated.valid());
    return 0;
}

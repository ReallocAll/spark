#include <cassert>
#include <string>

#include "core/util/base64.h"
#include "core/ws/ws_proto.h"
#include "proto/proto_reader.h"

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
    return 0;
}

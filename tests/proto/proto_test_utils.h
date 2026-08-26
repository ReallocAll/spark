#ifndef ENDSTONE_SPARK_PROTO_TEST_UTILS_H
#define ENDSTONE_SPARK_PROTO_TEST_UTILS_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "proto/proto_reader.h"

namespace spark::proto_test {

template <typename Callback>
bool findMessage(ProtoReader reader, int target, Callback callback)
{
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == target && wire_type == 2) {
            ProtoReader message = reader.readMessage();
            if (callback(message)) {
                return true;
            }
            continue;
        }
        reader.skip(wire_type);
    }
    return false;
}

template <typename Callback>
bool findMessage(std::string_view bytes, int target, Callback callback)
{
    return findMessage(ProtoReader(bytes), target, callback);
}

inline bool hasField(ProtoReader reader, int target)
{
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == target) {
            return true;
        }
        reader.skip(wire_type);
    }
    return false;
}

inline bool hasField(std::string_view bytes, int target)
{
    return hasField(ProtoReader(bytes), target);
}

inline bool hasMessage(ProtoReader reader, int target)
{
    return findMessage(reader, target, [](ProtoReader /*message*/) { return true; });
}

inline bool hasMessage(std::string_view bytes, int target)
{
    return hasMessage(ProtoReader(bytes), target);
}

inline bool hasVarint(ProtoReader reader, int target, std::uint64_t expected)
{
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == target && wire_type == 0) {
            if (reader.readVarint() == expected) {
                return true;
            }
            continue;
        }
        reader.skip(wire_type);
    }
    return false;
}

inline bool hasString(ProtoReader reader, int target, std::string_view expected)
{
    int field = 0;
    int wire_type = 0;
    while (reader.nextField(field, wire_type)) {
        if (field == target && wire_type == 2) {
            if (reader.readString() == expected) {
                return true;
            }
            continue;
        }
        reader.skip(wire_type);
    }
    return false;
}

inline bool hasVarint(std::string_view bytes, int target, std::uint64_t expected)
{
    return hasVarint(ProtoReader(bytes), target, expected);
}

inline bool hasString(std::string_view bytes, int target, std::string_view expected)
{
    return hasString(ProtoReader(bytes), target, expected);
}

namespace detail {

inline bool readVarint(std::string_view bytes, std::size_t &offset, std::uint64_t &value)
{
    value = 0;
    for (int shift = 0; shift < 64 && offset < bytes.size(); shift += 7) {
        const auto byte = static_cast<unsigned char>(bytes[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

template <typename Callback>
bool findRawField(std::string_view bytes, int target, int target_wire, Callback callback)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::uint64_t tag = 0;
        if (!readVarint(bytes, offset, tag) || tag == 0) {
            return false;
        }
        const int field = static_cast<int>(tag >> 3);
        const int wire_type = static_cast<int>(tag & 7);
        if (wire_type == 0) {
            std::uint64_t value = 0;
            if (!readVarint(bytes, offset, value)) {
                return false;
            }
            continue;
        }
        if (wire_type == 1) {
            if (offset + sizeof(std::uint64_t) > bytes.size()) {
                return false;
            }
            const std::string_view value = bytes.substr(offset, sizeof(std::uint64_t));
            offset += sizeof(std::uint64_t);
            if (field == target && target_wire == wire_type && callback(value)) {
                return true;
            }
            continue;
        }
        if (wire_type == 2) {
            std::uint64_t size = 0;
            if (!readVarint(bytes, offset, size) || size > bytes.size() - offset) {
                return false;
            }
            const std::string_view value = bytes.substr(offset, static_cast<std::size_t>(size));
            offset += static_cast<std::size_t>(size);
            if (field == target && target_wire == wire_type && callback(value)) {
                return true;
            }
            continue;
        }
        if (wire_type == 5) {
            if (offset + sizeof(std::uint32_t) > bytes.size()) {
                return false;
            }
            offset += sizeof(std::uint32_t);
            continue;
        }
        return false;
    }
    return false;
}

}  // namespace detail

template <typename Callback>
bool findMessageBytes(std::string_view bytes, int target, Callback callback)
{
    return detail::findRawField(bytes, target, 2, callback);
}

inline bool hasDouble(std::string_view bytes, int target, double expected)
{
    return detail::findRawField(bytes, target, 1, [expected](std::string_view value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, value.data(), sizeof(bits));
        double actual = 0.0;
        std::memcpy(&actual, &bits, sizeof(actual));
        return std::abs(actual - expected) < 0.000001;
    });
}

}  // namespace spark::proto_test

#endif  // ENDSTONE_SPARK_PROTO_TEST_UTILS_H

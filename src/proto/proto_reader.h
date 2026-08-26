#ifndef ENDSTONE_SPARK_PROTO_READER_H
#define ENDSTONE_SPARK_PROTO_READER_H

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

// Minimal read-only protobuf (proto3) decoder for the small ws schema.
class ProtoReader {
public:
    explicit ProtoReader(std::string_view data) : data_(data) {}

    [[nodiscard]] bool eof() const { return pos_ >= data_.size(); }
    [[nodiscard]] bool valid() const { return valid_; }

    // Read the next field tag + wire type. Returns false at end of message.
    bool nextField(int &field, int &wire_type)
    {
        if (!valid_ || pos_ >= data_.size()) {
            return false;
        }
        auto tag = readVarint();
        constexpr std::uint64_t kMaxFieldNumber = (1ULL << 29) - 1;
        if (!valid_ || (tag >> 3) == 0 || (tag >> 3) > kMaxFieldNumber) {
            valid_ = false;
            return false;
        }
        field = static_cast<int>(tag >> 3);
        wire_type = static_cast<int>(tag & 0x07);
        return true;
    }

    std::uint64_t readVarint()
    {
        std::uint64_t value = 0;
        for (int byte_index = 0; byte_index < 10; ++byte_index) {
            if (pos_ >= data_.size()) {
                valid_ = false;
                return 0;
            }
            auto byte = static_cast<unsigned char>(data_[pos_++]);
            if (byte_index == 9 && byte > 1) {
                valid_ = false;
                return 0;
            }
            value |= static_cast<std::uint64_t>(byte & 0x7f) << (byte_index * 7);
            if (!(byte & 0x80)) {
                return value;
            }
        }
        valid_ = false;
        return 0;
    }

    std::int32_t readInt32() { return static_cast<std::int32_t>(readVarint()); }
    std::int64_t readInt64() { return static_cast<std::int64_t>(readVarint()); }
    bool readBool() { return readVarint() != 0; }

    std::string_view readString()
    {
        auto len = readVarint();
        if (!valid_ || len > data_.size() - pos_) {
            valid_ = false;
            pos_ = data_.size();
            return {};
        }
        const auto length = static_cast<std::size_t>(len);
        std::string_view result(data_.data() + pos_, length);
        pos_ += length;
        return result;
    }

    std::string readBytes()
    {
        auto sv = readString();
        return std::string(sv.data(), sv.size());
    }

    ProtoReader readMessage()
    {
        auto len = readVarint();
        if (!valid_ || len > data_.size() - pos_) {
            valid_ = false;
            pos_ = data_.size();
            return ProtoReader(std::string_view{});
        }
        const auto length = static_cast<std::size_t>(len);
        ProtoReader sub(data_.substr(pos_, length));
        pos_ += length;
        return sub;
    }

    void skip(int wire_type)
    {
        if (!valid_) {
            return;
        }
        switch (wire_type) {
        case 0:
            readVarint();
            break;
        case 1:
            skipBytes(8);
            break;
        case 2: {
            auto len = readVarint();
            if (valid_) {
                if (len > data_.size() - pos_) {
                    valid_ = false;
                    pos_ = data_.size();
                }
                else {
                    pos_ += static_cast<std::size_t>(len);
                }
            }
        } break;
        case 5:
            skipBytes(4);
            break;
        default:
            valid_ = false;
            pos_ = data_.size();
            break;
        }
    }

private:
    void skipBytes(std::size_t count)
    {
        if (count > data_.size() - pos_) {
            valid_ = false;
            pos_ = data_.size();
            return;
        }
        pos_ += count;
    }

    std::string_view data_;
    std::size_t pos_ = 0;
    bool valid_ = true;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_PROTO_READER_H

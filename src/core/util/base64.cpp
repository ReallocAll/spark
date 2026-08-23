#include "core/util/base64.h"

#include <limits>

namespace spark {

namespace {

constexpr char KEncodeTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string base64Encode(const std::uint8_t *data, std::size_t length)
{
    std::string out;
    if (length > std::numeric_limits<std::size_t>::max() - 2) {
        return {};
    }
    const std::size_t groups = (length + 2) / 3;
    if (groups > out.max_size() / 4) {
        return {};
    }
    out.reserve(groups * 4);
    for (std::size_t i = 0; i < length;) {
        std::uint32_t triple = data[i] << 16;
        if (length - i > 1) {
            triple |= data[i + 1] << 8;
        }
        if (length - i > 2) {
            triple |= data[i + 2];
        }

        out.push_back(KEncodeTable[(triple >> 18) & 0x3f]);
        out.push_back(KEncodeTable[(triple >> 12) & 0x3f]);
        if (length - i > 1) {
            out.push_back(KEncodeTable[(triple >> 6) & 0x3f]);
        }
        else {
            out.push_back('=');
        }
        if (length - i > 2) {
            out.push_back(KEncodeTable[triple & 0x3f]);
        }
        else {
            out.push_back('=');
        }
        if (length - i <= 3) {
            break;
        }
        i += 3;
    }
    return out;
}

std::string base64Encode(std::string_view data)
{
    return base64Encode(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

std::vector<std::uint8_t> base64Decode(std::string_view input)
{
    if (input.empty() || input.size() % 4 != 0) {
        return {};
    }

    std::vector<std::uint8_t> out;
    const std::size_t groups = input.size() / 4;
    if (groups > out.max_size() / 3) {
        return {};
    }
    out.reserve(groups * 3);

    for (std::size_t group = 0; group < groups; ++group) {
        const std::size_t offset = group * 4;
        const bool last = group + 1 == groups;
        const char c0 = input[offset];
        const char c1 = input[offset + 1];
        const char c2 = input[offset + 2];
        const char c3 = input[offset + 3];
        const int v0 = decodeChar(c0);
        const int v1 = decodeChar(c1);
        if (v0 < 0 || v1 < 0) {
            return {};
        }

        const bool pad2 = c2 == '=';
        const bool pad3 = c3 == '=';
        if ((!last && (pad2 || pad3)) || (pad2 && !pad3)) {
            return {};
        }
        const int v2 = pad2 ? 0 : decodeChar(c2);
        const int v3 = pad3 ? 0 : decodeChar(c3);
        if (v2 < 0 || v3 < 0) {
            return {};
        }
        if ((pad2 && (v1 & 0x0f) != 0) || (pad3 && !pad2 && (v2 & 0x03) != 0)) {
            return {};
        }

        const std::uint32_t triple = (static_cast<std::uint32_t>(v0) << 18) | (static_cast<std::uint32_t>(v1) << 12) |
                                     (static_cast<std::uint32_t>(v2) << 6) | static_cast<std::uint32_t>(v3);
        out.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xff));
        if (!pad2) {
            out.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xff));
        }
        if (!pad3) {
            out.push_back(static_cast<std::uint8_t>(triple & 0xff));
        }
    }
    return out;
}

}  // namespace spark

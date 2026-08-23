#include "core/config/trusted_viewers.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "core/util/state_file.h"

namespace spark {

namespace {

constexpr std::size_t KMaxTrustedViewerFileBytes = 4U * 1024U * 1024U;
constexpr std::size_t KMaxTrustedViewerKeys = 1024;
constexpr std::size_t KMaxTrustedViewerKeyBytes = 16U * 1024U;

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            }
            else {
                out += ch;
            }
            break;
        }
    }
    return out;
}

// Minimal JSON string-array parser: accepts ["...", "...", ...] with optional whitespace.
bool parseStringArray(const std::string &text, std::vector<std::string> &out)
{
    std::size_t pos = 0;
    auto skip_ws = [&]() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
            ++pos;
        }
    };
    skip_ws();
    if (pos >= text.size() || text[pos] != '[') {
        return false;
    }
    ++pos;
    skip_ws();
    if (pos < text.size() && text[pos] == ']') {
        ++pos;
        skip_ws();
        return pos == text.size();
    }
    while (pos < text.size()) {
        skip_ws();
        if (pos >= text.size() || text[pos] != '"') {
            return false;
        }
        ++pos;
        std::string str;
        bool terminated = false;
        while (pos < text.size()) {
            char ch = text[pos++];
            if (ch == '"') {
                terminated = true;
                break;
            }
            if (ch == '\\' && pos < text.size()) {
                char esc = text[pos++];
                switch (esc) {
                case '"':
                    str += '"';
                    break;
                case '\\':
                    str += '\\';
                    break;
                case '/':
                    str += '/';
                    break;
                case 'b':
                    str += '\b';
                    break;
                case 'f':
                    str += '\f';
                    break;
                case 'n':
                    str += '\n';
                    break;
                case 'r':
                    str += '\r';
                    break;
                case 't':
                    str += '\t';
                    break;
                case 'u':
                    if (pos + 4 > text.size()) {
                        return false;
                    }
                    str += '?';
                    pos += 4;
                    break;
                default:
                    return false;
                }
            }
            else {
                str += ch;
            }
            if (str.size() > KMaxTrustedViewerKeyBytes) {
                return false;
            }
        }
        if (!terminated || str.size() > KMaxTrustedViewerKeyBytes) {
            return false;
        }
        if (out.size() < KMaxTrustedViewerKeys) {
            out.push_back(std::move(str));
        }
        skip_ws();
        if (pos >= text.size()) {
            return false;
        }
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == ']') {
            ++pos;
            skip_ws();
            return pos == text.size();
        }
        return false;
    }
    return false;
}

}  // namespace

TrustedViewersState::TrustedViewersState(std::filesystem::path file) : file_(std::move(file)) {}

bool TrustedViewersState::load()
{
    last_error_.clear();
    keys_.clear();

    std::string text;
    if (!readStateFile(file_, KMaxTrustedViewerFileBytes, text, last_error_)) {
        return false;
    }

    if (!parseStringArray(text, keys_)) {
        last_error_ = "Malformed JSON in trusted-viewers file";
        keys_.clear();
        return false;
    }
    return true;
}

bool TrustedViewersState::save() const
{
    last_error_.clear();

    std::ostringstream ss;
    ss << "[\n";
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        ss << "  \"" << jsonEscape(keys_[i]) << "\"";
        if (i + 1 < keys_.size()) {
            ss << ",";
        }
        ss << "\n";
    }
    ss << "]\n";

    const std::string text = ss.str();
    return writeStateFileAtomically(file_, text, last_error_);
}

bool TrustedViewersState::contains(const std::string &b64_key) const
{
    return std::ranges::find(keys_, b64_key) != keys_.end();
}

void TrustedViewersState::add(const std::string &b64_key)
{
    if (b64_key.size() > KMaxTrustedViewerKeyBytes || keys_.size() >= KMaxTrustedViewerKeys) {
        return;
    }
    if (!contains(b64_key)) {
        keys_.push_back(b64_key);
    }
}

}  // namespace spark

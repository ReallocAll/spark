#include "core/activity/activity_log.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>

#include "core/util/monotonic_time.h"
#include "core/util/state_file.h"

namespace spark {

namespace {

constexpr std::int64_t KUrlExpiryMs = 60LL * 24 * 3600 * 1000;  // 60 days
constexpr std::size_t KMaxActivityFileBytes = 8U * 1024U * 1024U;
constexpr std::size_t KMaxJsonDepth = 64;

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

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

// Minimal recursive-descent JSON parser for the known activity-log structure.
struct JsonValue {
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::vector<std::pair<std::string, JsonValue>> obj_val;

    [[nodiscard]] const JsonValue *find(std::string_view key) const
    {
        for (const auto &[k, v] : obj_val) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : text_(text) {}

    bool parse(JsonValue &out)
    {
        skipWs();
        if (!parseValue(out)) {
            return false;
        }
        skipWs();
        return pos_ == text_.size();
    }

private:
    void skipWs()
    {
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                ++pos_;
            }
            else {
                break;
            }
        }
    }

    bool parseValue(JsonValue &out)  // NOLINT(misc-no-recursion)
    {
        skipWs();
        if (pos_ >= text_.size()) {
            return false;
        }
        char ch = text_[pos_];
        if (ch == '{') {
            return parseObject(out);
        }
        if (ch == '[') {
            return parseArray(out);
        }
        if (ch == '"') {
            return parseString(out);
        }
        if (ch == 't' || ch == 'f') {
            return parseBool(out);
        }
        if (ch == 'n') {
            return parseNull(out);
        }
        return parseNumber(out);
    }

    bool parseObject(JsonValue &out)  // NOLINT(misc-no-recursion)
    {
        DepthGuard depth(depth_);
        if (!depth) {
            return false;
        }
        out.type = JsonValue::Type::Object;
        ++pos_;
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < text_.size()) {
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return false;
            }
            JsonValue key;
            if (!parseString(key)) {
                return false;
            }
            skipWs();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!parseValue(val)) {
                return false;
            }
            out.obj_val.emplace_back(key.str_val, std::move(val));
            skipWs();
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool parseArray(JsonValue &out)  // NOLINT(misc-no-recursion)
    {
        DepthGuard depth(depth_);
        if (!depth) {
            return false;
        }
        out.type = JsonValue::Type::Array;
        ++pos_;
        skipWs();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < text_.size()) {
            JsonValue val;
            if (!parseValue(val)) {
                return false;
            }
            out.arr_val.push_back(std::move(val));
            skipWs();
            if (pos_ >= text_.size()) {
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool parseString(JsonValue &out)
    {
        out.type = JsonValue::Type::String;
        ++pos_;
        out.str_val.clear();
        while (pos_ < text_.size()) {
            char ch = text_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (pos_ >= text_.size()) {
                    return false;
                }
                char esc = text_[pos_++];
                switch (esc) {
                case '"':
                    out.str_val += '"';
                    break;
                case '\\':
                    out.str_val += '\\';
                    break;
                case '/':
                    out.str_val += '/';
                    break;
                case 'b':
                    out.str_val += '\b';
                    break;
                case 'f':
                    out.str_val += '\f';
                    break;
                case 'n':
                    out.str_val += '\n';
                    break;
                case 'r':
                    out.str_val += '\r';
                    break;
                case 't':
                    out.str_val += '\t';
                    break;
                case 'u':
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    out.str_val += '?';
                    pos_ += 4;
                    break;
                default:
                    return false;
                }
            }
            else {
                out.str_val += ch;
            }
        }
        return false;
    }

    bool parseBool(JsonValue &out)
    {
        out.type = JsonValue::Type::Bool;
        if (text_.compare(pos_, 4, "true") == 0) {
            out.bool_val = true;
            pos_ += 4;
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            out.bool_val = false;
            pos_ += 5;
            return true;
        }
        return false;
    }

    bool parseNull(JsonValue &out)
    {
        out.type = JsonValue::Type::Null;
        if (text_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return true;
        }
        return false;
    }

    bool parseNumber(JsonValue &out)
    {
        out.type = JsonValue::Type::Number;
        std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        bool has_digit = false;
        while (pos_ < text_.size()) {
            char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
                ++pos_;
                has_digit = true;
            }
            else {
                break;
            }
        }
        if (!has_digit) {
            return false;
        }
        try {
            out.num_val = std::stod(text_.substr(start, pos_ - start));
        }
        catch (...) {
            return false;
        }
        return true;
    }

    const std::string &text_;
    std::size_t pos_ = 0;
    std::size_t depth_ = 0;

    class DepthGuard {
    public:
        explicit DepthGuard(std::size_t &depth) : depth_(depth), entered_(depth_ < KMaxJsonDepth)
        {
            if (entered_) {
                ++depth_;
            }
        }

        ~DepthGuard()
        {
            if (entered_) {
                --depth_;
            }
        }

        explicit operator bool() const { return entered_; }

    private:
        std::size_t &depth_;
        bool entered_;
    };
};

bool parseActivity(const JsonValue &elem, Activity &out)
{
    if (elem.type != JsonValue::Type::Object) {
        return false;
    }
    const JsonValue *user = elem.find("user");
    const JsonValue *time = elem.find("time");
    const JsonValue *type = elem.find("type");
    const JsonValue *data = elem.find("data");
    if (!user || !time || !type || !data) {
        return false;
    }
    if (user->type != JsonValue::Type::Object) {
        return false;
    }
    if (time->type != JsonValue::Type::Number) {
        return false;
    }
    if (type->type != JsonValue::Type::String) {
        return false;
    }
    if (data->type != JsonValue::Type::Object) {
        return false;
    }

    const JsonValue *name = user->find("name");
    const JsonValue *is_player = user->find("isPlayer");
    const JsonValue *unique_id = user->find("uniqueId");
    if (!name || name->type != JsonValue::Type::String) {
        return false;
    }
    if (!is_player || is_player->type != JsonValue::Type::Bool) {
        return false;
    }
    if (unique_id && unique_id->type != JsonValue::Type::String) {
        return false;
    }

    const JsonValue *data_type = data->find("type");
    const JsonValue *data_value = data->find("value");
    if (!data_type || data_type->type != JsonValue::Type::String) {
        return false;
    }
    if (!data_value || data_value->type != JsonValue::Type::String) {
        return false;
    }

    out.user_name = name->str_val;
    out.user_is_player = is_player->bool_val;
    out.user_unique_id = out.user_is_player && unique_id ? unique_id->str_val : std::string{};
    out.time_ms = static_cast<std::int64_t>(time->num_val);
    out.type = type->str_val;
    out.data_type = (data_type->str_val == "url") ? Activity::DataType::Url : Activity::DataType::File;
    out.data_value = data_value->str_val;
    return true;
}

}  // namespace

Activity Activity::url(std::string user_name, bool user_is_player, std::int64_t time_ms, std::string type,
                       std::string url, std::string user_unique_id)
{
    Activity a;
    a.user_name = std::move(user_name);
    a.user_is_player = user_is_player;
    a.user_unique_id = user_is_player ? std::move(user_unique_id) : std::string{};
    a.time_ms = time_ms;
    a.type = std::move(type);
    a.data_type = DataType::Url;
    a.data_value = std::move(url);
    return a;
}

Activity Activity::file(std::string user_name, bool user_is_player, std::int64_t time_ms, std::string type,
                        std::string path, std::string user_unique_id)
{
    Activity a;
    a.user_name = std::move(user_name);
    a.user_is_player = user_is_player;
    a.user_unique_id = user_is_player ? std::move(user_unique_id) : std::string{};
    a.time_ms = time_ms;
    a.type = std::move(type);
    a.data_type = DataType::File;
    a.data_value = std::move(path);
    return a;
}

bool Activity::shouldExpire(std::int64_t now_ms) const
{
    if (data_type == DataType::Url) {
        return (now_ms - time_ms) > KUrlExpiryMs;
    }
    return false;
}

std::string Activity::serialize() const
{
    std::ostringstream ss;
    ss << R"({"user":{"name":")" << jsonEscape(user_name) << R"(","isPlayer":)" << (user_is_player ? "true" : "false");
    if (user_is_player && !user_unique_id.empty()) {
        ss << R"(,"uniqueId":")" << jsonEscape(user_unique_id) << '"';
    }
    ss << "},\"time\":" << time_ms << R"(,"type":")" << jsonEscape(type) << "\""
       << R"(,"data":{"type":")" << (data_type == DataType::Url ? "url" : "file") << R"(","value":")"
       << jsonEscape(data_value) << "\"}}";
    return ss.str();
}

bool Activity::deserialize(const std::string &json, Activity &out)
{
    JsonParser parser(json);
    JsonValue root;
    if (!parser.parse(root)) {
        return false;
    }
    return parseActivity(root, out);
}

ActivityLog::ActivityLog(std::filesystem::path file) : file_(std::move(file)) {}

void ActivityLog::add(const Activity &activity)
{
    entries_.insert(entries_.begin(), activity);
    if (entries_.size() > kMaxEntries) {
        entries_.resize(kMaxEntries);
    }
    save();
}

std::vector<Activity> ActivityLog::entries() const
{
    return entries_;
}

void ActivityLog::load()
{
    entries_.clear();
    std::string text;
    std::string ignored_error;
    if (!readStateFile(file_, KMaxActivityFileBytes, text, ignored_error)) {
        return;
    }

    JsonParser parser(text);
    JsonValue root;
    if (!parser.parse(root) || root.type != JsonValue::Type::Array) {
        return;
    }

    bool need_save = false;
    const std::int64_t now_ms = nowMs();
    for (const JsonValue &elem : root.arr_val) {
        Activity activity;
        if (!parseActivity(elem, activity)) {
            continue;
        }
        if (activity.shouldExpire(now_ms)) {
            need_save = true;
            continue;
        }
        if (entries_.size() >= kMaxEntries) {
            need_save = true;
            continue;
        }
        entries_.push_back(std::move(activity));
    }

    if (need_save) {
        save();
    }
}

void ActivityLog::save() const
{
    std::ostringstream ss;
    if (entries_.empty()) {
        ss << "[]\n";
    }
    else {
        ss << "[\n";
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const Activity &a = entries_[i];
            ss << "  {\n";
            ss << "    \"user\": {\n";
            ss << R"(      "name": ")" << jsonEscape(a.user_name) << "\",\n";
            ss << "      \"isPlayer\": " << (a.user_is_player ? "true" : "false");
            if (a.user_is_player && !a.user_unique_id.empty()) {
                ss << ",\n";
                ss << R"(      "uniqueId": ")" << jsonEscape(a.user_unique_id) << "\"\n";
            }
            else {
                ss << "\n";
            }
            ss << "    },\n";
            ss << "    \"time\": " << a.time_ms << ",\n";
            ss << R"(    "type": ")" << jsonEscape(a.type) << "\",\n";
            ss << "    \"data\": {\n";
            ss << R"(      "type": ")" << (a.data_type == Activity::DataType::Url ? "url" : "file") << "\",\n";
            ss << R"(      "value": ")" << jsonEscape(a.data_value) << "\"\n";
            ss << "    }\n";
            ss << "  }";
            if (i + 1 < entries_.size()) {
                ss << ",";
            }
            ss << "\n";
        }
        ss << "]\n";
    }

    const std::string text = ss.str();
    std::string ignored_error;
    writeStateFileAtomically(file_, text, ignored_error);
}

}  // namespace spark

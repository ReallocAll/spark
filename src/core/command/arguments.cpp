#include "core/command/arguments.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace spark {

namespace {

bool isFlag(const std::string &token)
{
    return token.size() > 2 && token.starts_with("--");
}

std::string lowerCase(std::string value)
{
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

}  // namespace

std::vector<std::string> Arguments::tokenize(const std::string &line)
{
    std::vector<std::string> tokens;
    std::string token;
    char quote = '\0';
    for (char ch : line) {
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            else {
                token.push_back(ch);
            }
        }
        else if (ch == '\'' || ch == '"') {
            quote = ch;
        }
        else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!token.empty()) {
                tokens.push_back(std::move(token));
                token.clear();
            }
        }
        else {
            token.push_back(ch);
        }
    }
    if (!token.empty()) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

Arguments::Arguments(const std::vector<std::string> &tokens, bool allow_subcommand) : raw_(tokens)
{
    std::size_t i = 0;
    std::string flag;
    std::vector<std::string> values;

    const auto store = [this](const std::string &name, const std::vector<std::string> &flag_values) {
        std::string value;
        for (std::size_t i = 0; i < flag_values.size(); ++i) {
            if (i != 0) {
                value += ' ';
            }
            value += flag_values[i];
        }

        present_.insert(name);
        const auto range = values_.equal_range(name);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == value) {
                return;
            }
        }
        values_.emplace(name, std::move(value));
    };

    for (; i < raw_.size(); ++i) {
        const std::string &token = raw_[i];
        if (i == 0 && allow_subcommand && !isFlag(token)) {
            sub_ = token;
            continue;
        }
        if (isFlag(token)) {
            if (!flag.empty()) {
                store(flag, values);
            }
            flag = lowerCase(token.substr(2));
            values.clear();
            continue;
        }
        if (flag.empty()) {
            throw ParseError("Expected flag at position " + std::to_string(i) + " but got '" + token + "' instead!");
        }
        values.push_back(token);
    }
    if (!flag.empty()) {
        store(flag, values);
    }
}

bool Arguments::boolFlag(const std::string &name) const
{
    return present_.contains(lowerCase(name));
}

std::optional<std::int64_t> Arguments::intFlag(const std::string &name) const
{
    auto it = values_.find(lowerCase(name));
    if (it == values_.end()) {
        return std::nullopt;
    }
    const std::string &text = it->second;
    std::int64_t value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    if (value == std::numeric_limits<std::int64_t>::min()) {
        return value;
    }
    return value < 0 ? -value : value;
}

std::optional<double> Arguments::doubleFlag(const std::string &name) const
{
    auto it = values_.find(lowerCase(name));
    if (it == values_.end()) {
        return std::nullopt;
    }
    const std::string &text = it->second;
    double value = 0.0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return std::abs(value);
}

std::vector<std::string> Arguments::stringFlag(const std::string &name) const
{
    std::vector<std::string> out;
    auto range = values_.equal_range(lowerCase(name));
    for (auto it = range.first; it != range.second; ++it) {
        out.push_back(it->second);
    }
    return out;
}

}  // namespace spark

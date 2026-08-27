#include "core/metadata/server_properties.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace spark {

namespace {

// Known-safe diagnostics from the current Bedrock Dedicated Server
// server.properties reference. Identity-bearing values (server/world names),
// seeds, debugger endpoints and credentials are intentionally absent. Existing
// conservative exclusions such as the default game mode and listen ports are
// also preserved; administrators can opt those non-sensitive keys in explicitly.
constexpr std::array<std::string_view, 44> KKnownSafeProperties = {
    // World/session configuration
    "force-gamemode",
    "difficulty",
    "allow-cheats",
    "max-players",
    "online-mode",
    "allow-list",
    "view-distance",
    "tick-distance",
    "player-idle-timeout",
    "max-threads",
    "default-player-permission-level",
    "texturepack-required",
    // Network diagnostics
    "enable-lan-visibility",
    "compression-threshold",
    "compression-algorithm",
    // Server-authoritative simulation and movement
    "server-authoritative-movement-strict",
    "server-authoritative-dismount-strict",
    "server-authoritative-entity-interactions-strict",
    "player-position-acceptance-threshold",
    "player-movement-action-direction-threshold",
    "server-authoritative-block-breaking",
    "server-authoritative-block-breaking-range-scalar",
    // Client/world simulation behavior
    "chat-restriction",
    "disable-player-interaction",
    "client-side-chunk-generation-enabled",
    "block-network-ids-are-hashes",
    "disable-custom-skins",
    "server-build-radius-ratio",
    "disable-client-vibrant-visuals",
    // Logging and diagnostics capture
    "content-log-file-enabled",
    "content-log-console-output-enabled",
    "content-log-level",
    "diagnostics-capture-auto-start",
    "diagnostics-capture-max-files",
    "diagnostics-capture-max-file-size",
    // Script watchdog
    "script-watchdog-enable",
    "script-watchdog-enable-exception-handling",
    "script-watchdog-enable-shutdown",
    "script-watchdog-hang-exception",
    "script-watchdog-hang-threshold",
    "script-watchdog-spike-threshold",
    "script-watchdog-slow-threshold",
    "script-watchdog-memory-warning",
    "script-watchdog-memory-limit",
};

constexpr std::array<std::string_view, 5> KKnownSensitiveProperties = {
    "server-name",
    "level-name",
    "level-seed",
    "script-debugger-auto-attach-connect-address",
    "script-debugger-passcode",
};

constexpr std::array<std::string_view, 6> KSensitiveKeyFragments = {
    "password", "passcode", "token", "secret", "credential", "private-key",
};

std::mutex GAdditionalSafeKeysMutex;
std::vector<std::string> GAdditionalSafeKeys;

std::string lowerAscii(std::string_view value)
{
    std::string result(value);
    for (char &ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

bool isSensitiveKey(std::string_view key)
{
    const std::string normalized = lowerAscii(key);
    if (std::ranges::any_of(KKnownSensitiveProperties,
                            [&normalized](std::string_view candidate) { return normalized == candidate; })) {
        return true;
    }
    return std::ranges::any_of(KSensitiveKeyFragments, [&normalized](std::string_view fragment) {
        return normalized.find(fragment) != std::string::npos;
    });
}

bool containsKey(const std::vector<std::string> &keys, std::string_view key)
{
    return std::ranges::any_of(keys, [key](const std::string &candidate) { return key == candidate; });
}

bool isAllowlisted(std::string_view key, const std::vector<std::string> &additional_safe_keys)
{
    if (isSensitiveKey(key)) {
        return false;
    }
    if (std::ranges::any_of(KKnownSafeProperties, [key](std::string_view candidate) { return key == candidate; }) ||
        containsKey(additional_safe_keys, key)) {
        return true;
    }

    const std::scoped_lock lock(GAdditionalSafeKeysMutex);
    return containsKey(GAdditionalSafeKeys, key);
}

std::string trim(std::string_view s)
{
    std::size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

bool isAllDigits(std::string_view s)
{
    if (s.empty()) {
        return false;
    }
    return std::ranges::all_of(s, [](char value) { return value >= '0' && value <= '9'; });
}

bool isCanonicalUnsignedInteger(std::string_view value)
{
    return isAllDigits(value) && (value.size() == 1 || value.front() != '0');
}

void appendJsonString(std::string &out, std::string_view value)
{
    out += '"';
    for (char c : value) {
        switch (c) {
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
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

}  // namespace

void setAdditionalSafeServerPropertyKeys(std::vector<std::string> keys)
{
    const std::scoped_lock lock(GAdditionalSafeKeysMutex);
    GAdditionalSafeKeys = std::move(keys);
}

std::map<std::string, std::string> parseServerProperties(const std::filesystem::path &file,
                                                         const std::vector<std::string> &additional_safe_keys)
{
    std::map<std::string, std::string> result;
    if (!std::filesystem::exists(file)) {
        return result;
    }

    std::ifstream in(file);
    if (!in) {
        return result;
    }

    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (key.empty() || !isAllowlisted(key, additional_safe_keys)) {
            continue;
        }
        result[key] = value;
    }

    return result;
}

std::string serverPropertiesToJsonString(const std::map<std::string, std::string> &properties)
{
    std::string out;
    out += '{';
    bool first = true;
    for (const auto &[key, value] : properties) {
        if (!first) {
            out += ',';
        }
        first = false;
        appendJsonString(out, key);
        out += ':';
        if (value == "true" || value == "false" || isCanonicalUnsignedInteger(value)) {
            out += value;
        }
        else {
            appendJsonString(out, value);
        }
    }
    out += '}';
    return out;
}

}  // namespace spark

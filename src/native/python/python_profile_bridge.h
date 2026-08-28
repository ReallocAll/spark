#ifndef ENDSTONE_SPARK_PYTHON_PROFILE_BRIDGE_H
#define ENDSTONE_SPARK_PYTHON_PROFILE_BRIDGE_H

#include <atomic>
#include <string>
#include <string_view>

#include "native/python/python_attribution.h"

namespace spark {

inline std::atomic<PythonStackProvider *> &globalPythonStackProviderSlot() noexcept
{
    static std::atomic<PythonStackProvider *> provider{nullptr};
    return provider;
}

inline void setGlobalPythonStackProvider(PythonStackProvider *provider) noexcept
{
    globalPythonStackProviderSlot().store(provider, std::memory_order_release);
}

inline PythonStackProvider *globalPythonStackProvider() noexcept
{
    return globalPythonStackProviderSlot().load(std::memory_order_acquire);
}

inline std::string pythonFrameClassName(const PythonCodeMetadata &metadata)
{
    std::string name = "[Python] ";
    name += metadata.module.empty() ? "<unknown>" : metadata.module;
    return name;
}

inline std::string pythonJsonString(std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char ch : value) {
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
            if (ch < 0x20) {
                out += "\\u00";
                out.push_back(kHex[(ch >> 4) & 0x0f]);
                out.push_back(kHex[ch & 0x0f]);
            }
            else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace spark

#endif  // ENDSTONE_SPARK_PYTHON_PROFILE_BRIDGE_H

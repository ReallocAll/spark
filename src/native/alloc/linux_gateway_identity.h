#ifndef ENDSTONE_SPARK_LINUX_GATEWAY_IDENTITY_H
#define ENDSTONE_SPARK_LINUX_GATEWAY_IDENTITY_H

#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "native/alloc/linux_loader_handle.h"

namespace spark::gateway {

struct Identity {
    std::string path;
    std::string loader_path;
    std::uintptr_t base = 0;
    dev_t device = 0;
    ino_t inode = 0;
};

inline bool sameFile(const std::string &path, dev_t device, ino_t inode)
{
    struct stat status{};
    return ::stat(path.c_str(), &status) == 0 && status.st_dev == device && status.st_ino == inode;
}

inline std::string decodeMapsPath(const std::string &value)
{
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\') {
            if (i + 3 >= value.size() || value[i + 1] < '0' || value[i + 1] > '7' || value[i + 2] < '0' ||
                value[i + 2] > '7' || value[i + 3] < '0' || value[i + 3] > '7') {
                return {};
            }
            const auto decoded =
                static_cast<char>((value[i + 1] - '0') * 64 + (value[i + 2] - '0') * 8 + value[i + 3] - '0');
            if (decoded == '\0') {
                return {};
            }
            result += decoded;
            i += 3;
        }
        else {
            result += value[i];
        }
    }
    return result;
}

inline bool identify(const void *address, Identity &identity)
{
    struct Query {
        std::uintptr_t address;
        std::uintptr_t bias = 0;
        std::string loader_path;
        std::size_t images = 0;
        bool found = false;
    } query{.address = reinterpret_cast<std::uintptr_t>(address)};
    ::dl_iterate_phdr(
        [](dl_phdr_info *info, std::size_t, void *opaque) noexcept {
            auto &query = *static_cast<Query *>(opaque);
            try {
                if (++query.images > 4096 || info->dlpi_phnum > 4096) {
                    return 1;
                }
                for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
                    const auto &header = info->dlpi_phdr[i];
                    if (header.p_type != PT_LOAD ||
                        header.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - info->dlpi_addr) {
                        continue;
                    }
                    const auto begin = info->dlpi_addr + header.p_vaddr;
                    if (query.address >= begin && query.address - begin < header.p_memsz) {
                        query.loader_path = info->dlpi_name != nullptr ? info->dlpi_name : "";
                        query.bias = info->dlpi_addr;
                        query.found = true;
                        return 1;
                    }
                }
                return 0;
            }
            catch (...) {
                return 1;
            }
        },
        &query);
    if (!query.found) {
        return false;
    }
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::size_t lines = 0;
    std::size_t bytes = 0;
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    while (std::getline(maps, line)) {
        if (++lines > 65536 || line.size() > 64 * 1024 * 1024 - bytes) {
            return false;
        }
        bytes += line.size();
        std::istringstream stream(line);
        std::string range, permissions, offset, device;
        unsigned long long inode = 0;
        if (!(stream >> range >> permissions >> offset >> device >> inode)) {
            continue;
        }
        const auto dash = range.find('-');
        const auto colon = device.find(':');
        if (dash == std::string::npos || colon == std::string::npos || inode == 0) {
            continue;
        }
        const auto begin = std::stoull(range.substr(0, dash), nullptr, 16);
        const auto end = std::stoull(range.substr(dash + 1), nullptr, 16);
        if (value < begin || value >= end) {
            continue;
        }
        std::string mapped;
        std::getline(stream >> std::ws, mapped);
        mapped = decodeMapsPath(mapped);
        if (mapped.empty() || mapped.front() != '/') {
            return false;
        }
        identity.device = makedev(std::stoul(device.substr(0, colon), nullptr, 16),
                                  std::stoul(device.substr(colon + 1), nullptr, 16));
        identity.inode = static_cast<ino_t>(inode);
        std::string candidate =
            !query.loader_path.empty() && query.loader_path.front() == '/' ? query.loader_path : mapped;
        if (!sameFile(candidate, identity.device, identity.inode)) {
            candidate = mapped;
        }
        if (!sameFile(candidate, identity.device, identity.inode)) {
            return false;
        }
        std::error_code error;
        identity.loader_path = candidate;
        identity.path = std::filesystem::canonical(candidate, error).string();
        identity.base = query.bias;
        return !error && !identity.path.empty();
    }
    return false;
}

inline bool mainExecutable(const Identity &identity)
{
    return sameFile("/proc/self/exe", identity.device, identity.inode);
}

inline bool executableAddress(const void *address, const Identity &identity)
{
    struct Query {
        std::uintptr_t address;
        std::uintptr_t base;
        bool main_executable;
        bool found;
    } query{.address = reinterpret_cast<std::uintptr_t>(address),
            .base = identity.base,
            .main_executable = mainExecutable(identity),
            .found = false};
    ::dl_iterate_phdr(
        [](dl_phdr_info *info, std::size_t, void *opaque) {
            auto &query = *static_cast<Query *>(opaque);
            const bool main_executable = info->dlpi_name == nullptr || info->dlpi_name[0] == '\0';
            if (query.main_executable != main_executable ||
                (!main_executable && static_cast<std::uintptr_t>(info->dlpi_addr) != query.base)) {
                return 0;
            }
            for (std::size_t i = 0; i < info->dlpi_phnum; ++i) {
                const auto &header = info->dlpi_phdr[i];
                const auto begin = info->dlpi_addr + header.p_vaddr;
                if (header.p_type == PT_LOAD && (header.p_flags & PF_X) != 0 && query.address >= begin &&
                    query.address - begin < header.p_memsz) {
                    query.found = true;
                    return 1;
                }
            }
            return 0;
        },
        &query);
    return query.found;
}

inline void *lease(const Identity &identity)
{
    LoaderHandle handle(mainExecutable(identity)
                            ? ::dlopen(nullptr, RTLD_NOW | RTLD_LOCAL)
                            : ::dlopen(identity.path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD));
    if (!handle && !identity.loader_path.empty() && identity.loader_path.front() == '/' &&
        sameFile(identity.loader_path, identity.device, identity.inode)) {
        handle.reset(::dlopen(identity.loader_path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD));
    }
    if (!handle) {
        return nullptr;
    }
    link_map *map = nullptr;
    Lmid_t namespace_id = -1;
    if (::dlinfo(handle.get(), RTLD_DI_LINKMAP, &map) != 0 || map == nullptr ||
        ::dlinfo(handle.get(), RTLD_DI_LMID, &namespace_id) != 0 || namespace_id != LM_ID_BASE ||
        (!mainExecutable(identity) && static_cast<std::uintptr_t>(map->l_addr) != identity.base)) {
        return nullptr;
    }
    return handle.release();
}

inline bool installationRoot(const void *anchor, std::string &root, Identity &owner)
{
    if (!identify(anchor, owner)) {
        return false;
    }
    LoaderHandle handle(lease(owner));
    if (!handle) {
        return false;
    }
    const std::filesystem::path path(owner.path);
    if (mainExecutable(owner) || path.filename() == "endstone_spark.so") {
        root = path.parent_path().string();
        return true;
    }
    const auto name = path.filename().string();
    constexpr auto prefix = "endstone_spark-";
    if (path.parent_path().filename() != ".local" || !name.starts_with(prefix) || !name.ends_with(".so") ||
        name.size() <= std::char_traits<char>::length(prefix) + 3) {
        return false;
    }
    const auto installation = path.parent_path().parent_path();
    std::error_code error;
    const auto plugin = std::filesystem::canonical(installation / "endstone_spark.so", error);
    if (error || plugin.parent_path() != installation || !std::filesystem::is_regular_file(plugin)) {
        return false;
    }
    root = installation.string();
    return true;
}

}  // namespace spark::gateway

#endif

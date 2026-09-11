#ifndef ENDSTONE_SPARK_CAPTURE_LINUX_MODULE_H
#define ENDSTONE_SPARK_CAPTURE_LINUX_MODULE_H

#include <link.h>

#include <cstddef>
#include <cstdint>
#include <limits>

#include <sys/auxv.h>

namespace spark::detail {

inline constexpr std::size_t KMainPhdrInfoSize = offsetof(dl_phdr_info, dlpi_phnum) + sizeof(dl_phdr_info::dlpi_phnum);
inline constexpr std::size_t KMaxMainPhdrCount = 65536 / sizeof(ElfW(Phdr));

inline bool mainExecutableCandidateOwnsAddress(const dl_phdr_info &info, std::size_t info_size,
                                               std::uintptr_t main_phdr, std::size_t main_phnum, std::size_t main_phent,
                                               std::uintptr_t address)
{
    constexpr auto max_address = std::numeric_limits<std::uintptr_t>::max();
    if (address == 0 || main_phdr == 0 || main_phnum == 0 || main_phnum > KMaxMainPhdrCount ||
        main_phent != sizeof(ElfW(Phdr)) || info_size < KMainPhdrInfoSize ||
        main_phdr > max_address - main_phnum * sizeof(ElfW(Phdr))) {
        return false;
    }
    if (reinterpret_cast<std::uintptr_t>(info.dlpi_phdr) != main_phdr || info.dlpi_phnum != main_phnum) {
        return false;
    }
    bool owns_address = false;
    for (std::size_t i = 0; i < main_phnum; ++i) {
        const auto &segment = info.dlpi_phdr[i];
        if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0 || segment.p_memsz == 0) {
            continue;
        }
        if (segment.p_vaddr > max_address - info.dlpi_addr) {
            return false;
        }
        const auto begin = info.dlpi_addr + segment.p_vaddr;
        if (segment.p_memsz > max_address - begin) {
            return false;
        }
        owns_address |= address >= begin && address < begin + segment.p_memsz;
    }
    return owns_address;
}

inline bool mainExecutableOwnsAddress(std::uintptr_t address)
{
    struct Search {
        std::uintptr_t phdr;
        std::size_t phnum;
        std::size_t phent;
        std::uintptr_t address;
        bool owns_address = false;
    } search{.phdr = ::getauxval(AT_PHDR),
             .phnum = ::getauxval(AT_PHNUM),
             .phent = ::getauxval(AT_PHENT),
             .address = address};
    ::dl_iterate_phdr(
        [](dl_phdr_info *info, std::size_t size, void *opaque) {
            auto &state = *static_cast<Search *>(opaque);
            if (size < KMainPhdrInfoSize || reinterpret_cast<std::uintptr_t>(info->dlpi_phdr) != state.phdr ||
                info->dlpi_phnum != state.phnum) {
                return 0;
            }
            state.owns_address =
                mainExecutableCandidateOwnsAddress(*info, size, state.phdr, state.phnum, state.phent, state.address);
            return 1;
        },
        &search);
    return search.owns_address;
}

}  // namespace spark::detail

#endif  // ENDSTONE_SPARK_CAPTURE_LINUX_MODULE_H

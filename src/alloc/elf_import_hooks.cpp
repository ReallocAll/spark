#include "alloc/elf_import_hooks.h"

#if !defined(__linux__) || !defined(__x86_64__)
#error "elf_import_hooks.cpp requires Linux x86-64"
#endif

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <elf.h>
#include <fstream>
#include <link.h>
#include <limits>
#include <sstream>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace spark {
namespace {

struct MainImage {
    std::uintptr_t base = 0;
    std::uintptr_t load_begin = 0;
    std::uintptr_t load_end = 0;
    const ElfW(Phdr) *headers = nullptr;
    ElfW(Half) header_count = 0;
};

int findMainImage(dl_phdr_info *info, std::size_t, void *opaque)
{
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        return 0;
    }
    auto &image = *static_cast<MainImage *>(opaque);
    image.base = static_cast<std::uintptr_t>(info->dlpi_addr);
    image.headers = info->dlpi_phdr;
    image.header_count = info->dlpi_phnum;
    image.load_begin = (std::numeric_limits<std::uintptr_t>::max)();
    image.load_end = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &header = info->dlpi_phdr[i];
        if (header.p_type != PT_LOAD) {
            continue;
        }
        image.load_begin = (std::min)(image.load_begin,
                                      image.base + static_cast<std::uintptr_t>(header.p_vaddr));
        image.load_end = (std::max)(image.load_end,
                                    image.base + static_cast<std::uintptr_t>(header.p_vaddr + header.p_memsz));
    }
    return 1;
}

std::uintptr_t dynamicPointer(const MainImage &image, ElfW(Addr) value)
{
    const auto address = static_cast<std::uintptr_t>(value);
    if (address >= image.load_begin && address < image.load_end) {
        return address;
    }
    return image.base + address;
}

bool supportedRelocation(unsigned type) noexcept
{
    return type == R_X86_64_JUMP_SLOT || type == R_X86_64_GLOB_DAT ||
           type == R_X86_64_64;
}

int protectionForAddress(std::uintptr_t address)
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream parser(line);
        std::string range;
        std::string permissions;
        if (!(parser >> range >> permissions)) {
            continue;
        }
        const std::size_t separator = range.find('-');
        if (separator == std::string::npos) {
            continue;
        }
        const std::uintptr_t begin = std::stoull(range.substr(0, separator), nullptr, 16);
        const std::uintptr_t end = std::stoull(range.substr(separator + 1), nullptr, 16);
        if (address < begin || address >= end) {
            continue;
        }
        int protection = 0;
        protection |= permissions.size() > 0 && permissions[0] == 'r' ? PROT_READ : 0;
        protection |= permissions.size() > 1 && permissions[1] == 'w' ? PROT_WRITE : 0;
        protection |= permissions.size() > 2 && permissions[2] == 'x' ? PROT_EXEC : 0;
        return protection;
    }
    return -1;
}

std::string systemError(const char *operation)
{
    return std::string(operation) + " failed: " + std::strerror(errno);
}

}  // namespace

bool ElfImportHooks::prepare(std::span<const ElfImportHookSpec> specs, std::string &error)
{
    error.clear();
    if (prepared_) {
        return true;
    }
    if (specs.empty()) {
        error = "no ELF import hooks were requested";
        return false;
    }

    MainImage image;
    if (::dl_iterate_phdr(findMainImage, &image) == 0 || image.headers == nullptr) {
        error = "could not locate the Linux main executable image";
        return false;
    }

    const ElfW(Dyn) *dynamic = nullptr;
    for (ElfW(Half) i = 0; i < image.header_count; ++i) {
        if (image.headers[i].p_type == PT_DYNAMIC) {
            dynamic = reinterpret_cast<const ElfW(Dyn) *>(
                image.base + static_cast<std::uintptr_t>(image.headers[i].p_vaddr));
            break;
        }
    }
    if (dynamic == nullptr) {
        error = "the Linux main executable has no PT_DYNAMIC segment";
        return false;
    }

    const ElfW(Sym) *symbols = nullptr;
    const char *strings = nullptr;
    const ElfW(Rela) *rela = nullptr;
    std::size_t rela_size = 0;
    const ElfW(Rela) *jmprel = nullptr;
    std::size_t jmprel_size = 0;
    ElfW(Sword) plt_type = DT_RELA;
    for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; ++entry) {
        switch (entry->d_tag) {
        case DT_SYMTAB:
            symbols = reinterpret_cast<const ElfW(Sym) *>(dynamicPointer(image, entry->d_un.d_ptr));
            break;
        case DT_STRTAB:
            strings = reinterpret_cast<const char *>(dynamicPointer(image, entry->d_un.d_ptr));
            break;
        case DT_RELA:
            rela = reinterpret_cast<const ElfW(Rela) *>(dynamicPointer(image, entry->d_un.d_ptr));
            break;
        case DT_RELASZ:
            rela_size = static_cast<std::size_t>(entry->d_un.d_val);
            break;
        case DT_JMPREL:
            jmprel = reinterpret_cast<const ElfW(Rela) *>(dynamicPointer(image, entry->d_un.d_ptr));
            break;
        case DT_PLTRELSZ:
            jmprel_size = static_cast<std::size_t>(entry->d_un.d_val);
            break;
        case DT_PLTREL:
            plt_type = static_cast<ElfW(Sword)>(entry->d_un.d_val);
            break;
        default:
            break;
        }
    }
    if (symbols == nullptr || strings == nullptr || plt_type != DT_RELA) {
        error = "unsupported Linux dynamic relocation layout";
        return false;
    }

    capabilities_.reserve(specs.size());
    for (const ElfImportHookSpec &spec : specs) {
        capabilities_.push_back({spec.name != nullptr ? spec.name : "", false, 0, {}});
    }

    auto visit = [&](const ElfW(Rela) *entries, std::size_t bytes) {
        if (entries == nullptr) {
            return;
        }
        const std::size_t count = bytes / sizeof(ElfW(Rela));
        for (std::size_t i = 0; i < count; ++i) {
            const ElfW(Rela) &relocation = entries[i];
            const unsigned type = static_cast<unsigned>(ELF64_R_TYPE(relocation.r_info));
            if (!supportedRelocation(type)) {
                continue;
            }
            const std::size_t symbol_index = ELF64_R_SYM(relocation.r_info);
            const char *name = strings + symbols[symbol_index].st_name;
            for (std::size_t spec_index = 0; spec_index < specs.size(); ++spec_index) {
                const ElfImportHookSpec &spec = specs[spec_index];
                if (spec.name == nullptr || std::strcmp(name, spec.name) != 0) {
                    continue;
                }
                auto **slot = reinterpret_cast<void **>(
                    image.base + static_cast<std::uintptr_t>(relocation.r_offset));
                if ((reinterpret_cast<std::uintptr_t>(slot) % alignof(void *)) != 0) {
                    capabilities_[spec_index].detail = "unaligned import slot";
                    continue;
                }
                const bool duplicate = std::any_of(
                    targets_.begin(), targets_.end(),
                    [slot](const Target &target) { return target.slot == slot; });
                if (!duplicate) {
                    targets_.push_back({slot, __atomic_load_n(slot, __ATOMIC_ACQUIRE),
                                        spec.replacement});
                    ++capabilities_[spec_index].slots;
                }
            }
        }
    };
    visit(rela, rela_size);
    visit(jmprel, jmprel_size);

    const long page_size_value = ::sysconf(_SC_PAGESIZE);
    if (page_size_value <= 0) {
        error = "sysconf(_SC_PAGESIZE) failed";
        targets_.clear();
        capabilities_.clear();
        return false;
    }
    const std::uintptr_t page_size = static_cast<std::uintptr_t>(page_size_value);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        ElfImportHookCapability &capability = capabilities_[i];
        capability.available = capability.slots != 0;
        if (!capability.available && capability.detail.empty()) {
            capability.detail = "import not found";
        }
        if (!capability.available && specs[i].required) {
            error = "required Linux allocator import not found: " + capability.name;
            targets_.clear();
            pages_.clear();
            return false;
        }
    }
    for (const Target &target : targets_) {
        const auto page_address = reinterpret_cast<std::uintptr_t>(target.slot) & ~(page_size - 1);
        if (std::none_of(pages_.begin(), pages_.end(), [page_address](const Page &page) {
                return reinterpret_cast<std::uintptr_t>(page.address) == page_address;
            })) {
            const int protection = protectionForAddress(reinterpret_cast<std::uintptr_t>(target.slot));
            if (protection < 0) {
                error = "could not determine protection for Linux import slot";
                targets_.clear();
                pages_.clear();
                return false;
            }
            pages_.push_back({reinterpret_cast<void *>(page_address), protection});
        }
    }
    prepared_ = true;
    return true;
}

bool ElfImportHooks::patch(bool replacements, std::string &error)
{
    error.clear();
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        error = "sysconf(_SC_PAGESIZE) failed";
        return false;
    }

    std::size_t writable_pages = 0;
    for (const Page &page : pages_) {
        if ((page.protection & PROT_WRITE) == 0 &&
            ::mprotect(page.address, static_cast<std::size_t>(page_size),
                       page.protection | PROT_WRITE) != 0) {
            error = systemError("mprotect writable");
            break;
        }
        ++writable_pages;
    }
    if (writable_pages != pages_.size()) {
        for (std::size_t i = 0; i < writable_pages; ++i) {
            if ((pages_[i].protection & PROT_WRITE) == 0) {
                ::mprotect(pages_[i].address, static_cast<std::size_t>(page_size),
                           pages_[i].protection);
            }
        }
        return false;
    }

    for (const Target &target : targets_) {
        __atomic_store_n(target.slot,
                         replacements ? target.replacement : target.original,
                         __ATOMIC_RELEASE);
    }

    bool restored = true;
    for (const Page &page : pages_) {
        if ((page.protection & PROT_WRITE) == 0 &&
            ::mprotect(page.address, static_cast<std::size_t>(page_size),
                       page.protection) != 0) {
            if (error.empty()) {
                error = systemError("mprotect restore");
            }
            restored = false;
        }
    }
    return restored;
}

bool ElfImportHooks::install(std::string &error)
{
    if (!prepared_) {
        error = "ELF import hooks have not been prepared";
        return false;
    }
    if (installed_) {
        error.clear();
        return true;
    }
    if (!patch(true, error)) {
        std::string rollback_error;
        if (!patch(false, rollback_error)) {
            std::abort();
        }
        if (!rollback_error.empty()) {
            error += "; rollback: " + rollback_error;
        }
        return false;
    }
    installed_ = true;
    return true;
}

bool ElfImportHooks::uninstall(std::string &error)
{
    if (!installed_) {
        error.clear();
        return true;
    }
    if (!patch(false, error)) {
        // All slots have already been restored atomically; only page-protection
        // restoration can fail after that point. The caller must treat this as
        // strict-cleanup failure, but no import slot still references plugin code.
        installed_ = false;
        return false;
    }
    installed_ = false;
    return true;
}

}  // namespace spark

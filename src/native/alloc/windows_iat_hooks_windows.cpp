#include "native/alloc/windows_iat_hooks.h"

#ifndef _WIN32
#error "windows_iat_hooks_windows.cpp must only be compiled on Windows"
#endif

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off: Windows SDK headers require windows.h first.
#include <windows.h>
#include <psapi.h>
// clang-format on

namespace spark {
namespace {

constexpr std::size_t KInitialModuleCapacity = 256;
constexpr std::size_t KMaxModuleCapacity = 4096;

void nativeBackendAnchor() {}

void setErrorNoexcept(std::string &error, const char *operation, DWORD code) noexcept
{
    try {
        error = std::string(operation) + " failed: " + std::to_string(code);
    }
    catch (...) {
        error.clear();
    }
}

bool spanInside(std::uint32_t offset, std::size_t length, std::uint32_t image_size) noexcept
{
    return offset <= image_size && length <= static_cast<std::size_t>(image_size - offset);
}

bool executableProtection(DWORD protection) noexcept
{
    const DWORD base = protection & 0xFFU;
    return base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ || base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

bool equalsIgnoreCase(const char *left, const std::string &right) noexcept
{
    return left != nullptr && ::_stricmp(left, right.c_str()) == 0;
}

bool providerAllowed(const char *provider, const WindowsIatHookTarget &target) noexcept
{
    if (target.import_modules.empty()) {
        return true;
    }
    return std::ranges::any_of(target.import_modules, [provider](const std::string &candidate) {
        return equalsIgnoreCase(provider, candidate);
    });
}

class PinnedModule {
public:
    PinnedModule() = default;
    explicit PinnedModule(HMODULE module) : module_(module) {}
    ~PinnedModule()
    {
        if (module_ != nullptr) {
            ::FreeLibrary(module_);
        }
    }

    PinnedModule(const PinnedModule &) = delete;
    PinnedModule &operator=(const PinnedModule &) = delete;

    PinnedModule(PinnedModule &&other) noexcept : module_(other.module_) { other.module_ = nullptr; }
    PinnedModule &operator=(PinnedModule &&other) noexcept
    {
        if (this != &other) {
            if (module_ != nullptr) {
                ::FreeLibrary(module_);
            }
            module_ = other.module_;
            other.module_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HMODULE get() const noexcept { return module_; }
    [[nodiscard]] explicit operator bool() const noexcept { return module_ != nullptr; }

private:
    HMODULE module_ = nullptr;
};

PinnedModule pinAddress(const void *address) noexcept
{
    HMODULE module = nullptr;
    if (address == nullptr || ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                                   reinterpret_cast<LPCWSTR>(address), &module) == FALSE) {
        return {};
    }
    return PinnedModule(module);
}

bool moduleIdentity(HMODULE module, WindowsIatModuleIdentity &identity, std::string *error = nullptr)
{
    MODULEINFO info{};
    if (::GetModuleInformation(::GetCurrentProcess(), module, &info, sizeof(info)) == FALSE) {
        if (error != nullptr) {
            *error = "GetModuleInformation failed: " + std::to_string(::GetLastError());
        }
        return false;
    }
    if (info.lpBaseOfDll == nullptr || info.SizeOfImage < sizeof(IMAGE_DOS_HEADER)) {
        if (error != nullptr) {
            *error = "loaded module has an invalid image range";
        }
        return false;
    }

    const auto *base = static_cast<const std::byte *>(info.lpBaseOfDll);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        !spanInside(static_cast<std::uint32_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64), info.SizeOfImage)) {
        if (error != nullptr) {
            *error = "loaded module has invalid DOS/NT headers";
        }
        return false;
    }

    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + static_cast<std::uint32_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.SizeOfImage != info.SizeOfImage) {
        if (error != nullptr) {
            *error = "loaded module is not a consistent PE32+ image";
        }
        return false;
    }

    identity = {.base = reinterpret_cast<std::uintptr_t>(base),
                .image_size = info.SizeOfImage,
                .timestamp = nt->FileHeader.TimeDateStamp,
                .checksum = nt->OptionalHeader.CheckSum};
    return true;
}

bool boundedCString(const char *value, std::size_t maximum) noexcept
{
    return value != nullptr && maximum != 0 && std::memchr(value, '\0', maximum) != nullptr;
}

class NativeWindowsIatHookBackend final : public WindowsIatHookBackend {
public:
    explicit NativeWindowsIatHookBackend(void *excluded_address)
    {
        void *anchor = excluded_address != nullptr ? excluded_address : reinterpret_cast<void *>(&nativeBackendAnchor);
        HMODULE module = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(anchor), &module) != FALSE) {
            excluded_module_ = module;
        }
    }

    bool enumerate(const std::vector<WindowsIatHookTarget> &targets, std::vector<WindowsIatSlot> &slots,
                   std::string &error) override
    {
        error.clear();
        slots.clear();

        std::vector<HMODULE> modules(KInitialModuleCapacity);
        DWORD needed = 0;
        for (;;) {
            if (::EnumProcessModulesEx(::GetCurrentProcess(), modules.data(),
                                       static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &needed,
                                       LIST_MODULES_ALL) == FALSE) {
                error = "EnumProcessModulesEx failed: " + std::to_string(::GetLastError());
                return false;
            }
            const std::size_t count = (static_cast<std::size_t>(needed) + sizeof(HMODULE) - 1) / sizeof(HMODULE);
            if (count <= modules.size()) {
                modules.resize(count);
                break;
            }
            if (count > KMaxModuleCapacity) {
                error = "loaded module count exceeds Windows IAT scan capacity";
                return false;
            }
            modules.resize(count);
        }

        for (HMODULE observed : modules) {
            if (observed == nullptr || observed == excluded_module_) {
                continue;
            }
            PinnedModule pinned = pinAddress(observed);
            if (!pinned) {
                continue;  // Unloaded between enumeration and pinning.
            }
            if (pinned.get() == excluded_module_) {
                continue;
            }

            WindowsIatModuleIdentity identity;
            std::string identity_error;
            if (!moduleIdentity(pinned.get(), identity, &identity_error)) {
                error = identity_error;
                return false;
            }
            if (!enumerateModule(pinned.get(), identity, targets, slots, error)) {
                return false;
            }
        }
        return true;
    }

    WindowsIatAccessStatus read(const WindowsIatSlot &slot, void *&value, std::string &error) noexcept override
    {
        error.clear();
        std::uintptr_t *address = nullptr;
        PinnedModule pinned;
        const WindowsIatAccessStatus status = resolveSlot(slot, pinned, address, error);
        if (status != WindowsIatAccessStatus::Accessible) {
            return status;
        }

        std::atomic_ref<std::uintptr_t> atomic_slot(*address);
        value =
            reinterpret_cast<void *>(atomic_slot.load(std::memory_order_acquire));  // NOLINT(performance-no-int-to-ptr)
        return WindowsIatAccessStatus::Accessible;
    }

    WindowsIatExchangeResult compareExchange(const WindowsIatSlot &slot, void *expected, void *desired,
                                             std::string &error) noexcept override
    {
        error.clear();
        std::uintptr_t *address = nullptr;
        PinnedModule pinned;
        const WindowsIatAccessStatus status = resolveSlot(slot, pinned, address, error);
        if (status == WindowsIatAccessStatus::Stale) {
            return {.status = WindowsIatExchangeStatus::Stale, .observed = nullptr};
        }
        if (status == WindowsIatAccessStatus::Error) {
            return {.status = WindowsIatExchangeStatus::Error, .observed = nullptr};
        }

        MEMORY_BASIC_INFORMATION region{};
        if (::VirtualQuery(address, &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT) {
            setErrorNoexcept(error, "VirtualQuery", ::GetLastError());
            return {.status = WindowsIatExchangeStatus::Error, .observed = nullptr};
        }
        const DWORD writable = executableProtection(region.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        DWORD old_protection = 0;
        if (::VirtualProtect(address, sizeof(*address), writable, &old_protection) == FALSE) {
            setErrorNoexcept(error, "VirtualProtect(IAT writable)", ::GetLastError());
            return {.status = WindowsIatExchangeStatus::Error, .observed = nullptr};
        }

        std::atomic_ref<std::uintptr_t> atomic_slot(*address);
        auto expected_value = reinterpret_cast<std::uintptr_t>(expected);
        const auto desired_value = reinterpret_cast<std::uintptr_t>(desired);
        const bool exchanged = atomic_slot.compare_exchange_strong(
            expected_value, desired_value, std::memory_order_acq_rel, std::memory_order_acquire);
        void *observed = reinterpret_cast<void *>(expected_value);  // NOLINT(performance-no-int-to-ptr)

        DWORD ignored = 0;
        if (::VirtualProtect(address, sizeof(*address), old_protection, &ignored) == FALSE) {
            setErrorNoexcept(error, "VirtualProtect(IAT restore)", ::GetLastError());
            return {.status = WindowsIatExchangeStatus::Error, .observed = observed};
        }
        return {.status = exchanged ? WindowsIatExchangeStatus::Exchanged : WindowsIatExchangeStatus::Mismatch,
                .observed = observed};
    }

private:
    static bool enumerateModule(HMODULE module, const WindowsIatModuleIdentity &identity,
                                const std::vector<WindowsIatHookTarget> &targets, std::vector<WindowsIatSlot> &slots,
                                std::string &error)
    {
        const auto *base = reinterpret_cast<const std::byte *>(module);
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + static_cast<std::uint32_t>(dos->e_lfanew));
        const IMAGE_DATA_DIRECTORY directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0 || directory.Size == 0) {
            return true;
        }
        if (!spanInside(directory.VirtualAddress, directory.Size, identity.image_size)) {
            error = "PE import directory is outside the loaded image";
            return false;
        }

        const std::size_t descriptor_capacity = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
        const auto *descriptors = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
        for (std::size_t descriptor_index = 0; descriptor_index < descriptor_capacity; ++descriptor_index) {
            const IMAGE_IMPORT_DESCRIPTOR &descriptor = descriptors[descriptor_index];
            if (descriptor.Name == 0 && descriptor.FirstThunk == 0 && descriptor.OriginalFirstThunk == 0) {
                break;
            }
            if (descriptor.Name == 0 || descriptor.FirstThunk == 0 || descriptor.OriginalFirstThunk == 0 ||
                !spanInside(descriptor.Name, 1, identity.image_size)) {
                continue;  // Bound imports/no original thunk cannot be matched safely by name.
            }

            const char *provider = reinterpret_cast<const char *>(base + descriptor.Name);
            if (!boundedCString(provider, identity.image_size - descriptor.Name)) {
                error = "PE import provider name is unterminated";
                return false;
            }

            const std::uint32_t original_rva = descriptor.OriginalFirstThunk;
            const std::uint32_t first_rva = descriptor.FirstThunk;
            if (!spanInside(original_rva, sizeof(IMAGE_THUNK_DATA64), identity.image_size) ||
                !spanInside(first_rva, sizeof(IMAGE_THUNK_DATA64), identity.image_size)) {
                error = "PE import thunk lies outside the loaded image";
                return false;
            }

            const std::size_t original_capacity = (identity.image_size - original_rva) / sizeof(IMAGE_THUNK_DATA64);
            const std::size_t first_capacity = (identity.image_size - first_rva) / sizeof(IMAGE_THUNK_DATA64);
            const std::size_t thunk_capacity = (std::min)(original_capacity, first_capacity);
            const auto *original = reinterpret_cast<const IMAGE_THUNK_DATA64 *>(base + original_rva);
            for (std::size_t thunk_index = 0; thunk_index < thunk_capacity; ++thunk_index) {
                const ULONGLONG import_value = original[thunk_index].u1.AddressOfData;
                if (import_value == 0) {
                    break;
                }
                if (IMAGE_SNAP_BY_ORDINAL64(import_value)) {
                    continue;
                }
                if (import_value > UINT32_MAX) {
                    error = "PE import-by-name RVA exceeds 32-bit image range";
                    return false;
                }
                const auto import_rva = static_cast<std::uint32_t>(import_value);
                if (!spanInside(import_rva, sizeof(WORD) + 1, identity.image_size)) {
                    error = "PE import-by-name record lies outside the loaded image";
                    return false;
                }
                const auto *import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + import_rva);
                const char *name = reinterpret_cast<const char *>(import->Name);
                const std::size_t name_offset = import_rva + offsetof(IMAGE_IMPORT_BY_NAME, Name);
                if (!boundedCString(name, identity.image_size - name_offset)) {
                    error = "PE import name is unterminated";
                    return false;
                }

                for (std::size_t target_index = 0; target_index < targets.size(); ++target_index) {
                    const WindowsIatHookTarget &target = targets[target_index];
                    if (target.import_name == name && providerAllowed(provider, target)) {
                        const std::uintptr_t slot_rva =
                            first_rva + thunk_index * static_cast<std::uintptr_t>(sizeof(IMAGE_THUNK_DATA64));
                        slots.push_back({.module = identity, .slot_rva = slot_rva, .target_index = target_index});
                        break;
                    }
                }
            }
        }
        return true;
    }

    static WindowsIatAccessStatus resolveSlot(const WindowsIatSlot &slot, PinnedModule &pinned,
                                              std::uintptr_t *&address, std::string &error) noexcept
    {
        // Stable module identity stores the image base as an integer across scans.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        pinned = pinAddress(reinterpret_cast<void *>(slot.module.base));
        if (!pinned || reinterpret_cast<std::uintptr_t>(pinned.get()) != slot.module.base) {
            return WindowsIatAccessStatus::Stale;
        }

        WindowsIatModuleIdentity current;
        try {
            if (!moduleIdentity(pinned.get(), current, &error)) {
                return WindowsIatAccessStatus::Error;
            }
        }
        catch (...) {
            error.clear();
            return WindowsIatAccessStatus::Error;
        }
        if (current != slot.module) {
            return WindowsIatAccessStatus::Stale;
        }
        if (slot.slot_rva > UINT32_MAX ||
            !spanInside(static_cast<std::uint32_t>(slot.slot_rva), sizeof(std::uintptr_t), current.image_size)) {
            try {
                error = "Windows IAT slot RVA is outside its module image";
            }
            catch (...) {
                error.clear();
            }
            return WindowsIatAccessStatus::Error;
        }

        auto *raw = reinterpret_cast<std::byte *>(pinned.get()) + slot.slot_rva;
        if (reinterpret_cast<std::uintptr_t>(raw) % alignof(std::uintptr_t) != 0) {
            try {
                error = "Windows IAT slot is not pointer-aligned";
            }
            catch (...) {
                error.clear();
            }
            return WindowsIatAccessStatus::Error;
        }
        address = reinterpret_cast<std::uintptr_t *>(raw);
        return WindowsIatAccessStatus::Accessible;
    }

    HMODULE excluded_module_ = nullptr;
};

}  // namespace

std::unique_ptr<WindowsIatHookBackend> makeNativeWindowsIatHookBackend(void *excluded_address)
{
    return std::make_unique<NativeWindowsIatHookBackend>(excluded_address);
}

}  // namespace spark

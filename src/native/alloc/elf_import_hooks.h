#ifndef ENDSTONE_SPARK_ELF_IMPORT_HOOKS_H
#define ENDSTONE_SPARK_ELF_IMPORT_HOOKS_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace spark {

struct ElfImportHookSpec {
    const char *name = nullptr;
    void *replacement = nullptr;
    bool required = false;
};

struct ElfImportHookCapability {
    std::string name;
    bool available = false;
    std::size_t slots = 0;
    std::string detail;
};

// Atomically redirects allocator import slots in supported loaded ELF images.
// No instruction bytes are modified, so concurrent callers see either the
// complete original pointer or the complete replacement pointer.
class ElfImportHooks {
public:
    using ScanModuleGate = bool (*)(std::string_view) noexcept;

    ElfImportHooks() = default;
    ~ElfImportHooks() = default;

    ElfImportHooks(const ElfImportHooks &) = delete;
    ElfImportHooks &operator=(const ElfImportHooks &) = delete;

    bool prepare(std::span<const ElfImportHookSpec> specs, std::string &error);
    bool rescan(std::string &error);
    bool install(std::string &error);
    bool uninstall(std::string &error);

    bool installed() const noexcept { return installed_; }
    std::size_t targetCount() const noexcept { return targets_.size(); }
    std::size_t pageCount() const noexcept { return pages_.size(); }
    std::size_t hookedModuleCount() const noexcept { return hooked_modules_; }
    std::size_t skippedModuleCount() const noexcept { return skipped_modules_; }
    std::size_t failedModuleCount() const noexcept { return failed_modules_; }
    const std::vector<ElfImportHookCapability> &capabilities() const noexcept { return capabilities_; }
    void setScanModuleGateForTesting(ScanModuleGate gate) noexcept { scan_module_gate_ = gate; }

private:
    struct Target {
        void **slot = nullptr;
        void *original = nullptr;
        void *replacement = nullptr;
        std::size_t spec_index = 0;
        std::uintptr_t module_base = 0;
        std::string module_name;
        bool main_executable = false;
    };

    struct Page {
        void *address = nullptr;
        int protection = 0;
        std::uintptr_t module_base = 0;
        std::string module_name;
        bool main_executable = false;
    };

    bool scan(std::string &error);
    bool patch(bool replacements, std::string &error);

    std::vector<ElfImportHookSpec> specs_;
    std::vector<Target> targets_;
    std::vector<Page> pages_;
    std::vector<ElfImportHookCapability> capabilities_;
    ScanModuleGate scan_module_gate_ = nullptr;
    bool prepared_ = false;
    bool installed_ = false;
    std::size_t hooked_modules_ = 0;
    std::size_t skipped_modules_ = 0;
    std::size_t failed_modules_ = 0;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ELF_IMPORT_HOOKS_H

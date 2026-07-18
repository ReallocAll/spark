#ifndef ENDSTONE_SPARK_ELF_IMPORT_HOOKS_H
#define ENDSTONE_SPARK_ELF_IMPORT_HOOKS_H

#include <cstddef>
#include <span>
#include <string>
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

// Atomically redirects imported function-pointer slots in the Linux main
// executable. No instruction bytes are modified, so concurrent callers see
// either the complete original pointer or the complete replacement pointer.
class ElfImportHooks {
public:
    ElfImportHooks() = default;
    ~ElfImportHooks() = default;

    ElfImportHooks(const ElfImportHooks &) = delete;
    ElfImportHooks &operator=(const ElfImportHooks &) = delete;

    bool prepare(std::span<const ElfImportHookSpec> specs, std::string &error);
    bool install(std::string &error);
    bool uninstall(std::string &error);

    bool installed() const noexcept { return installed_; }
    std::size_t targetCount() const noexcept { return targets_.size(); }
    const std::vector<ElfImportHookCapability> &capabilities() const noexcept
    {
        return capabilities_;
    }

private:
    struct Target {
        void **slot = nullptr;
        void *original = nullptr;
        void *replacement = nullptr;
    };

    struct Page {
        void *address = nullptr;
        int protection = 0;
    };

    bool patch(bool replacements, std::string &error);

    std::vector<Target> targets_;
    std::vector<Page> pages_;
    std::vector<ElfImportHookCapability> capabilities_;
    bool prepared_ = false;
    bool installed_ = false;
};

}  // namespace spark

#endif  // ENDSTONE_SPARK_ELF_IMPORT_HOOKS_H

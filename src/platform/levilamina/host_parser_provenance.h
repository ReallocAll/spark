#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace spark::levilamina {

struct HostRawParameterTemplate;

namespace detail {

inline constexpr std::uint32_t kMemoryImage = 0x1000000U;
inline constexpr std::uint32_t kMemoryMapped = 0x40000U;
inline constexpr std::uint32_t kMemoryCommit = 0x1000U;
inline constexpr std::uint32_t kPageNoAccess = 0x01U;
inline constexpr std::uint32_t kPageReadOnly = 0x02U;
inline constexpr std::uint32_t kPageReadWrite = 0x04U;
inline constexpr std::uint32_t kPageWriteCopy = 0x08U;
inline constexpr std::uint32_t kPageExecute = 0x10U;
inline constexpr std::uint32_t kPageExecuteRead = 0x20U;
inline constexpr std::uint32_t kPageExecuteReadWrite = 0x40U;
inline constexpr std::uint32_t kPageExecuteWriteCopy = 0x80U;
inline constexpr std::uint32_t kPageGuard = 0x100U;

inline constexpr std::string_view kRawTextParserSymbol =
    "??$parse@VCommandRawText@@@CommandRegistry@@QEBA_NPEAXAEBUParseToken@0@AEBVCommandOrigin@@HAEAV?$basic_string@DU?$"
    "char_traits@D@std@@V?$allocator@D@2@@std@@AEAV?$vector@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@"
    "std@@V?$allocator@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@2@@4@@Z";

struct MemoryRegion final {
    std::uintptr_t allocation_base = 0;
    std::uintptr_t region_base = 0;
    std::uintptr_t region_size = 0;
    std::uint32_t protection = 0;
    std::uint32_t state = 0;
    std::uint32_t type = 0;
};

class BoundedImageReader {
public:
    virtual ~BoundedImageReader() = default;

    // query succeeds only when the complete range is in one readable region.
    [[nodiscard]] virtual bool query(std::uintptr_t address, std::size_t size, MemoryRegion &region) const noexcept = 0;

    [[nodiscard]] virtual bool read(std::uintptr_t address, void *destination, std::size_t size) const noexcept = 0;
};

enum class HostRawImageAccess {
    Readable,
    Executable
};

class HostRawParameterValidationEnvironment {
public:
    virtual ~HostRawParameterValidationEnvironment() = default;

    [[nodiscard]] virtual bool imageRangeOwnedByHost(std::uintptr_t address, std::size_t size,
                                                     std::uintptr_t current_module, HostRawImageAccess access,
                                                     std::uintptr_t &containing_module,
                                                     std::string &basename) const noexcept = 0;

    [[nodiscard]] virtual std::uintptr_t loadedLeviLamina() const noexcept = 0;

    [[nodiscard]] virtual BoundedImageReader const &reader() const noexcept = 0;
};

struct RawTextImportCell final {
    std::uintptr_t address = 0;
    std::uintptr_t value = 0;
};

[[nodiscard]] bool findRawTextDelayImport(BoundedImageReader const &reader, std::uintptr_t importer_module,
                                          std::uintptr_t loaded_ll_module, RawTextImportCell &result,
                                          std::string &error);

[[nodiscard]] bool validateRawTextParserProvenance(BoundedImageReader const &reader, std::uintptr_t importer_module,
                                                   std::uintptr_t loaded_ll_module, std::uintptr_t parser,
                                                   std::uintptr_t spark_module, std::string &provenance,
                                                   std::string &error);

[[nodiscard]] bool validateHostRawParameterTemplateInternal(HostRawParameterTemplate &value,
                                                            std::uintptr_t current_module,
                                                            HostRawParameterValidationEnvironment const &environment,
                                                            std::string &error);

}  // namespace detail
}  // namespace spark::levilamina

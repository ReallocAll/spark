#include "platform/levilamina/host_parser_provenance.h"

#include <delayimp.h>
#include <windows.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

#include "platform/levilamina/host_command_parameter.h"

namespace spark::levilamina::detail {

namespace {

constexpr std::size_t kMaxImageSize = 0x80000000ULL;
constexpr std::size_t kMaxDelayDescriptors = 64;
constexpr std::size_t kMaxThunks = 4096;
constexpr std::size_t kMaxImportName = 2048;
constexpr std::string_view kTargetDll = "bedrock_runtime.dll";

bool checkedAdd(std::uintptr_t base, std::size_t offset, std::size_t size, std::uintptr_t &result) noexcept
{
    if (offset > (std::numeric_limits<std::uintptr_t>::max)() - base ||
        size > (std::numeric_limits<std::uintptr_t>::max)() - base - offset) {
        return false;
    }
    result = base + offset;
    return true;
}

bool checkedRva(std::uint32_t rva, std::size_t size, std::uint32_t image_size) noexcept
{
    return rva <= image_size && size <= static_cast<std::size_t>(image_size - rva);
}

bool readableProtection(std::uint32_t protection) noexcept
{
    const auto base = protection & 0xffU;
    if ((protection & kPageGuard) != 0 || base == kPageNoAccess) {
        return false;
    }
    return base == kPageReadOnly || base == kPageReadWrite || base == kPageWriteCopy || base == kPageExecuteRead ||
           base == kPageExecuteReadWrite || base == kPageExecuteWriteCopy;
}

bool executableProtection(std::uint32_t protection) noexcept
{
    const auto base = protection & 0xffU;
    return base == kPageExecute || base == kPageExecuteRead || base == kPageExecuteReadWrite ||
           base == kPageExecuteWriteCopy;
}

bool setError(std::string &error, std::string_view value)
{
    error.assign(value);
    return false;
}

bool readAt(BoundedImageReader const &reader, std::uintptr_t address, std::size_t size,
            std::uintptr_t expected_allocation, bool require_image, void *destination, std::string &error)
{
    if (address == 0 || size == 0) {
        return setError(error, "parser provenance read has a null or empty range");
    }
    MemoryRegion region;
    if (!reader.query(address, size, region)) {
        return setError(error, "parser provenance read is outside a readable region");
    }
    std::uintptr_t region_end = 0;
    if (region.region_base == 0 || region.region_size == 0 ||
        !checkedAdd(region.region_base, static_cast<std::size_t>(region.region_size), 0, region_end) ||
        address < region.region_base || address >= region_end || size > region_end - address) {
        return setError(error, "parser provenance region bounds are invalid");
    }
    if (region.allocation_base != expected_allocation || region.state != kMemoryCommit ||
        !readableProtection(region.protection) || (require_image && region.type != kMemoryImage)) {
        return setError(error, "parser provenance read failed image-region validation");
    }
    if (!reader.read(address, destination, size)) {
        return setError(error, "parser provenance reader failed");
    }
    return true;
}

template <class T>
bool readAt(BoundedImageReader const &reader, std::uintptr_t address, std::uintptr_t expected_allocation,
            bool require_image, T &value, std::string &error)
{
    static_assert(std::is_trivially_copyable_v<T>);
    return readAt(reader, address, sizeof(T), expected_allocation, require_image, &value, error);
}

template <class T>
bool readRva(BoundedImageReader const &reader, std::uintptr_t image_base, std::uint32_t image_size, std::uint32_t rva,
             T &value, std::string &error)
{
    static_assert(std::is_trivially_copyable_v<T>);
    if (!checkedRva(rva, sizeof(T), image_size)) {
        return setError(error, "parser provenance RVA is outside the image");
    }
    std::uintptr_t address = 0;
    if (!checkedAdd(image_base, rva, sizeof(T), address)) {
        return setError(error, "parser provenance RVA address overflow");
    }
    return readAt(reader, address, image_base, true, value, error);
}

bool readStringRva(BoundedImageReader const &reader, std::uintptr_t image_base, std::uint32_t image_size,
                   std::uint32_t rva, std::string &value, std::string &error)
{
    value.clear();
    if (!checkedRva(rva, 1, image_size)) {
        return setError(error, "parser provenance string RVA is outside the image");
    }
    for (std::size_t index = 0; index < kMaxImportName; ++index) {
        if (index > static_cast<std::size_t>(image_size - rva) - 1) {
            return setError(error, "parser provenance string exceeds the image");
        }
        const auto current_rva = static_cast<std::uint32_t>(rva + index);
        char character = '\0';
        if (!readRva(reader, image_base, image_size, current_rva, character, error)) {
            return false;
        }
        if (character == '\0') {
            return true;
        }
        value.push_back(character);
    }
    return setError(error, "parser provenance string terminator exceeds the bound");
}

bool readThunk(BoundedImageReader const &reader, std::uintptr_t image_base, std::uint32_t image_size, std::uint32_t rva,
               std::size_t index, std::uint64_t &value, std::string &error)
{
    if (index > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint64_t)) {
        return setError(error, "parser provenance thunk index overflow");
    }
    const auto offset = index * sizeof(std::uint64_t);
    if (offset > image_size || offset > (std::numeric_limits<std::size_t>::max)() - sizeof(std::uint64_t) ||
        !checkedRva(rva, offset + sizeof(std::uint64_t), image_size)) {
        return setError(error, "parser provenance thunk range is outside the image");
    }
    const auto thunk_rva = static_cast<std::uint32_t>(rva + offset);
    return readRva(reader, image_base, image_size, thunk_rva, value, error);
}

bool validateTarget(BoundedImageReader const &reader, std::uintptr_t parser, std::uintptr_t spark_module,
                    std::string &error)
{
    if (parser == 0 || spark_module == 0) {
        return setError(error, "parser provenance target is null");
    }
    MemoryRegion region;
    if (!reader.query(parser, 1, region)) {
        return setError(error, "parser provenance target is outside a valid region");
    }
    std::uintptr_t region_end = 0;
    if (region.region_base == 0 || region.region_size == 0 ||
        !checkedAdd(region.region_base, static_cast<std::size_t>(region.region_size), 0, region_end) ||
        parser < region.region_base || parser >= region_end) {
        return setError(error, "parser provenance target region is invalid");
    }
    if (region.allocation_base == 0 || region.allocation_base == spark_module || region.state != kMemoryCommit ||
        (region.protection & kPageGuard) != 0 || (region.protection & 0xffU) == kPageNoAccess ||
        !executableProtection(region.protection)) {
        return setError(error, "parser provenance target is not a committed executable non-Spark region");
    }
    return true;
}

struct ScanState final {
    RawTextImportCell cell;
    std::size_t matches = 0;
};

bool validateParser(::CommandParameterData::ParseFunction parser, std::uintptr_t current_module,
                    std::uintptr_t rule_module, std::string_view rule_module_name,
                    HostRawParameterValidationEnvironment const &environment, std::string &basename)
{
    if (parser == nullptr) {
        basename = "null";
        return true;
    }
    static_assert(sizeof(::CommandParameterData::ParseFunction) == sizeof(std::uintptr_t));
    const auto address = std::bit_cast<std::uintptr_t>(parser);
    std::uintptr_t parser_module = 0;
    if (environment.imageRangeOwnedByHost(address, 1, current_module, HostRawImageAccess::Executable, parser_module,
                                          basename)) {
        return true;
    }
    const auto loaded_ll = environment.loadedLeviLamina();
    if (rule_module == 0 || loaded_ll == 0 || rule_module != loaded_ll) {
        return false;
    }
    std::string provenance;
    std::string error;
    if (!validateRawTextParserProvenance(environment.reader(), rule_module, loaded_ll, address, current_module,
                                         provenance, error)) {
        return false;
    }
    basename.assign(rule_module_name);
    basename.append(" (");
    basename.append(provenance);
    basename.push_back(')');
    return !basename.empty();
}

bool scanThunkTable(BoundedImageReader const &reader, std::uintptr_t image_base, std::uint32_t image_size,
                    std::uint32_t rva_int, std::uint32_t rva_iat, bool target_descriptor, ScanState &state,
                    std::string &error)
{
    if (rva_int % alignof(std::uint64_t) != 0 || rva_iat % alignof(std::uint64_t) != 0) {
        return setError(error, "parser provenance thunk table is unaligned");
    }
    bool int_terminated = false;
    for (std::size_t index = 0; index < kMaxThunks; ++index) {
        std::uint64_t int_value = 0;
        std::uint64_t iat_value = 0;
        if (!readThunk(reader, image_base, image_size, rva_int, index, int_value, error) ||
            !readThunk(reader, image_base, image_size, rva_iat, index, iat_value, error)) {
            return false;
        }
        if (int_value == 0) {
            if (iat_value != 0) {
                return setError(error, "parser provenance INT/IAT terminators do not match");
            }
            int_terminated = true;
            break;
        }
        if (iat_value == 0) {
            return setError(error, "parser provenance IAT terminator precedes INT terminator");
        }
        if (!target_descriptor || (int_value & IMAGE_ORDINAL_FLAG64) != 0) {
            continue;
        }
        if (int_value > (std::numeric_limits<std::uint32_t>::max)()) {
            return setError(error, "parser provenance import name RVA is not 32-bit");
        }
        const auto name_rva = static_cast<std::uint32_t>(int_value);
        if (!checkedRva(name_rva, sizeof(WORD), image_size)) {
            return setError(error, "parser provenance import name hint is outside the image");
        }
        if (name_rva > (std::numeric_limits<std::uint32_t>::max)() - sizeof(WORD)) {
            return setError(error, "parser provenance import name RVA overflows");
        }
        std::string symbol;
        if (!readStringRva(reader, image_base, image_size, name_rva + sizeof(WORD), symbol, error)) {
            return false;
        }
        if (symbol != kRawTextParserSymbol) {
            continue;
        }
        if (index > (std::numeric_limits<std::size_t>::max)() / sizeof(std::uint64_t)) {
            return setError(error, "parser provenance IAT index overflow");
        }
        const auto iat_offset = index * sizeof(std::uint64_t);
        if (!checkedRva(rva_iat, iat_offset + sizeof(std::uint64_t), image_size)) {
            return setError(error, "parser provenance named IAT cell is outside the image");
        }
        std::uintptr_t cell_address = 0;
        if (!checkedAdd(image_base, static_cast<std::size_t>(rva_iat) + iat_offset, sizeof(std::uintptr_t),
                        cell_address)) {
            return setError(error, "parser provenance named IAT cell address overflows");
        }
        ++state.matches;
        if (state.matches == 1) {
            state.cell.address = cell_address;
            state.cell.value = static_cast<std::uintptr_t>(iat_value);
        }
    }
    if (!int_terminated) {
        return setError(error, "parser provenance thunk bound exhausted without a terminator");
    }
    return true;
}

}  // namespace

bool findRawTextDelayImport(BoundedImageReader const &reader, std::uintptr_t importer_module,
                            std::uintptr_t loaded_ll_module, RawTextImportCell &result, std::string &error)
{
    result = {};
    error.clear();
    if (importer_module == 0 || loaded_ll_module == 0 || importer_module != loaded_ll_module) {
        return setError(error, "parser provenance importer is not the loaded LL module");
    }

    IMAGE_DOS_HEADER dos{};
    if (!readAt(reader, importer_module, sizeof(dos), importer_module, true, &dos, error)) {
        return false;
    }
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
        dos.e_lfanew > 0x100000) {
        return setError(error, "parser provenance DOS header is invalid");
    }

    std::uintptr_t nt_address = 0;
    if (!checkedAdd(importer_module, static_cast<std::size_t>(dos.e_lfanew), sizeof(DWORD), nt_address)) {
        return setError(error, "parser provenance NT header address overflows");
    }
    DWORD signature = 0;
    if (!readAt(reader, nt_address, importer_module, true, signature, error)) {
        return false;
    }
    if (signature != IMAGE_NT_SIGNATURE) {
        return setError(error, "parser provenance PE signature is invalid");
    }

    std::uintptr_t file_header_address = 0;
    if (!checkedAdd(nt_address, sizeof(DWORD), sizeof(IMAGE_FILE_HEADER), file_header_address)) {
        return setError(error, "parser provenance file header address overflows");
    }
    IMAGE_FILE_HEADER file_header{};
    if (!readAt(reader, file_header_address, importer_module, true, file_header, error)) {
        return false;
    }
    if (file_header.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        file_header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || file_header.NumberOfSections > 128) {
        return setError(error, "parser provenance PE file header is unsupported");
    }

    std::uintptr_t optional_address = 0;
    if (!checkedAdd(file_header_address, sizeof(IMAGE_FILE_HEADER), sizeof(IMAGE_OPTIONAL_HEADER64),
                    optional_address)) {
        return setError(error, "parser provenance optional header address overflows");
    }
    IMAGE_OPTIONAL_HEADER64 optional{};
    if (!readAt(reader, optional_address, importer_module, true, optional, error)) {
        return false;
    }
    if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || optional.SizeOfImage == 0 ||
        optional.SizeOfImage > kMaxImageSize || optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT ||
        optional.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES) {
        return setError(error, "parser provenance PE optional header is unsupported");
    }
    const auto image_size = optional.SizeOfImage;
    const auto optional_offset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    if (!checkedRva(static_cast<std::uint32_t>(optional_offset), sizeof(IMAGE_OPTIONAL_HEADER64), image_size)) {
        return setError(error, "parser provenance PE headers exceed the declared image");
    }
    const auto delay_directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delay_directory.VirtualAddress == 0 || delay_directory.Size < sizeof(ImgDelayDescr) ||
        delay_directory.Size % sizeof(ImgDelayDescr) != 0 ||
        !checkedRva(delay_directory.VirtualAddress, delay_directory.Size, image_size)) {
        return setError(error, "parser provenance delay-import directory is malformed");
    }

    ScanState state;
    bool descriptor_terminated = false;
    for (std::size_t descriptor_index = 0; descriptor_index < kMaxDelayDescriptors; ++descriptor_index) {
        if (descriptor_index > (std::numeric_limits<std::size_t>::max)() / sizeof(ImgDelayDescr)) {
            return setError(error, "parser provenance descriptor index overflows");
        }
        const auto descriptor_offset = descriptor_index * sizeof(ImgDelayDescr);
        if (descriptor_offset > delay_directory.Size ||
            sizeof(ImgDelayDescr) > delay_directory.Size - descriptor_offset) {
            return setError(error, "parser provenance descriptor bound exhausted");
        }
        const auto descriptor_rva = static_cast<std::uint32_t>(delay_directory.VirtualAddress + descriptor_offset);
        ImgDelayDescr descriptor{};
        if (!readRva(reader, importer_module, image_size, descriptor_rva, descriptor, error)) {
            return false;
        }
        if (descriptor.rvaDLLName == 0) {
            if (descriptor.grAttrs != 0 || descriptor.rvaINT != 0 || descriptor.rvaIAT != 0 ||
                descriptor.rvaHmod != 0 || descriptor.rvaBoundIAT != 0 || descriptor.rvaUnloadIAT != 0 ||
                descriptor.dwTimeStamp != 0) {
                return setError(error, "parser provenance delay-import terminator is nonzero");
            }
            descriptor_terminated = true;
            break;
        }
        if (descriptor.grAttrs != dlattrRva || descriptor.rvaINT == 0 || descriptor.rvaIAT == 0) {
            return setError(error, "parser provenance delay-import descriptor format is unsupported");
        }
        for (const auto optional_rva : {descriptor.rvaHmod, descriptor.rvaBoundIAT, descriptor.rvaUnloadIAT}) {
            if (optional_rva != 0 && !checkedRva(optional_rva, sizeof(std::uint64_t), image_size)) {
                return setError(error, "parser provenance optional delay-import table is outside the image");
            }
        }
        std::string dll_name;
        if (!readStringRva(reader, importer_module, image_size, descriptor.rvaDLLName, dll_name, error)) {
            return false;
        }
        if (!checkedRva(descriptor.rvaINT, sizeof(std::uint64_t), image_size) ||
            !checkedRva(descriptor.rvaIAT, sizeof(std::uint64_t), image_size)) {
            return setError(error, "parser provenance delay-import thunk table is outside the image");
        }
        if (!scanThunkTable(reader, importer_module, image_size, descriptor.rvaINT, descriptor.rvaIAT,
                            dll_name == kTargetDll, state, error)) {
            return false;
        }
    }
    if (!descriptor_terminated) {
        return setError(error, delay_directory.Size / sizeof(ImgDelayDescr) > kMaxDelayDescriptors
                                   ? "parser provenance delay-import descriptor bound exhausted"
                                   : "parser provenance delay-import descriptor terminator is missing");
    }
    if (state.matches != 1) {
        return setError(error, state.matches == 0 ? "parser provenance exact RawText import is missing"
                                                  : "parser provenance exact RawText import is duplicated");
    }
    result = state.cell;
    return true;
}

bool validateRawTextParserProvenance(BoundedImageReader const &reader, std::uintptr_t importer_module,
                                     std::uintptr_t loaded_ll_module, std::uintptr_t parser,
                                     std::uintptr_t spark_module, std::string &provenance, std::string &error)
{
    provenance.clear();
    RawTextImportCell cell;
    if (!findRawTextDelayImport(reader, importer_module, loaded_ll_module, cell, error)) {
        return false;
    }
    if (parser == 0 || parser != cell.value) {
        return setError(error, "parser provenance parser does not equal the current named IAT cell");
    }
    if (!validateTarget(reader, parser, spark_module, error)) {
        return false;
    }
    provenance = "named-delay-import:bedrock_runtime.dll";
    return true;
}

bool validateHostRawParameterTemplateInternal(HostRawParameterTemplate &value, std::uintptr_t current_module,
                                              HostRawParameterValidationEnvironment const &environment,
                                              std::string &error)
{
    error.clear();
    if (current_module == 0 || value.parse_rule == nullptr) {
        return setError(error, "host raw template has no verifiable current module or parse rule");
    }
    std::uintptr_t rule_module_handle = 0;
    std::string rule_module;
    if (!environment.imageRangeOwnedByHost(reinterpret_cast<std::uintptr_t>(value.parse_rule),
                                           sizeof(::CommandRegistry::ParamParseRule), current_module,
                                           HostRawImageAccess::Readable, rule_module_handle, rule_module)) {
        return setError(error, "host raw parse rule is outside the LL/BDS image");
    }
    if (value.parse_rule->symbol.get() != ::CommandRegistry::Symbol{::CommandRegistry::HardNonTerminal::RawText}) {
        return setError(error, "host raw parse rule symbol is not RawText");
    }
    std::string parser_module;
    if (!validateParser(value.parse_override, current_module, rule_module_handle, rule_module, environment,
                        parser_module)) {
        return setError(error, "host raw parser is outside the LL/BDS image");
    }
    std::string rule_parser_module;
    if (!validateParser(value.parse_rule->parse.get(), current_module, rule_module_handle, rule_module, environment,
                        rule_parser_module)) {
        return setError(error, "host raw rule parser is outside the LL/BDS image");
    }
    value.rule_module = std::move(rule_module);
    value.parser_module = value.parse_override == nullptr ? std::move(rule_parser_module) : std::move(parser_module);
    return true;
}

}  // namespace spark::levilamina::detail

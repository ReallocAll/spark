#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "platform/levilamina/host_command_parameter.h"
#include "platform/levilamina/host_parser_provenance.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <delayimp.h>
#endif

namespace {

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

struct SimulatedDisabledOverload final {
    spark::levilamina::HostRawParameterTemplate params;
    int factory_generation = 0;
};

#ifdef _WIN32

using Reader = spark::levilamina::detail::BoundedImageReader;
using Region = spark::levilamina::detail::MemoryRegion;

class SyntheticReader final : public Reader {
public:
    struct Mapping final {
        std::uintptr_t begin = 0;
        std::size_t size = 0;
        std::uintptr_t allocation_base = 0;
        std::uint32_t protection = 0;
        std::uint32_t state = 0;
        std::uint32_t type = 0;
    };

    std::vector<Mapping> mappings;
    bool deny_reads = false;

    [[nodiscard]] bool query(std::uintptr_t address, std::size_t size, Region &region) const noexcept override
    {
        region = {};
        if (address == 0 || size == 0 || size > (std::numeric_limits<std::uintptr_t>::max)() - address) {
            return false;
        }
        const auto end = address + size;
        for (auto const &mapping : mappings) {
            if (mapping.begin == 0 || mapping.size == 0 || address < mapping.begin ||
                address > (std::numeric_limits<std::uintptr_t>::max)() - mapping.size) {
                continue;
            }
            const auto mapping_end = mapping.begin + mapping.size;
            if (end > mapping_end) {
                continue;
            }
            region = Region{
                .allocation_base = mapping.allocation_base,
                .region_base = mapping.begin,
                .region_size = mapping.size,
                .protection = mapping.protection,
                .state = mapping.state,
                .type = mapping.type,
            };
            return true;
        }
        return false;
    }

    [[nodiscard]] bool read(std::uintptr_t address, void *destination, std::size_t size) const noexcept override
    {
        if (deny_reads) {
            return false;
        }
        Region region;
        if (!query(address, size, region)) {
            return false;
        }
        std::memcpy(destination, reinterpret_cast<void const *>(address), size);
        return true;
    }
};

struct ImportFixture final {
    static constexpr std::uint32_t kDelayDirectoryRva = 0x300;
    static constexpr std::uint32_t kDllNameRva = 0x2000;
    static constexpr std::uint32_t kOtherDllNameRva = 0x2100;
    static constexpr std::uint32_t kIatRva = 0x2800;
    static constexpr std::uint32_t kIntRva = 0x3000;
    static constexpr std::uint32_t kImportNameRva = 0x3800;
    static constexpr std::size_t kImageSize = 0x10000;

    std::vector<std::uint8_t> image = std::vector<std::uint8_t>(kImageSize);
    std::array<std::uint8_t, 128> parser_memory{};
    std::array<std::uint8_t, 128> adjacent_memory{};
    std::array<std::uint8_t, 128> spark_memory{};
    SyntheticReader reader;

    ImportFixture()
    {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        write(0, dos);

        constexpr DWORD signature = IMAGE_NT_SIGNATURE;
        write(0x80, signature);

        IMAGE_FILE_HEADER file_header{};
        file_header.Machine = IMAGE_FILE_MACHINE_AMD64;
        file_header.NumberOfSections = 1;
        file_header.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        write(0x84, file_header);

        IMAGE_OPTIONAL_HEADER64 optional{};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = static_cast<DWORD>(image.size());
        optional.SizeOfHeaders = 0x400;
        optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT] = {
            kDelayDirectoryRva,
            static_cast<DWORD>(2 * sizeof(ImgDelayDescr)),
        };
        write(0x98, optional);

        writeDescriptor(0, kDllNameRva, kIntRva, kIatRva);
        writeDescriptor(1, 0, 0, 0);
        writeString(kDllNameRva, "bedrock_runtime.dll");
        writeString(kOtherDllNameRva, "other_import.dll");
        writeString(kImportNameRva + sizeof(WORD), spark::levilamina::detail::kRawTextParserSymbol);
        write<std::uint16_t>(kImportNameRva, 0);
        write<std::uint64_t>(kIntRva, kImportNameRva);
        write<std::uint64_t>(kIntRva + sizeof(std::uint64_t), 0);
        setIatValue(reinterpret_cast<std::uintptr_t>(parser_memory.data()));

        reader.mappings = {
            SyntheticReader::Mapping{
                reinterpret_cast<std::uintptr_t>(image.data()),
                image.size(),
                reinterpret_cast<std::uintptr_t>(image.data()),
                spark::levilamina::detail::kPageReadWrite,
                spark::levilamina::detail::kMemoryCommit,
                spark::levilamina::detail::kMemoryImage,
            },
            SyntheticReader::Mapping{
                reinterpret_cast<std::uintptr_t>(parser_memory.data()),
                parser_memory.size(),
                reinterpret_cast<std::uintptr_t>(parser_memory.data()),
                spark::levilamina::detail::kPageExecuteRead,
                spark::levilamina::detail::kMemoryCommit,
                spark::levilamina::detail::kMemoryMapped,
            },
            SyntheticReader::Mapping{
                reinterpret_cast<std::uintptr_t>(adjacent_memory.data()),
                adjacent_memory.size(),
                reinterpret_cast<std::uintptr_t>(adjacent_memory.data()),
                spark::levilamina::detail::kPageExecuteRead,
                spark::levilamina::detail::kMemoryCommit,
                spark::levilamina::detail::kMemoryMapped,
            },
            SyntheticReader::Mapping{
                reinterpret_cast<std::uintptr_t>(spark_memory.data()),
                spark_memory.size(),
                reinterpret_cast<std::uintptr_t>(spark_memory.data()),
                spark::levilamina::detail::kPageExecuteRead,
                spark::levilamina::detail::kMemoryCommit,
                spark::levilamina::detail::kMemoryImage,
            },
        };
    }

    [[nodiscard]] std::uintptr_t imageBase() const noexcept { return reinterpret_cast<std::uintptr_t>(image.data()); }

    [[nodiscard]] std::uintptr_t parser() const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(parser_memory.data());
    }

    [[nodiscard]] std::uintptr_t adjacent() const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(adjacent_memory.data());
    }

    [[nodiscard]] std::uintptr_t spark() const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(spark_memory.data());
    }

    void setIatValue(std::uintptr_t value) { write<std::uint64_t>(kIatRva, static_cast<std::uint64_t>(value)); }

    void setIntValue(std::uint64_t value) { write<std::uint64_t>(kIntRva, value); }

    void setSymbol(std::string_view symbol)
    {
        std::memset(image.data() + kImportNameRva + sizeof(WORD), 0, 4096);
        std::memcpy(image.data() + kImportNameRva + sizeof(WORD), symbol.data(), symbol.size());
    }

    void addAdjacentImport()
    {
        constexpr auto adjacent_name_rva = 0x4000U;
        writeString(adjacent_name_rva, "other_symbol");
        write<std::uint16_t>(adjacent_name_rva - sizeof(std::uint16_t), 0);
        write<std::uint64_t>(kIntRva + sizeof(std::uint64_t), adjacent_name_rva - sizeof(std::uint16_t));
        write<std::uint64_t>(kIntRva + 2 * sizeof(std::uint64_t), 0);
        write<std::uint64_t>(kIatRva + sizeof(std::uint64_t), static_cast<std::uint64_t>(adjacent()));
        write<std::uint64_t>(kIatRva + 2 * sizeof(std::uint64_t), 0);
    }

    void fillThunkTablesWithoutTerminator()
    {
        for (std::size_t index = 0; index < 4096; ++index) {
            write<std::uint64_t>(kIntRva + index * sizeof(std::uint64_t), 1);
            write<std::uint64_t>(kIatRva + index * sizeof(std::uint64_t), static_cast<std::uint64_t>(adjacent()));
        }
    }

    void addDuplicateDescriptor()
    {
        writeDescriptor(1, kDllNameRva, 0x1100, 0x1200);
        writeDescriptor(2, 0, 0, 0);
        write<std::uint64_t>(0x1100, kImportNameRva);
        write<std::uint64_t>(0x1100 + sizeof(std::uint64_t), 0);
        write<std::uint64_t>(0x1200, static_cast<std::uint64_t>(adjacent()));
        write<std::uint64_t>(0x1200 + sizeof(std::uint64_t), 0);
        setDelayDirectorySize(3 * sizeof(ImgDelayDescr));
    }

    void setDelayDirectorySize(std::size_t size)
    {
        IMAGE_DATA_DIRECTORY directory{};
        constexpr auto directory_offset = 0x98 + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
                                          IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
        std::memcpy(&directory, image.data() + directory_offset, sizeof(directory));
        directory.Size = static_cast<DWORD>(size);
        std::memcpy(image.data() + directory_offset, &directory, sizeof(directory));
    }

    void setDelayDirectoryRva(std::uint32_t rva)
    {
        IMAGE_DATA_DIRECTORY directory{};
        constexpr auto directory_offset = 0x98 + offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
                                          IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
        std::memcpy(&directory, image.data() + directory_offset, sizeof(directory));
        directory.VirtualAddress = rva;
        std::memcpy(image.data() + directory_offset, &directory, sizeof(directory));
    }

    void setImageSize(std::uint32_t size)
    {
        IMAGE_OPTIONAL_HEADER64 optional{};
        std::memcpy(&optional, image.data() + 0x98, sizeof(optional));
        optional.SizeOfImage = size;
        std::memcpy(image.data() + 0x98, &optional, sizeof(optional));
    }

    template <class T>
    void write(std::size_t offset, T const &value)
    {
        require(offset <= image.size() && sizeof(T) <= image.size() - offset, "fixture write exceeded image");
        std::memcpy(image.data() + offset, &value, sizeof(T));
    }

    void writeString(std::size_t offset, std::string_view value)
    {
        require(offset <= image.size() && value.size() < image.size() - offset, "fixture string exceeded image");
        std::memcpy(image.data() + offset, value.data(), value.size());
        image[offset + value.size()] = '\0';
    }

private:
    void writeDescriptor(std::size_t index, std::uint32_t dll_name, std::uint32_t int_rva, std::uint32_t iat_rva)
    {
        ImgDelayDescr descriptor{};
        descriptor.grAttrs = dll_name == 0 ? 0 : dlattrRva;
        descriptor.rvaDLLName = dll_name;
        descriptor.rvaINT = int_rva;
        descriptor.rvaIAT = iat_rva;
        write(kDelayDirectoryRva + index * sizeof(ImgDelayDescr), descriptor);
    }
};

class OverflowReader final : public Reader {
public:
    explicit OverflowReader(std::vector<std::uint8_t> const &image)
        : image_(image), base_((std::numeric_limits<std::uintptr_t>::max)() - 0x7fffff00ULL)
    {}

    [[nodiscard]] bool query(std::uintptr_t address, std::size_t size, Region &region) const noexcept override
    {
        region = {};
        if (address < base_ || size == 0 || size > (std::numeric_limits<std::uintptr_t>::max)() - address) {
            return false;
        }
        const auto offset = address - base_;
        if (offset > image_.size() || size > image_.size() - offset) {
            return false;
        }
        region = Region{
            .allocation_base = base_,
            .region_base = address,
            .region_size = size,
            .protection = spark::levilamina::detail::kPageReadWrite,
            .state = spark::levilamina::detail::kMemoryCommit,
            .type = spark::levilamina::detail::kMemoryImage,
        };
        return true;
    }

    [[nodiscard]] bool read(std::uintptr_t address, void *destination, std::size_t size) const noexcept override
    {
        Region region;
        if (!query(address, size, region)) {
            return false;
        }
        std::memcpy(destination, image_.data() + (address - base_), size);
        return true;
    }

    [[nodiscard]] std::uintptr_t base() const noexcept { return base_; }

private:
    std::vector<std::uint8_t> const &image_;
    std::uintptr_t base_;
};

class SyntheticValidationEnvironment final : public spark::levilamina::detail::HostRawParameterValidationEnvironment {
public:
    std::uintptr_t rule_address = 0;
    std::uintptr_t loaded_module = 0x2000;
    bool allow_rule = true;
    bool allow_executable = false;
    SyntheticReader reader_storage;

    [[nodiscard]] bool imageRangeOwnedByHost(
        std::uintptr_t address, std::size_t size, std::uintptr_t current_module,
        spark::levilamina::detail::HostRawImageAccess access, std::uintptr_t &containing_module,
        std::string &basename) const noexcept override
    {
        containing_module = 0;
        basename.clear();
        if (current_module == 0 || size == 0) {
            return false;
        }
        if (access == spark::levilamina::detail::HostRawImageAccess::Readable && allow_rule &&
            address == rule_address && size == sizeof(::CommandRegistry::ParamParseRule)) {
            containing_module = loaded_module;
            basename = "levilamina.dll";
            return true;
        }
        if (access == spark::levilamina::detail::HostRawImageAccess::Executable && allow_executable) {
            containing_module = loaded_module;
            basename = "bedrock_server.exe";
            return true;
        }
        return false;
    }

    [[nodiscard]] std::uintptr_t loadedLeviLamina() const noexcept override { return loaded_module; }

    [[nodiscard]] Reader const &reader() const noexcept override { return reader_storage; }
};

static_assert(sizeof(::CommandRegistry::ParseFunction) == sizeof(std::uintptr_t));

::CommandRegistry::ParseFunction fakeParser() noexcept
{
    return std::bit_cast<::CommandRegistry::ParseFunction>(std::uintptr_t{1});
}

bool valid(ImportFixture &fixture)
{
    spark::levilamina::detail::RawTextImportCell cell;
    std::string error;
    return spark::levilamina::detail::findRawTextDelayImport(fixture.reader, fixture.imageBase(), fixture.imageBase(),
                                                             cell, error) &&
           cell.address == fixture.imageBase() + ImportFixture::kIatRva && cell.value == fixture.parser();
}

bool validParser(ImportFixture &fixture, std::uintptr_t parser)
{
    std::string provenance;
    std::string error;
    return spark::levilamina::detail::validateRawTextParserProvenance(
        fixture.reader, fixture.imageBase(), fixture.imageBase(), parser, fixture.spark(), provenance, error);
}

void requireRejected(ImportFixture &fixture, std::string_view expected_error, char const *message)
{
    spark::levilamina::detail::RawTextImportCell cell;
    std::string error;
    const bool found = spark::levilamina::detail::findRawTextDelayImport(
        fixture.reader, fixture.imageBase(), fixture.imageBase(), cell, error);
    require(!found, message);
    require(error == expected_error, message);
}

#endif

}  // namespace

int main()
{
    try {
#ifdef _WIN32
        require(
            spark::levilamina::hostImageProtectionAllowed(PAGE_READONLY, spark::levilamina::HostImageAccess::Readable),
            "readable host protection was rejected");
        require(
            !spark::levilamina::hostImageProtectionAllowed(PAGE_NOACCESS, spark::levilamina::HostImageAccess::Readable),
            "no-access host protection was accepted");
        require(!spark::levilamina::hostImageProtectionAllowed(PAGE_READONLY | PAGE_GUARD,
                                                               spark::levilamina::HostImageAccess::Readable),
                "guarded host protection was accepted");
        require(spark::levilamina::hostImageProtectionAllowed(PAGE_EXECUTE_READ,
                                                              spark::levilamina::HostImageAccess::Executable),
                "executable host protection was rejected");
        require(!spark::levilamina::hostImageProtectionAllowed(PAGE_READONLY,
                                                               spark::levilamina::HostImageAccess::Executable),
                "non-executable host protection was accepted");

        ImportFixture fixture;
        require(valid(fixture), "unique exact named delay import was not found");
        require(validParser(fixture, fixture.parser()), "named-import parser provenance was rejected");

        std::string error;
        spark::levilamina::detail::RawTextImportCell cell;
        require(!spark::levilamina::detail::findRawTextDelayImport(fixture.reader, fixture.imageBase(),
                                                                   fixture.imageBase() + 1, cell, error),
                "wrong importer handle was accepted");

        {
            ImportFixture candidate;
            auto descriptor = ImgDelayDescr{};
            std::memcpy(&descriptor, candidate.image.data() + ImportFixture::kDelayDirectoryRva, sizeof(descriptor));
            descriptor.grAttrs = 0;
            candidate.write(ImportFixture::kDelayDirectoryRva, descriptor);
            requireRejected(candidate, "parser provenance delay-import descriptor format is unsupported",
                            "unsupported delay descriptor format was accepted");
        }
        {
            ImportFixture candidate;
            candidate.writeString(ImportFixture::kDllNameRva, "wrong_import.dll");
            requireRejected(candidate, "parser provenance exact RawText import is missing",
                            "wrong importer name was accepted");
        }
        {
            ImportFixture candidate;
            candidate.setSymbol("wrong_symbol");
            requireRejected(candidate, "parser provenance exact RawText import is missing",
                            "wrong decorated symbol was accepted");
        }
        {
            ImportFixture candidate;
            candidate.setIntValue(IMAGE_ORDINAL_FLAG64 | 7);
            requireRejected(candidate, "parser provenance exact RawText import is missing",
                            "ordinal import was accepted as RawText");
        }
        {
            ImportFixture candidate;
            candidate.addAdjacentImport();
            require(valid(candidate), "adjacent non-target import changed the positive fixture");
            require(validParser(candidate, candidate.parser()), "positive parser provenance changed after adjacent import");
            require(!validParser(candidate, candidate.adjacent()), "adjacent import cell was accepted");
        }
        {
            ImportFixture candidate;
            require(!validParser(candidate, candidate.adjacent()), "mismatched parser pointer was accepted");
            candidate.setIatValue(0);
            require(!validParser(candidate, candidate.parser()), "null current IAT cell was accepted");
        }
        {
            ImportFixture candidate;
            candidate.addDuplicateDescriptor();
            requireRejected(candidate, "parser provenance exact RawText import is duplicated",
                            "duplicate named import cells were accepted");
        }
        {
            ImportFixture candidate;
            candidate.setIatValue(candidate.spark());
            require(!validParser(candidate, candidate.spark()), "Spark allocation was accepted as parser target");
        }
        {
            ImportFixture candidate;
            candidate.reader.mappings[1].protection |= spark::levilamina::detail::kPageGuard;
            require(!validParser(candidate, candidate.parser()), "guarded parser target was accepted");
            candidate.reader.mappings[1].protection = spark::levilamina::detail::kPageNoAccess;
            require(!validParser(candidate, candidate.parser()), "no-access parser target was accepted");
            candidate.reader.mappings[1].protection = spark::levilamina::detail::kPageExecuteRead;
            candidate.reader.mappings[1].state = 0;
            require(!validParser(candidate, candidate.parser()), "uncommitted parser target was accepted");
            candidate.reader.mappings[1].state = spark::levilamina::detail::kMemoryCommit;
            candidate.reader.mappings[1].protection = spark::levilamina::detail::kPageReadOnly;
            require(!validParser(candidate, candidate.parser()), "non-executable parser target was accepted");
        }
        {
            ImportFixture candidate;
            auto dos = IMAGE_DOS_HEADER{};
            std::memcpy(&dos, candidate.image.data(), sizeof(dos));
            dos.e_magic = 0;
            candidate.write(0, dos);
            requireRejected(candidate, "parser provenance DOS header is invalid", "invalid PE header was accepted");
        }
        {
            ImportFixture candidate;
            candidate.reader.mappings[0].size = 0x100;
            requireRejected(candidate, "parser provenance read is outside a readable region",
                            "cross-region PE read was accepted");
        }
        {
            ImportFixture candidate;
            candidate.reader.mappings[0].allocation_base = candidate.imageBase() + 1;
            requireRejected(candidate, "parser provenance read failed image-region validation",
                            "cross-allocation PE read was accepted");
        }
        {
            ImportFixture candidate;
            candidate.reader.deny_reads = true;
            requireRejected(candidate, "parser provenance reader failed", "read-denial fixture was accepted");
        }
        {
            ImportFixture candidate;
            std::memset(candidate.image.data() + ImportFixture::kImportNameRva + sizeof(WORD), 'x', 2048);
            requireRejected(candidate, "parser provenance string terminator exceeds the bound",
                            "unterminated bounded import name was accepted");
        }
        {
            ImportFixture candidate;
            require(!spark::levilamina::detail::findRawTextDelayImport(
                        candidate.reader, (std::numeric_limits<std::uintptr_t>::max)(),
                        (std::numeric_limits<std::uintptr_t>::max)(), cell, error),
                    "overflowing importer address was accepted");
            require(error == "parser provenance read is outside a readable region",
                    "overflowing importer address returned the wrong error");
        }
        {
            ImportFixture candidate;
            auto descriptor = ImgDelayDescr{};
            std::memcpy(&descriptor, candidate.image.data() + ImportFixture::kDelayDirectoryRva + sizeof(ImgDelayDescr),
                        sizeof(descriptor));
            descriptor.rvaDLLName = ImportFixture::kOtherDllNameRva;
            descriptor.grAttrs = dlattrRva;
            descriptor.rvaINT = ImportFixture::kIntRva;
            descriptor.rvaIAT = ImportFixture::kIatRva;
            candidate.write(ImportFixture::kDelayDirectoryRva + sizeof(ImgDelayDescr), descriptor);
            requireRejected(candidate, "parser provenance descriptor bound exhausted",
                            "missing delay descriptor terminator was accepted");
        }
        {
            ImportFixture candidate;
            candidate.setDelayDirectorySize(65 * sizeof(ImgDelayDescr));
            for (std::size_t index = 1; index < 65; ++index) {
                ImgDelayDescr descriptor{};
                descriptor.grAttrs = dlattrRva;
                descriptor.rvaDLLName = ImportFixture::kOtherDllNameRva;
                descriptor.rvaINT = ImportFixture::kIntRva;
                descriptor.rvaIAT = ImportFixture::kIatRva;
                candidate.write(ImportFixture::kDelayDirectoryRva + index * sizeof(ImgDelayDescr), descriptor);
            }
            requireRejected(candidate, "parser provenance delay-import descriptor bound exhausted",
                            "descriptor bound exhaustion was accepted");
        }
        {
            ImportFixture candidate;
            candidate.writeString(ImportFixture::kDllNameRva, "other_import.dll");
            candidate.fillThunkTablesWithoutTerminator();
            requireRejected(candidate, "parser provenance thunk bound exhausted without a terminator",
                            "thunk bound exhaustion was accepted");
        }
        {
            ImportFixture candidate;
            candidate.setImageSize(0x80000000U);
            candidate.setDelayDirectoryRva(0x7fffffe0U);
            candidate.setDelayDirectorySize(sizeof(ImgDelayDescr));
            OverflowReader reader{candidate.image};
            require(!spark::levilamina::detail::findRawTextDelayImport(reader, reader.base(), reader.base(), cell, error),
                    "in-image RVA address overflow was accepted");
            require(error == "parser provenance RVA address overflow", "in-image RVA returned the wrong error");
        }

        static_assert(sizeof(::CommandRegistry::ParamParseRule) == 16);
        static ::CommandRegistry::ParamParseRule host_rule{};
        host_rule.symbol.get() = ::CommandRegistry::Symbol{::CommandRegistry::HardNonTerminal::RawText};

        ::CommandParameterData host_data{};
        host_data.mTypeIndex = Bedrock::typeid_t<::CommandRegistry>{42};
        host_data.mParseRule = &host_rule;
        host_data.mParseOverride = nullptr;
        host_data.mName = "raw";
        host_data.mParamType = ::CommandParameterDataType::Basic;
        host_data.mIsOptional = false;

        const auto copied = spark::levilamina::copyHostRawParameterFields(host_data, "LeviLamina.dll", "null");
        require(copied.parse_rule == &host_rule, "host rule pointer was not copied");
        require(copied.parse_override == nullptr, "null host parser was not preserved");
        require(copied.type_index == host_data.mTypeIndex, "host type index was not copied");

        SimulatedDisabledOverload disabled{copied, 1};
        SimulatedDisabledOverload reloaded{disabled.params, 2};
        require(reloaded.params.parse_rule == &host_rule, "disabled overload lost the host rule");
        require(reloaded.params.type_index == copied.type_index, "disabled overload changed host type index");
        require(reloaded.factory_generation == 2 && disabled.factory_generation == 1,
                "reload simulation did not replace only the factory");
        std::fprintf(stderr, "host-raw-cache-simulation: rule=host parser=null factory=1->2\n");

        {
            SyntheticValidationEnvironment environment;
            environment.rule_address = reinterpret_cast<std::uintptr_t>(&host_rule);
            spark::levilamina::HostRawParameterTemplate value{.parse_rule = &host_rule};
            require(spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0x1000, environment, error),
                    "production validator rejected a valid null parser");
            require(value.rule_module == "levilamina.dll" && value.parser_module == "null",
                    "production validator did not preserve null parser provenance");
        }
        {
            SyntheticValidationEnvironment environment;
            environment.rule_address = reinterpret_cast<std::uintptr_t>(&host_rule);
            environment.allow_executable = true;
            spark::levilamina::HostRawParameterTemplate value{.parse_override = fakeParser(), .parse_rule = &host_rule};
            require(spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0x1000, environment, error),
                    "production validator rejected a valid parser override");
            require(value.parser_module == "bedrock_server.exe", "production validator lost parser override identity");
        }
        {
            SyntheticValidationEnvironment environment;
            environment.rule_address = reinterpret_cast<std::uintptr_t>(&host_rule);
            spark::levilamina::HostRawParameterTemplate value{.parse_rule = &host_rule};
            host_rule.parse.get() = fakeParser();
            require(!spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0x1000, environment, error),
                    "production validator accepted an invalid rule parser");
            require(error == "host raw rule parser is outside the LL/BDS image",
                    "invalid rule parser returned the wrong error");
            host_rule.parse.get() = nullptr;
        }
        {
            SyntheticValidationEnvironment environment;
            environment.rule_address = reinterpret_cast<std::uintptr_t>(&host_rule);
            spark::levilamina::HostRawParameterTemplate value{.parse_rule = &host_rule};
            host_rule.symbol.get() = ::CommandRegistry::Symbol{::CommandRegistry::HardNonTerminal::Int};
            require(!spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0x1000, environment, error),
                    "production validator accepted a non-RawText rule");
            require(error == "host raw parse rule symbol is not RawText", "invalid rule symbol returned the wrong error");
            host_rule.symbol.get() = ::CommandRegistry::Symbol{::CommandRegistry::HardNonTerminal::RawText};
        }
        {
            SyntheticValidationEnvironment environment;
            environment.rule_address = reinterpret_cast<std::uintptr_t>(&host_rule);
            environment.allow_rule = false;
            spark::levilamina::HostRawParameterTemplate value{.parse_rule = &host_rule};
            require(!spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0x1000, environment, error),
                    "production validator accepted a rule outside host images");
            require(error == "host raw parse rule is outside the LL/BDS image",
                    "invalid rule image returned the wrong error");
        }
        {
            SyntheticValidationEnvironment environment;
            environment.rule_address = reinterpret_cast<std::uintptr_t>(&host_rule);
            spark::levilamina::HostRawParameterTemplate value;
            require(!spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0x1000, environment, error),
                    "production validator accepted a null parse rule");
            require(error == "host raw template has no verifiable current module or parse rule",
                    "null parse rule returned the wrong error");
            value.parse_rule = &host_rule;
            require(!spark::levilamina::detail::validateHostRawParameterTemplateInternal(
                        value, 0, environment, error),
                    "production validator accepted a null current module");
            require(error == "host raw template has no verifiable current module or parse rule",
                    "null current module returned the wrong error");
        }
        return 0;
#else
        return 0;
#endif
    }
    catch (std::exception const& exception) {
        std::fprintf(stderr, "host-raw-parameter-test: %s\n", exception.what());
        return 1;
    }
    catch (...) {
        return 1;
    }
}

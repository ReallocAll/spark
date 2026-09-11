#include <elf.h>

#undef DT_RELRSZ
#undef DT_RELR
#undef DT_RELRENT

#include <cstdio>
#include <memory>

#include "native/alloc/linux_elf_admission.h"

namespace {

bool verifyRelocationTags(const char *path, const spark::gateway::elf::Object &object)
{
    using spark::gateway::elf::require;
    spark::gateway::elf::File source(path);
    struct stat status{};
    require(::fstat(source.descriptor(), &status) == 0, "cannot size relocation fixture");
    std::vector<char> bytes(status.st_size);
    source.read(0, bytes.data(), bytes.size());
    const auto segment =
        std::ranges::find_if(object.headers, [](const auto &header) { return header.p_type == PT_DYNAMIC; });
    require(segment != object.headers.end() && segment->p_offset <= bytes.size() &&
                segment->p_filesz <= bytes.size() - segment->p_offset &&
                segment->p_filesz / sizeof(Elf64_Dyn) >= object.dynamic.size() + 4,
            "relocation fixture lacks dynamic padding");
    const auto offset = segment->p_offset + object.dynamic.size() * sizeof(Elf64_Dyn);
    const std::array<std::vector<Elf64_Dyn>, 3> cases = {
        std::vector<Elf64_Dyn>{{.d_tag = DT_RELSZ, .d_un = {.d_val = sizeof(Elf64_Rel)}}},
        std::vector<Elf64_Dyn>{{.d_tag = 35, .d_un = {.d_val = sizeof(Elf64_Addr)}}},
        std::vector<Elf64_Dyn>{{.d_tag = 35, .d_un = {.d_val = sizeof(Elf64_Addr)}},
                               {.d_tag = 36, .d_un = {.d_val = object.tag(DT_RELA)}},
                               {.d_tag = 37, .d_un = {.d_val = sizeof(Elf64_Addr)}}}};
    for (const auto &tags : cases) {
        auto modified = bytes;
        std::memcpy(modified.data() + offset, tags.data(), tags.size() * sizeof(Elf64_Dyn));
        const Elf64_Dyn end{};
        std::memcpy(modified.data() + offset + tags.size() * sizeof(Elf64_Dyn), &end, sizeof(end));
        const std::unique_ptr<std::FILE, decltype(&std::fclose)> temporary(std::tmpfile(), &std::fclose);
        require(temporary != nullptr, "cannot create relocation fixture");
        require(std::fwrite(modified.data(), 1, modified.size(), temporary.get()) == modified.size() &&
                    std::fflush(temporary.get()) == 0,
                "cannot write relocation fixture");
        try {
            spark::gateway::elf::Object candidate;
            candidate.read("/proc/self/fd/" + std::to_string(::fileno(temporary.get())), true);
        }
        catch (const std::exception &error) {
            if (std::string_view(error.what()) == "helper has unsupported REL or RELR relocations") {
                continue;
            }
            throw;
        }
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        return 2;
    }
    try {
        spark::gateway::elf::Object object;
        const bool helper = argc == 3 || std::string_view(argv[3]) != "dependency";
        object.read(argv[1], helper);
        if (helper && std::string_view(argv[2]) == "accept" && !verifyRelocationTags(argv[1], object)) {
            return 1;
        }
        return std::string_view(argv[2]) == "accept" ? 0 : 1;
    }
    catch (const std::exception &error) {
        std::printf("parser rejection: %s\n", error.what());
        return std::string_view(argv[2]) == "reject" ? 0 : 1;
    }
}

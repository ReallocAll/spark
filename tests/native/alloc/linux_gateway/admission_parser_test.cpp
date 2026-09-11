#include <cstdio>

#include "native/alloc/linux_elf_admission.h"

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        return 2;
    }
    try {
        spark::gateway::elf::Object object;
        const bool helper = argc == 3 || std::string_view(argv[3]) != "dependency";
        object.read(argv[1], helper);
        return std::string_view(argv[2]) == "accept" ? 0 : 1;
    }
    catch (const std::exception &error) {
        std::printf("parser rejection: %s\n", error.what());
        return std::string_view(argv[2]) == "reject" ? 0 : 1;
    }
}

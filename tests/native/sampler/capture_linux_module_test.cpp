#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "native/sampler/capture.h"
#include "native/sampler/capture_linux_module.h"

namespace {

void mainCode() {}

void testCandidates()
{
    using spark::detail::mainExecutableCandidateOwnsAddress;
    constexpr auto max_address = std::numeric_limits<std::uintptr_t>::max();
    std::array<ElfW(Phdr), 3> headers{};
    headers[0].p_type = PT_LOAD;
    headers[0].p_flags = PF_R | PF_X;
    headers[0].p_vaddr = 0x1000;
    headers[0].p_memsz = 0x100;
    headers[1].p_type = PT_LOAD;
    headers[1].p_flags = PF_R;
    headers[1].p_vaddr = 0x2000;
    headers[1].p_memsz = 0x100;
    dl_phdr_info info{};
    info.dlpi_phdr = headers.data();
    info.dlpi_phnum = headers.size();
    info.dlpi_name = "same-name";
    const auto table = reinterpret_cast<std::uintptr_t>(headers.data());
    const auto owns = [&](std::uintptr_t address) {
        return mainExecutableCandidateOwnsAddress(info, sizeof(info), table, headers.size(), sizeof(ElfW(Phdr)),
                                                  address);
    };
    assert(owns(0x1000));
    assert(owns(0x10ff));
    assert(!owns(0x1100));
    assert(!owns(0xfff));
    assert(!owns(0));
    assert(!owns(0x2000));
    info.dlpi_addr = 0x10000;
    assert(owns(0x11000));
    assert(owns(0x110ff));
    assert(!owns(0x11100));
    assert(!owns(0x1000));
    auto other_headers = headers;
    info.dlpi_phdr = other_headers.data();
    assert(!owns(0x11000));
    info.dlpi_phdr = headers.data();
    --info.dlpi_phnum;
    assert(!owns(0x11000));
    ++info.dlpi_phnum;
    info.dlpi_name = nullptr;
    assert(owns(0x11000));
    info.dlpi_addr = 0;
    assert(mainExecutableCandidateOwnsAddress(info, spark::detail::KMainPhdrInfoSize, table, headers.size(),
                                              sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, spark::detail::KMainPhdrInfoSize - 1, table, headers.size(),
                                               sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 0, headers.size(), sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), table, 0, sizeof(ElfW(Phdr)), 0x1000));
    assert(
        !mainExecutableCandidateOwnsAddress(info, sizeof(info), table, headers.size(), sizeof(ElfW(Phdr)) - 1, 0x1000));
    info.dlpi_phdr = reinterpret_cast<const ElfW(Phdr) *>(1);
    info.dlpi_phnum = 1;
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 1, 1, sizeof(ElfW(Phdr)), 0));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 0, 1, sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 1, 0, sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 1, 1, sizeof(ElfW(Phdr)) - 1, 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, 0, 1, 1, sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 2, 1, sizeof(ElfW(Phdr)), 0x1000));
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 1, 2, sizeof(ElfW(Phdr)), 0x1000));
    info.dlpi_phnum = spark::detail::KMaxMainPhdrCount + 1;
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), 1, info.dlpi_phnum, sizeof(ElfW(Phdr)), 0x1000));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    info.dlpi_phdr = reinterpret_cast<const ElfW(Phdr) *>(max_address - sizeof(ElfW(Phdr)) + 1);
    info.dlpi_phnum = 1;
    assert(!mainExecutableCandidateOwnsAddress(info, sizeof(info), reinterpret_cast<std::uintptr_t>(info.dlpi_phdr), 1,
                                               sizeof(ElfW(Phdr)), 0x1000));
    info.dlpi_phdr = headers.data();
    info.dlpi_phnum = headers.size();
    const auto original = headers;
    for (const auto overflow_bias : {false, true}) {
        headers = original;
        headers[2] = headers[0];
        headers[2].p_vaddr = max_address - 8;
        headers[2].p_memsz = overflow_bias ? 1 : 16;
        info.dlpi_addr = overflow_bias ? 16 : 0;
        const auto address = 0x1000 + info.dlpi_addr;
        assert(!owns(address));
        std::ranges::reverse(headers);
        assert(!owns(address));
    }
    headers = original;
    info.dlpi_addr = 0;
    headers[0].p_type = PT_NOTE;
    assert(!owns(0x1000));
    headers[0] = original[0];
    headers[0].p_memsz = 0;
    assert(!owns(0x1000));
    headers = original;
    std::ranges::reverse(headers);
    assert(owns(0x1000));
    std::array<ElfW(Phdr), spark::detail::KMaxMainPhdrCount> maximum_headers{};
    maximum_headers.back() = original[0];
    info.dlpi_phdr = maximum_headers.data();
    info.dlpi_phnum = maximum_headers.size();
    assert(mainExecutableCandidateOwnsAddress(info, sizeof(info), reinterpret_cast<std::uintptr_t>(info.dlpi_phdr),
                                              maximum_headers.size(), sizeof(ElfW(Phdr)), 0x1000));
}

template <typename Function>
Function loadFunction(void *module, const char *name)
{
    auto function = reinterpret_cast<Function>(::dlsym(module, name));
    assert(function != nullptr);
    return function;
}

void testModule(bool fail_named_pin)
{
    void *module = ::dlopen(SPARK_CAPTURE_MODULE_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    assert(module != nullptr);
    const auto arm = loadFunction<bool (*)(bool)>(module, "sparkCaptureFixtureArm");
    const auto disarm = loadFunction<bool (*)()>(module, "sparkCaptureFixtureDisarm");
    const auto handler = loadFunction<std::uintptr_t (*)()>(module, "sparkCaptureFixtureHandler");
    const auto named_calls = loadFunction<int (*)()>(module, "sparkCaptureFixtureNamedCalls");
    const auto null_calls = loadFunction<int (*)()>(module, "sparkCaptureFixtureNullCalls");
    const auto flags = loadFunction<int (*)()>(module, "sparkCaptureFixtureFlags");
    assert(!spark::detail::mainExecutableOwnsAddress(handler()));
    assert(arm(fail_named_pin) == !fail_named_pin);
    assert(named_calls() == 1);
    assert(null_calls() == 0);
    assert(flags() == (RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE));
    if (fail_named_pin) {
        assert(!arm(false));
        assert(named_calls() == 1);
        assert(null_calls() == 0);
        assert(disarm());
        assert(::dlclose(module) == 0);
    }
    else {
        assert(::dlclose(module) == 0);
        void *resident = ::dlopen(SPARK_CAPTURE_MODULE_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        assert(resident != nullptr);
        assert(disarm());
        assert(arm(false));
        assert(disarm());
        assert(named_calls() == 1);
        assert(null_calls() == 0);
        assert(::dlclose(resident) == 0);
    }
    std::printf("DSO pin %s: named=1 null=0 flags=%d residency=%s\n", fail_named_pin ? "failure" : "success",
                RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE, fail_named_pin ? "not-required" : "RTLD_NOLOAD-proven");
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--dso-success") == 0) {
        testModule(false);
    }
    else if (argc == 2 && std::strcmp(argv[1], "--dso-failure") == 0) {
        testModule(true);
    }
    else {
        assert(argc == 1);
        testCandidates();
        assert(spark::detail::mainExecutableOwnsAddress(reinterpret_cast<std::uintptr_t>(&mainCode)));
        assert(!spark::detail::mainExecutableOwnsAddress(0));
        assert(spark::Capture::arm());
        assert(spark::Capture::disarm());
        std::puts("ELF predicates and main executable arm/disarm passed");
    }
}

#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <chrono>
#include <cstdint>
#include <span>

namespace spark::symbol_guess::windows_test {

bool testPeAndFunctionRanges()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    windows::Engine engine = fixture.engine();
    SPARK_SYMBOL_GUESS_CHECK(engine.valid());
    SPARK_SYMBOL_GUESS_CHECK(engine.stats().function_ranges == 1);
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x0fff) == nullptr);
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x1000)->root == 0x1000);
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x107f)->root == 0x1000);
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x1080) == nullptr);
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(UINT64_MAX) == nullptr);

    fixture.bytes()[0] = 0;
    SPARK_SYMBOL_GUESS_CHECK(!fixture.engine().valid());
    fixture.bytes()[0] = 'M';
    fixture.bytes()[1] = 'Z';
    fixture.exceptionDirectory(0x4ffc, sizeof(RUNTIME_FUNCTION));
    SPARK_SYMBOL_GUESS_CHECK(!fixture.engine().valid());
    return true;
}

bool testChainedAndMalformedUnwind()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    RUNTIME_FUNCTION parent{};
    parent.BeginAddress = 0x1000;
    parent.EndAddress = 0x1080;
    parent.UnwindData = 0x5000;
    fixture.chainedUnwind(0x5020, 1, parent);
    fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5020);
    windows::Engine engine = fixture.engine();
    SPARK_SYMBOL_GUESS_CHECK(engine.valid());
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x117f)->root == 0x1000);
    SPARK_SYMBOL_GUESS_CHECK(engine.stats().chained_ranges == 1);

    RUNTIME_FUNCTION cycle{};
    cycle.BeginAddress = 0x1200;
    cycle.EndAddress = 0x1280;
    cycle.UnwindData = 0x5040;
    fixture.chainedUnwind(0x5040, 0, cycle);
    fixture.runtimeFunction(2, 0x1200, 0x1280, 0x5040);
    windows::Engine malformed = fixture.engine();
    SPARK_SYMBOL_GUESS_CHECK(malformed.valid());
    SPARK_SYMBOL_GUESS_CHECK(malformed.functionContaining(0x1210) == nullptr);
    SPARK_SYMBOL_GUESS_CHECK(malformed.stats().rejected_ranges >= 1);
    return true;
}

bool testDuplicateOverlapAndDeterminism()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    fixture.runtimeFunction(1, 0x1000, 0x1080, 0x5000);
    fixture.runtimeFunction(2, 0x1040, 0x10c0, 0x5000);
    fixture.runtimeFunction(3, 0x1100, 0x1180, 0x5000);
    windows::Engine engine = fixture.engine();
    SPARK_SYMBOL_GUESS_CHECK(engine.valid());
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x1050) == nullptr);
    SPARK_SYMBOL_GUESS_CHECK(engine.functionContaining(0x1110) != nullptr);
    SPARK_SYMBOL_GUESS_CHECK(engine.stats().overlap_ranges == 2);
    return true;
}

bool testLargeRangeLookup()
{
    constexpr std::uint32_t k_count = 120000;
    constexpr std::uint32_t k_text = 0x1000;
    constexpr std::uint32_t k_pdata = 0x200000;
    constexpr std::uint32_t k_xdata = 0x380000;
    constexpr std::uint32_t k_size = 0x390000;
    PeFixture fixture(k_size);
    fixture.section(0, ".text", k_text, k_pdata - k_text,
                    IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE);
    fixture.section(1, ".rdata", 0x100, 0x100, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(2, ".pdata", k_pdata, k_count * sizeof(RUNTIME_FUNCTION),
                    IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.section(3, ".xdata", k_xdata, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA);
    fixture.leafUnwind(k_xdata);
    for (std::uint32_t i = 0; i < k_count; ++i) {
        RUNTIME_FUNCTION function{};
        function.BeginAddress = k_text + i * 8;
        function.EndAddress = function.BeginAddress + 4;
        function.UnwindData = k_xdata;
        fixture.put(k_pdata + i * sizeof(function), function);
    }
    fixture.exceptionDirectory(k_pdata, k_count * sizeof(RUNTIME_FUNCTION));
    windows::Engine engine = fixture.engine();
    SPARK_SYMBOL_GUESS_CHECK(engine.valid());
    SPARK_SYMBOL_GUESS_CHECK(engine.stats().function_ranges == k_count);
    const auto start = std::chrono::steady_clock::now();
    std::uint64_t checksum = 0;
    for (std::uint32_t i = 0; i < 1000000; ++i) {
        const std::uint64_t rva = k_text + (i % k_count) * 8;
        const windows::FunctionRange *range = engine.functionContaining(rva);
        SPARK_SYMBOL_GUESS_CHECK(range != nullptr);
        checksum += range->root;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    SPARK_SYMBOL_GUESS_CHECK(checksum != 0);
    SPARK_SYMBOL_GUESS_CHECK(elapsed < std::chrono::seconds(5));
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif

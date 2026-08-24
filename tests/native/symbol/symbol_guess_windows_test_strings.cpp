#include "symbol_guess_windows_test_support.h"

#ifdef _WIN32

#include <cstdint>
#include <span>

namespace spark::symbol_guess::windows_test {

bool testDecodedStringsAndScoring()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    fixture.string(0x3000, "%.2f MB of dynamic properties were saved during the "
                           "last minute, exceeding the limit");
    fixture.string(0x3100, "Server Level - tick");
    fixture.lea(0x1000, 0x3000);
    fixture.lea(0x1007, 0x3100);
    fixture.putBytes(0x100e, {0xc3});
    windows::Engine engine = fixture.engine();
    const std::uint64_t query = 0x1008;
    const auto guesses = engine.guess(std::span(&query, 1));
    SPARK_SYMBOL_GUESS_CHECK(guesses.at(query).label == "str: Server Level - tick");
    SPARK_SYMBOL_GUESS_CHECK(
        windows::scoreStringHint("Server Level - tick") >
        windows::scoreStringHint("%.2f MB of dynamic properties were saved during the last minute"));
    SPARK_SYMBOL_GUESS_CHECK(
        windows::scoreStringHint("T *Bedrock::NonOwnerPointer<ChunkPerformanceData>::_get() const") < 50);
    SPARK_SYMBOL_GUESS_CHECK(windows::scoreStringHint("Name: ") < 50);
    SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query) == guesses.at(query));
    return true;
}

bool testInstructionMiddleAndSharedString()
{
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.string(0x3100, "Level - tick redstone");
        fixture.putBytes(0x1000, {0x48, 0xb8, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0, 0xc3});
        const auto fake = static_cast<std::int32_t>(0x3100 - (0x1002 + 7));
        fixture.put(0x1005, fake);
        windows::Engine engine = fixture.engine();
        const std::uint64_t query = 0x1002;
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).empty());
    }
    {
        PeFixture fixture;
        fixture.leafUnwind(0x5000);
        fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
        fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5000);
        fixture.string(0x3100, "Level - tick shared work");
        fixture.lea(0x1000, 0x3100);
        fixture.putBytes(0x1007, {0xc3});
        fixture.lea(0x1100, 0x3100);
        fixture.putBytes(0x1107, {0xc3});
        windows::Engine engine = fixture.engine();
        const std::uint64_t queries[] = {0x1000, 0x1100};
        SPARK_SYMBOL_GUESS_CHECK(engine.guess(queries).empty());
        SPARK_SYMBOL_GUESS_CHECK(engine.stats().shared_strings >= 2);
    }
    return true;
}

bool testChainedRootStringUniqueness()
{
    PeFixture fixture;
    fixture.leafUnwind(0x5000);
    fixture.runtimeFunction(0, 0x1000, 0x1080, 0x5000);
    RUNTIME_FUNCTION parent{};
    parent.BeginAddress = 0x1000;
    parent.EndAddress = 0x1080;
    parent.UnwindData = 0x5000;
    fixture.chainedUnwind(0x5020, 2, parent);
    fixture.runtimeFunction(1, 0x1100, 0x1180, 0x5020);
    fixture.string(0x3100, "Level - tick chained work");
    fixture.lea(0x1000, 0x3100);
    fixture.putBytes(0x1007, {0xc3});
    fixture.lea(0x1100, 0x3100);
    fixture.putBytes(0x1107, {0xc3});
    windows::Engine engine = fixture.engine();
    const std::uint64_t query = 0x1110;
    SPARK_SYMBOL_GUESS_CHECK(engine.guess(std::span(&query, 1)).at(query).label == "str: Level - tick chained work");
    return true;
}

}  // namespace spark::symbol_guess::windows_test

#endif

#include <cassert>
#include <filesystem>
#include <iostream>
#include <vector>

#include "core/recovery/recovery_player.h"
#include "journal_test_support.h"
#include "native/sampler/sampler.h"

namespace spark {

struct RecoveryPlayerTestAccess {
    static RecoveredProfile replay(const std::filesystem::path &directory, std::size_t nodes, std::size_t times,
                                   std::size_t threads)
    {
        return RecoveryPlayer::replay(directory, nodes, times, threads);
    }
};

}  // namespace spark

namespace {

using namespace spark;
using namespace spark::journal_test;

Sample sample(std::uint64_t thread, std::int32_t window = 0)
{
    Sample value;
    value.thread_id = thread;
    value.tick_id = static_cast<std::uint64_t>(window);
    value.window = window;
    value.weight = 4000;
    value.frames = {{.module = 0, .rva = 0x1000}, {.module = 0, .rva = 0x2000}};
    return value;
}

void writeSamples(const std::filesystem::path &directory, const std::vector<Sample> &samples)
{
    std::vector<RecordSpec> records{
        {.type = RecordType::SessionConfig,
         .sequence = 0,
         .payload = buildSessionConfigPayload(4000, 0, false, false, false, 1, 0, false, "Console", false, {}, {}, 0)},
        {.type = RecordType::ModuleDef, .sequence = 1, .payload = buildModuleDefPayload(0, "recovery-fixture")}};
    for (const auto &value : samples) {
        records.push_back({.type = RecordType::Sample,
                           .sequence = static_cast<std::uint32_t>(records.size()),
                           .payload = buildSamplePayload(value)});
    }
    writeSegmentMulti(directory / "segment-0.jnl", 900000, 0, records);
}

void expectLimit(const RecoveredProfile &result, const std::string &error)
{
    assert(!result.valid);
    assert(result.resource_limit_exceeded);
    assert(result.serialized_proto.empty());
    assert(result.error == error);
}

void expectSuccess(const RecoveredProfile &result, std::uint64_t samples, std::uint64_t threads)
{
    assert(result.valid);
    assert(!result.resource_limit_exceeded);
    assert(!result.serialized_proto.empty());
    assert(result.sample_count == samples);
    assert(result.thread_count == threads);
}

}  // namespace

int main()
{
    const auto directory = std::filesystem::temp_directory_path() / "spark_recovery_admission_test";
    std::filesystem::create_directories(directory);

    writeSamples(directory, {sample(1), sample(1)});
    expectSuccess(RecoveryPlayerTestAccess::replay(directory, 4, 6, 1), 2, 1);
    expectLimit(RecoveryPlayerTestAccess::replay(directory, 3, 100, 10), "recovery exceeds profile node capacity");
    expectLimit(RecoveryPlayerTestAccess::replay(directory, 100, 5, 10),
                "recovery exceeds profile time entry capacity");

    // A new thread pays for its full path even when the global path already exists.
    writeSamples(directory, {sample(1), sample(2)});
    expectSuccess(RecoveryPlayerTestAccess::replay(directory, 6, 9, 2), 2, 2);
    expectLimit(RecoveryPlayerTestAccess::replay(directory, 5, 100, 10), "recovery exceeds profile node capacity");
    expectLimit(RecoveryPlayerTestAccess::replay(directory, 100, 8, 10),
                "recovery exceeds profile time entry capacity");
    expectLimit(RecoveryPlayerTestAccess::replay(directory, 100, 100, 1), "recovery exceeds thread root capacity");

    writeSamples(directory, {sample(1), sample(1, 60)});
    expectSuccess(RecoveryPlayerTestAccess::replay(directory, 4, 12, 1), 2, 1);
    expectLimit(RecoveryPlayerTestAccess::replay(directory, 100, 11, 10),
                "recovery exceeds profile time entry capacity");

    std::vector<Sample> history;
    for (std::uint64_t i = 0; i <= Sampler::threadRootCapacity(); ++i) {
        auto old = sample(i);
        old.frames[0].rva += i;
        history.push_back(old);
    }
    history.push_back(sample(1000, 61));
    writeSamples(directory, history);
    expectSuccess(RecoveryPlayerTestAccess::replay(directory, 4, 6, 1), 1, 1);

    std::vector<Sample> threads;
    for (std::uint64_t i = 0; i < Sampler::threadRootCapacity(); ++i) {
        threads.push_back(sample(i));
    }
    writeSamples(directory, threads);
    expectSuccess(RecoveryPlayer::replay(directory), threads.size(), threads.size());
    threads.push_back(sample(threads.size()));
    writeSamples(directory, threads);
    expectLimit(RecoveryPlayer::replay(directory), "recovery exceeds thread root capacity");

    std::filesystem::remove_all(directory);
    std::cout << "Recovery admission tests passed.\n";
}

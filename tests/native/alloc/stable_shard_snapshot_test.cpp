#include <cassert>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "native/alloc/stable_shard_snapshot.h"

namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

void successAndBalancedUnlock()
{
    std::vector<int> captured;
    std::size_t locks = 0;
    std::size_t unlocks = 0;
    const TimePoint now{};
    const bool success = spark::detail::captureStableShardSnapshot(
        3, now + std::chrono::seconds(1), [&] { captured.clear(); }, [] { return 7; },
        [&](std::size_t) {
            ++locks;
            return true;
        },
        [&](std::size_t) { ++unlocks; }, [&](std::size_t shard) { captured.push_back(static_cast<int>(shard)); },
        [&] { return now; }, [] {});
    assert(success);
    assert((captured == std::vector<int>{0, 1, 2}));
    assert(locks == 3);
    assert(unlocks == locks);
}

void contentionYieldsUntilLock()
{
    std::vector<int> captured;
    std::size_t attempts = 0;
    std::size_t yields = 0;
    const TimePoint now{};
    const bool success = spark::detail::captureStableShardSnapshot(
        1, now + std::chrono::seconds(1), [&] { captured.clear(); }, [] { return 1; },
        [&](std::size_t) { return ++attempts >= 3; }, [](std::size_t) {}, [&](std::size_t) { captured.push_back(1); },
        [&] { return now; }, [&] { ++yields; });
    assert(success);
    assert(attempts == 3);
    assert(yields == 2);
    assert(captured.size() == 1);
}

void versionRetryDiscardsPartialAttempt()
{
    std::vector<int> captured;
    int attempt = 0;
    int version_value = 4;
    const TimePoint now{};
    const bool success = spark::detail::captureStableShardSnapshot(
        2, now + std::chrono::seconds(1),
        [&] {
            captured.clear();
            ++attempt;
        },
        [&] { return version_value; }, [](std::size_t) { return true; }, [](std::size_t) {},
        [&](std::size_t shard) {
            captured.push_back(attempt * 10 + static_cast<int>(shard));
            if (attempt == 1 && shard == 0) {
                version_value = 5;
            }
        },
        [&] { return now; }, [] {});
    assert(success);
    assert(attempt == 2);
    assert((captured == std::vector<int>{20, 21}));
}

void timeoutDiscardsPartialAttempt()
{
    std::vector<int> captured;
    int ticks = 0;
    std::size_t yields = 0;
    const TimePoint start{};
    const bool success = spark::detail::captureStableShardSnapshot(
        2, start + std::chrono::milliseconds(3), [&] { captured.clear(); }, [] { return 1; },
        [](std::size_t) { return false; }, [](std::size_t) {}, [&](std::size_t) { captured.push_back(1); },
        [&] { return start + std::chrono::milliseconds(ticks); },
        [&] {
            ++yields;
            ++ticks;
        });
    assert(!success);
    assert(captured.empty());
    assert(yields == 3);
}

void exceptionUnlocksShard()
{
    std::size_t unlocks = 0;
    const TimePoint now{};
    bool threw = false;
    try {
        (void)spark::detail::captureStableShardSnapshot(
            1, now + std::chrono::seconds(1), [] {}, [] { return 1; }, [](std::size_t) { return true; },
            [&](std::size_t) { ++unlocks; }, [](std::size_t) { throw std::runtime_error("scan failed"); },
            [&] { return now; }, [] {});
    }
    catch (const std::runtime_error &) {
        threw = true;
    }
    assert(threw);
    assert(unlocks == 1);
}

}  // namespace

int main()
{
    successAndBalancedUnlock();
    contentionYieldsUntilLock();
    versionRetryDiscardsPartialAttempt();
    timeoutDiscardsPartialAttempt();
    exceptionUnlocksShard();
    return 0;
}

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

#include "native/alloc/windows_stable_entry_atomic.h"

using spark::stable_entry_experiment::encodeAtomic16AbsoluteEntry;
using spark::stable_entry_experiment::encodeAtomic8RelayEntry;
using spark::stable_entry_experiment::isAlignedForAtomic16;
using spark::stable_entry_experiment::isAlignedForAtomic8;
using spark::stable_entry_experiment::rel32Reachable;

int main()
{
    assert(isAlignedForAtomic8(0x1000));
    assert(!isAlignedForAtomic8(0x1004));
    assert(isAlignedForAtomic16(0x2000));
    assert(!isAlignedForAtomic16(0x2008));

    assert(rel32Reachable(0x1005, 0x2000));
    assert(rel32Reachable(0x2005, 0x1000));
    assert(rel32Reachable(0x1000, 0x1000 + static_cast<std::uintptr_t>(INT32_MAX)));
    assert(!rel32Reachable(0x1000, 0x1000 + static_cast<std::uintptr_t>(INT32_MAX) + 1ULL));

    std::array<std::uint8_t, 16> original{};
    for (std::size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<std::uint8_t>(0x40 + i);
    }

    std::array<std::uint8_t, 16> installed{};
    std::string error;
    constexpr std::uintptr_t entry = 0x10000000;
    constexpr std::uintptr_t relay = 0x10001000;
    assert(encodeAtomic8RelayEntry(entry, relay, original, installed, error));
    assert(error.empty());
    assert(installed[0] == 0xE9);
    std::int32_t displacement = 0;
    std::memcpy(&displacement, installed.data() + 1, sizeof(displacement));
    assert(static_cast<std::uintptr_t>(static_cast<std::int64_t>(entry + 5) + displacement) == relay);
    for (std::size_t i = 5; i < installed.size(); ++i) {
        assert(installed[i] == original[i]);
    }

    assert(!encodeAtomic8RelayEntry(entry + 4, relay, original, installed, error));
    assert(error.find("8-byte aligned") != std::string::npos);

    constexpr std::uintptr_t hook = 0x123456789ABCDEF0ULL;
    assert(encodeAtomic16AbsoluteEntry(entry, hook, original, installed, error));
    assert(installed[0] == 0xFF && installed[1] == 0x25);
    for (std::size_t i = 2; i < 6; ++i) {
        assert(installed[i] == 0);
    }
    std::uint64_t encoded_hook = 0;
    std::memcpy(&encoded_hook, installed.data() + 6, sizeof(encoded_hook));
    assert(encoded_hook == hook);
    assert(installed[14] == 0x90 && installed[15] == 0x90);

    assert(!encodeAtomic16AbsoluteEntry(entry + 8, hook, original, installed, error));
    assert(error.find("16-byte aligned") != std::string::npos);

    std::cout << "stable-entry atomic encoding tests passed\n";
    return 0;
}

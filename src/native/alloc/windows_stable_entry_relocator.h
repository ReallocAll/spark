#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace spark::stable_entry_experiment {

#ifdef _WIN32

struct BoundedRelocation {
    void *memory = nullptr;
    void *entry = nullptr;
    std::size_t allocation_size = 0;
    std::size_t code_size = 0;
    std::size_t patch_length = 0;
};

// Decode a complete >=5-byte x64 entry window, relocate it into a nearby
// executable buffer, expand short direct branches when necessary, and append an
// absolute jump back to source+patch_length. Unsupported or ambiguous encodings
// fail closed without modifying source.
bool prepareBoundedRelocation(void *source, BoundedRelocation &relocation, std::string &error);
void releaseBoundedRelocation(BoundedRelocation &relocation) noexcept;

#endif

}  // namespace spark::stable_entry_experiment

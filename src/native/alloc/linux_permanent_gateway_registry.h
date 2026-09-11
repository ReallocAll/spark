#ifndef ENDSTONE_SPARK_LINUX_PERMANENT_GATEWAY_REGISTRY_H
#define ENDSTONE_SPARK_LINUX_PERMANENT_GATEWAY_REGISTRY_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace spark::gateway::permanent {

inline constexpr std::uint64_t KClosed = std::uint64_t{1} << 63;
inline constexpr std::size_t KCapacity = 256;
inline constexpr std::size_t KEntryCount = KCapacity * 8;
inline constexpr std::size_t KProviderCapacity = KCapacity * 7 + 1;
inline constexpr std::size_t KCodeStride = 512;
inline constexpr std::size_t KCieSize = 32;
inline constexpr std::size_t KFdeStride = 64;
inline constexpr std::size_t KCodeSize = KCodeStride * KEntryCount;
inline constexpr std::size_t KTableOffset = KCieSize + KFdeStride * KEntryCount + 8;
inline constexpr std::size_t KMetadataSize = (KTableOffset + KEntryCount * 8 + 4095) & ~std::size_t{4095};
using Deadline = std::chrono::steady_clock::time_point;

struct alignas(64) Entry {
    std::atomic<std::uint64_t> state{KClosed};
    void *original = nullptr;
    void *callback = nullptr;
    void *context = nullptr;
#if defined(SPARK_GATEWAY_TESTING)
    std::uint64_t *gate = nullptr;
    std::uint32_t phase = 0;
#endif
};

struct Group {
    std::array<Entry, 8> entries;
    std::array<void *, 7> pending_leases{};
    std::uint32_t pending_count = 0;
    bool reserved = false;
    bool published = false;
    bool retired = false;
    bool final_close = false;
};

struct Provider {
    std::uint64_t base = 0;
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    void *handle = nullptr;
};

enum class HostRegistrationMode : std::uint32_t {
    Unknown,
    Sequence,
    SingleFde
};

struct HostUnwinder {
    Provider provider;
    std::array<std::uintptr_t, 5> functions{};
    HostRegistrationMode mode = HostRegistrationMode::Unknown;
    std::uint32_t version = 1;
};

struct State {
    std::atomic<std::uint32_t> lock{0};
    std::atomic<std::uint32_t> status{0};
    std::array<Group, KCapacity> groups;
    std::array<Provider, KProviderCapacity> providers{};
    std::uint32_t provider_count = 0;
#if defined(SPARK_GATEWAY_TESTING)
    std::uint32_t host_registrations = 0;
#endif
};

inline constexpr std::size_t KStateSize = (sizeof(State) + 4095) & ~std::size_t{4095};

struct Directory {
    std::uint64_t magic = 0x53475045524d3032;
    std::uint32_t version = 2;
    std::uint32_t size = sizeof(Directory);
    std::uint64_t pid = 0;
    std::uint64_t start_time = 0;
    std::uint64_t namespace_device = 0;
    std::uint64_t namespace_inode = 0;
    std::uint64_t code = 0;
    std::uint64_t code_size = KCodeSize;
    std::uint64_t metadata = 0;
    std::uint64_t metadata_size = KMetadataSize;
    std::uint64_t state = 0;
    std::uint64_t state_size = KStateSize;
    HostUnwinder host;
    char compatibility[65]{};
    char installation[4096]{};
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(void *) == sizeof(std::uint64_t));
static_assert(offsetof(Entry, state) == 0 && sizeof(Entry) == 64);
static_assert(KCodeSize + KMetadataSize + KStateSize < 4 * 1024 * 1024);

struct Layout {
    std::uint32_t push_end = 0;
    std::uint32_t frame_end = 0;
    std::uint32_t pop_end = 0;
    std::uint32_t callback_return = 0;
};

Layout emitEntry(unsigned char *output, std::uintptr_t entry, unsigned api);
void emitMetadata(unsigned char *output, std::uintptr_t code, const std::array<Layout, KEntryCount> &layouts);
bool bootstrap(const char *root, const void *anchor, std::string &error);
State *state() noexcept;
const Directory *directory() noexcept;

class Lock {
public:
    explicit Lock(State *state);
    ~Lock();
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;

private:
    State *state_;
};

}  // namespace spark::gateway::permanent

#endif

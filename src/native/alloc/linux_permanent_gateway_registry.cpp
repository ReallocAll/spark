#include "native/alloc/linux_permanent_gateway_registry.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "native/alloc/linux_elf_admission.h"
#include "native/alloc/linux_permanent_gateway_unwind.h"
#include "spark_gateway_compatibility.h"

namespace spark::gateway::permanent {
// NOLINTBEGIN(performance-no-int-to-ptr)
namespace {

std::atomic<State *> Current{nullptr};
Directory CurrentDirectory;
std::atomic<std::uint32_t> BootstrapLock{0};
constexpr int KSeals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;

struct Descriptor {
    int value = -1;
    ~Descriptor()
    {
        if (value >= 0) {
            ::close(value);
        }
    }
    int release() { return std::exchange(value, -1); }
};

struct Bytes {
    unsigned char *output;
    std::size_t size = 0;
    std::size_t capacity;

    void byte(unsigned char value)
    {
        elf::require(size < capacity, "gateway byte stream exceeds capacity");
        output[size++] = value;
    }
    void bytes(std::initializer_list<unsigned char> values)
    {
        for (auto value : values) {
            byte(value);
        }
    }
    template <typename T>
    void integer(T value)
    {
        elf::require(sizeof(value) <= capacity - size, "gateway byte stream exceeds capacity");
        std::memcpy(output + size, &value, sizeof(value));
        size += sizeof(value);
    }
    void pointer(std::uintptr_t value) { integer(value); }
    std::size_t branch(unsigned char condition)
    {
        bytes({0x0f, condition});
        const auto offset = size;
        integer(std::int32_t{0});
        return offset;
    }
    void target(std::size_t offset, std::size_t destination) const
    {
        const auto value = static_cast<std::int32_t>(destination) - static_cast<std::int32_t>(offset + 4);
        std::memcpy(output + offset, &value, sizeof(value));
    }
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void gate(unsigned phase)
    {
#if defined(SPARK_GATEWAY_TESTING)
        bytes(
            {0x41, 0x83, 0x7b, static_cast<unsigned char>(offsetof(Entry, phase)), static_cast<unsigned char>(phase)});
        const auto skip_phase = branch(0x85);
        bytes({0x4d, 0x8b, 0x53, static_cast<unsigned char>(offsetof(Entry, gate)), 0x4d, 0x85, 0xd2});
        const auto skip_null = branch(0x84);
        bytes({0x49, 0xc7, 0x02, 1, 0, 0, 0});
        const auto loop = size;
        bytes({0xf3, 0x90, 0x49, 0x83, 0x3a, 2});
        const auto wait = branch(0x85);
        target(wait, loop);
        target(skip_null, size);
        target(skip_phase, size);
#else
        (void)phase;
#endif
    }
};

bool mapped(std::uintptr_t begin, std::size_t size, const char *expected)
{
    if (begin == 0 || size == 0 || begin % 4096 != 0 || size % 4096 != 0 ||
        begin > std::numeric_limits<std::uintptr_t>::max() - size) {
        return false;
    }
    const auto end = begin + size;
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::size_t lines = 0;
    std::size_t bytes = 0;
    auto cursor = begin;
    while (std::getline(maps, line)) {
        if (++lines > 65536 || line.size() > 64 * 1024 * 1024 - bytes) {
            return false;
        }
        bytes += line.size();
        // NOLINTNEXTLINE(google-runtime-int)
        unsigned long long first = 0, last = 0, offset = 0, inode = 0;
        unsigned major = 0, minor = 0;
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s %llx %x:%x %llu", &first, &last, permissions, &offset, &major,
                        &minor, &inode) != 7 ||
            last <= cursor || first > cursor) {
            continue;
        }
        if (std::strcmp(permissions, expected) != 0 || inode != 0 || major != 0 || minor != 0) {
            return false;
        }
        cursor = std::min<std::uintptr_t>(last, end);
        if (cursor == end) {
            return true;
        }
    }
    return false;
}

Directory identity(const char *root)
{
    Directory result;
    elf::require(root != nullptr && root[0] == '/' && std::strlen(root) < sizeof(result.installation),
                 "invalid gateway installation identity");
    elf::require(std::filesystem::canonical(root).string() == root, "gateway installation is not canonical");
    std::strcpy(result.installation, root);
    static_assert(sizeof(SPARK_GATEWAY_COMPATIBILITY) == sizeof(result.compatibility));
    std::memcpy(result.compatibility, SPARK_GATEWAY_COMPATIBILITY, sizeof(result.compatibility));
    result.pid = ::getpid();
    struct stat status{};
    elf::require(::stat("/proc/self/ns/pid", &status) == 0, "cannot identify gateway PID namespace");
    result.namespace_device = status.st_dev;
    result.namespace_inode = status.st_ino;
    std::ifstream process("/proc/self/stat");
    std::string line;
    std::getline(process, line);
    const auto end = line.rfind(')');
    elf::require(end != std::string::npos, "cannot identify gateway process start");
    std::istringstream fields(line.substr(end + 1));
    std::string value;
    for (int field = 3; field <= 22; ++field) {
        elf::require(static_cast<bool>(fields >> value), "cannot identify gateway process start");
    }
    result.start_time = std::stoull(value);
    return result;
}

void render(const Directory &directory, unsigned char *code, unsigned char *metadata)
{
    std::array<Layout, KEntryCount> layouts{};
    for (std::size_t i = 0; i < KEntryCount; ++i) {
        const auto entry = directory.state + offsetof(State, groups) + (i / 8) * sizeof(Group) +
                           offsetof(Group, entries) + (i % 8) * sizeof(Entry);
        layouts[i] = emitEntry(code + i * KCodeStride, entry, i % 8);
    }
    emitMetadata(metadata, directory.code, layouts);
}

void validate(const Directory &candidate, const Directory &expected, const void *anchor, Deadline deadline)
{
    elf::require(
        candidate.magic == expected.magic && candidate.version == expected.version && candidate.size == expected.size &&
            candidate.pid == expected.pid && candidate.start_time == expected.start_time &&
            candidate.namespace_device == expected.namespace_device &&
            candidate.namespace_inode == expected.namespace_inode &&
            std::memcmp(candidate.installation, expected.installation, sizeof(candidate.installation)) == 0 &&
            std::memcmp(candidate.compatibility, expected.compatibility, sizeof(candidate.compatibility)) == 0 &&
            candidate.code_size == KCodeSize && candidate.metadata_size == KMetadataSize &&
            candidate.state_size == KStateSize && candidate.metadata == candidate.code + KCodeSize,
        "resident permanent gateway is incompatible; restart required");
    elf::require(mapped(candidate.code, KCodeSize, "r-xp") && mapped(candidate.metadata, KMetadataSize, "r--p") &&
                     mapped(candidate.state, KStateSize, "rw-p"),
                 "resident permanent gateway mapping validation failed");
    const auto code_end = candidate.metadata + KMetadataSize;
    elf::require(candidate.state + KStateSize <= candidate.code || candidate.state >= code_end,
                 "resident gateway mappings overlap");
    std::vector<unsigned char> generated(KCodeSize + KMetadataSize);
    render(candidate, generated.data(), generated.data() + KCodeSize);
    elf::require(std::memcmp(generated.data(), reinterpret_cast<const void *>(candidate.code), generated.size()) == 0,
                 "resident gateway code or CFI differs from its fingerprint");
    auto host = resolveHost(anchor);
    LoaderHandle lease(host.provider.handle);
    elf::require(host.functions == candidate.host.functions && host.provider.base == candidate.host.provider.base &&
                     host.provider.device == candidate.host.provider.device &&
                     host.provider.inode == candidate.host.provider.inode && verifyHost(candidate, deadline),
                 "resident gateway host unwind registration is incompatible");
    auto *state = reinterpret_cast<State *>(candidate.state);
    elf::require(state->status.load(std::memory_order_acquire) == 1,
                 "resident gateway is quarantined; restart required");
}

bool findDirectory(Directory &result, const Directory &expected, const void *anchor, Deadline deadline)
{
    DIR *raw = ::opendir("/proc/self/fd");
    elf::require(raw != nullptr, "cannot inspect gateway directory descriptors");
    std::unique_ptr<DIR, decltype(&::closedir)> descriptors(raw, ::closedir);
    std::size_t count = 0;
    std::size_t matches = 0;
    while (auto *entry = ::readdir(raw)) {
        elf::require(++count <= 65536 && std::chrono::steady_clock::now() < deadline,
                     "gateway descriptor scan exceeds bounds");
        char *end = nullptr;
        const auto number = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || number < 0 || number > std::numeric_limits<int>::max()) {
            continue;
        }
        Descriptor duplicate{::fcntl(static_cast<int>(number), F_DUPFD_CLOEXEC, 0)};
        if (duplicate.value < 0) {
            continue;
        }
        struct stat status{};
        if (::fstat(duplicate.value, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size != sizeof(Directory) ||
            ::fcntl(duplicate.value, F_GET_SEALS) != KSeals) {
            continue;
        }
        Directory candidate;
        if (::pread(duplicate.value, &candidate, sizeof(candidate), 0) != sizeof(candidate) ||
            candidate.magic != expected.magic) {
            continue;
        }
        validate(candidate, expected, anchor, deadline);
        elf::require(++matches == 1, "ambiguous permanent gateway directories; restart required");
        result = candidate;
    }
    return matches == 1;
}

}  // namespace

Layout emitEntry(unsigned char *output, std::uintptr_t entry, unsigned api)
{
    std::memset(output, 0xcc, KCodeStride);
    Bytes bytes{.output = output, .capacity = KCodeStride};
    Layout layout;
    bytes.bytes({0xf3, 0x0f, 0x1e, 0xfa, 0x49, 0xbb});
    bytes.pointer(entry);
    bytes.gate(1);
    bytes.bytes({0x41, 0xb9, 4, 0, 0, 0, 0x49, 0x8b, 0x03});
    const auto retry = bytes.size;
    bytes.bytes({0x48, 0x85, 0xc0});
    const auto closed = bytes.branch(0x88);
    bytes.bytes({0x41, 0xba, 0xff, 0xff, 0xff, 0xff, 0x4c, 0x39, 0xd0});
    const auto saturated = bytes.branch(0x83);
    bytes.bytes({0x4c, 0x8d, 0x50, 1, 0xf0, 0x4d, 0x0f, 0xb1, 0x13});
    const auto admitted = bytes.branch(0x84);
    bytes.bytes({0x41, 0xff, 0xc9});
    const auto again = bytes.branch(0x85);
    bytes.target(again, retry);
    const auto fallback = bytes.size;
    bytes.target(closed, fallback);
    bytes.target(saturated, fallback);
    if (api == 7) {
        bytes.byte(0xc3);
    }
    else {
        bytes.bytes({0x4d, 0x8b, 0x43, static_cast<unsigned char>(offsetof(Entry, original))});
        bytes.gate(6);
        bytes.bytes({0x41, 0xff, 0xe0});
    }
    bytes.target(admitted, bytes.size);
    bytes.gate(2);
    bytes.gate(3);
    bytes.bytes({0x49, 0x8b, 0x43, static_cast<unsigned char>(offsetof(Entry, callback))});
    if (api == 4 || api == 6) {
        bytes.bytes({0x48, 0x89, 0xd1});
    }
    if (api == 1 || api == 2 || api == 4 || api == 5 || api == 6) {
        bytes.bytes({0x48, 0x89, 0xf2});
    }
    bytes.bytes({0x48, 0x89, 0xfe, 0x49, 0x8b, 0x7b, static_cast<unsigned char>(offsetof(Entry, context)), 0x55});
    layout.push_end = bytes.size;
    bytes.bytes({0x48, 0x89, 0xe5});
    layout.frame_end = bytes.size;
    bytes.bytes({0xff, 0xd0});
    layout.callback_return = bytes.size;
    bytes.bytes({0x49, 0xbb});
    bytes.pointer(entry);
    bytes.gate(4);
    bytes.bytes({0xf0, 0x49, 0x83, 0x2b, 1});
    bytes.gate(5);
    bytes.byte(0x5d);
    layout.pop_end = bytes.size;
    bytes.byte(0xc3);
    return layout;
}

void emitMetadata(unsigned char *output, std::uintptr_t code, const std::array<Layout, KEntryCount> &layouts)
{
    std::memset(output, 0, KMetadataSize);
    Bytes cie{.output = output, .capacity = KCieSize};
    cie.integer(std::uint32_t{KCieSize - 4});
    cie.integer(std::uint32_t{0});
    cie.bytes({1, 'z', 'R', 0, 1, 0x78, 16, 1, 0, 0x0c, 7, 8, 0x90, 1, 0x08, 6});
    for (std::size_t i = 0; i < KEntryCount; ++i) {
        const auto offset = KCieSize + i * KFdeStride;
        Bytes fde{.output = output + offset, .capacity = KFdeStride};
        fde.integer(std::uint32_t{KFdeStride - 4});
        fde.integer(static_cast<std::uint32_t>(offset + 4));
        fde.pointer(code + i * KCodeStride);
        fde.pointer(KCodeStride);
        fde.byte(0);
        std::uint32_t previous = 0;
        const auto advance = [&](std::uint32_t destination) {
            fde.byte(0x03);
            fde.integer(static_cast<std::uint16_t>(destination - previous));
            previous = destination;
        };
        advance(layouts[i].push_end);
        fde.bytes({0x0e, 16, 0x86, 2});
        advance(layouts[i].frame_end);
        fde.bytes({0x0d, 6});
        advance(layouts[i].pop_end);
        fde.bytes({0x0c, 7, 8, 0xc6});
        Bytes table{.output = output + KTableOffset + i * 8, .capacity = 8};
        table.integer(static_cast<std::int32_t>(i * KCodeStride));
        table.integer(static_cast<std::int32_t>(KCodeSize + offset));
    }
}

State *state() noexcept
{
    return Current.load(std::memory_order_acquire);
}
const Directory *directory() noexcept
{
    return state() != nullptr ? &CurrentDirectory : nullptr;
}

Lock::Lock(State *state) : state_(state)
{
    elf::require(state_ != nullptr, "permanent gateway is not initialized");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::uint32_t expected = 0;
    while (!state_->lock.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        elf::require(std::chrono::steady_clock::now() < deadline, "permanent gateway setup lock deadline expired");
        expected = 0;
        std::this_thread::yield();
    }
}

Lock::~Lock()
{
    state_->lock.store(0, std::memory_order_release);
}

bool bootstrap(const char *root, const void *anchor, std::string &error)
{
    std::uint32_t expected = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!BootstrapLock.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "permanent gateway bootstrap deadline expired";
            return false;
        }
        expected = 0;
        std::this_thread::yield();
    }
    struct Release {
        ~Release() { BootstrapLock.store(0, std::memory_order_release); }
    } release;
    try {
        auto description = identity(root);
        if (state() != nullptr) {
            elf::require(std::strcmp(CurrentDirectory.installation, root) == 0,
                         "permanent gateway installation changed");
            return true;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto length = std::snprintf(
            address.sun_path + 1, sizeof(address.sun_path) - 1, "spark.alloc.gateway:%llu:%llu:%llu:%llu",
            static_cast<unsigned long long>(description.namespace_device),
            static_cast<unsigned long long>(description.namespace_inode),
            static_cast<unsigned long long>(description.pid), static_cast<unsigned long long>(description.start_time));
        elf::require(length > 0 && static_cast<std::size_t>(length) < sizeof(address.sun_path) - 1,
                     "gateway singleton identity exceeds bounds");
        Descriptor socket{::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0)};
        elf::require(socket.value >= 0, "cannot create gateway singleton");
        if (::bind(socket.value, reinterpret_cast<const sockaddr *>(&address),
                   static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length)) != 0) {
            elf::require(errno == EADDRINUSE && findDirectory(CurrentDirectory, description, anchor, deadline),
                         "gateway already claimed without a valid directory; restart required");
            elf::require(registerPrivate(CurrentDirectory, anchor), "private GNU libunwind registration failed");
            Current.store(reinterpret_cast<State *>(CurrentDirectory.state), std::memory_order_release);
            return true;
        }
        Descriptor directory{::memfd_create("spark.alloc.permanent.v2", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
        elf::require(directory.value >= 0 && ::ftruncate(directory.value, sizeof(Directory)) == 0,
                     "cannot create permanent gateway directory");
        description.host = resolveHost(anchor);
        LoaderHandle host_lease(description.host.provider.handle);
        auto *code = static_cast<unsigned char *>(
            ::mmap(nullptr, KCodeSize + KMetadataSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        elf::require(code != MAP_FAILED, "cannot allocate permanent gateway code");
        void *storage = ::mmap(nullptr, KStateSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (storage == MAP_FAILED) {
            ::munmap(code, KCodeSize + KMetadataSize);
            throw std::runtime_error("cannot allocate permanent gateway state");
        }
        auto *allocated = new (storage) State;
        description.code = reinterpret_cast<std::uintptr_t>(code);
        description.metadata = description.code + KCodeSize;
        description.state = reinterpret_cast<std::uintptr_t>(allocated);
        bool host_registered = false;
        bool quarantined = false;
        try {
            render(description, code, code + KCodeSize);
            elf::require(::mprotect(code, KCodeSize, PROT_READ | PROT_EXEC) == 0 &&
                             ::mprotect(code + KCodeSize, KMetadataSize, PROT_READ) == 0,
                         "cannot protect permanent gateway arena");
            elf::require(registerHost(description, deadline, quarantined), "host unwinder rejected permanent gateway");
            host_registered = true;
            elf::require(registerPrivate(description, anchor), "private GNU libunwind registration failed");
            elf::require(::pwrite(directory.value, &description, sizeof(description), 0) == sizeof(description) &&
                             ::fcntl(directory.value, F_ADD_SEALS, KSeals) == 0,
                         "cannot seal permanent gateway directory");
            allocated->providers[0] = description.host.provider;
            allocated->provider_count = 1;
            allocated->status.store(1, std::memory_order_release);
            CurrentDirectory = description;
            Current.store(allocated, std::memory_order_release);
            host_lease.release();
            directory.release();
            socket.release();
            return true;
        }
        catch (...) {
            if (host_registered) {
                unregisterPrivate();
                quarantined = !unregisterHost(description, deadline);
            }
            if (quarantined) {
                allocated->status.store(2, std::memory_order_release);
                host_lease.release();
                directory.release();
                socket.release();
            }
            else {
                ::munmap(storage, KStateSize);
                ::munmap(code, KCodeSize + KMetadataSize);
            }
            throw;
        }
    }
    catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

// NOLINTEND(performance-no-int-to-ptr)
}  // namespace spark::gateway::permanent

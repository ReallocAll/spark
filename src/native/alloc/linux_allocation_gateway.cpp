#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>

#include "native/alloc/linux_allocation_gateway_abi.h"
#include "native/alloc/linux_elf_admission.h"
#include "native/alloc/linux_gateway_identity.h"
#include "spark_gateway_compatibility.h"

#ifndef SPARK_GATEWAY_CAPACITY
#define SPARK_GATEWAY_CAPACITY 256
#endif

#if defined(SPARK_GATEWAY_EXTERNAL_IMPORT_TESTING)
extern "C" void spark_gateway_external_import();
#endif

namespace {

constexpr std::uint64_t KClosed = std::uint64_t{1} << 63;
constexpr std::size_t KCapacity = SPARK_GATEWAY_CAPACITY;
constexpr std::size_t KProviderCapacity = KCapacity * 7;
static_assert(KCapacity > 0 && KCapacity <= 256);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

struct alignas(64) Entry {
    std::atomic<std::uint64_t> state{KClosed};
    std::atomic<std::uint32_t> tickets{0};
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
    bool reserved = false;
    bool published = false;
    bool retired = false;
    bool final_close = false;
    std::array<void *, 7> pending_leases{};
    std::size_t pending_count = 0;
};

struct Provider {
    std::uintptr_t base = 0;
    dev_t device = 0;
    ino_t inode = 0;
    void *handle = nullptr;
};

constinit std::array<Group, KCapacity> Groups;
constinit std::array<Provider, KProviderCapacity> Providers;
std::size_t ProviderCount = 0;
pthread_mutex_t SetupMutex = PTHREAD_MUTEX_INITIALIZER;
char Installation[4096]{};
int Singleton = -1;

static_assert(sizeof(Groups) + sizeof(Providers) + sizeof(Installation) < 1024 * 1024);

struct Lock {
    Lock() { ::pthread_mutex_lock(&SetupMutex); }
    ~Lock() { ::pthread_mutex_unlock(&SetupMutex); }
};

void testGate(Entry &entry, std::uint32_t phase) noexcept
{
#if defined(SPARK_GATEWAY_TESTING)
    const bool paired = entry.phase == 6 && (phase == 5 || phase == 6);
    if (entry.gate != nullptr && (entry.phase == phase || paired)) {
        const std::uint64_t reached = paired && phase == 6 ? 3 : 1;
        __atomic_store_n(entry.gate, reached, __ATOMIC_RELEASE);
        while (__atomic_load_n(entry.gate, __ATOMIC_ACQUIRE) != reached + 1) {
            __asm__ volatile("pause");
        }
    }
#else
    (void)entry;
    (void)phase;
#endif
}

bool admit(Entry &entry) noexcept
{
    testGate(entry, 1);
    if ((entry.state.load(std::memory_order_acquire) & KClosed) != 0) {
        return false;
    }
    auto tickets = entry.tickets.load(std::memory_order_relaxed);
    bool acquired = false;
    for (int attempt = 0; attempt < 4 && tickets < std::numeric_limits<std::uint32_t>::max(); ++attempt) {
        if (entry.tickets.compare_exchange_weak(tickets, tickets + 1, std::memory_order_relaxed)) {
            acquired = true;
            break;
        }
    }
    if (!acquired) {
        return false;
    }
    testGate(entry, 5);
    const auto previous = entry.state.fetch_add(1, std::memory_order_acq_rel);
    if ((previous & KClosed) != 0) {
        testGate(entry, 6);
        entry.state.fetch_sub(1, std::memory_order_release);
        entry.tickets.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    testGate(entry, 2);
    return true;
}

template <std::size_t Index, std::size_t Api, typename Result, typename... Args>
__attribute__((noinline)) Result invoke(Args... args) noexcept
{
    Entry &entry = Groups[Index].entries[Api];
    if (!admit(entry)) {
        if constexpr (Api == 7) {
            return;
        }
        else {
            return reinterpret_cast<Result (*)(Args...)>(entry.original)(args...);
        }
    }
    auto callback = reinterpret_cast<Result (*)(void *, Args...)>(entry.callback);
    if constexpr (std::is_void_v<Result>) {
        callback(entry.context, args...);
        testGate(entry, 3);
        entry.state.fetch_sub(1, std::memory_order_release);
        entry.tickets.fetch_sub(1, std::memory_order_relaxed);
        testGate(entry, 4);
    }
    else {
        Result result = callback(entry.context, args...);
        testGate(entry, 3);
        entry.state.fetch_sub(1, std::memory_order_release);
        entry.tickets.fetch_sub(1, std::memory_order_relaxed);
        testGate(entry, 4);
        return result;
    }
}

template <std::size_t Index>
SparkGatewayBindingV1 binding()
{
    return {sizeof(SparkGatewayBindingV1),
            static_cast<std::uint32_t>(Index),
            {reinterpret_cast<void *>(&invoke<Index, 0, void *, std::size_t>),
             reinterpret_cast<void *>(&invoke<Index, 1, void *, std::size_t, std::size_t>),
             reinterpret_cast<void *>(&invoke<Index, 2, void *, void *, std::size_t>),
             reinterpret_cast<void *>(&invoke<Index, 3, void, void *>),
             reinterpret_cast<void *>(&invoke<Index, 4, void *, void *, std::size_t, std::size_t>),
             reinterpret_cast<void *>(&invoke<Index, 5, void *, std::size_t, std::size_t>),
             reinterpret_cast<void *>(&invoke<Index, 6, int, void **, std::size_t, std::size_t>)},
            &invoke<Index, 7, void, void *>};
}

template <std::size_t... Indices>
constexpr auto bindings(std::index_sequence<Indices...>)
{
    return std::array{&binding<Indices>...};
}

constexpr auto Bindings = bindings(std::make_index_sequence<KCapacity>{});

int failure(char *error, std::size_t size, const char *message)
{
    if (error != nullptr && size != 0) {
        std::snprintf(error, size, "%s", message);
    }
    return 0;
}

int bootstrap(const char *root, const void *spark_address, char *error, std::size_t size)
{
    Lock lock;
    try {
        if (root == nullptr || root[0] != '/' || std::strlen(root) >= sizeof(Installation)) {
            return failure(error, size, "invalid gateway installation identity");
        }
        const auto canonical = std::filesystem::canonical(root).string();
        if (canonical != root) {
            return failure(error, size, "gateway installation identity is not canonical");
        }
        spark::gateway::Identity self;
        if (!spark::gateway::identify(reinterpret_cast<const void *>(&bootstrap), self) ||
            self.path != (std::filesystem::path(root) / ".spark-native" / SPARK_GATEWAY_FILENAME).string()) {
            return failure(error, size, "gateway helper is outside the installed runtime layout");
        }
        spark::gateway::LoaderHandle namespace_handle(spark::gateway::lease(self));
        if (!namespace_handle) {
            return failure(error, size, "gateway requires the main loader namespace");
        }
        spark::gateway::elf::Admission admission(spark_address);
        const auto self_index = admission.snapshot.owner(reinterpret_cast<const void *>(&bootstrap), PF_R | PF_X);
        auto helper = admission.snapshot.objects[self_index];
        helper.dynamic.clear();
        helper.needed.clear();
        helper.version_names.clear();
        helper.read(self.path, true);
        admission.preflight(helper);
        admission.bindings(helper);
        if (Singleton >= 0) {
            return canonical == Installation ? 1
                                             : failure(error, size, "resident gateway belongs to another installation");
        }
        struct stat pid_namespace{};
        if (::stat("/proc/self/ns/pid", &pid_namespace) != 0) {
            return failure(error, size, "cannot identify gateway PID namespace");
        }
        std::ifstream status("/proc/self/stat");
        std::string line;
        std::getline(status, line);
        const auto end_name = line.rfind(')');
        if (end_name == std::string::npos) {
            return failure(error, size, "cannot identify gateway process start");
        }
        std::istringstream fields(line.substr(end_name + 1));
        std::string start;
        for (int field = 3; field <= 22; ++field) {
            if (!(fields >> start)) {
                return failure(error, size, "cannot identify gateway process start");
            }
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const int length = std::snprintf(
            address.sun_path + 1, sizeof(address.sun_path) - 1, "spark.alloc.gateway:%llu:%llu:%ld:%s",
            static_cast<unsigned long long>(pid_namespace.st_dev),
            static_cast<unsigned long long>(pid_namespace.st_ino), static_cast<long>(::getpid()), start.c_str());
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(address.sun_path) - 1) {
            return failure(error, size, "gateway singleton identity is too long");
        }
        const int socket = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (socket < 0) {
            return failure(error, size, "cannot create gateway singleton");
        }
        if (::bind(socket, reinterpret_cast<const sockaddr *>(&address),
                   static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + length)) != 0) {
            ::close(socket);
            return failure(error, size, "a gateway family already exists; restart required");
        }
        spark::gateway::LoaderHandle resident(
            ::dlopen(self.path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD | RTLD_NODELETE));
        if (!resident) {
            ::close(socket);
            return failure(error, size, "cannot retain verified allocation gateway");
        }
        std::memcpy(Installation, root, std::strlen(root) + 1);
        Singleton = socket;
        return 1;
    }
    catch (const std::exception &exception) {
        return failure(error, size, exception.what());
    }
    catch (...) {
        return failure(error, size, "cannot establish allocation gateway identity");
    }
}

const char *installation()
{
    return Installation;
}

int reserve(void *const *originals, const void *spark_address, SparkGatewayBindingV1 *result, char *error,
            std::size_t size)
{
    Lock lock;
    if (Singleton < 0 || originals == nullptr || result == nullptr || result->size != sizeof(*result)) {
        return failure(error, size, "allocation gateway is not initialized");
    }
    for (const auto &group : Groups) {
        if (group.reserved && !group.published && !group.retired) {
            return failure(error, size, "another allocation gateway binding awaits publication");
        }
    }
    std::size_t index = 0;
    while (index < KCapacity && Groups[index].reserved) {
        ++index;
    }
    if (index == KCapacity) {
        return failure(error, size, "allocation gateway lifetime capacity exhausted; restart required");
    }
    std::array<Provider, 7> acquired{};
    std::array<spark::gateway::LoaderHandle, 7> handles;
    std::size_t acquired_count = 0;
    try {
        spark::gateway::elf::Admission admission(spark_address);
        for (std::size_t api = 0; api < 7; ++api) {
            const auto index = admission.snapshot.owner(originals[api], PF_R | PF_X);
            const auto &provider = admission.snapshot.objects[index].identity;
            if (!admission.resident.contains(index) || index == admission.spark ||
                spark::gateway::elf::sparkObject(admission.snapshot.objects[index])) {
                return failure(error, size, "allocator original has an unsafe provider");
            }
            spark::gateway::LoaderHandle handle(spark::gateway::lease(provider));
            spark::gateway::loaderFault(10 + api);
            if (!handle) {
                return failure(error, size, "cannot retain allocator provider");
            }
            const auto same = [&provider](const Provider &entry) {
                return entry.base == provider.base && entry.device == provider.device && entry.inode == provider.inode;
            };
            if (spark::gateway::mainExecutable(provider) ||
                std::find_if(Providers.begin(), Providers.begin() + ProviderCount, same) !=
                    Providers.begin() + ProviderCount ||
                std::find_if(acquired.begin(), acquired.begin() + acquired_count, same) !=
                    acquired.begin() + acquired_count) {
            }
            else {
                spark::gateway::loaderFault(20 + api);
                acquired[acquired_count++] = {
                    .base = provider.base, .device = provider.device, .inode = provider.inode, .handle = handle.get()};
                handles[acquired_count - 1] = std::move(handle);
            }
        }
        if (ProviderCount + acquired_count > KProviderCapacity) {
            return failure(error, size, "allocation gateway provider capacity exhausted");
        }
        Group &group = Groups[index];
        spark::gateway::elf::require(admission.snapshot.unchanged(), "loader changed during provider admission");
        for (std::size_t api = 0; api < 7; ++api) {
            group.entries[api].original = originals[api];
        }
        for (std::size_t i = 0; i < acquired_count; ++i) {
            Providers[ProviderCount++] = acquired[i];
            group.pending_leases[group.pending_count++] = acquired[i].handle;
            handles[i].release();
        }
        group.reserved = true;
        *result = Bindings[index]();
        return 1;
    }
    catch (const std::exception &exception) {
        return failure(error, size, exception.what());
    }
    catch (...) {
        return failure(error, size, "cannot reserve allocation gateway");
    }
}

int open(std::uint32_t index, const SparkGatewayCallbacksV1 *callbacks, void *context, int tls)
{
    Lock lock;
    if (index >= KCapacity || callbacks == nullptr || context == nullptr || !Groups[index].reserved ||
        Groups[index].retired || Groups[index].final_close) {
        return 0;
    }
    std::array<void *, 8> values{reinterpret_cast<void *>(callbacks->malloc_callback),
                                 reinterpret_cast<void *>(callbacks->calloc_callback),
                                 reinterpret_cast<void *>(callbacks->realloc_callback),
                                 reinterpret_cast<void *>(callbacks->free_callback),
                                 reinterpret_cast<void *>(callbacks->reallocarray_callback),
                                 reinterpret_cast<void *>(callbacks->aligned_alloc_callback),
                                 reinterpret_cast<void *>(callbacks->posix_memalign_callback),
                                 reinterpret_cast<void *>(callbacks->tls_callback)};
    const std::size_t begin = tls != 0 ? 7 : 0;
    const std::size_t end = tls != 0 ? 8 : 7;
    for (std::size_t api = begin; api < end; ++api) {
        const auto state = Groups[index].entries[api].state.load(std::memory_order_acquire);
        const auto &entry = Groups[index].entries[api];
        if (values[api] == nullptr || (state & KClosed) == 0 || entry.callback != nullptr || entry.context != nullptr) {
            return 0;
        }
    }
    for (std::size_t api = begin; api < end; ++api) {
        auto &entry = Groups[index].entries[api];
        entry.callback = values[api];
        entry.context = context;
        entry.state.fetch_and(~KClosed, std::memory_order_release);
    }
    return 1;
}

void close(std::uint32_t index, int final)
{
    if (index >= KCapacity) {
        return;
    }
    Lock lock;
    if (final != 0) {
        Groups[index].final_close = true;
    }
    const std::size_t end = final != 0 ? 8 : 7;
    for (std::size_t api = 0; api < end; ++api) {
        Groups[index].entries[api].state.fetch_or(KClosed, std::memory_order_acq_rel);
    }
}

std::uint64_t active(std::uint32_t index, int tls)
{
    if (index >= KCapacity) {
        return 0;
    }
    std::uint64_t total = 0;
    const std::size_t begin = tls != 0 ? 7 : 0;
    const std::size_t end = tls != 0 ? 8 : 7;
    for (std::size_t api = begin; api < end; ++api) {
        total += Groups[index].entries[api].state.load(std::memory_order_acquire) & ~KClosed;
    }
    return total;
}

int clear(std::uint32_t index, int tls)
{
    Lock lock;
    if (index >= KCapacity) {
        return 0;
    }
    const std::size_t begin = tls != 0 ? 7 : 0;
    const std::size_t end = tls != 0 ? 8 : 7;
    for (std::size_t api = begin; api < end; ++api) {
        if (Groups[index].entries[api].state.load(std::memory_order_acquire) != KClosed) {
            return 0;
        }
    }
    for (std::size_t api = begin; api < end; ++api) {
        Groups[index].entries[api].callback = nullptr;
        Groups[index].entries[api].context = nullptr;
    }
    return 1;
}

void publish(std::uint32_t index)
{
    Lock lock;
    if (index < KCapacity && Groups[index].reserved) {
        Groups[index].published = true;
        Groups[index].pending_count = 0;
    }
}

int retire(std::uint32_t index)
{
    if (!clear(index, 0) || !clear(index, 1)) {
        return 0;
    }
    Lock lock;
    if (!Groups[index].final_close) {
        return 0;
    }
    Groups[index].retired = true;
    return 1;
}

int cancel(std::uint32_t index)
{
    Lock lock;
    if (index >= KCapacity || !Groups[index].reserved || Groups[index].published) {
        return 0;
    }
    auto &group = Groups[index];
    for (auto &entry : group.entries) {
        if (entry.state.load(std::memory_order_acquire) != KClosed) {
            return 0;
        }
    }
    // Unpublished reservations are serialized until publish or cancellation.
    for (std::size_t i = 0; i < group.pending_count; ++i) {
        for (std::size_t provider = 0; provider < ProviderCount; ++provider) {
            if (Providers[provider].handle == group.pending_leases[i]) {
                ::dlclose(Providers[provider].handle);
                Providers[provider] = Providers[--ProviderCount];
                break;
            }
        }
    }
    group.pending_count = 0;
    group.reserved = false;
    group.final_close = false;
    group.retired = false;
    for (auto &entry : group.entries) {
        entry.original = nullptr;
        entry.callback = nullptr;
        entry.context = nullptr;
    }
    return 1;
}

std::uint32_t used()
{
    Lock lock;
    std::uint32_t count = 0;
    for (const auto &group : Groups) {
        count += group.reserved ? 1 : 0;
    }
    return count;
}

std::uint32_t leases()
{
    Lock lock;
    return static_cast<std::uint32_t>(ProviderCount);
}

// NOLINTNEXTLINE(readability-non-const-parameter)
void setTestGate(std::uint32_t index, std::uint32_t api, std::uint32_t phase, std::uint64_t *gate)
{
#if defined(SPARK_GATEWAY_TESTING)
    if (index < KCapacity && api < 8) {
        Groups[index].entries[api].gate = gate;
        Groups[index].entries[api].phase = phase;
    }
#else
    (void)index;
    (void)api;
    (void)phase;
    (void)gate;
#endif
}

const SparkGatewayV1 Api{sizeof(SparkGatewayV1),
                         SPARK_GATEWAY_ABI_VERSION,
                         SPARK_GATEWAY_FAMILY,
                         SPARK_GATEWAY_COMPATIBILITY,
                         KCapacity,
                         KProviderCapacity,
                         &bootstrap,
                         &installation,
                         &reserve,
                         &open,
                         &close,
                         &active,
                         &clear,
                         &retire,
                         &publish,
                         &cancel,
                         &used,
                         &leases,
#if defined(SPARK_GATEWAY_TESTING)
                         &setTestGate
#else
                         nullptr
#endif
};

}  // namespace

extern "C" __attribute__((visibility("default"))) const SparkGatewayV1 *spark_allocation_gateway_v1()
{
#if defined(SPARK_GATEWAY_EXTERNAL_IMPORT_TESTING)
    spark_gateway_external_import();
#endif
    return &Api;
}

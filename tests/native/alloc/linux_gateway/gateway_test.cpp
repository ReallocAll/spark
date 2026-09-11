#include <dlfcn.h>
#include <link.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>

#include "fixture_api.h"
#include "native/alloc/linux_gateway_identity.h"

namespace {

std::atomic<unsigned> ProviderCounters[7]{};
void *IfuncTarget = nullptr;
unsigned ApprovedQueries = 0;
void approvedImport()
{
    ++ApprovedQueries;
}

std::size_t descriptorCount()
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        ++count;
    }
    return count;
}

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", message, ::dlerror());
        std::abort();
    }
}

bool loaded(const std::filesystem::path &path)
{
    void *handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return false;
    }
    ::dlclose(handle);
    return true;
}

struct Fixture {
    void *handle = nullptr;
    GatewayFixtureState state;
    GatewayFixtureInfo info;
    int (*start)(void *, GatewayFixtureState *, GatewayFixtureInfo *) = nullptr;
    int (*stop)() = nullptr;
    int (*restart)() = nullptr;
    int (*shutdown)() = nullptr;
    std::filesystem::path path;

    explicit Fixture(std::filesystem::path location = SPARK_GATEWAY_FIXTURE) : path(std::move(location))
    {
        handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        require(handle != nullptr, "load synthetic Spark DSO");
        start = reinterpret_cast<decltype(start)>(::dlsym(handle, "fixture_start"));
        stop = reinterpret_cast<decltype(stop)>(::dlsym(handle, "fixture_stop"));
        restart = reinterpret_cast<decltype(restart)>(::dlsym(handle, "fixture_restart"));
        shutdown = reinterpret_cast<decltype(shutdown)>(::dlsym(handle, "fixture_shutdown"));
        require(start != nullptr && stop != nullptr && restart != nullptr && shutdown != nullptr, "fixture exports");
    }

    void unload()
    {
        require(shutdown() != 0, "complete shutdown before fixture unload");
        require(::dlclose(handle) == 0, "dlclose Spark");
        handle = nullptr;
        require(!loaded(path), "real dlclose removes Spark image");
    }

    ~Fixture()
    {
        if (handle != nullptr) {
            unload();
        }
    }
};

void *provider()
{
    void *handle = ::dlopen(SPARK_GATEWAY_PROVIDER, RTLD_NOW | RTLD_LOCAL);
    require(handle != nullptr, "load provider");
    auto counters = reinterpret_cast<void (*)(std::atomic<unsigned> *)>(::dlsym(handle, "provider_counters"));
    require(counters != nullptr, "provider counter export");
    counters(ProviderCounters);
    return handle;
}

const SparkGatewayV1 *helper()
{
    void *handle = ::dlopen(SPARK_GATEWAY_HELPER, RTLD_NOW | RTLD_NOLOAD);
    require(handle != nullptr, "resident helper exists");
    auto query = reinterpret_cast<SparkGatewayQueryV1>(::dlsym(handle, SPARK_GATEWAY_SYMBOL));
    require(query != nullptr, "helper ABI export");
    const auto *api = query();
    ::dlclose(handle);
    return api;
}

void invokeOne(const SparkGatewayBindingV1 &binding, unsigned api, void *payload = nullptr)
{
    void *result = nullptr;
    switch (api) {
    case 0:
        result = reinterpret_cast<void *(*)(std::size_t)>(binding.entries[0])(64);
        break;
    case 1:
        result = reinterpret_cast<void *(*)(std::size_t, std::size_t)>(binding.entries[1])(2, 32);
        require(result == nullptr || *static_cast<unsigned char *>(result) == 0, "calloc zero semantics");
        break;
    case 2:
        result = reinterpret_cast<void *(*)(void *, std::size_t)>(binding.entries[2])(nullptr, 64);
        break;
    case 3:
        reinterpret_cast<void (*)(void *)>(binding.entries[3])(nullptr);
        return;
    case 4:
        result = reinterpret_cast<void *(*)(void *, std::size_t, std::size_t)>(binding.entries[4])(nullptr, 2, 32);
        break;
    case 5:
        result = reinterpret_cast<void *(*)(std::size_t, std::size_t)>(binding.entries[5])(16, 64);
        require(reinterpret_cast<std::uintptr_t>(result) % 16 == 0, "aligned allocation result");
        break;
    case 6:
        require(reinterpret_cast<int (*)(void **, std::size_t, std::size_t)>(binding.entries[6])(&result, 16, 64) == 0,
                "posix_memalign result");
        break;
    case 7:
        binding.tls_entry(payload);
        return;
    default:
        std::abort();
    }
    require(result != nullptr, "original allocator return value");
    reinterpret_cast<void (*)(void *)>(binding.entries[3])(result);
}

std::array<unsigned, 8> counts(const GatewayFixtureState &state)
{
    std::array<unsigned, 8> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = state.callbacks[i].load();
    }
    return values;
}

void all(const SparkGatewayBindingV1 &binding)
{
    for (unsigned api = 0; api < 7; ++api) {
        const auto before = ProviderCounters[api].load();
        invokeOne(binding, api);
        require(ProviderCounters[api].load() > before, "original provider counter");
    }
    errno = 0;
    require(reinterpret_cast<void *(*)(std::size_t)>(binding.entries[0])(0x12345678) == nullptr && errno == ENOMEM,
            "malloc failure and errno preserved");
    void *pointer = reinterpret_cast<void *>(1234);
    errno = E2BIG;
    require(reinterpret_cast<int (*)(void **, std::size_t, std::size_t)>(binding.entries[6])(&pointer, 3, 64) ==
                    EINVAL &&
                pointer == reinterpret_cast<void *>(1234) && errno == E2BIG,
            "posix_memalign failure preserves output and errno");
}

void waitGate(std::uint64_t &gate, std::uint64_t reached = 1)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (__atomic_load_n(&gate, __ATOMIC_ACQUIRE) != reached) {
        require(std::chrono::steady_clock::now() < deadline, "deterministic gateway gate reached");
        std::this_thread::yield();
    }
}

void lifetime()
{
    void *original_provider = provider();
    Fixture first;
    require(first.start(original_provider, &first.state, &first.info) != 0, "start first Spark owner");
    const auto old = first.info.binding;
    all(old);
    require(first.state.unwind_mask.load() == 3, "actual unwind traverses helper and host caller");
    require(first.stop() != 0, "ordinary stop");
    const auto stopped = counts(first.state);
    all(old);
    require(counts(first.state) == stopped, "stopped allocator entries use originals only");
    require(first.restart() != 0, "restart same owner group");
    all(old);
    first.unload();
    require(loaded(SPARK_GATEWAY_HELPER), "helper survives Spark dlclose");
    ::dlclose(original_provider);
    require(loaded(SPARK_GATEWAY_PROVIDER), "provider survives external dlclose");
    const auto retired = counts(first.state);
    all(old);
    require(counts(first.state) == retired, "retired entries never call old owner");
    original_provider = provider();
    Fixture second;
    require(second.start(original_provider, &second.state, &second.info) != 0, "reload Spark with new owner");
    require(second.info.binding.group != old.group, "new owner gets fresh lifetime group");
    const auto fresh = counts(second.state);
    all(old);
    require(counts(first.state) == retired && counts(second.state) == fresh, "old entries cannot attach to new owner");
    second.unload();
    ::dlclose(original_provider);

    original_provider = provider();
    for (unsigned api = 0; api < 8; ++api) {
        for (unsigned phase = 1; phase <= 5; ++phase) {
            Fixture fixture;
            require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0, "gate fixture start");
            const auto binding = fixture.info.binding;
            const auto *resident = helper();
            alignas(8) std::uint64_t gate = 0;
            unsigned payload = 42;
            resident->test_gate(binding.group, api, phase, &gate);
            std::thread caller([&] { invokeOne(binding, api, &payload); });
            waitGate(gate);
            if (phase == 2 || phase == 3) {
                require(fixture.shutdown() == 0, "admitted callback/reference delays shutdown");
                require(loaded(SPARK_GATEWAY_FIXTURE), "pending owner code remains loaded");
                require(fixture.restart() == 0, "final shutdown cannot reopen entries");
            }
            else {
                fixture.unload();
            }
            __atomic_store_n(&gate, 2, __ATOMIC_RELEASE);
            caller.join();
            resident->test_gate(binding.group, api, 0, nullptr);
            if (fixture.handle != nullptr) {
                fixture.unload();
            }
            all(binding);
            std::printf("gate PASS: entry=%u phase=%u real Spark unload\n", api, phase);
        }
    }
    ::dlclose(original_provider);
}

void tls()
{
    void *original_provider = provider();
    Fixture fixture;
    require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0, "TLS fixture start");
    const auto old = fixture.info;
    for (int cycle = 0; cycle < 4; ++cycle) {
        require(fixture.stop() != 0, "stop keeps TLS reclamation open");
        for (int iteration = 0; iteration < 256; ++iteration) {
            unsigned payload = 7;
            std::thread exited([&] { require(::pthread_setspecific(old.key, &payload) == 0, "set real pthread key"); });
            exited.join();
            require(payload == 0, "real TLS destructor reclaims payload during stop");
        }
        require(fixture.restart() != 0, "restart retains independent TLS descriptor");
    }
    require(fixture.state.callbacks[7].load() == 1024, "all thread exits reclaimed");
    fixture.state.block_callback.store(true);
    unsigned payload = 9;
    std::thread exiting([&] { require(::pthread_setspecific(old.key, &payload) == 0, "set blocked pthread key"); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fixture.state.callback_entered.load()) {
        require(std::chrono::steady_clock::now() < deadline, "TLS callback entered");
        std::this_thread::yield();
    }
    require(fixture.shutdown() == 0, "blocked TLS callback retains owner and key");
    fixture.state.release_callback.store(true);
    exiting.join();
    require(payload == 0, "admitted TLS callback completed");
    fixture.state.fail_key_delete.store(true);
    require(fixture.shutdown() == 0, "key deletion failure retains cleanup obligation");
    fixture.state.fail_key_delete.store(false);
    fixture.unload();
    void *inaccessible = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(inaccessible != MAP_FAILED, "inaccessible old TLS payload");
    old.binding.tls_entry(inaccessible);
    Fixture next;
    require(next.start(original_provider, &next.state, &next.info) != 0, "new TLS owner");
    require(next.info.key == old.key, "actual pthread key reuse observed");
    require(next.info.binding.group != old.binding.group, "reused pthread key uses a new descriptor");
    old.binding.tls_entry(inaccessible);
    require(next.state.callbacks[7].load() == 0, "old descriptor ignores inaccessible payload after key reuse");
    next.unload();
    ::munmap(inaccessible, 4096);
    Fixture failed;
    failed.state.fail_after_key_create.store(true);
    require(failed.start(original_provider, &failed.state, &failed.info) == 0, "failure after key creation");
    require(failed.info.binding.tls_entry != nullptr, "created key consumed a lifetime entry");
    failed.unload();
    ::dlclose(original_provider);
    std::puts("TLS PASS: stopped-thread churn, blocked destructor, delete failure, pre-hook failure, actual key reuse");
}

void budget()
{
    void *original_provider = provider();
    std::vector<SparkGatewayBindingV1> retired;
    for (unsigned i = 0; i < 64; ++i) {
        Fixture fixture;
        require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0,
                "reserve cumulative lifetime capacity");
        retired.push_back(fixture.info.binding);
        fixture.unload();
        require(helper()->used() == i + 1 && helper()->leases() == 1, "bounded group and deduplicated lease counters");
    }
    Fixture exhausted;
    require(exhausted.start(original_provider, &exhausted.state, &exhausted.info) == 0, "capacity rejects next owner");
    for (const auto &binding : retired) {
        all(binding);
    }
    require(helper()->used() == 64 && helper()->leases() == 1, "exhaustion creates no extra pool or leases");
    ::dlclose(original_provider);
}

std::filesystem::path scratch()
{
    char pattern[] = "/tmp/spark-gateway-fixture-XXXXXX";
    char *created = ::mkdtemp(pattern);
    require(created != nullptr, "create isolated layout fixture");
    return created;
}

void copyRuntime(const std::filesystem::path &root)
{
    std::filesystem::create_directories(root / ".spark-native");
    std::filesystem::copy_file(SPARK_GATEWAY_FIXTURE, root / "endstone_spark.so");
    std::filesystem::copy_file(SPARK_GATEWAY_HELPER, root / ".spark-native" / SPARK_GATEWAY_FILENAME);
}

void layout()
{
    const auto root = scratch();
    copyRuntime(root);
    std::filesystem::create_directory(root / ".local");
    const auto first = root / ".local" / "endstone_spark-aaa.so";
    const auto second = root / ".local" / "endstone_spark-bbb.so";
    std::filesystem::copy_file(root / "endstone_spark.so", first);
    std::filesystem::copy_file(root / "endstone_spark.so", second);
    const auto malicious = scratch();
    copyRuntime(malicious);
    std::filesystem::current_path(malicious);
    void *original_provider = provider();
    unsigned group = 0;
    {
        Fixture fixture(first);
        require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0,
                "shadow aaa resolves installation");
        group = fixture.info.binding.group;
        fixture.unload();
    }
    require(!loaded(malicious / ".spark-native" / SPARK_GATEWAY_FILENAME), "malicious CWD helper was ignored");
    {
        Fixture fixture(second);
        require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0,
                "shadow bbb shares resident pool");
        require(fixture.info.binding.group == group + 1, "shadow hashes use the same pool");
        fixture.unload();
    }
    {
        Fixture other(malicious / "endstone_spark.so");
        require(other.start(original_provider, &other.state, &other.info) == 0, "second installation rejected");
        require(!loaded(malicious / ".spark-native" / SPARK_GATEWAY_FILENAME), "second pool never loaded");
    }
    const auto missing = scratch();
    std::filesystem::copy_file(SPARK_GATEWAY_FIXTURE, missing / "endstone_spark.so");
    {
        Fixture fixture(missing / "endstone_spark.so");
        require(fixture.start(original_provider, &fixture.state, &fixture.info) == 0,
                "missing helper fails explicitly");
    }
    std::filesystem::create_directory(missing / ".spark-native");
    std::filesystem::create_symlink(root / ".spark-native" / SPARK_GATEWAY_FILENAME,
                                    missing / ".spark-native" / SPARK_GATEWAY_FILENAME);
    {
        Fixture fixture(missing / "endstone_spark.so");
        require(fixture.start(original_provider, &fixture.state, &fixture.info) == 0, "helper symlink escape rejected");
    }
    ::dlclose(original_provider);
}

void plain()
{
    const auto descriptors = descriptorCount();
    void *handle = ::dlopen(SPARK_GATEWAY_HELPER, RTLD_NOW | RTLD_LOCAL);
    require(handle != nullptr, "plain helper dlopen");
    auto query = reinterpret_cast<SparkGatewayQueryV1>(::dlsym(handle, SPARK_GATEWAY_SYMBOL));
    require(query != nullptr && query()->installation()[0] == '\0' && query()->used() == 0 && query()->leases() == 0,
            "plain helper has no bootstrap or provider state");
    ::dlclose(handle);
    require(!loaded(SPARK_GATEWAY_HELPER), "plain helper dlclose actually unloads");
    require(descriptorCount() == descriptors, "plain helper creates no persistent FD");
}

void concurrent()
{
    const auto first = scratch();
    const auto second = scratch();
    copyRuntime(first);
    copyRuntime(second);
    void *handles[2]{};
    const SparkGatewayV1 *apis[2]{};
    const std::array roots{first, second};
    const auto descriptors = descriptorCount();
    for (unsigned i = 0; i < 2; ++i) {
        handles[i] = ::dlopen((roots[i] / ".spark-native" / SPARK_GATEWAY_FILENAME).c_str(), RTLD_NOW | RTLD_LOCAL);
        require(handles[i] != nullptr, "load copied uninitialized helper");
        apis[i] = reinterpret_cast<SparkGatewayQueryV1>(::dlsym(handles[i], SPARK_GATEWAY_SYMBOL))();
    }
    std::atomic<unsigned> ready{0};
    int results[2]{};
    auto attempt = [&](unsigned i) {
        ready.fetch_add(1);
        while (ready.load() != 2) {
            std::this_thread::yield();
        }
        char error[256]{};
        results[i] = apis[i]->bootstrap(roots[i].c_str(), reinterpret_cast<void *>(&concurrent), error, sizeof(error));
    };
    std::thread a(attempt, 0), b(attempt, 1);
    a.join();
    b.join();
    require(results[0] + results[1] == 1, "atomic family gate permits exactly one copied helper");
    require(descriptorCount() == descriptors + 1, "exactly one process-lifetime singleton FD");
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned retry = 0; retry < 8; ++retry) {
            char error[256]{};
            require(apis[i]->bootstrap(roots[i].c_str(), reinterpret_cast<void *>(&concurrent), error, sizeof(error)) ==
                        results[i],
                    "repeated bootstrap preserves singleton decision");
        }
    }
    require(descriptorCount() == descriptors + 1, "failed bootstraps do not grow FD budget");
    for (unsigned i = 0; i < 2; ++i) {
        ::dlclose(handles[i]);
        require(loaded(roots[i] / ".spark-native" / SPARK_GATEWAY_FILENAME) == (results[i] == 1),
                "only successful bootstrap retains a helper");
    }
}

void incompatible()
{
    const auto root = scratch();
    std::filesystem::create_directory(root / ".spark-native");
    std::filesystem::copy_file(SPARK_GATEWAY_FIXTURE, root / "endstone_spark.so");
    const auto path = root / ".spark-native" / SPARK_GATEWAY_FILENAME;
    std::filesystem::copy_file(SPARK_GATEWAY_INCOMPATIBLE, path);
    const auto descriptors = descriptorCount();
    void *existing = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(existing != nullptr, "load equal-SONAME incompatible helper");
    void *original_provider = provider();
    Fixture fixture(root / "endstone_spark.so");
    require(fixture.start(original_provider, &fixture.state, &fixture.info) == 0,
            "incompatible implementation rejected");
    require(!loaded(SPARK_GATEWAY_HELPER), "incompatible family never loads a fallback pool");
    ::dlclose(existing);
    require(!loaded(path), "unpublished incompatible helper remains unloadable");
    require(descriptorCount() == descriptors, "incompatible helper never establishes permanent state");
    ::dlclose(original_provider);
}

void alternateNamespace()
{
    void *object = ::dlmopen(LM_ID_NEWLM, SPARK_GATEWAY_HELPER, RTLD_NOW | RTLD_LOCAL);
    require(object != nullptr, "load helper in another loader namespace");
    auto query = reinterpret_cast<SparkGatewayQueryV1>(::dlsym(object, SPARK_GATEWAY_SYMBOL));
    require(query != nullptr, "alternate namespace ABI available");
    std::array<char, 256> error{};
    const auto root = std::filesystem::path(SPARK_GATEWAY_FIXTURE).parent_path();
    require(query()->bootstrap(root.c_str(), reinterpret_cast<void *>(&alternateNamespace), error.data(),
                               error.size()) == 0,
            "alternate namespace bootstrap fails closed");
    ::dlclose(object);
    require(!loaded(SPARK_GATEWAY_HELPER), "alternate namespace creates no main-namespace pool");
}

void installed(const std::filesystem::path &root)
{
    void *original_provider = provider();
    Fixture fixture(root / "endstone_spark.so");
    require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0,
            "extracted archive finds its installed helper");
    const auto entries = fixture.info.binding;
    all(entries);
    fixture.unload();
    all(entries);
    ::dlclose(original_provider);
}

void providerRejection()
{
    void *unsafe = ::dlopen(SPARK_GATEWAY_DEPENDENT_PROVIDER, RTLD_NOW | RTLD_LOCAL);
    require(unsafe != nullptr, "load provider depending on Spark");
    Fixture fixture;
    require(fixture.start(unsafe, &fixture.state, &fixture.info) == 0,
            "provider dependency closure cannot retain Spark");
    require(helper()->used() == 0 && helper()->leases() == 0, "rejected provider consumes no entries or leases");
    void *original_provider = provider();
    constexpr std::array names{"provider_malloc",        "provider_calloc",       "provider_realloc",
                               "provider_free",          "provider_reallocarray", "provider_aligned_alloc",
                               "provider_posix_memalign"};
    std::array<void *, 7> originals{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        originals[i] = ::dlsym(original_provider, names[i]);
    }
    void *anonymous = ::mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(anonymous != MAP_FAILED, "anonymous original fixture");
    const std::array bad_targets{static_cast<void *>(nullptr), anonymous, reinterpret_cast<void *>(fixture.start),
                                 static_cast<void *>(&ProviderCounters)};
    for (void *target : bad_targets) {
        originals[6] = target;
        SparkGatewayBindingV1 binding{
            .size = sizeof(SparkGatewayBindingV1), .group = 0, .entries = {}, .tls_entry = nullptr};
        char error[256]{};
        require(helper()->reserve(originals.data(), reinterpret_cast<void *>(fixture.start), &binding, error,
                                  sizeof(error)) == 0,
                "unresolved/anonymous/Spark/non-executable original rejected");
        require(helper()->used() == 0 && helper()->leases() == 0, "partial provider acquisition rolls back");
    }
    ::munmap(anonymous, 4096);
    ::dlclose(original_provider);
    ::dlclose(unsafe);
    fixture.unload();
}

void multipleCandidates()
{
    void *first = ::dlopen(SPARK_GATEWAY_HELPER, RTLD_NOW | RTLD_LOCAL);
    void *second = ::dlopen(SPARK_GATEWAY_INCOMPATIBLE, RTLD_NOW | RTLD_LOCAL);
    require(first != nullptr && second != nullptr, "load multiple uninitialized family candidates");
    void *original_provider = provider();
    Fixture fixture;
    require(fixture.start(original_provider, &fixture.state, &fixture.info) == 0,
            "multiple family candidates reject admission");
    require(helper()->installation()[0] == '\0', "multiple candidates never bootstrap another pool");
    ::dlclose(second);
    ::dlclose(first);
    require(!loaded(SPARK_GATEWAY_HELPER), "unpublished family candidates remain unloadable");
    ::dlclose(original_provider);
}

void lateClosed()
{
    void *original_provider = provider();
    for (unsigned api = 0; api < 8; ++api) {
        for (unsigned final = api == 7 ? 1 : 0; final <= 1; ++final) {
            Fixture fixture;
            require(fixture.start(original_provider, &fixture.state, &fixture.info) != 0, "late CLOSED fixture start");
            const auto binding = fixture.info.binding;
            const auto *resident = helper();
            alignas(8) std::uint64_t gate = 0;
            unsigned payload = 9;
            resident->test_gate(binding.group, api, 6, &gate);
            std::thread caller([&] { invokeOne(binding, api, &payload); });
            waitGate(gate);
            if (final != 0) {
                fixture.unload();
            }
            else {
                require(fixture.stop() != 0, "ordinary close observes zero before late increment");
            }
            const auto before = counts(fixture.state);
            __atomic_store_n(&gate, 2, __ATOMIC_RELEASE);
            waitGate(gate, 3);
            require(resident->active(binding.group, api == 7 ? 1 : 0) == 1, "late CLOSED increment is retained");
            if (final == 0) {
                require(fixture.restart() != 0, "reopen preserves a transient CLOSED-path count");
                require(resident->active(binding.group, 0) == 1, "reopen never overwrites the reference word");
            }
            __atomic_store_n(&gate, 4, __ATOMIC_RELEASE);
            caller.join();
            resident->test_gate(binding.group, api, 0, nullptr);
            require(resident->active(binding.group, api == 7 ? 1 : 0) == 0, "late decrement never underflows");
            auto after = counts(fixture.state);
            if (api != 3) {
                after[3] = before[3];
            }
            require(after == before, "closed-path caller does not read reopened callbacks");
            if (fixture.handle != nullptr) {
                fixture.unload();
            }
            std::printf("late CLOSED PASS: entry=%u final=%u\n", api, final);
        }
    }
    ::dlclose(original_provider);
}

const char *lastError(Fixture &fixture)
{
    const auto error = reinterpret_cast<const char *(*)()>(::dlsym(fixture.handle, "fixture_error"));
    require(error != nullptr, "fixture error export");
    return error();
}

void dynamicRejection()
{
    void *dynamic = ::dlopen(SPARK_GATEWAY_DYNAMIC_PROVIDER, RTLD_NOW | RTLD_GLOBAL);
    require(dynamic != nullptr, "load harmless dynamic provider");
    require(::dlsym(RTLD_DEFAULT, "provider_malloc") != nullptr, "dynamic provider default lookup");
    {
        Fixture fixture;
        require(fixture.start(dynamic, &fixture.state, &fixture.info) == 0, "dynamic provider rejected");
        require(std::strstr(lastError(fixture), "unsafe provider") != nullptr, "dynamic provider admission stage");
        require(helper()->used() == 0 && helper()->leases() == 0, "dynamic provider rejected before group and lease");
    }
    require(::dlclose(dynamic) == 0 && !loaded(SPARK_GATEWAY_DYNAMIC_PROVIDER),
            "dynamic provider external lease released");
}

void handleFault(unsigned point)
{
    void *preloaded = nullptr;
    if (point == 1) {
        preloaded = ::dlopen(SPARK_GATEWAY_HELPER, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        require(preloaded != nullptr, "preload helper for discovery acquisition fault");
    }
    require(::setenv("SPARK_GATEWAY_HANDLE_FAULT", std::to_string(point).c_str(), 1) == 0, "set handle fault");
    void *originals = provider();
    {
        Fixture fixture;
        require(fixture.start(originals, &fixture.state, &fixture.info) == 0, "fault rejects actual acquisition path");
        require(std::strstr(lastError(fixture), "bad_alloc") != nullptr, "actual acquisition fault was reached");
    }
    ::unsetenv("SPARK_GATEWAY_HANDLE_FAULT");
    ::dlclose(originals);
    if (preloaded != nullptr) {
        ::dlclose(preloaded);
    }
    if (point < 10) {
        require(!loaded(SPARK_GATEWAY_HELPER), "failed client acquisition releases helper exactly once");
    }
    else {
        require(helper()->used() == 0 && helper()->leases() == 0,
                "failed provider acquisition rolls back earlier leases");
    }
}

void bindingRejection()
{
    void *preloaded = ::dlopen(SPARK_GATEWAY_HELPER, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    require(preloaded != nullptr, "preload helper before bootstrap");
    link_map *map = nullptr;
    require(::dlinfo(preloaded, RTLD_DI_LINKMAP, static_cast<void *>(&map)) == 0 && map != nullptr,
            "helper loader identity");
    const Elf64_Sym *symbols = nullptr;
    const char *strings = nullptr;
    const Elf64_Rela *relocations = nullptr;
    std::size_t count = 0;
    for (const auto *entry = map->l_ld; entry->d_tag != DT_NULL; ++entry) {
        if (entry->d_tag == DT_SYMTAB) {
            symbols = std::bit_cast<const Elf64_Sym *>(entry->d_un.d_ptr);
        }
        if (entry->d_tag == DT_STRTAB) {
            strings = std::bit_cast<const char *>(entry->d_un.d_ptr);
        }
        if (entry->d_tag == DT_JMPREL) {
            relocations = std::bit_cast<const Elf64_Rela *>(entry->d_un.d_ptr);
        }
        if (entry->d_tag == DT_PLTRELSZ) {
            count = entry->d_un.d_val / sizeof(Elf64_Rela);
        }
    }
    require(symbols != nullptr && strings != nullptr && relocations != nullptr, "helper relocation fixture metadata");
    bool patched = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &entry = relocations[i];
        if (std::strcmp(strings + symbols[ELF64_R_SYM(entry.r_info)].st_name, "getpid") != 0) {
            continue;
        }
        const auto address = map->l_addr + entry.r_offset;
        const auto page_size = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
        auto *page = std::bit_cast<void *>(address & ~(page_size - 1));
        require(::mprotect(page, page_size, PROT_READ | PROT_WRITE) == 0, "open fixture RELRO slot");
        *std::bit_cast<void **>(address) = reinterpret_cast<void *>(&::getppid);
        require(::mprotect(page, page_size, PROT_READ) == 0, "restore fixture RELRO slot");
        patched = true;
        break;
    }
    require(patched, "change exact getpid binding to a different function in the same approved libc");
    void *originals = provider();
    {
        Fixture fixture;
        require(fixture.start(originals, &fixture.state, &fixture.info) == 0, "wrong same-image binding rejected");
        require(std::strstr(lastError(fixture), "external binding differs") != nullptr,
                "actual binding validation rejects fixture");
    }
    ::dlclose(originals);
    require(::dlclose(preloaded) == 0 && !loaded(SPARK_GATEWAY_HELPER), "rejected helper was never promoted NODELETE");
}

void transitiveProvider()
{
    void *transitive = ::dlopen(SPARK_GATEWAY_TRANSITIVE_PROVIDER, RTLD_NOW | RTLD_NOLOAD);
    require(transitive != nullptr, "provider C belongs to startup A DT_NEEDED closure");
    {
        Fixture fixture;
        require(fixture.start(transitive, &fixture.state, &fixture.info) != 0, "transitive startup provider admitted");
        invokeOne(fixture.info.binding, 0);
    }
    ::dlclose(transitive);
}

void unlinkedProvider(bool lazy)
{
    {
        Fixture fixture;
        void *global = ::dlopen(fixture.path.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        require(global != nullptr, "expose Spark fixture symbol");
        void *unlinked = ::dlopen(SPARK_GATEWAY_UNLINKED_PROVIDER, (lazy ? RTLD_LAZY : RTLD_NOW) | RTLD_GLOBAL);
        require(unlinked != nullptr, "load dynamic provider with undefined Spark binding and no DT_NEEDED edge");
        const auto lookup = reinterpret_cast<void *(*)()>(::dlsym(global, "fixture_default_binding"));
        require(lookup != nullptr, "Spark-origin RTLD_DEFAULT fixture lookup");
        const auto anchor = reinterpret_cast<void *(*)()>(lookup());
        require(anchor != nullptr && anchor() == ::dlsym(global, "spark_gateway_unlinked_import"),
                "dynamic provider really resolved an undefined reference into Spark");
        require(fixture.start(unlinked, &fixture.state, &fixture.info) == 0, "unlinked dynamic provider rejected");
        require(std::strstr(lastError(fixture), "unsafe provider") != nullptr,
                "unlinked provider rejected at admission");
        require(helper()->used() == 0 && helper()->leases() == 0,
                "unlinked provider creates no permanent lease or group");
        ::dlclose(unlinked);
        ::dlclose(global);
    }
    require(!loaded(SPARK_GATEWAY_UNLINKED_PROVIDER), "external unlinked provider release removes image");
}

void externalImport(bool preload, unsigned ifunc = 0)
{
    std::string pattern = (std::filesystem::temp_directory_path() / "spark-helper-import-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back(0);
    require(::mkdtemp(buffer.data()) != nullptr, "create isolated helper binding fixture");
    const std::filesystem::path root(buffer.data());
    const auto helper_path = root / ".spark-native" / SPARK_GATEWAY_FILENAME;
    std::filesystem::create_directory(root / ".spark-native");
    std::filesystem::copy_file(SPARK_GATEWAY_FIXTURE, root / "endstone_spark.so");
    std::filesystem::copy_file(ifunc == 0 ? SPARK_GATEWAY_EXTERNAL_IMPORT : SPARK_GATEWAY_IFUNC_HELPER, helper_path);
    {
        Fixture fixture(root / "endstone_spark.so");
        void *global = ::dlopen(fixture.path.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        require(global != nullptr, "expose import fixture definition");
        const auto queries = reinterpret_cast<unsigned (*)()>(::dlsym(global, "fixture_query_calls"));
        require(queries != nullptr && queries() == 0, "helper query has not executed");
        IfuncTarget =
            ifunc == 1 ? reinterpret_cast<void *>(&approvedImport) : ::dlsym(global, "spark_gateway_external_import");
        if (preload) {
            void *handle = ::dlopen(helper_path.c_str(), RTLD_NOW | RTLD_LOCAL);
            require(handle != nullptr, "externally preload helper bound to Spark");
            const auto query = reinterpret_cast<SparkGatewayQueryV1>(::dlsym(handle, SPARK_GATEWAY_SYMBOL));
            require(query != nullptr, "preloaded helper query export");
            const auto *api = query();
            require(queries() == 1, "preloaded helper has an actual Spark binding");
            char error[256]{};
            require(api->bootstrap(root.c_str(), reinterpret_cast<void *>(fixture.start), error, sizeof(error)) == 0,
                    "independent helper bootstrap rejects external Spark binding");
            require(std::strstr(error, "no approved versioned definition") != nullptr,
                    "bootstrap rejected import evidence before NODELETE");
            require(::dlclose(handle) == 0 && !loaded(helper_path), "rejected preloaded helper unloads");
        }
        else {
            void *originals = provider();
            if (ifunc == 1) {
                require(fixture.start(originals, &fixture.state, &fixture.info) != 0,
                        "approved startup IFUNC target admitted");
                require(ApprovedQueries == 1 && queries() == 0, "approved IFUNC target executed exactly once");
            }
            else {
                require(fixture.start(originals, &fixture.state, &fixture.info) == 0,
                        "Spark-only helper import rejected");
                require(std::strstr(lastError(fixture), ifunc == 0 ? "no approved versioned definition"
                                                                   : "external binding differs") != nullptr,
                        "helper preflight or actual IFUNC binding rejected import evidence");
                require(queries() == 0 && !loaded(helper_path),
                        "rejection executes no helper query and releases helper");
            }
            ::dlclose(originals);
        }
        ::dlclose(global);
    }
    std::filesystem::remove_all(root);
}

void providerLookup(bool perform_lookup)
{
    {
        Fixture fixture;
        void *global = ::dlopen(fixture.path.c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_GLOBAL);
        require(global != nullptr, "expose Spark to provider-origin lookup");
        void *dynamic = ::dlopen(SPARK_GATEWAY_LOOKUP_PROVIDER, RTLD_NOW | RTLD_LOCAL);
        require(dynamic != nullptr, "load dynamic provider and its transitive dependency");
        if (perform_lookup) {
            const auto lookup = reinterpret_cast<void *(*)()>(::dlsym(dynamic, "provider_lookup_spark"));
            require(lookup != nullptr && lookup() == ::dlsym(global, "spark_gateway_unlinked_import"),
                    "dynamic provider itself previously resolves Spark using RTLD_DEFAULT");
        }
        require(fixture.start(dynamic, &fixture.state, &fixture.info) == 0,
                "dynamic transitive or lookup provider rejected");
        require(std::strstr(lastError(fixture), "unsafe provider") != nullptr && helper()->leases() == 0 &&
                    helper()->used() == 0,
                "provider rejected before permanent lease and group publication");
        ::dlclose(dynamic);
        ::dlclose(global);
    }
    require(!loaded(SPARK_GATEWAY_LOOKUP_PROVIDER), "external provider handles release lookup dependency");
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void *spark_fixture_ifunc_target()
{
    return IfuncTarget;
}

int main(int argc, char **argv)
{
#if defined(SPARK_EXPECT_NONPIE)
    spark::gateway::Identity identity;
    Dl_info info{};
    require(spark::gateway::identify(reinterpret_cast<void *>(&approvedImport), identity) && identity.base == 0 &&
                ::dladdr(reinterpret_cast<void *>(&approvedImport), &info) != 0 && info.dli_fbase != nullptr,
            "non-PIE identity uses zero ELF load bias rather than nonzero dli_fbase");
#endif
    require(argc >= 2, "test mode");
    const std::string mode = argv[1];
    if (mode == "lifetime") {
        lifetime();
    }
    else if (mode == "tls") {
        tls();
    }
    else if (mode == "budget") {
        budget();
    }
    else if (mode == "layout") {
        layout();
    }
    else if (mode == "plain") {
        plain();
    }
    else if (mode == "concurrent") {
        concurrent();
    }
    else if (mode == "incompatible") {
        incompatible();
    }
    else if (mode == "namespace") {
        alternateNamespace();
    }
    else if (mode == "installed" && argc == 3) {
        installed(argv[2]);
    }
    else if (mode == "provider_rejection") {
        providerRejection();
    }
    else if (mode == "late_closed") {
        lateClosed();
    }
    else if (mode == "multiple") {
        multipleCandidates();
    }
    else if (mode == "dynamic_rejection") {
        dynamicRejection();
    }
    else if (mode == "binding_rejection") {
        bindingRejection();
    }
    else if (mode == "handle_fault" && argc == 3) {
        handleFault(static_cast<unsigned>(std::stoul(argv[2])));
    }
    else if (mode == "transitive") {
        transitiveProvider();
    }
    else if (mode == "unlinked" || mode == "unlinked_lazy") {
        unlinkedProvider(mode == "unlinked_lazy");
    }
    else if (mode == "external_import" || mode == "external_preloaded") {
        externalImport(mode == "external_preloaded");
    }
    else if (mode == "ifunc_positive" || mode == "ifunc_spark") {
        externalImport(false, mode == "ifunc_positive" ? 1 : 2);
    }
    else if (mode == "provider_lookup" || mode == "dynamic_transitive") {
        providerLookup(mode == "provider_lookup");
    }
    else {
        std::abort();
    }
    std::printf("PASS: %s\n", mode.c_str());
}

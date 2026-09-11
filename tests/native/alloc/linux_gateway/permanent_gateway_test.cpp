#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

#include <sys/mman.h>

#include "fixture_api.h"
#include "native/alloc/linux_permanent_gateway_registry.h"

#if defined(SPARK_GATEWAY_MIXED_HOST)
extern "C" __attribute__((visibility("default"))) _Unwind_Ptr _Unwind_GetIP(_Unwind_Context *context)
{
    const auto original = reinterpret_cast<_Unwind_Ptr (*)(_Unwind_Context *)>(::dlsym(RTLD_NEXT, "_Unwind_GetIP"));
    if (original == nullptr) {
        std::abort();
    }
    return original(context);
}
#endif

namespace {
using namespace spark::gateway::permanent;
void require(bool condition, const char *message);

Directory SampleDirectory;
std::atomic<unsigned> SampleFrames{0};
std::atomic<bool> SampleGateway{false};
std::atomic<bool> SampleDone{false};

_Unwind_Reason_Code sampledFrame(_Unwind_Context *context, void *)
{
    const auto pc = reinterpret_cast<decltype(&_Unwind_GetIP)>(SampleDirectory.host.functions[4])(context);
    SampleFrames.fetch_add(1);
    if (pc >= SampleDirectory.code && pc < SampleDirectory.code + SampleDirectory.code_size) {
        SampleGateway.store(true);
    }
    return _URC_NO_REASON;
}

void sampleSignal(int)
{
    reinterpret_cast<decltype(&_Unwind_Backtrace)>(SampleDirectory.host.functions[3])(&sampledFrame, nullptr);
    SampleDone.store(true, std::memory_order_release);
}

void sampleRemovedGateway(std::thread &thread, const Directory &directory)
{
    SampleDirectory = directory;
    SampleFrames.store(0);
    SampleGateway.store(false);
    SampleDone.store(false);
    require(::pthread_kill(thread.native_handle(), SIGUSR1) == 0, "sample permanent continuation after image removal");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!SampleDone.load(std::memory_order_acquire)) {
        require(std::chrono::steady_clock::now() < deadline, "host unwind after image removal deadline");
        std::this_thread::yield();
    }
    require(SampleGateway.load() && SampleFrames.load() >= 4, "real host unwind crosses removed-owner continuation");
}

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

template <typename Function>
Function symbol(void *handle, const char *name)
{
    auto result = reinterpret_cast<Function>(::dlsym(handle, name));
    require(result != nullptr, name);
    return result;
}

void wait(std::uint64_t &gate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (__atomic_load_n(&gate, __ATOMIC_ACQUIRE) != 1) {
        require(std::chrono::steady_clock::now() < deadline, "gateway phase gate deadline");
        std::this_thread::yield();
    }
}

struct Fixture {
    const char *path;
    void *handle;
    GatewayFixtureState state;
    GatewayFixtureInfo info;
    int (*start)(void *, GatewayFixtureState *, GatewayFixtureInfo *);
    int (*stop)();
    int (*restart)();
    int (*shutdown)();
    const SparkGatewayV1 *api;
    Directory directory;

    explicit Fixture(const char *location = SPARK_GATEWAY_FIXTURE)
        : path(location), handle(::dlopen(path, RTLD_NOW | RTLD_LOCAL))
    {
        if (handle == nullptr) {
            std::fprintf(stderr, "dlopen: %s\n", ::dlerror());
        }
        require(handle != nullptr, "load synthetic Spark image");
        start = symbol<decltype(start)>(handle, "fixture_start");
        stop = symbol<decltype(stop)>(handle, "fixture_stop");
        restart = symbol<decltype(restart)>(handle, "fixture_restart");
        shutdown = symbol<decltype(shutdown)>(handle, "fixture_shutdown");
        api = symbol<const SparkGatewayV1 *(*)()>(handle, "fixture_api")();
    }

    void begin(void *provider)
    {
        require(start(provider, &state, &info) == 1, "start synthetic Spark owner");
        directory = *symbol<const Directory *(*)()>(handle, "fixture_directory")();
        require(symbol<int (*)()>(handle, "fixture_private_rows")() == 1,
                "actual private GNU unwind restores IP/SP/RBP across every prologue and epilogue state");
        Dl_info owner{};
        require(::dladdr(reinterpret_cast<void *>(directory.host.functions[0]), &owner) != 0, "host tuple owner");
        std::printf("host mode=%u provider=%s group=%u\n", static_cast<unsigned>(directory.host.mode), owner.dli_fname,
                    info.binding.group);
    }

    void unload()
    {
        require(shutdown() == 1, "drain and retire synthetic owner");
        require(::dlclose(handle) == 0, "dlclose synthetic Spark image");
        handle = nullptr;
        void *remaining = ::dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        if (remaining != nullptr) {
            ::dlclose(remaining);
        }
        require(remaining == nullptr, "actual Spark image removal");
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
    void *handle = ::dlopen(SPARK_GATEWAY_PROVIDER, RTLD_NOW | RTLD_NOLOAD);
    require(handle != nullptr, "allocator provider is in startup closure");
    return handle;
}

void invoke(const SparkGatewayBindingV1 &binding, unsigned api)
{
    void *result = nullptr;
    switch (api) {
    case 0:
        result = reinterpret_cast<void *(*)(std::size_t)>(binding.entries[0])(64);
        break;
    case 1:
        result = reinterpret_cast<void *(*)(std::size_t, std::size_t)>(binding.entries[1])(3, 32);
        require(result != nullptr && static_cast<unsigned char *>(result)[95] == 0, "calloc arguments and result");
        break;
    case 2:
        result = reinterpret_cast<void *(*)(void *, std::size_t)>(binding.entries[2])(nullptr, 96);
        break;
    case 3:
        reinterpret_cast<void (*)(void *)>(binding.entries[3])(nullptr);
        return;
    case 4:
        result = reinterpret_cast<void *(*)(void *, std::size_t, std::size_t)>(binding.entries[4])(nullptr, 3, 32);
        break;
    case 5:
        result = reinterpret_cast<void *(*)(std::size_t, std::size_t)>(binding.entries[5])(16, 64);
        require(result != nullptr && reinterpret_cast<std::uintptr_t>(result) % 16 == 0, "aligned allocation result");
        break;
    case 6:
        require(reinterpret_cast<int (*)(void **, std::size_t, std::size_t)>(binding.entries[6])(&result, 16, 64) == 0,
                "posix_memalign arguments and return");
        break;
    case 7: {
        unsigned value = 19;
        binding.tls_entry(&value);
        return;
    }
    }
    require(result != nullptr, "allocator signature result");
    reinterpret_cast<void (*)(void *)>(binding.entries[3])(result);
}

void verifyLookups(const Directory &directory)
{
    struct Bases {
        void *text;
        void *data;
        void *function;
    };
    const auto find = reinterpret_cast<const void *(*)(const void *, Bases *)>(directory.host.functions[2]);
    for (std::size_t i = 0; i < KEntryCount; ++i) {
        Bases bases{};
        const auto code = directory.code + i * KCodeStride;
        require(find(reinterpret_cast<void *>(code + 1), &bases) ==
                        reinterpret_cast<void *>(directory.metadata + KCieSize + i * KFdeStride) &&
                    bases.function == reinterpret_cast<void *>(code),
                "all permanent host FDEs survive image removal");
    }
}

void basic()
{
    auto *allocator = provider();
    Fixture first;
    first.begin(allocator);
    first.state.nested.store(true);
    for (unsigned api = 0; api < 8; ++api) {
        invoke(first.info.binding, api);
        require(first.state.callbacks[api].load() != 0, "all callback signatures admitted");
    }
    require(first.state.unwind_mask.load() == 15, "actual host and cpptrace unwind through gateway into caller");
    require(first.api->active(first.info.binding.group, 0) == 0, "nested gateway callbacks drain every admission");
    auto &entry = reinterpret_cast<State *>(first.directory.state)->groups[first.info.binding.group].entries[0];
    const auto before_saturation = first.state.callbacks[0].load();
    entry.state.store(UINT32_MAX);
    invoke(first.info.binding, 0);
    require(first.state.callbacks[0].load() == before_saturation && entry.state.load() == UINT32_MAX,
            "saturated admission preserves count and falls back");
    entry.state.store(0);
    errno = EDOM;
    require(reinterpret_cast<void *(*)(std::size_t)>(first.info.binding.entries[0])(0x12345678) == nullptr &&
                errno == ENOMEM,
            "admitted allocator errno");
    const auto entries = first.info.binding;
    const auto directory = first.directory;
    const auto registrations = reinterpret_cast<State *>(directory.state)->host_registrations;
    const auto used = first.api->used();
    const auto leases = first.api->leases();
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        require(first.stop() == 1, "ordinary stop drains");
        for (unsigned api = 0; api < 7; ++api) {
            invoke(entries, api);
        }
        require(first.restart() == 1, "same-owner restart reuses entries");
        require(first.api->used() == used && first.api->leases() == leases,
                "stable start-stop group and provider counts");
    }
    first.unload();
    verifyLookups(directory);
    Fixture next;
    next.begin(allocator);
    require(reinterpret_cast<State *>(directory.state)->host_registrations == registrations,
            "reload never registers host frames again");
    require(next.directory.code == directory.code && next.directory.host.mode == directory.host.mode,
            "reload adopts permanent arena and host registration");
    require(next.info.binding.group != entries.group, "new owner never reuses a published group");
    for (unsigned api = 0; api < 7; ++api) {
        const auto before = next.state.callbacks[api].load();
        invoke(entries, api);
        require(next.state.callbacks[api].load() == before, "old entry never calls new owner");
        invoke(next.info.binding, api);
    }
    errno = EDOM;
    require(reinterpret_cast<void *(*)(std::size_t)>(entries.entries[0])(0x12345678) == nullptr && errno == ENOMEM,
            "retired allocator errno");
    next.unload();
    ::dlclose(allocator);
}

void gates()
{
    struct sigaction action{};
    struct sigaction previous{};
    action.sa_handler = &sampleSignal;
    ::sigemptyset(&action.sa_mask);
    require(::sigaction(SIGUSR1, &action, &previous) == 0, "install isolated unwind sampler");
    auto *allocator = provider();
    for (unsigned phase = 1; phase <= 6; ++phase) {
        for (unsigned api = 0; api < 8; ++api) {
            if (phase == 6 && api == 7) {
                continue;
            }
            Fixture fixture;
            fixture.begin(allocator);
            const auto entry = fixture.info.binding;
            const auto directory = fixture.directory;
            std::uint64_t gate = 0;
            if (phase == 6) {
                require(fixture.stop() == 1, "close before original tail-jump gate");
            }
            fixture.api->test_gate(entry.group, api, phase, &gate);
            std::thread caller([&] { invoke(entry, api); });
            wait(gate);
            if (phase >= 2 && phase <= 4) {
                require(fixture.shutdown() == 0, "admitted callback retains cleanup ownership");
                __atomic_store_n(&gate, 2, __ATOMIC_RELEASE);
                caller.join();
                fixture.unload();
            }
            else {
                fixture.unload();
                sampleRemovedGateway(caller, directory);
                verifyLookups(directory);
                Fixture next;
                next.begin(allocator);
                __atomic_store_n(&gate, 2, __ATOMIC_RELEASE);
                caller.join();
                require(next.state.callbacks[api].load() == 0, "paused old continuation cannot enter reload owner");
            }
        }
    }
    ::dlclose(allocator);
    require(::sigaction(SIGUSR1, &previous, nullptr) == 0, "restore isolated unwind sampler");
}

void tls()
{
    auto *allocator = provider();
    Fixture fixture;
    fixture.begin(allocator);
    const auto entry = fixture.info.binding;
    const auto key = fixture.info.key;
    std::uint64_t gate = 0;
    fixture.api->test_gate(entry.group, 7, 2, &gate);
    unsigned payload = 19;
    std::thread caller([&] { require(::pthread_setspecific(key, &payload) == 0, "arm real pthread destructor"); });
    wait(gate);
    require(fixture.shutdown() == 0, "admitted pthread destructor prevents shutdown");
    __atomic_store_n(&gate, 2, __ATOMIC_RELEASE);
    caller.join();
    require(payload == 0, "admitted destructor completed payload work");
    fixture.unload();
    void *inaccessible = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(inaccessible != MAP_FAILED, "inaccessible TLS payload fixture");
    pthread_key_t reused{};
    require(::pthread_key_create(&reused, nullptr) == 0 && reused == key, "deleted pthread key reused");
    entry.tls_entry(inaccessible);
    require(::pthread_key_delete(reused) == 0, "delete reused key");
    ::munmap(inaccessible, 4096);
    ::dlclose(allocator);
}

void exhaustion()
{
    auto *allocator = provider();
    for (std::size_t i = 0; i < KCapacity; ++i) {
        Fixture fixture;
        fixture.begin(allocator);
        require(fixture.info.binding.group == i, "monotonic permanent group ownership");
    }
    Fixture exhausted;
    require(exhausted.start(allocator, &exhausted.state, &exhausted.info) == 0, "lifetime capacity fails closed");
    ::dlclose(allocator);
}

void unsafeProvider()
{
    void *dynamic = ::dlopen(SPARK_GATEWAY_DYNAMIC_PROVIDER, RTLD_NOW | RTLD_LOCAL);
    require(dynamic != nullptr, "load provider outside startup closure");
    Fixture fixture;
    require(fixture.start(dynamic, &fixture.state, &fixture.info) == 0, "reject unloadable allocator originals");
    require(fixture.api->used() == 0 && fixture.api->leases() == 1, "provider rejection leaves only host lease");
    ::dlclose(dynamic);
}

std::size_t descriptors()
{
    std::size_t count = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        ++count;
    }
    return count;
}

void rollback()
{
    auto *allocator = provider();
    const bool llvm = std::string_view(SPARK_GATEWAY_HOST) == "llvm";
    for (auto index : {0, 1024, 2047}) {
        if (!llvm && index != 0) {
            continue;
        }
        Fixture failed;
        const auto before = descriptors();
        require(::setenv("SPARK_GATEWAY_REGISTRATION_FAULT", std::to_string(index).c_str(), 1) == 0,
                "arm returned-registration failure");
        require(failed.start(allocator, &failed.state, &failed.info) == 0, "injected host registration fails closed");
        ::unsetenv("SPARK_GATEWAY_REGISTRATION_FAULT");
        require(descriptors() == before, "verified rollback releases singleton, memfd and provider acquisition");
    }
    Fixture recovered;
    recovered.begin(allocator);
    require(recovered.info.binding.group == 0, "clean bootstrap rollback permits a fresh first arena");
    invoke(recovered.info.binding, 0);
    require(recovered.state.unwind_mask.load() == 15, "host and private unwind remain functional after rollback");
    ::dlclose(allocator);
}

void registry()
{
    auto *allocator = provider();
    Fixture initial;
    initial.begin(allocator);
    const auto directory = initial.directory;
    initial.unload();
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
        auto invalid = directory;
        if (mutation == 1) {
            invalid.compatibility[0] ^= 1;
        }
        else if (mutation == 2) {
            invalid.state_size += 4096;
        }
        const int descriptor = ::memfd_create("spark.alloc.permanent.invalid", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        require(descriptor >= 0 && ::ftruncate(descriptor, sizeof(invalid)) == 0 &&
                    ::pwrite(descriptor, &invalid, sizeof(invalid), 0) == sizeof(invalid) &&
                    ::fcntl(descriptor, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) == 0,
                "create sealed malformed or duplicate directory");
        {
            Fixture rejected;
            require(rejected.start(allocator, &rejected.state, &rejected.info) == 0,
                    "reject ambiguous or invalid directory");
        }
        ::close(descriptor);
    }
    const auto last_page = reinterpret_cast<void *>(directory.code + directory.code_size - 4096);
    require(::mprotect(last_page, 4096, PROT_READ) == 0, "invalidate final code mapping page");
    {
        Fixture rejected;
        require(rejected.start(allocator, &rejected.state, &rejected.info) == 0, "validate the full executable range");
    }
    require(::mprotect(last_page, 4096, PROT_READ | PROT_EXEC) == 0, "restore test mapping protection");
    Fixture valid;
    valid.begin(allocator);
    require(valid.directory.code == directory.code, "rejected adoption never allocates a second arena");
    ::dlclose(allocator);
}

void arena(const char *output)
{
    auto *allocator = provider();
    Fixture fixture(SPARK_GATEWAY_PRODUCTION_FIXTURE);
    fixture.begin(allocator);
    require(fixture.api->test_gate == nullptr, "production gateway excludes test gates");
    invoke(fixture.info.binding, 0);
    require(fixture.state.unwind_mask.load() == 15, "production gateway host and private unwind");
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output);
    std::ofstream code(path / "code.bin", std::ios::binary);
    code.write(reinterpret_cast<const char *>(fixture.directory.code), KCodeSize);
    std::ofstream metadata(path / "metadata.bin", std::ios::binary);
    metadata.write(reinterpret_cast<const char *>(fixture.directory.metadata), KMetadataSize);
    std::ofstream manifest(path / "manifest.json");
    manifest << "{\"code\":" << fixture.directory.code << ",\"metadata\":" << fixture.directory.metadata
             << ",\"state\":" << fixture.directory.state << ",\"groups_offset\":" << offsetof(State, groups)
             << ",\"group_stride\":" << sizeof(Group) << ",\"entry_stride\":" << sizeof(Entry) << "}";
    require(code.good() && metadata.good() && manifest.good(), "write synthetic permanent arena evidence");
    ::dlclose(allocator);
}

void cancellation()
{
    auto *allocator = provider();
    Fixture fixture;
    const auto reserve = symbol<int (*)(void *)>(fixture.handle, "fixture_reserve");
    const auto cancel = symbol<int (*)()>(fixture.handle, "fixture_cancel");
    require(reserve(allocator) == 1, "reserve unpublished group");
    const auto directory = *symbol<const Directory *(*)()>(fixture.handle, "fixture_directory")();
    auto *state = reinterpret_cast<State *>(directory.state);
    require(state->lock.exchange(1) == 0, "hold setup lock for cancellation deadline");
    const auto begin = std::chrono::steady_clock::now();
    require(cancel() == 0, "noexcept cancellation retains ownership on setup deadline");
    require(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2), "bounded cancellation deadline");
    state->lock.store(0, std::memory_order_release);
    require(cancel() == 1, "retry cancellation after setup lock release");
    const auto baseline = descriptors();
    for (unsigned repeat = 0; repeat < 32; ++repeat) {
        require(reserve(allocator) == 1 && cancel() == 1, "transactional unpublished reservation reuse");
        require(fixture.api->used() == 0 && fixture.api->leases() == 1 && descriptors() == baseline,
                "cancellation restores group and deduplicated provider resources");
    }
    for (const char *point : {"10", "16", "20"}) {
        require(::setenv("SPARK_GATEWAY_HANDLE_FAULT", point, 1) == 0, "inject provider acquisition failure");
        require(reserve(allocator) == 0, "failed provider acquisition leaves no reservation");
        ::unsetenv("SPARK_GATEWAY_HANDLE_FAULT");
        require(fixture.api->used() == 0 && fixture.api->leases() == 1 && descriptors() == baseline,
                "provider acquisition rollback restores all resources");
    }
    ::dlclose(allocator);
}

void quarantine()
{
    auto *allocator = provider();
    Fixture failed;
    const auto before = descriptors();
    require(::setenv("SPARK_GATEWAY_UNPROVEN_ROLLBACK", "1", 1) == 0, "inject unproven host rollback");
    require(failed.start(allocator, &failed.state, &failed.info) == 0, "unproven rollback rejects bootstrap");
    ::unsetenv("SPARK_GATEWAY_UNPROVEN_ROLLBACK");
    require(descriptors() == before + 2, "quarantine retains singleton and directory descriptors");
    failed.unload();
    Fixture retry;
    const auto retained = descriptors();
    require(retry.start(allocator, &retry.state, &retry.info) == 0, "quarantined bootstrap requires process restart");
    require(descriptors() == retained, "quarantine never creates another arena or registration set");
    ::dlclose(allocator);
}

}  // namespace

int main(int argc, char **argv)
{
    require(argc >= 2, "test mode");
    const std::string_view mode(argv[1]);
    if (mode == "basic") {
        basic();
    }
    else if (mode == "gates") {
        gates();
    }
    else if (mode == "tls") {
        tls();
    }
    else if (mode == "exhaustion") {
        exhaustion();
    }
    else if (mode == "provider") {
        unsafeProvider();
    }
    else if (mode == "rollback") {
        rollback();
    }
    else if (mode == "registry") {
        registry();
    }
    else if (mode == "arena") {
        require(argc == 3, "arena output directory");
        arena(argv[2]);
    }
    else if (mode == "cancellation") {
        cancellation();
    }
    else if (mode == "quarantine") {
        quarantine();
    }
    else if (mode == "mixed") {
        auto *allocator = provider();
        Fixture fixture;
        const auto before = descriptors();
        require(fixture.start(allocator, &fixture.state, &fixture.info) == 0, "mixed-owner host tuple rejected");
        require(descriptors() == before, "mixed-owner rejection releases bootstrap resources");
        ::dlclose(allocator);
    }
    else {
        require(false, "unknown test mode");
    }
}

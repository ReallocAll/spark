#include <dlfcn.h>
#include <unwind.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "fixture_api.h"
#include "native/alloc/linux_allocation_gateway_client.h"

namespace {
struct Owner {
    spark::LinuxAllocationGateway gateway;
    std::array<void *, 7> originals{};
    GatewayFixtureState *state = nullptr;
    pthread_key_t key{};
    bool key_created = false;
};

Owner *Current = nullptr;
char LastError[256]{};
unsigned QueryCalls = 0;

_Unwind_Reason_Code unwindFrame(_Unwind_Context *context, void *opaque)
{
    auto &state = *static_cast<GatewayFixtureState *>(opaque);
    Dl_info info{};
    const auto pc = _Unwind_GetIP(context);
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    if (pc != 0 && ::dladdr(reinterpret_cast<void *>(pc), &info) != 0 && info.dli_fname != nullptr) {
        if (std::strstr(info.dli_fname, "allocation_gateway_v1") != nullptr) {
            state.unwind_mask.fetch_or(1);
        }
        else if (std::strstr(info.dli_fname, "spark_linux_gateway_test") != nullptr) {
            state.unwind_mask.fetch_or(2);
        }
    }
    return _URC_NO_REASON;
}

void enter(Owner &owner, std::size_t api)
{
    owner.state->callbacks[api].fetch_add(1);
    if (owner.state->block_callback.load()) {
        owner.state->callback_entered.store(true);
        while (!owner.state->release_callback.load()) {
            __asm__ volatile("pause");
        }
    }
}

void *mallocCallback(void *context, std::size_t size)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 0);
    _Unwind_Backtrace(&unwindFrame, owner.state);
    return reinterpret_cast<void *(*)(std::size_t)>(owner.originals[0])(size);
}
void *callocCallback(void *context, std::size_t count, std::size_t size)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 1);
    return reinterpret_cast<void *(*)(std::size_t, std::size_t)>(owner.originals[1])(count, size);
}
void *reallocCallback(void *context, void *pointer, std::size_t size)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 2);
    return reinterpret_cast<void *(*)(void *, std::size_t)>(owner.originals[2])(pointer, size);
}
void freeCallback(void *context, void *pointer)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 3);
    reinterpret_cast<void (*)(void *)>(owner.originals[3])(pointer);
}
void *reallocarrayCallback(void *context, void *pointer, std::size_t count, std::size_t size)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 4);
    return reinterpret_cast<void *(*)(void *, std::size_t, std::size_t)>(owner.originals[4])(pointer, count, size);
}
void *alignedCallback(void *context, std::size_t alignment, std::size_t size)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 5);
    return reinterpret_cast<void *(*)(std::size_t, std::size_t)>(owner.originals[5])(alignment, size);
}
int posixCallback(void *context, void **pointer, std::size_t alignment, std::size_t size)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 6);
    return reinterpret_cast<int (*)(void **, std::size_t, std::size_t)>(owner.originals[6])(pointer, alignment, size);
}
void tlsCallback(void *context, void *payload)
{
    auto &owner = *static_cast<Owner *>(context);
    enter(owner, 7);
    *static_cast<unsigned *>(payload) = 0;
}

SparkGatewayCallbacksV1 callbacks()
{
    return {mallocCallback,       callocCallback,  reallocCallback, freeCallback,
            reallocarrayCallback, alignedCallback, posixCallback,   tlsCallback};
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int fixture_start(void *provider, GatewayFixtureState *state,
                                                                    GatewayFixtureInfo *info)
{
    if (Current != nullptr) {
        return 0;
    }
    Current = new Owner;
    Current->state = state;
    constexpr std::array names{"provider_malloc",        "provider_calloc",       "provider_realloc",
                               "provider_free",          "provider_reallocarray", "provider_aligned_alloc",
                               "provider_posix_memalign"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        Current->originals[i] = ::dlsym(provider, names[i]);
    }
    std::string error;
    if (!Current->gateway.reserve(Current->originals, error) ||
        !Current->gateway.open(callbacks(), Current, true, error)) {
        std::fprintf(stderr, "fixture initialization: %s\n", error.c_str());
        std::snprintf(LastError, sizeof(LastError), "%s", error.c_str());
        delete Current;
        Current = nullptr;
        return 0;
    }
    Current->gateway.publish();
    if (::pthread_key_create(&Current->key, Current->gateway.binding().tls_entry) != 0) {
        return 0;
    }
    Current->key_created = true;
    *info = {.binding = Current->gateway.binding(), .key = Current->key};
    if (state->fail_after_key_create.load()) {
        return 0;
    }
    return Current->gateway.open(callbacks(), Current, false, error) ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) const char *fixture_error()
{
    return LastError;
}

extern "C" __attribute__((visibility("default"))) void spark_gateway_external_import()
{
    ++QueryCalls;
}

extern "C" __attribute__((visibility("default"))) unsigned fixture_query_calls()
{
    return QueryCalls;
}

extern "C" __attribute__((visibility("default"))) void spark_gateway_unlinked_import() {}

extern "C" __attribute__((visibility("default"))) void *fixture_default_binding()
{
    return ::dlsym(RTLD_DEFAULT, "provider_binding_anchor");
}

extern "C" __attribute__((visibility("default"))) int fixture_stop()
{
    std::string error;
    Current->gateway.close(false);
    return Current->gateway.waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(50), false, error) &&
                   Current->gateway.clear(false, error)
             ? 1
             : 0;
}

extern "C" __attribute__((visibility("default"))) int fixture_restart()
{
    std::string error;
    return Current->gateway.open(callbacks(), Current, false, error) ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int fixture_shutdown()
{
    if (Current == nullptr) {
        return 1;
    }
    std::string error;
    Current->gateway.close(true);
    if (!Current->gateway.waitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(50), true, error)) {
        return 0;
    }
    if (Current->key_created) {
        if (Current->state->fail_key_delete.load() || ::pthread_key_delete(Current->key) != 0) {
            return 0;
        }
        Current->key_created = false;
    }
    if (!Current->gateway.retire(error)) {
        return 0;
    }
    delete Current;
    Current = nullptr;
    return 1;
}

#include <dlfcn.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

#include "native/alloc/elf_import_hooks.h"

namespace {
pid_t (*volatile Imported)() = &::getpid;
void *(*OriginalMalloc)(std::size_t) = nullptr;
void (*OriginalFree)(void *) = nullptr;
unsigned MallocCalls = 0;
unsigned FreeCalls = 0;
bool ChangeInstall = false;
bool ChangeUninstall = false;
pid_t replacement()
{
    return -42;
}
pid_t external()
{
    return -84;
}
void *mallocHook(std::size_t size)
{
    ++MallocCalls;
    return OriginalMalloc(size);
}
void freeHook(void *pointer)
{
    ++FreeCalls;
    OriginalFree(pointer);
}

void require(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}

void beforeWrite(void **slot, bool install) noexcept
{
    if ((install && ChangeInstall) || (!install && ChangeUninstall)) {
        if (slot == reinterpret_cast<void **>(const_cast<pid_t (**)()>(&Imported))) {
            __atomic_store_n(slot, reinterpret_cast<void *>(&external), __ATOMIC_RELEASE);
        }
    }
}
}  // namespace

int main()
{
    std::string error;
    const auto real = ::getpid();
    if (std::getenv("SPARK_EXPECT_INTERPOSER") != nullptr) {
        require(real == 314159, "normal LD_PRELOAD interposition is active");
    }
    spark::ElfImportHooks hooks;
    const spark::ElfImportHookSpec spec{.name = "getpid",
                                        .replacement = reinterpret_cast<void *>(&replacement),
                                        .required = true,
                                        .original = ::dlsym(RTLD_DEFAULT, "getpid")};
    require(hooks.prepare(std::span(&spec, 1), error), error.c_str());
    hooks.setBeforeWriteGateForTesting(beforeWrite);
    ChangeInstall = true;
    require(!hooks.install(error), "install CAS observes an external prewrite");
    require(Imported() == -84, "install rollback preserves external rewrite");
    ChangeInstall = false;
    Imported = &::getpid;
    require(hooks.prepare(std::span(&spec, 1), error) && hooks.install(error), error.c_str());
    require(Imported() == -42, "eligible original is hooked");
    ChangeUninstall = true;
    require(hooks.uninstall(error), "uninstall CAS tolerates external rewrite");
    require(Imported() == -84, "uninstall preserves external rewrite");
    ChangeUninstall = false;
    Imported = &::getpid;
    require(hooks.rescan(error) && hooks.install(error), error.c_str());
    Imported = &external;
    require(hooks.rescan(error), error.c_str());
    require(Imported() == -84, "rescan skips a mismatched allocator domain");
    Imported = &::getpid;
    ChangeInstall = true;
    require(!hooks.rescan(error), "rescan CAS observes an external prewrite");
    require(Imported() == -84, "rescan CAS preserves external rewrite");
    ChangeInstall = false;
    require(hooks.uninstall(error), error.c_str());
    Imported = &::getpid;
    require(Imported() == real, "original import restored");
    void *a = ::dlopen(SPARK_GATEWAY_CONSUMER_A, RTLD_NOW | RTLD_LOCAL);
    void *b = ::dlopen(SPARK_GATEWAY_CONSUMER_B, RTLD_NOW | RTLD_LOCAL);
    require(a != nullptr && b != nullptr, "load allocator-domain consumers");
    auto malloc_a = reinterpret_cast<void *(*)(std::size_t)>(::dlsym(a, "consumer_malloc"));
    auto malloc_b = reinterpret_cast<void *(*)(std::size_t)>(::dlsym(b, "consumer_malloc"));
    auto free_a = reinterpret_cast<void (*)(void *)>(::dlsym(a, "consumer_free"));
    auto free_b = reinterpret_cast<void (*)(void *)>(::dlsym(b, "consumer_free"));
    OriginalMalloc = reinterpret_cast<decltype(OriginalMalloc)>(::dlsym(a, "provider_malloc"));
    OriginalFree = reinterpret_cast<decltype(OriginalFree)>(::dlsym(a, "provider_free"));
    require(malloc_a != nullptr && malloc_b != nullptr && free_a != nullptr && free_b != nullptr &&
                OriginalMalloc != nullptr && OriginalFree != nullptr,
            "resolve allocator domains");
    void *old_b = malloc_b(64);
    spark::ElfImportHooks domains;
    const std::array specs{spark::ElfImportHookSpec{.name = "provider_malloc",
                                                    .replacement = reinterpret_cast<void *>(&mallocHook),
                                                    .required = true,
                                                    .original = reinterpret_cast<void *>(OriginalMalloc)},
                           spark::ElfImportHookSpec{.name = "provider_free",
                                                    .replacement = reinterpret_cast<void *>(&freeHook),
                                                    .required = true,
                                                    .original = reinterpret_cast<void *>(OriginalFree)}};
    require(domains.prepare(specs, error) && domains.install(error), error.c_str());
    free_b(old_b);
    void *new_b = malloc_b(64);
    free_b(new_b);
    require(MallocCalls == 0 && FreeCalls == 0, "domain B allocation/free never crosses to A");
    void *new_a = malloc_a(64);
    free_a(new_a);
    require(MallocCalls == 1 && FreeCalls == 1, "domain A forwards through its exact original");
    require(domains.uninstall(error), error.c_str());
    ::dlclose(a);
    ::dlclose(b);
    std::puts("PASS: install/uninstall CAS and rescan external rewrite preservation");
}

#include <type_traits>

namespace spark::test {
namespace {

constexpr DWORD kEntryError = 0x13572468;
constexpr DWORD kAllocatorError = 0x24681357;
alignas(64) std::array<unsigned char, 256> storage{};

struct Observation {
    DWORD entry_error = 0;
    std::size_t calls = 0;
    std::size_t family = 0;
    std::array<std::uintptr_t, 5> arguments{};
    bool operator==(const Observation &) const = default;
};

Observation observed;
bool preserve_error = false;
bool succeed = false;
std::size_t checked_cases = 0;

template <typename T>
std::uintptr_t argumentWord(T value)
{
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<std::uintptr_t>(value);
    }
    else {
        return static_cast<std::uintptr_t>(value);
    }
}

template <std::size_t Family, typename Result, typename... Args>
Result __cdecl allocatorStub(Args... args)
{
    const DWORD entry = ::GetLastError();
    observed.entry_error = entry;
    ++observed.calls;
    observed.family = Family;
    observed.arguments = {argumentWord(args)...};
    if (!preserve_error) {
        ::SetLastError(kAllocatorError);
    }
    if constexpr (std::is_pointer_v<Result>) {
        return succeed ? storage.data() + 128 : nullptr;
    }
    else if constexpr (!std::is_void_v<Result>) {
        return succeed ? TRUE : FALSE;
    }
}

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
std::uintptr_t invoke(Function function)
{
    if constexpr (std::is_void_v<decltype(function())>) {
        function();
        return 0;
    }
    else {
        return argumentWord(function());
    }
}

template <typename Direct, typename Hooked>
void compareCalls(const char *name, Direct direct, Hooked hooked)
{
    observed = {};
    ::SetLastError(kEntryError);
    const auto expected_result = invoke(direct);
    const DWORD expected_error = ::GetLastError();
    const Observation expected = observed;
    observed = {};
    ::SetLastError(kEntryError);
    const auto actual_result = invoke(hooked);
    const DWORD actual_error = ::GetLastError();
    if (observed != expected || actual_result != expected_result || actual_error != expected_error ||
        observed.calls != 1 || observed.entry_error != kEntryError) {
        std::fprintf(stderr, "%s: entry=%lu expected=%lu final=%lu expected=%lu calls=%zu\n", name,
                     observed.entry_error, expected.entry_error, actual_error, expected_error, observed.calls);
        throw std::runtime_error("allocator handler differs from direct call");
    }
    ++checked_cases;
}

enum class Mode {
    FreshTls,
    CachedTls,
    Suppressed,
    Reentry,
    TrackingOff,
    CountOnly,
    CountReentry,
    NoTls,
    Tombstone
};

template <typename Impl>
void prepareMode(Impl &impl, Mode mode)
{
    impl.releaseThreadStateRegistry();
    impl.config.count_only = mode == Mode::CountOnly || mode == Mode::CountReentry;
    impl.tracking.store(mode != Mode::TrackingOff);
    Impl::mCountOnlyInsideHook = mode == Mode::CountReentry;
    if (mode == Mode::NoTls) {
        return;
    }
    impl.tls_index = ::TlsAlloc();
    require(impl.tls_index != TLS_OUT_OF_INDEXES, "TlsAlloc failed");
    if (mode == Mode::Tombstone) {
        require(::TlsSetValue(impl.tls_index, reinterpret_cast<void *>(std::uintptr_t{1})) != FALSE,
                "TlsSetValue failed");
    }
    else if (mode == Mode::CachedTls || mode == Mode::Suppressed || mode == Mode::Reentry) {
        auto *state = impl.currentThreadState();
        require(state != nullptr && state->registry_state.load() == 2 && state->thread_handle != nullptr,
                "production TLS registration failed");
        state->tracking_suppressed = mode == Mode::Suppressed;
        state->inside_hook = mode == Mode::Reentry;
    }
}

template <typename Impl>
void verifyMode(Impl &impl, Mode mode)
{
    if (mode == Mode::FreshTls || mode == Mode::CachedTls || mode == Mode::Suppressed || mode == Mode::Reentry) {
        auto *state = impl.currentThreadState();
        require(state != nullptr && state->registry_state.load() == 2 && state->thread_handle != nullptr,
                "production TLS path was bypassed");
        require(state->inside_hook == (mode == Mode::Reentry), "recursion guard ownership changed");
    }
    require(Impl::mCountOnlyInsideHook == (mode == Mode::CountReentry), "count-only recursion ownership changed");
    require(impl.hotCountersForCurrentThread().tracking_hook_calls.load() == 0, "tracking guard did not release");
}

template <typename Impl>
void runMatrix(Impl &impl)
{
    impl.real_malloc = &allocatorStub<0, void *, std::size_t>;
    impl.real_calloc = &allocatorStub<1, void *, std::size_t, std::size_t>;
    impl.real_realloc = &allocatorStub<2, void *, void *, std::size_t>;
    impl.real_recalloc = &allocatorStub<3, void *, void *, std::size_t, std::size_t>;
    impl.real_free = &allocatorStub<4, void, void *>;
    impl.real_aligned_malloc = &allocatorStub<5, void *, std::size_t, std::size_t>;
    impl.real_aligned_realloc = &allocatorStub<6, void *, void *, std::size_t, std::size_t>;
    impl.real_aligned_recalloc = &allocatorStub<7, void *, void *, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_offset_malloc = &allocatorStub<8, void *, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_offset_realloc = &allocatorStub<9, void *, void *, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_offset_recalloc =
        &allocatorStub<10, void *, void *, std::size_t, std::size_t, std::size_t, std::size_t>;
    impl.real_aligned_free = &allocatorStub<11, void, void *>;
    impl.real_malloc_base = &allocatorStub<12, void *, std::size_t>;
    impl.real_calloc_base = &allocatorStub<13, void *, std::size_t, std::size_t>;
    impl.real_realloc_base = &allocatorStub<14, void *, void *, std::size_t>;
    impl.real_free_base = &allocatorStub<15, void, void *>;
    impl.real_heap_alloc = &allocatorStub<16, void *, HANDLE, DWORD, SIZE_T>;
    impl.real_heap_realloc = &allocatorStub<17, void *, HANDLE, DWORD, void *, SIZE_T>;
    impl.real_heap_free = &allocatorStub<18, BOOL, HANDLE, DWORD, void *>;
    void *pointer = storage.data();
    HANDLE heap = storage.data() + 64;
    for (const auto mode : {Mode::FreshTls, Mode::CachedTls, Mode::Suppressed, Mode::Reentry, Mode::TrackingOff,
                            Mode::CountOnly, Mode::CountReentry, Mode::NoTls, Mode::Tombstone}) {
        for (const bool preserve : {false, true}) {
            preserve_error = preserve;
            for (const bool success : {false, true}) {
                succeed = success;
#define CHECK_ALLOCATOR(member, hook, ...)                                                                  \
    prepareMode(impl, mode);                                                                                \
    compareCalls(#hook, [&] { return impl.member(__VA_ARGS__); }, [&] { return Impl::hook(__VA_ARGS__); }); \
    verifyMode(impl, mode)
                CHECK_ALLOCATOR(real_malloc, hookMalloc, 17);
                CHECK_ALLOCATOR(real_calloc, hookCalloc, 3, 17);
                CHECK_ALLOCATOR(real_realloc, hookRealloc, pointer, 17);
                CHECK_ALLOCATOR(real_recalloc, hookRecalloc, pointer, 3, 17);
                CHECK_ALLOCATOR(real_free, hookFree, pointer);
                CHECK_ALLOCATOR(real_aligned_malloc, hookAlignedMalloc, 17, 64);
                CHECK_ALLOCATOR(real_aligned_realloc, hookAlignedRealloc, pointer, 17, 64);
                CHECK_ALLOCATOR(real_aligned_recalloc, hookAlignedRecalloc, pointer, 3, 17, 64);
                CHECK_ALLOCATOR(real_aligned_offset_malloc, hookAlignedOffsetMalloc, 17, 64, 5);
                CHECK_ALLOCATOR(real_aligned_offset_realloc, hookAlignedOffsetRealloc, pointer, 17, 64, 5);
                CHECK_ALLOCATOR(real_aligned_offset_recalloc, hookAlignedOffsetRecalloc, pointer, 3, 17, 64, 5);
                CHECK_ALLOCATOR(real_aligned_free, hookAlignedFree, pointer);
                CHECK_ALLOCATOR(real_malloc_base, hookMallocBase, 17);
                CHECK_ALLOCATOR(real_calloc_base, hookCallocBase, 3, 17);
                CHECK_ALLOCATOR(real_realloc_base, hookReallocBase, pointer, 17);
                CHECK_ALLOCATOR(real_free_base, hookFreeBase, pointer);
                CHECK_ALLOCATOR(real_heap_alloc, hookHeapAlloc, heap, 0x40, 17);
                CHECK_ALLOCATOR(real_heap_realloc, hookHeapReAlloc, heap, 0x40, pointer, 17);
                CHECK_ALLOCATOR(real_heap_free, hookHeapFree, heap, 0x40, pointer);
#undef CHECK_ALLOCATOR
            }
        }
    }
}

}  // namespace

std::size_t windowsAllocationLastErrorCaseCount() noexcept
{
    return checked_cases;
}

template <typename Impl>
void runWindowsAllocationLastErrorTests(Impl &impl)
{
    std::string error;
    require(impl.allocateEventPool(error), "event pool allocation failed");
    Impl::mActiveInstance.store(&impl);
    try {
        runMatrix(impl);
    }
    catch (...) {
        Impl::mActiveInstance.store(nullptr);
        Impl::mCountOnlyInsideHook = false;
        impl.releaseThreadStateRegistry();
        impl.freeEventPool();
        throw;
    }
    Impl::mActiveInstance.store(nullptr);
    Impl::mCountOnlyInsideHook = false;
    impl.tracking.store(false);
    impl.releaseThreadStateRegistry();
    impl.freeEventPool();
    impl.clearFunctionPointers();
    std::printf("Windows allocation LastError: %zu direct-call equivalence cases passed\n", checked_cases);
}

}  // namespace spark::test

from pathlib import Path

path = Path("src/native/alloc/allocation_sampler_linux.cpp")
text = path.read_text()

old = """    inline static thread_local bool mCountOnlyInsideHook = false;
    inline static thread_local std::uint8_t mHookShardAnchor = 0;

    static std::size_t currentHookShard() noexcept
    {
        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(&mHookShardAnchor);
        value ^= value >> 17;
        value *= 0x9e3779b97f4a7c15ULL;
        value ^= value >> 29;
        return static_cast<std::size_t>(value & (KHookCallShards - 1));
    }
"""
new = """    inline static thread_local bool mCountOnlyInsideHook = false;

    static std::size_t currentHookShard() noexcept
    {
        std::uintptr_t value = reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer());
        value ^= value >> 17;
        value *= 0x9e3779b97f4a7c15ULL;
        value ^= value >> 29;
        return static_cast<std::size_t>(value & (KHookCallShards - 1));
    }
"""
if text.count(old) != 1:
    raise SystemExit(f"thread-pointer shard marker count={text.count(old)}")
path.write_text(text.replace(old, new, 1))

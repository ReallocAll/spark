#ifndef ENDSTONE_SPARK_WINDOWS_IAT_HOOKS_H
#define ENDSTONE_SPARK_WINDOWS_IAT_HOOKS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spark {

enum class WindowsIatAccessStatus {
    Accessible,
    Stale,
    Error,
};

enum class WindowsIatExchangeStatus {
    Exchanged,
    Mismatch,
    Stale,
    Error,
};

struct WindowsIatModuleIdentity {
    std::uintptr_t base = 0;
    std::uint32_t image_size = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t checksum = 0;

    friend bool operator==(const WindowsIatModuleIdentity &, const WindowsIatModuleIdentity &) = default;
};

struct WindowsIatHookTarget {
    std::string import_name;
    void *original = nullptr;
    void *replacement = nullptr;
    bool required = false;
};

struct WindowsIatSlot {
    WindowsIatModuleIdentity module;
    std::uintptr_t slot_rva = 0;
    std::size_t target_index = 0;

    friend bool operator==(const WindowsIatSlot &, const WindowsIatSlot &) = default;
};

struct WindowsIatExchangeResult {
    WindowsIatExchangeStatus status = WindowsIatExchangeStatus::Error;
    void *observed = nullptr;
};

class WindowsIatHookBackend {
public:
    virtual ~WindowsIatHookBackend() = default;

    virtual bool enumerate(const std::vector<WindowsIatHookTarget> &targets, std::vector<WindowsIatSlot> &slots,
                           std::string &error) = 0;
    virtual WindowsIatAccessStatus read(const WindowsIatSlot &slot, void *&value, std::string &error) noexcept = 0;
    virtual WindowsIatExchangeResult compareExchange(const WindowsIatSlot &slot, void *expected, void *desired,
                                                      std::string &error) noexcept = 0;
};

class WindowsIatHooks {
public:
    explicit WindowsIatHooks(std::unique_ptr<WindowsIatHookBackend> backend);
    ~WindowsIatHooks();

    WindowsIatHooks(const WindowsIatHooks &) = delete;
    WindowsIatHooks &operator=(const WindowsIatHooks &) = delete;

    bool configure(std::vector<WindowsIatHookTarget> targets, std::string &error);
    bool install(std::string &error);
    bool refresh(std::string &error);
    bool uninstall(std::string &error) noexcept;

    [[nodiscard]] bool installed() const noexcept;
    [[nodiscard]] bool unsafeState() const noexcept;
    [[nodiscard]] std::size_t activeSlotCount() const noexcept;
    [[nodiscard]] const std::vector<WindowsIatSlot> &activeSlots() const noexcept;
    [[nodiscard]] const std::vector<WindowsIatHookTarget> &targets() const noexcept;

private:
    bool reconcile(bool initial_install, std::string &error);
    bool rollbackInstalled(std::string &error) noexcept;
    bool restoreSlot(const WindowsIatSlot &slot, std::string &error) noexcept;
    bool slotAlreadyTracked(const WindowsIatSlot &slot) const noexcept;
    bool requiredCoverageSatisfied(std::string &error) const;
    void markUnsafe(const std::string &reason, std::string &error) noexcept;

    std::unique_ptr<WindowsIatHookBackend> backend_;
    std::vector<WindowsIatHookTarget> targets_;
    std::vector<WindowsIatSlot> active_slots_;
    bool installed_ = false;
    bool unsafe_state_ = false;
};

#ifdef _WIN32
std::unique_ptr<WindowsIatHookBackend> makeNativeWindowsIatHookBackend(void *excluded_address = nullptr);
#endif

}  // namespace spark

#endif

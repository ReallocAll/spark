#ifndef ENDSTONE_SPARK_WINDOWS_ALLOCATION_IAT_HOOKS_H
#define ENDSTONE_SPARK_WINDOWS_ALLOCATION_IAT_HOOKS_H

#include <memory>
#include <string>

namespace spark {

class WindowsAllocationIatHooks {
public:
    WindowsAllocationIatHooks();
    ~WindowsAllocationIatHooks();

    WindowsAllocationIatHooks(const WindowsAllocationIatHooks &) = delete;
    WindowsAllocationIatHooks &operator=(const WindowsAllocationIatHooks &) = delete;

    bool addTarget(void *target, void *handler, std::string &error);
    bool install(std::string &error);
    bool refresh(std::string &error);
    bool uninstall(std::string &error) noexcept;

    [[nodiscard]] bool installed() const noexcept;
    [[nodiscard]] const std::string &lastError() const noexcept;

    static constexpr const char *backendId() noexcept { return "native-ucrt/permanent-iat"; }
    static constexpr const char *backendName() noexcept { return "Windows UCRT permanent IAT gateway"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spark

#endif

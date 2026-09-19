#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace spark {

class CommandSender;
class MainThreadDispatcher;
class ProfileMetadataProvider;
class ResultNotifier;

namespace levilamina {

// Opaque bridge keeping the application layer independent of LeviLamina SDK headers.
class ApplicationBridge final {
public:
    ApplicationBridge(std::filesystem::path data_dir, std::filesystem::path config_dir,
                      MainThreadDispatcher &dispatcher, ProfileMetadataProvider &metadata_provider,
                      ResultNotifier &notifier);
    ~ApplicationBridge();

    ApplicationBridge(const ApplicationBridge &) = delete;
    ApplicationBridge &operator=(const ApplicationBridge &) = delete;
    ApplicationBridge(ApplicationBridge &&) = delete;
    ApplicationBridge &operator=(ApplicationBridge &&) = delete;

    void enable();
    bool dispatch(CommandSender &sender, std::string_view raw_text);
    void onTick(double measured_ms, std::uint64_t main_tid);
    bool shutdown(std::string &error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace levilamina
}  // namespace spark

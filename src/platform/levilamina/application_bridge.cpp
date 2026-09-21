#include "platform/levilamina/application_bridge.h"

#include <exception>
#include <utility>

#include "application/command/command_sender.h"
#include "application/spark_application.h"
#include "core/command/arguments.h"
#include "core/config/spark_config.h"
#include "core/config/trusted_viewers.h"
#include "core/stats/executable_hash.h"
#include "net/profile_file.h"

namespace spark::levilamina {

struct ApplicationBridge::Impl final {
    Impl(std::filesystem::path data_dir, std::filesystem::path config_dir, MainThreadDispatcher &dispatcher,
         ProfileMetadataProvider &metadata_provider, ResultNotifier &notifier)
    {
        std::string hash_error;
        std::string bds_executable_sha256 = currentExecutableSha256(hash_error);
        (void)hash_error;

        SparkConfig config(config_dir / "config.toml");
        (void)config.loadOrCreate();

        TrustedViewersState trusted_viewers(data_dir / "trusted-viewers.json");
        (void)trusted_viewers.load();

        app = std::make_unique<SparkApplication>(
            std::move(bds_executable_sha256), profileStorageDirectory(data_dir), data_dir / "activity.json",
            std::move(config), std::move(trusted_viewers), dispatcher, metadata_provider, notifier);
    }

    std::unique_ptr<SparkApplication> app;
    bool shutdown_complete = false;
};

ApplicationBridge::ApplicationBridge(std::filesystem::path data_dir, std::filesystem::path config_dir,
                                     MainThreadDispatcher &dispatcher, ProfileMetadataProvider &metadata_provider,
                                     ResultNotifier &notifier)
    : impl_(std::make_unique<Impl>(std::move(data_dir), std::move(config_dir), dispatcher, metadata_provider, notifier))
{
}

ApplicationBridge::~ApplicationBridge()
{
    if (impl_ != nullptr && impl_->app != nullptr && !impl_->shutdown_complete) {
        std::terminate();
    }
}

void ApplicationBridge::enable()
{
    impl_->app->statistics().start();
    impl_->app->enable();
}

bool ApplicationBridge::dispatch(CommandSender &sender, std::string_view raw_text)
{
    const auto tokens = Arguments::tokenize(std::string(raw_text));
    return impl_->app->dispatchCommand(sender, tokens);
}

void ApplicationBridge::onTick(double measured_ms, std::uint64_t main_tid)
{
    impl_->app->setMainThreadId(main_tid);
    impl_->app->onTick(measured_ms);
}

bool ApplicationBridge::shutdown(std::string &error)
{
    if (impl_->shutdown_complete) {
        error.clear();
        return true;
    }

    if (!impl_->app->shutdown(error)) {
        return false;
    }
    if (!impl_->app->shutdownProfilerBackend(error)) {
        if (error.empty()) {
            error = "profiler backend shutdown failed";
        }
        return false;
    }

    impl_->shutdown_complete = true;
    return true;
}

}  // namespace spark::levilamina

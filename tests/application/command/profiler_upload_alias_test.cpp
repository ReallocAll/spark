#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>

#include <sys/syscall.h>
#endif

#include "application/command/command_sender.h"
#include "application/spark_application.h"
#include "core/config/spark_config.h"
#include "core/config/trusted_viewers.h"
#include "core/util/monotonic_time.h"

namespace {

std::uint64_t currentThreadId()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

class TestDispatcher final : public spark::MainThreadDispatcher {
public:
    void runOnMainThread(std::function<void()> task) override { task(); }
};

class TestMetadataProvider final : public spark::ProfileMetadataProvider {
public:
    void gatherServerMetadata(spark::ServerMetadata & /*metadata*/, std::int64_t /*now_ms*/) override {}
    void gatherWorldMetadata(spark::WorldInfo & /*world*/, std::string_view /*minecraft_version*/) override {}
    std::int64_t serverUptimeSeconds() override { return 0; }
    std::int64_t playerCount() override { return 0; }
    bool worldGaugesAvailable() override
    {
        ++world_gauge_availability_calls;
        return world_gauges_read ? world_gauges_available_after_read : world_gauges_available;
    }
    spark::WorldGaugeValues worldGauges() override
    {
        ++world_gauge_calls;
        world_gauges_read = true;
        return world_gauges;
    }
    spark::PlayerPingProvider *playerPingProvider() override { return nullptr; }

    bool world_gauges_available = true;
    bool world_gauges_available_after_read = true;
    bool world_gauges_read = false;
    int world_gauge_availability_calls = 0;
    int world_gauge_calls = 0;
    spark::WorldGaugeValues world_gauges{.entities = 7, .chunks = 11};
};

class TestNotifier final : public spark::ResultNotifier {
public:
    void notify(const std::string & /*sender_name*/, const std::string & /*text*/) override {}
};

class TestCommandSender final : public spark::CommandSender {
public:
    [[nodiscard]] std::string getName() const override { return "Console"; }
    [[nodiscard]] bool isPlayer() const override { return false; }
    std::vector<std::string> messages;
    std::vector<std::string> errors;

private:
    void sendImpl(const std::string &message) override { messages.push_back(message); }
    void errorImpl(const std::string &message) override { errors.push_back(message); }
};

// Returns the single message produced by dispatching tokens, or empty if
// the dispatch did not yield exactly one message.
std::string singleMessage(spark::SparkApplication &app, TestCommandSender &sender,
                          const std::vector<std::string> &tokens)
{
    sender.messages.clear();
    sender.errors.clear();
    app.dispatchCommand(sender, tokens);
    if (sender.messages.size() == 1) {
        return sender.messages[0];
    }
    return {};
}

void testUploadAliasesToStop()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_upload_alias_stop";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);
    TestCommandSender sender;

    // No active profiler: stop and upload must produce the identical message.
    const std::string stop_msg = singleMessage(app, sender, {"profiler", "stop"});
    const std::string upload_msg = singleMessage(app, sender, {"profiler", "upload"});
    assert(!stop_msg.empty());
    assert(stop_msg == upload_msg);
    assert(stop_msg == "There isn't an active profiler running.");

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testUploadAliasesToStop: PASS\n";
}

void testSamplerUploadAlias()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_upload_alias_sampler";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);
    TestCommandSender sender;

    const std::string stop_msg = singleMessage(app, sender, {"profiler", "stop"});
    const std::string sampler_upload_msg = singleMessage(app, sender, {"sampler", "upload"});
    assert(stop_msg == sampler_upload_msg);

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testSamplerUploadAlias: PASS\n";
}

void testUploadFlagSyntax()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_upload_alias_flag";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);
    TestCommandSender sender;

    // Legacy flag syntax: --upload and --stop behave like the stop sub-command.
    const std::string stop_msg = singleMessage(app, sender, {"profiler", "stop"});
    const std::string upload_flag_msg = singleMessage(app, sender, {"profiler", "--upload"});
    const std::string stop_flag_msg = singleMessage(app, sender, {"profiler", "--stop"});
    assert(stop_msg == upload_flag_msg);
    assert(stop_msg == stop_flag_msg);

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testUploadFlagSyntax: PASS\n";
}

void testUploadSaveToFileFlag()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_upload_alias_save";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);
    TestCommandSender sender;

    // --save-to-file passes through identically for both stop and upload.
    const std::string stop_save_msg = singleMessage(app, sender, {"profiler", "stop", "--save-to-file"});
    const std::string upload_save_msg = singleMessage(app, sender, {"profiler", "upload", "--save-to-file"});
    assert(stop_save_msg == upload_save_msg);

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testUploadSaveToFileFlag: PASS\n";
}

void testOtherCommandsUnaffected()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_upload_alias_regression";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);
    TestCommandSender sender;

    // cancel and open must still produce their own messages, not stop's.
    const std::string cancel_msg = singleMessage(app, sender, {"profiler", "cancel"});
    assert(cancel_msg == "There isn't an active profiler running.");
    const std::string uppercase_cancel_msg = singleMessage(app, sender, {"PrOfIlEr", "cancel"});
    assert(uppercase_cancel_msg == cancel_msg);

    // open without a running profiler reports its own message.
    sender.messages.clear();
    app.dispatchCommand(sender, {"profiler", "open"});
    assert(!sender.messages.empty());

    // info (default fallback) produces multi-line output, not the stop message.
    sender.messages.clear();
    app.dispatchCommand(sender, {"profiler", "info"});
    assert(!sender.messages.empty());
    assert(sender.messages[0] != "There isn't an active profiler running.");

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testOtherCommandsUnaffected: PASS\n";
}

void testMixedFormPrecedence()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_upload_alias_precedence";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);
    TestCommandSender sender;

    sender.messages.clear();
    app.dispatchCommand(sender, {"profiler", "info", "--upload"});
    assert(sender.messages.size() >= 2);
    assert(sender.messages[0] == "The profiler isn't running!");

    const std::string open_msg = singleMessage(app, sender, {"profiler", "open", "--stop"});
    assert(open_msg.find("Start it first") != std::string::npos);

    const std::string start_upload_msg = singleMessage(app, sender, {"profiler", "start", "--upload"});
    assert(start_upload_msg == "There isn't an active profiler running.");

    app.setMainThreadId(currentThreadId());
    sender.messages.clear();
    sender.errors.clear();
    app.dispatchCommand(sender, {"profiler", "start", "--interval", "10"});
    assert(sender.errors.empty());
    assert(!sender.messages.empty());
    sender.messages.clear();
    app.dispatchCommand(sender, {"profiler", "cancel", "--upload"});
    assert(sender.messages.size() == 1);
    assert(sender.messages[0].find("cancelled") != std::string::npos);

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testMixedFormPrecedence: PASS\n";
}

void testUnavailableWorldGaugesAreOmitted()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_unavailable_world_gauges";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    metadata.world_gauges_available = false;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);

    const std::int64_t steady_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const std::int64_t unix_now = spark::monotonicUnixMillis();
    app.statistics().startAt(steady_now - 1'001, unix_now - 1'001, spark::CpuSnapshot{});
    app.onTick(1.0);

    assert(metadata.world_gauge_calls == 0);
    const auto windows = app.statistics().profileWindows(unix_now - 1'001, unix_now + 1'000);
    assert(!windows.empty());
    for (const auto &[window, stats] : windows) {
        static_cast<void>(window);
        assert(!stats.entities_present);
        assert(!stats.chunks_present);
        assert(!stats.tile_entities_present);
    }

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testUnavailableWorldGaugesAreOmitted: PASS\n";
}

void testWorldGaugesUnavailableAfterReadAreOmitted()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_world_gauges_invalidated";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    metadata.world_gauges_available_after_read = false;
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);

    const std::int64_t steady_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const std::int64_t unix_now = spark::monotonicUnixMillis();
    app.statistics().startAt(steady_now - 1'001, unix_now - 1'001, spark::CpuSnapshot{});
    app.onTick(1.0);

    assert(metadata.world_gauge_availability_calls == 2);
    assert(metadata.world_gauge_calls == 1);
    const auto windows = app.statistics().profileWindows(unix_now - 1'001, unix_now + 1'000);
    assert(!windows.empty());
    for (const auto &[window, stats] : windows) {
        static_cast<void>(window);
        assert(!stats.entities_present);
        assert(!stats.chunks_present);
        assert(!stats.tile_entities_present);
    }

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testWorldGaugesUnavailableAfterReadAreOmitted: PASS\n";
}

void testAvailableZeroWorldGaugesAreRecorded()
{
    const auto root = std::filesystem::temp_directory_path() / "spark_world_gauges_zero";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    spark::SparkConfig config(root / "config.toml");
    config.background_profiler_enabled = false;
    spark::TrustedViewersState trusted(root / "trusted-viewers.json");
    TestDispatcher dispatcher;
    TestMetadataProvider metadata;
    metadata.world_gauges = {};
    TestNotifier notifier;
    spark::SparkApplication app({}, root, root / "activity.json", std::move(config), std::move(trusted), dispatcher,
                                metadata, notifier);

    const std::int64_t steady_now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const std::int64_t unix_now = spark::monotonicUnixMillis();
    app.statistics().startAt(steady_now - 1'001, unix_now - 1'001, spark::CpuSnapshot{});
    app.onTick(1.0);

    assert(metadata.world_gauge_availability_calls == 2);
    assert(metadata.world_gauge_calls == 1);
    const auto windows = app.statistics().profileWindows(unix_now - 1'001, unix_now + 1'000);
    assert(!windows.empty());
    for (const auto &[window, stats] : windows) {
        static_cast<void>(window);
        assert(stats.entities_present);
        assert(stats.entities == 0);
        assert(stats.chunks_present);
        assert(stats.chunks == 0);
        assert(!stats.tile_entities_present);
    }

    app.shutdown();
    std::filesystem::remove_all(root);
    std::cout << "testAvailableZeroWorldGaugesAreRecorded: PASS\n";
}

}  // namespace

int main()
{
    testUploadAliasesToStop();
    testSamplerUploadAlias();
    testUploadFlagSyntax();
    testUploadSaveToFileFlag();
    testOtherCommandsUnaffected();
    testMixedFormPrecedence();
    testUnavailableWorldGaugesAreOmitted();
    testWorldGaugesUnavailableAfterReadAreOmitted();
    testAvailableZeroWorldGaugesAreRecorded();
    std::cout << "All profiler upload alias tests passed.\n";
    return 0;
}

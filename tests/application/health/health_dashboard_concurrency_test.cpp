#include <atomic>
#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "health_dashboard_test_support.h"

using namespace spark::health_dashboard_test;  // NOLINT(google-build-using-namespace)

namespace {

void testFactoryShutdownRace()
{
    constexpr int k_iterations = 50;
    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        Probe probe;
        probe.configureFactory(true);
        auto dashboard = makeDashboard(probe);

        spark::HealthDashboard::OpenResult open_result;
        std::atomic<bool> open_returned{false};
        std::thread opener([&] {
            open_result = dashboard->open(dataAt(0), "Race");
            open_returned.store(true, std::memory_order_release);
        });
        assert(probe.waitFactoryEntered());

        std::atomic<bool> shutdown_started{false};
        std::atomic<bool> shutdown_returned{false};
        std::thread stopper([&] {
            shutdown_started.store(true, std::memory_order_release);
            dashboard->shutdown();
            shutdown_returned.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        while (!shutdown_started.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        assert(shutdown_started.load(std::memory_order_acquire));
        while (!shutdown_returned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        assert(!shutdown_returned.load(std::memory_order_acquire));

        probe.releaseFactory();
        opener.join();
        stopper.join();
        assert(open_returned.load(std::memory_order_acquire));
        assert(open_result.accepted);
        assert(!dashboard->isOpen());
        assert(!dashboard->openPending());
        assert(spark::HealthDashboardTestAccess::idle(*dashboard));
    }
}

void testCompletionCanShutdown()
{
    Probe probe;
    std::unique_ptr<spark::HealthDashboard> dashboard;
    std::atomic<bool> callback_returned{false};
    dashboard = makeDashboard(probe, [&](const spark::HealthDashboard::OpenResult &) {
        {
            std::scoped_lock lock(probe.mutex);
            probe.cv.notify_all();
        }
        dashboard->shutdown();
        callback_returned.store(true, std::memory_order_release);
        probe.cv.notify_all();
    });

    const auto queued = dashboard->open(dataAt(0), "Callback");
    assert(queued.accepted);
    assert(waitFor(probe, [&] { return callback_returned.load(std::memory_order_acquire); }));
    assert(!dashboard->isOpen());
    assert(!dashboard->openPending());
    assert(spark::HealthDashboardTestAccess::idle(*dashboard));
    dashboard->shutdown();
}

void testConcurrentShutdownAndCallbackShutdown()
{
    Probe probe;
    std::unique_ptr<spark::HealthDashboard> dashboard;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_returned = false;
    dashboard = makeDashboard(probe, [&](const spark::HealthDashboard::OpenResult &) {
        {
            std::unique_lock lock(probe.mutex);
            callback_entered = true;
            probe.cv.notify_all();
            probe.cv.wait(lock, [&] { return release_callback; });
        }
        dashboard->shutdown();
        {
            std::scoped_lock lock(probe.mutex);
            callback_returned = true;
            probe.cv.notify_all();
        }
    });

    assert(dashboard->open(dataAt(0), "Concurrent").accepted);
    assert(waitFor(probe, [&] { return callback_entered; }));
    std::atomic<bool> shutdown_returned{false};
    std::thread stopper([&] {
        dashboard->shutdown();
        shutdown_returned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!spark::HealthDashboardTestAccess::shutdownActive(*dashboard) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    assert(spark::HealthDashboardTestAccess::shutdownActive(*dashboard));
    {
        std::scoped_lock lock(probe.mutex);
        release_callback = true;
        probe.cv.notify_all();
    }
    stopper.join();
    assert(shutdown_returned.load(std::memory_order_acquire));
    assert(waitFor(probe, [&] { return callback_returned; }));
}

void testCallbackShutdownRejectsImmediateReopen()
{
    Probe probe;
    std::unique_ptr<spark::HealthDashboard> dashboard;
    std::optional<spark::HealthDashboard::OpenResult> callback_reopen;
    bool callback_shutdown_done = false;
    bool allow_callback_reopen = false;
    std::size_t callbacks = 0;
    dashboard = makeDashboard(probe, [&](const spark::HealthDashboard::OpenResult &) {
        std::size_t invocation = 0;
        {
            std::scoped_lock lock(probe.mutex);
            invocation = ++callbacks;
            probe.cv.notify_all();
        }
        if (invocation == 1) {
            dashboard->shutdown();
            {
                std::unique_lock lock(probe.mutex);
                callback_shutdown_done = true;
                probe.cv.notify_all();
                probe.cv.wait(lock, [&] { return allow_callback_reopen; });
            }
            auto result = dashboard->open(dataAt(0), "Immediate");
            std::scoped_lock lock(probe.mutex);
            callback_reopen = std::move(result);
            probe.cv.notify_all();
        }
    });

    assert(dashboard->open(dataAt(0), "Initial").accepted);
    assert(waitFor(probe, [&] { return callback_shutdown_done; }));

    spark::HealthDashboard::OpenResult external_reopen;
    std::thread external_opener([&] { external_reopen = dashboard->open(dataAt(0), "External"); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!spark::HealthDashboardTestAccess::shutdownActive(*dashboard) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    assert(spark::HealthDashboardTestAccess::shutdownActive(*dashboard));
    {
        std::scoped_lock lock(probe.mutex);
        allow_callback_reopen = true;
        probe.cv.notify_all();
    }
    assert(waitFor(probe, [&] { return callback_reopen.has_value(); }));
    external_opener.join();
    assert(callback_reopen.has_value());
    if (!callback_reopen) {
        return;
    }
    assert(!callback_reopen->accepted);
    assert(!callback_reopen->error.empty());
    assert(external_reopen.accepted);
    assert(waitFor(probe, [&] { return callbacks == 2; }));
    dashboard->shutdown();
}

}  // namespace

int main()
{
    testFactoryShutdownRace();
    testCompletionCanShutdown();
    testConcurrentShutdownAndCallbackShutdown();
    testCallbackShutdownRejectsImmediateReopen();
    return 0;
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "platform/levilamina/command_lifecycle.h"

namespace {

using spark::levilamina::CommandLifetimeGuard;

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void testFreshSessionAdmission()
{
    CommandLifetimeGuard guard;
    require(!guard.acquireForConstruction().has_value(), "unobserved construction was admitted");

    guard.observeServerThread(std::this_thread::get_id());
    auto lease = guard.acquireForConstruction();
    require(lease.has_value(), "observed construction was rejected");
    require(guard.activeCommands() == 1, "successful construction was not counted");
    require(!guard.beginCleanup(), "retained command did not block cleanup");
    require(lease->admitExecution(), "retained command did not remain executable");
    lease.reset();
    require(guard.activeCommands() == 0, "command destruction did not release its count");
    require(guard.beginCleanup(), "cleanup did not become admissible after release");
    require(guard.completeCleanup(), "cleanup did not complete");
    require(!guard.acquireForConstruction().has_value(), "closed session admitted a command");
}

void testThreadAdmissionAndDestructorFailure()
{
    CommandLifetimeGuard guard;
    guard.observeServerThread(std::this_thread::get_id());

    std::atomic_bool construction_rejected{false};
    std::thread wrong_constructor([&] { construction_rejected = !guard.acquireForConstruction().has_value(); });
    wrong_constructor.join();
    require(construction_rejected.load(), "wrong-thread construction was admitted");
    require(guard.activeCommands() == 0, "rejected construction changed command count");

    auto retained = guard.acquireForConstruction();
    require(retained.has_value(), "setup construction was rejected");
    std::thread wrong_destructor([lease = std::move(*retained)]() mutable { lease.reset(); });
    wrong_destructor.join();
    require(guard.activeCommands() == 1, "wrong-thread destruction decremented the count");
    require(guard.unsafeViolation(), "wrong-thread destruction was not recorded");
    require(!guard.beginCleanup(), "unsafe command lifetime admitted cleanup");
}

void testExecutionThreadMismatchAndReactivation()
{
    CommandLifetimeGuard first;
    first.observeServerThread(std::this_thread::get_id());
    auto lease = first.acquireForConstruction();
    require(lease.has_value(), "first session construction failed");

    std::atomic_bool rejected{false};
    std::thread wrong_executor([&] { rejected = !lease->admitExecution(); });
    wrong_executor.join();
    require(rejected.load(), "wrong-thread execution was admitted");
    require(first.unsafeViolation(), "wrong-thread execution was not recorded");
    lease.reset();
    require(!first.beginCleanup(), "unsafe session became clean after command release");

    CommandLifetimeGuard second;
    second.observeServerThread(std::this_thread::get_id());
    require(second.acquireForConstruction().has_value(), "fresh enable did not create an open session");
}

void testConstructionExceptionReleasesLease()
{
    CommandLifetimeGuard guard;
    guard.observeServerThread(std::this_thread::get_id());
    try {
        auto lease = guard.acquireForConstruction();
        require(lease.has_value(), "exception setup construction failed");
        throw std::runtime_error{"synthetic command construction failure"};
    }
    catch (std::runtime_error const &) {
    }
    require(guard.activeCommands() == 0, "construction exception leaked the command count");
    require(guard.beginCleanup(), "construction exception left cleanup blocked");
    require(guard.completeCleanup(), "cleanup after construction exception failed");
}

void testListenerSnapshotWait()
{
    auto listener = std::make_shared<int>(7);
    auto snapshot = listener;
    const auto short_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
    require(!spark::levilamina::waitForSoleSharedOwner(listener, short_deadline),
            "listener snapshot was not boundedly rejected");
    snapshot.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    require(spark::levilamina::waitForSoleSharedOwner(listener, deadline),
            "listener did not reach its sole owner after snapshot release");
}

}  // namespace

int main()
{
    try {
        testFreshSessionAdmission();
        testThreadAdmissionAndDestructorFailure();
        testExecutionThreadMismatchAndReactivation();
        testConstructionExceptionReleasesLease();
        testListenerSnapshotWait();
        return 0;
    }
    catch (...) {
        return 1;
    }
}

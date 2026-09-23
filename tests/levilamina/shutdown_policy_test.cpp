#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>

#include "ll/api/event/ListenerBase.h"
#include "ll/api/service/GamingStatus.h"
#include "platform/levilamina/cleanup_deadline_guard.h"
#include "platform/levilamina/command_lifecycle.h"

namespace spark::levilamina {
class CallbackState;
class StartupClock;
}  // namespace spark::levilamina

namespace ll::io {
class Logger;
}

namespace ll::mod {
class NativeMod;
}

namespace ll::thread {
class ServerThreadExecutor;
}

namespace {

struct HostSession;
struct CommandContext;
using CommandLifetimeGuard = spark::levilamina::CommandLifetimeGuard;
using PublicationBoundary = spark::levilamina::PublicationBoundary;

#include "platform/levilamina/spark_mod_decl.inc"

struct CallState final {
    ll::GamingStatus status = ll::GamingStatus::Default;
    bool close_result = true;
    bool require_server_thread = true;
    int gaming_status_calls = 0;
    int close_resources_calls = 0;
};

CallState call_state;

struct SparkModTestAccess final {
    static void setEnabled(SparkMod &mod, bool enabled) { mod.enabled_.store(enabled, std::memory_order_release); }
};

ll::GamingStatus SparkMod::gamingStatus()
{
    ++call_state.gaming_status_calls;
    return call_state.status;
}

bool SparkMod::closeResources(bool require_server_thread)
{
    ++call_state.close_resources_calls;
    call_state.require_server_thread = require_server_thread;
    return call_state.close_result;
}

#include "platform/levilamina/spark_mod_disable.inc"

void require(bool condition, char const *message)
{
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void resetCallState(ll::GamingStatus status, bool close_result)
{
    call_state.status = status;
    call_state.close_result = close_result;
    call_state.require_server_thread = true;
    call_state.gaming_status_calls = 0;
    call_state.close_resources_calls = 0;
}

void testDisabledInstanceSkipsDispatch()
{
    resetCallState(ll::GamingStatus::Stopping, false);
    SparkMod mod;
    SparkModTestAccess::setEnabled(mod, false);

    require(mod.disable(), "disabled SparkMod did not report success");
    require(call_state.gaming_status_calls == 0, "disabled SparkMod queried gaming status");
    require(call_state.close_resources_calls == 0, "disabled SparkMod called closeResources");
}

void testEnabledInstanceDispatchesStatusAndResult(ll::GamingStatus status, bool expected_require_server_thread,
                                                  bool close_result)
{
    resetCallState(status, close_result);
    SparkMod mod;
    SparkModTestAccess::setEnabled(mod, true);

    require(mod.disable() == close_result, "disable did not propagate closeResources result");
    require(call_state.gaming_status_calls == 1, "disable did not query gaming status exactly once");
    require(call_state.close_resources_calls == 1, "disable did not call closeResources exactly once");
    require(call_state.require_server_thread == expected_require_server_thread,
            "disable passed the wrong server-thread admission to closeResources");
}

void testDisableDispatchesStoppingAndStrictStatuses()
{
    struct Case final {
        ll::GamingStatus status;
        bool require_server_thread;
    };
    constexpr std::array cases{
        Case{ll::GamingStatus::Stopping, false},       Case{ll::GamingStatus::Starting, true},
        Case{ll::GamingStatus::Running, true},         Case{ll::GamingStatus::Default, true},
        Case{static_cast<ll::GamingStatus>(99), true},
    };
    for (auto const &test_case : cases) {
        testEnabledInstanceDispatchesStatusAndResult(test_case.status, test_case.require_server_thread, true);
        testEnabledInstanceDispatchesStatusAndResult(test_case.status, test_case.require_server_thread, false);
    }
}

}  // namespace

int main()
{
    try {
        testDisabledInstanceSkipsDispatch();
        testDisableDispatchesStoppingAndStrictStatuses();
        return 0;
    }
    catch (std::exception const &error) {
        std::fprintf(stderr, "shutdown policy: %s\n", error.what());
        return 1;
    }
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "native/sampler/thread_info.h"
#include "platform/endstone/python_attribution.h"

namespace {

using spark::PythonAttributionExport;
using spark::PythonCodeId;
using spark::PythonCodeMetadata;
using spark::PythonStackProvider;
using spark::endstone_adapter::EndstonePythonAttribution;

struct SnapshotRecord {
    std::uint64_t native_tid = 0;
    PythonStackProvider::Snapshot snapshot;
};

EndstonePythonAttribution *g_bridge = nullptr;
std::vector<SnapshotRecord> g_snapshots;
std::string g_late_attach_diagnostic;

void snapshotThunk() noexcept
{
    if (g_bridge == nullptr) {
        return;
    }
    SnapshotRecord record;
    record.native_tid = spark::currentNativeThreadId();
    if (g_bridge->snapshot(record.native_tid, record.snapshot)) {
        g_snapshots.push_back(record);
    }
}

void lateAttachThunk() noexcept
{
    if (g_bridge == nullptr) {
        return;
    }
    if (!g_bridge->start(g_late_attach_diagnostic)) {
        return;
    }
    snapshotThunk();
}

std::unordered_map<PythonCodeId, PythonCodeMetadata> codeMap(const PythonAttributionExport &state)
{
    std::unordered_map<PythonCodeId, PythonCodeMetadata> result;
    for (const auto &metadata : state.codes) {
        result.emplace(metadata.code_id, metadata);
    }
    return result;
}

std::vector<std::string> namesFor(const SnapshotRecord &record,
                                  const std::unordered_map<PythonCodeId, PythonCodeMetadata> &codes)
{
    std::vector<std::string> result;
    result.reserve(record.snapshot.depth);
    for (std::size_t i = 0; i < record.snapshot.depth; ++i) {
        const auto it = codes.find(record.snapshot.codes[i]);
        result.push_back(it == codes.end() ? "<unknown>" : it->second.qualname);
    }
    return result;
}

bool containsOrdered(const std::vector<std::string> &actual, const std::vector<std::string_view> &expected)
{
    std::size_t next = 0;
    for (const std::string &name : actual) {
        if (next < expected.size() && name == expected[next]) {
            ++next;
        }
    }
    return next == expected.size();
}

bool anySnapshotContains(const PythonAttributionExport &state, const std::vector<std::string_view> &expected)
{
    const auto codes = codeMap(state);
    return std::ranges::any_of(g_snapshots, [&](const SnapshotRecord &record) {
        return containsOrdered(namesFor(record, codes), expected);
    });
}

std::size_t countName(const SnapshotRecord &record, const std::unordered_map<PythonCodeId, PythonCodeMetadata> &codes,
                      std::string_view name)
{
    const auto names = namesFor(record, codes);
    return static_cast<std::size_t>(std::ranges::count(names, name));
}

bool expect(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "python attribution runtime test: " << message << '\n';
    }
    return condition;
}

#ifndef _WIN32

class PythonRuntime {
public:
    bool open(const char *path)
    {
        handle_ = ::dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (handle_ == nullptr) {
            std::cerr << "dlopen failed: " << ::dlerror() << '\n';
            return false;
        }
        initialize_ = reinterpret_cast<InitializeFn>(::dlsym(handle_, "Py_Initialize"));
        finalize_ = reinterpret_cast<FinalizeFn>(::dlsym(handle_, "Py_FinalizeEx"));
        run_ = reinterpret_cast<RunFn>(::dlsym(handle_, "PyRun_SimpleStringFlags"));
        return initialize_ != nullptr && finalize_ != nullptr && run_ != nullptr;
    }

    void initialize() const { initialize_(); }
    int run(const std::string &script) const { return run_(script.c_str(), nullptr); }
    int finalize() const { return finalize_(); }

    ~PythonRuntime()
    {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
        }
    }

private:
    using InitializeFn = void (*)();
    using FinalizeFn = int (*)();
    using RunFn = int (*)(const char *, void *);

    void *handle_ = nullptr;
    InitializeFn initialize_ = nullptr;
    FinalizeFn finalize_ = nullptr;
    RunFn run_ = nullptr;
};

#endif

bool verifyFallback(EndstonePythonAttribution &bridge)
{
    std::string diagnostic;
    if (!bridge.start(diagnostic)) {
        return expect(false, "3.11 start unexpectedly failed");
    }
    const PythonAttributionExport state = bridge.exportState();
    bool ok = true;
    ok &= expect(!state.diagnostics.supported, "3.11 must not report function attribution support");
    ok &= expect(!state.diagnostics.monitoring_active, "3.11 must not enable monitoring");
    ok &= expect(state.diagnostics.backend == "native-only", "3.11 backend must be native-only");
    ok &= expect(state.diagnostics.unavailable_reason.find("Python >= 3.12") != std::string::npos,
                 "3.11 fallback diagnostic must direct users to Python >= 3.12");
    bridge.stop();
    return ok;
}

bool verifyLifecycleWorkloads(PythonRuntime &runtime, EndstonePythonAttribution &bridge)
{
    g_bridge = &bridge;
    g_snapshots.clear();
    std::string diagnostic;
    if (!bridge.start(diagnostic)) {
        return expect(false, "PEP 669 bridge start failed");
    }

    const auto snapshot_address = reinterpret_cast<std::uintptr_t>(&snapshotThunk);
    std::string script = R"PY(
import asyncio
import ctypes
import os
import sys
import sysconfig
import threading
import types

_SNAP = ctypes.PYFUNCTYPE(None)(SNAPSHOT_ADDRESS)

def c():
    _SNAP()

def b():
    c()

def a():
    b()

a()


def recurse(depth):
    if depth == 0:
        _SNAP()
        return 1
    return 1 + recurse(depth - 1)

recurse(300)


def raises():
    raise RuntimeError('expected')

def catches_exception():
    try:
        raises()
    except RuntimeError:
        _SNAP()

catches_exception()


def generator_hotspot():
    _SNAP()
    incoming = yield 1
    _SNAP()
    yield incoming

_g = generator_hotspot()
next(_g)
_g.send(2)
try:
    _g.throw(ValueError('throw path'))
except (ValueError, StopIteration):
    pass


async def async_leaf():
    _SNAP()
    await asyncio.sleep(0)
    _SNAP()

async def async_hotspot():
    await async_leaf()

asyncio.run(async_hotspot())


_thread_done = threading.Event()
def worker_thread_hotspot():
    def worker_leaf():
        _SNAP()
    worker_leaf()
    _thread_done.set()

_t = threading.Thread(target=worker_thread_hotspot, name='spark-python-attribution-test')
_t.start()
_t.join()
assert _thread_done.is_set()


_plugin_root = os.path.join(os.getcwd(), 'plugins', '.local', 'lib', 'python-test', 'site-packages')
_plugin_file = os.path.join(_plugin_root, 'endstone_hotspot', 'main.py')
_plugin = types.ModuleType('endstone_spark_python_hotspot_test.main')
_plugin.__file__ = _plugin_file
sys.modules['endstone_spark_python_hotspot_test.main'] = _plugin
exec(compile('def plugin_handler():\n    fake_dep.external_leaf()\n', _plugin_file, 'exec'), _plugin.__dict__)

_purelib = sysconfig.get_paths().get('purelib') or sysconfig.get_paths().get('platlib')
_external_file = os.path.join(_purelib, 'spark_fake_dependency.py')
_external = types.ModuleType('spark_fake_dependency')
_external.__file__ = _external_file
_external.__dict__['_SNAP'] = _SNAP
exec(compile('def external_leaf():\n    _SNAP()\n', _external_file, 'exec'), _external.__dict__)
sys.modules['spark_fake_dependency'] = _external
_plugin.__dict__['fake_dep'] = _external
_plugin.plugin_handler()

import json
json.dumps({'spark': [1, 2, 3], 'nested': {'ok': True}})
)PY";
    const std::string marker = "SNAPSHOT_ADDRESS";
    script.replace(script.find(marker), marker.size(), std::to_string(snapshot_address));

    bool ok = true;
    ok &= expect(runtime.run(script) == 0, "real Python workload script failed");

    PythonStackProvider::Snapshot final_snapshot;
    ok &= expect(bridge.snapshot(spark::currentNativeThreadId(), final_snapshot), "final snapshot was inconsistent");
    ok &= expect(final_snapshot.depth == 0, "completed workloads left stale frames on main-thread shadow stack");

    const PythonAttributionExport state = bridge.exportState();
    ok &= expect(state.diagnostics.supported, "Python >=3.12 did not report support");
    ok &= expect(state.diagnostics.monitoring_active, "PEP 669 monitoring is not active");
    ok &= expect(state.diagnostics.backend == "PEP669", "unexpected Python attribution backend");
    ok &= expect(anySnapshotContains(state, {"a", "b", "c"}), "ordinary a -> b -> c stack was not observed");
    ok &= expect(anySnapshotContains(state, {"catches_exception"}), "exception/catch execution was not observed");
    ok &= expect(anySnapshotContains(state, {"generator_hotspot"}), "generator execution was not observed");
    ok &= expect(anySnapshotContains(state, {"async_hotspot", "async_leaf"}), "async execution stack was not observed");
    ok &= expect(anySnapshotContains(state, {"worker_thread_hotspot", "worker_leaf"}),
                 "worker Python thread stack was not observed");
    ok &= expect(anySnapshotContains(state, {"plugin_handler", "external_leaf"}),
                 "plugin -> external dependency stack was not observed");

    const auto codes = codeMap(state);
    const bool deep_recursion = std::ranges::any_of(g_snapshots, [&](const SnapshotRecord &record) {
        return record.snapshot.depth == PythonStackProvider::kMaxDepth && countName(record, codes, "recurse") >= 250;
    });
    ok &= expect(deep_recursion, "deep recursion did not reach bounded shadow-stack capacity");
    ok &= expect(state.diagnostics.max_depth > PythonStackProvider::kMaxDepth, "deep recursion max depth was not recorded");
    ok &= expect(state.diagnostics.overflows > 0, "deep recursion did not increment overflow diagnostics");
    ok &= expect(state.diagnostics.py_start > 0 && state.diagnostics.py_return > 0,
                 "PY_START/PY_RETURN lifecycle counters are empty");
    ok &= expect(state.diagnostics.py_yield > 0 && state.diagnostics.py_resume > 0,
                 "generator/async PY_YIELD/PY_RESUME counters are empty");
    ok &= expect(state.diagnostics.py_throw > 0, "generator throw() did not produce PY_THROW");
    ok &= expect(state.diagnostics.py_unwind > 0, "exception propagation did not produce PY_UNWIND");
    ok &= expect(state.diagnostics.registered_threads >= 2, "per-thread shadow state did not register worker thread");

    bool plugin_code = false;
    bool external_code = false;
    bool stdlib_code = false;
    for (const PythonCodeMetadata &metadata : state.codes) {
        if (metadata.qualname == "plugin_handler") {
            plugin_code = metadata.category == spark::PythonCodeCategory::Plugin && metadata.plugin_source == "spark-python-hotspot-test" &&
                          metadata.filename.find("plugins") != std::string::npos;
        }
        if (metadata.qualname == "external_leaf") {
            external_code = metadata.category == spark::PythonCodeCategory::External;
        }
        if (metadata.module.starts_with("json") && metadata.category == spark::PythonCodeCategory::Stdlib) {
            stdlib_code = true;
        }
    }
    ok &= expect(plugin_code, "plugin filename/module ownership metadata is incorrect");
    ok &= expect(external_code, "external dependency attribution is incorrect");
    ok &= expect(stdlib_code, "stdlib attribution is missing");
    ok &= expect(state.diagnostics.snapshot_failures == 0, "single-threaded lifecycle checks had snapshot failures");

    bridge.stop();
    g_bridge = nullptr;
    return ok;
}

bool verifyLateAttach(PythonRuntime &runtime)
{
    EndstonePythonAttribution bridge;
    g_bridge = &bridge;
    g_snapshots.clear();
    g_late_attach_diagnostic.clear();

    const auto attach_address = reinterpret_cast<std::uintptr_t>(&lateAttachThunk);
    std::string script = R"PY(
import ctypes
_ATTACH = ctypes.PYFUNCTYPE(None)(ATTACH_ADDRESS)

def late_c():
    _ATTACH()

def late_b():
    late_c()

def late_a():
    late_b()

late_a()
)PY";
    const std::string marker = "ATTACH_ADDRESS";
    script.replace(script.find(marker), marker.size(), std::to_string(attach_address));

    bool ok = true;
    ok &= expect(runtime.run(script) == 0, "late-attach Python workload failed");
    const PythonAttributionExport state = bridge.exportState();
    ok &= expect(state.diagnostics.monitoring_active, "late attach failed to enable PEP 669");
    ok &= expect(anySnapshotContains(state, {"late_a", "late_b", "late_c"}),
                 "late attach did not bootstrap the active Python execution chain");

    PythonStackProvider::Snapshot final_snapshot;
    ok &= expect(bridge.snapshot(spark::currentNativeThreadId(), final_snapshot), "late-attach final snapshot inconsistent");
    ok &= expect(final_snapshot.depth == 0, "late-attached frames did not unwind after returning to native caller");
    bridge.stop();
    g_bridge = nullptr;
    return ok;
}

}  // namespace

int main()
{
#ifdef _WIN32
    std::cerr << "This standalone runtime test currently targets Linux CI; Windows compilation is covered by the main build.\n";
    return 0;
#else
    const char *library_path = std::getenv("SPARK_TEST_LIBPYTHON");
    if (library_path == nullptr || library_path[0] == '\0') {
        std::cerr << "SPARK_TEST_LIBPYTHON is not set\n";
        return 2;
    }

    PythonRuntime runtime;
    if (!runtime.open(library_path)) {
        return 2;
    }
    runtime.initialize();

    EndstonePythonAttribution bridge;
    std::string diagnostic;
    if (!bridge.start(diagnostic)) {
        std::cerr << "bridge.start failed: " << diagnostic << '\n';
        runtime.finalize();
        return 1;
    }
    const PythonAttributionExport initial = bridge.exportState();
    bridge.stop();

    bool ok = true;
    if (!initial.diagnostics.supported) {
        ok &= verifyFallback(bridge);
    }
    else {
        ok &= verifyLifecycleWorkloads(runtime, bridge);
        ok &= verifyLateAttach(runtime);
    }

    const int finalize_result = runtime.finalize();
    ok &= expect(finalize_result == 0, "Py_FinalizeEx failed");
    return ok ? 0 : 1;
#endif
}

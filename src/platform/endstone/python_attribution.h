#ifndef ENDSTONE_SPARK_ENDSTONE_PYTHON_ATTRIBUTION_H
#define ENDSTONE_SPARK_ENDSTONE_PYTHON_ATTRIBUTION_H

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#include <windows.h>
// clang-format on
#else
#include <dlfcn.h>
#endif

#include "native/python/python_attribution.h"
#include "native/sampler/thread_info.h"

namespace spark::endstone_adapter {

// Runtime-only CPython integration. Spark deliberately does not link libpython:
// Endstone owns the embedded interpreter and this bridge resolves only public
// CPython API entry points from that already-loaded runtime. No frame/private ABI
// is accessed anywhere in Spark.
class EndstonePythonAttribution final : public PythonStackProvider {
public:
    EndstonePythonAttribution() = default;
    ~EndstonePythonAttribution() override { stop(); }

    EndstonePythonAttribution(const EndstonePythonAttribution &) = delete;
    EndstonePythonAttribution &operator=(const EndstonePythonAttribution &) = delete;

    bool start(std::string &diagnostic) noexcept
    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        diagnostic.clear();
        if (monitoring_active_.load(std::memory_order_acquire)) {
            diagnostic = "PEP 669 monitoring already active";
            return true;
        }

        resetSessionState();
        if (!resolvePythonApi()) {
            backend_ = "native-only";
            unavailable_reason_ = "embedded CPython public API is unavailable";
            diagnostic = unavailable_reason_;
            return true;  // Native profiling remains fully usable.
        }

        const char *version = api_.get_version != nullptr ? api_.get_version() : nullptr;
        python_version_ = version != nullptr ? version : "unknown";
        const auto [major, minor] = parseVersion(python_version_);
        if (major < 3 || (major == 3 && minor < 12)) {
            supported_.store(false, std::memory_order_release);
            backend_ = "native-only";
            unavailable_reason_ = "Python >= 3.12 / PEP 669 required for Python function attribution";
            diagnostic = unavailable_reason_;
            return true;
        }
        supported_.store(true, std::memory_order_release);
        backend_ = "PEP669";

        if (api_.is_initialized != nullptr && api_.is_initialized() == 0) {
            unavailable_reason_ = "embedded CPython interpreter is not initialized";
            diagnostic = unavailable_reason_;
            return true;
        }

        active_backend_.store(this, std::memory_order_release);
        const int gil_state = api_.gil_ensure();
        const std::string script = buildStartScript();
        const int result = api_.run_simple_string(script.c_str(), nullptr);
        api_.gil_release(gil_state);
        if (result != 0 || !monitoring_active_.load(std::memory_order_acquire)) {
            code_id_helper_ = nullptr;
            active_backend_.store(nullptr, std::memory_order_release);
            if (unavailable_reason_.empty()) {
                unavailable_reason_ = "failed to initialize sys.monitoring PEP 669 callbacks";
            }
            diagnostic = unavailable_reason_;
            return true;
        }

        diagnostic = "PEP 669 Python function attribution enabled for Python " + shortVersion(python_version_);
        return true;
    }

    void stop() noexcept
    {
        std::scoped_lock lifecycle_lock(lifecycle_mutex_);
        if (!monitoring_active_.load(std::memory_order_acquire)) {
            code_id_helper_ = nullptr;
            if (active_backend_.load(std::memory_order_acquire) == this) {
                active_backend_.store(nullptr, std::memory_order_release);
            }
            return;
        }
        if (api_.gil_ensure != nullptr && api_.gil_release != nullptr && api_.run_simple_string != nullptr &&
            (api_.is_initialized == nullptr || api_.is_initialized() != 0)) {
            const int gil_state = api_.gil_ensure();
            static constexpr char kStopScript[] = "import sys\n"
                                                  "_spark_m = sys.modules.get('_endstone_spark_monitor')\n"
                                                  "if _spark_m is None:\n"
                                                  "    raise RuntimeError('Spark PEP 669 monitor module disappeared before cleanup')\n"
                                                  "_spark_m.stop()\n"
                                                  "if _spark_m._tool_id is not None:\n"
                                                  "    raise RuntimeError('Spark PEP 669 cleanup left its tool id active')\n"
                                                  "sys.modules.pop('_endstone_spark_monitor', None)\n";
            const int result = api_.run_simple_string(kStopScript, nullptr);
            api_.gil_release(gil_state);
            if (result != 0 || monitoring_active_.load(std::memory_order_acquire)) {
                // Unloading Spark while sys.monitoring still owns ctypes callbacks
                // into this image would leave callable stale code pointers. Match
                // the profiler-backend unload contract and fail-stop instead.
                std::abort();
            }
            code_id_helper_ = nullptr;
        }
        monitoring_active_.store(false, std::memory_order_release);
        if (active_backend_.load(std::memory_order_acquire) == this) {
            active_backend_.store(nullptr, std::memory_order_release);
        }
    }

    bool active() const noexcept { return monitoring_active_.load(std::memory_order_acquire); }
    bool supported() const noexcept { return supported_.load(std::memory_order_acquire); }

    bool snapshot(std::uint64_t native_tid, Snapshot &out) noexcept override
    {
        return shadow_.snapshot(native_tid, out);
    }

    void recordSample(bool attributed, bool boundary_miss) noexcept override
    {
        if (attributed) {
            attribution_samples_.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            native_only_samples_.fetch_add(1, std::memory_order_relaxed);
        }
        if (boundary_miss) {
            boundary_misses_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void recordUnknownCodeId() noexcept override { unknown_export_code_ids_.fetch_add(1, std::memory_order_relaxed); }

    PythonAttributionExport exportState() const override
    {
        PythonAttributionExport out;
        {
            std::scoped_lock lock(registry_mutex_);
            out.codes = codes_;
            out.diagnostics.backend = backend_;
            out.diagnostics.python_version = python_version_;
            out.diagnostics.unavailable_reason = unavailable_reason_;
        }
        out.diagnostics.supported = supported_.load(std::memory_order_acquire);
        out.diagnostics.monitoring_active = monitoring_active_.load(std::memory_order_acquire);
        out.diagnostics.py_start = event_counts_[0].load(std::memory_order_relaxed);
        out.diagnostics.py_resume = event_counts_[1].load(std::memory_order_relaxed);
        out.diagnostics.py_throw = event_counts_[2].load(std::memory_order_relaxed);
        out.diagnostics.py_return = event_counts_[3].load(std::memory_order_relaxed);
        out.diagnostics.py_yield = event_counts_[4].load(std::memory_order_relaxed);
        out.diagnostics.py_unwind = event_counts_[5].load(std::memory_order_relaxed);
        out.diagnostics.registered_threads = shadow_.registeredThreads();
        out.diagnostics.max_depth = shadow_.maxDepth();
        out.diagnostics.overflows = shadow_.overflows();
        out.diagnostics.snapshot_attempts = shadow_.snapshotAttempts();
        out.diagnostics.snapshot_failures = shadow_.snapshotFailures();
        out.diagnostics.attribution_samples = attribution_samples_.load(std::memory_order_relaxed);
        out.diagnostics.native_only_samples = native_only_samples_.load(std::memory_order_relaxed);
        out.diagnostics.boundary_misses = boundary_misses_.load(std::memory_order_relaxed);
        out.diagnostics.thread_mismatches = shadow_.threadMismatches();
        out.diagnostics.unknown_code_ids =
            shadow_.unknownCodeIds() + unknown_export_code_ids_.load(std::memory_order_relaxed);
        out.diagnostics.code_objects = out.codes.size();
        out.diagnostics.plugin_code = category_counts_[0].load(std::memory_order_relaxed);
        out.diagnostics.stdlib_code = category_counts_[1].load(std::memory_order_relaxed);
        out.diagnostics.external_code = category_counts_[2].load(std::memory_order_relaxed);
        out.diagnostics.endstone_code = category_counts_[3].load(std::memory_order_relaxed);
        out.diagnostics.unknown_code = category_counts_[4].load(std::memory_order_relaxed);
        const std::uint64_t total_events = out.diagnostics.py_start + out.diagnostics.py_resume +
                                           out.diagnostics.py_throw + out.diagnostics.py_return +
                                           out.diagnostics.py_yield + out.diagnostics.py_unwind;
        const std::uint64_t misses = code_cache_misses_.load(std::memory_order_relaxed);
        out.diagnostics.code_cache_misses = misses;
        out.diagnostics.code_cache_hits = total_events > misses ? total_events - misses : 0;
        out.diagnostics.monitoring_callbacks_failed = callback_failures_.load(std::memory_order_relaxed);
        return out;
    }

private:
    using PyObject = void;
    using GetVersionFn = const char *(*)();
    using GilEnsureFn = int (*)();
    using GilReleaseFn = void (*)(int);
    using RunSimpleStringFn = int (*)(const char *, void *);
    using IsInitializedFn = int (*)();
    using ObjectCallOneArgFn = PyObject *(*)(PyObject *, PyObject *);
    using LongAsUnsignedLongLongFn = unsigned long long (*)(PyObject *);
    using DecRefFn = void (*)(PyObject *);
    using ErrClearFn = void (*)();

    struct PythonApi {
        GetVersionFn get_version = nullptr;
        GilEnsureFn gil_ensure = nullptr;
        GilReleaseFn gil_release = nullptr;
        RunSimpleStringFn run_simple_string = nullptr;
        IsInitializedFn is_initialized = nullptr;
        ObjectCallOneArgFn object_call_one_arg = nullptr;
        LongAsUnsignedLongLongFn long_as_unsigned_long_long = nullptr;
        DecRefFn dec_ref = nullptr;
        ErrClearFn err_clear = nullptr;
    };

    struct EventCodeCacheEntry {
        std::uintptr_t object = 0;
        PythonCodeId code_id = kInvalidPythonCodeId;
    };

    struct EventCodeCache {
        EndstonePythonAttribution *owner = nullptr;
        std::uint64_t epoch = 0;
        std::array<EventCodeCacheEntry, 512> entries{};
    };

    static constexpr std::size_t kCodeRegistryCapacity = 131072;

    static std::pair<int, int> parseVersion(std::string_view version) noexcept
    {
        int major = 0;
        int minor = 0;
        std::size_t pos = 0;
        while (pos < version.size() && version[pos] >= '0' && version[pos] <= '9') {
            major = major * 10 + (version[pos++] - '0');
        }
        if (pos < version.size() && version[pos] == '.') {
            ++pos;
        }
        while (pos < version.size() && version[pos] >= '0' && version[pos] <= '9') {
            minor = minor * 10 + (version[pos++] - '0');
        }
        return {major, minor};
    }

    static std::string shortVersion(std::string_view version)
    {
        const std::size_t end = version.find_first_of(" \t\r\n");
        return std::string(version.substr(0, end));
    }

    bool resolvePythonApi() noexcept
    {
        api_ = {};
#ifdef _WIN32
        HMODULE python = nullptr;
        constexpr std::array candidates{"python314.dll", "python313.dll", "python312.dll", "python311.dll"};
        for (const char *candidate : candidates) {
            python = GetModuleHandleA(candidate);
            if (python != nullptr) {
                break;
            }
        }
        if (python == nullptr) {
            return false;
        }
        const auto symbol = [python](const char *name) noexcept -> void * {
            return reinterpret_cast<void *>(GetProcAddress(python, name));
        };
#else
        const auto symbol = [](const char *name) noexcept -> void * {
            return dlsym(RTLD_DEFAULT, name);
        };
#endif
        api_.get_version = reinterpret_cast<GetVersionFn>(symbol("Py_GetVersion"));
        api_.gil_ensure = reinterpret_cast<GilEnsureFn>(symbol("PyGILState_Ensure"));
        api_.gil_release = reinterpret_cast<GilReleaseFn>(symbol("PyGILState_Release"));
        api_.run_simple_string = reinterpret_cast<RunSimpleStringFn>(symbol("PyRun_SimpleStringFlags"));
        api_.is_initialized = reinterpret_cast<IsInitializedFn>(symbol("Py_IsInitialized"));
        api_.object_call_one_arg = reinterpret_cast<ObjectCallOneArgFn>(symbol("PyObject_CallOneArg"));
        api_.long_as_unsigned_long_long =
            reinterpret_cast<LongAsUnsignedLongLongFn>(symbol("PyLong_AsUnsignedLongLong"));
        api_.dec_ref = reinterpret_cast<DecRefFn>(symbol("Py_DecRef"));
        api_.err_clear = reinterpret_cast<ErrClearFn>(symbol("PyErr_Clear"));
        return api_.get_version != nullptr && api_.gil_ensure != nullptr && api_.gil_release != nullptr &&
               api_.run_simple_string != nullptr && api_.object_call_one_arg != nullptr &&
               api_.long_as_unsigned_long_long != nullptr && api_.dec_ref != nullptr && api_.err_clear != nullptr;
    }

    void resetSessionState() noexcept
    {
        shadow_.resetSession();
        callback_epoch_.fetch_add(1, std::memory_order_acq_rel);
        code_id_helper_ = nullptr;
        supported_.store(false, std::memory_order_relaxed);
        monitoring_active_.store(false, std::memory_order_relaxed);
        attribution_samples_.store(0, std::memory_order_relaxed);
        native_only_samples_.store(0, std::memory_order_relaxed);
        boundary_misses_.store(0, std::memory_order_relaxed);
        unknown_export_code_ids_.store(0, std::memory_order_relaxed);
        callback_failures_.store(0, std::memory_order_relaxed);
        code_cache_misses_.store(0, std::memory_order_relaxed);
        for (auto &counter : event_counts_) {
            counter.store(0, std::memory_order_relaxed);
        }
        for (auto &counter : category_counts_) {
            counter.store(0, std::memory_order_relaxed);
        }
        std::scoped_lock lock(registry_mutex_);
        codes_.clear();
        backend_ = "none";
        python_version_.clear();
        unavailable_reason_.clear();
    }

    static std::string buildStartScript()
    {
        std::ostringstream script;
        script << "import sys, types\n";
        script << "_old = sys.modules.get('_endstone_spark_monitor')\n";
        script << "if _old is not None:\n    _old.stop()\n";
        script << "_m = types.ModuleType('_endstone_spark_monitor')\n";
        script << "_m.REGISTER_ADDR = " << reinterpret_cast<std::uintptr_t>(&registerThunk) << "\n";
        script << "_m.SET_CODE_HELPER_ADDR = " << reinterpret_cast<std::uintptr_t>(&setCodeIdHelperThunk) << "\n";
        script << "_m.PY_START_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyStartThunk) << "\n";
        script << "_m.PY_RESUME_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyResumeThunk) << "\n";
        script << "_m.PY_THROW_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyThrowThunk) << "\n";
        script << "_m.PY_RETURN_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyReturnThunk) << "\n";
        script << "_m.PY_YIELD_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyYieldThunk) << "\n";
        script << "_m.PY_UNWIND_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyUnwindThunk) << "\n";
        script << "_m.BOOT_RESET_ADDR = " << reinterpret_cast<std::uintptr_t>(&bootstrapResetThunk) << "\n";
        script << "_m.BOOT_PUSH_ADDR = " << reinterpret_cast<std::uintptr_t>(&bootstrapPushThunk) << "\n";
        script << "_m.STATUS_ADDR = " << reinterpret_cast<std::uintptr_t>(&statusThunk) << "\n";
        script << "_m.FAILURE_ADDR = " << reinterpret_cast<std::uintptr_t>(&callbackFailureThunk) << "\n";
        script << "sys.modules['_endstone_spark_monitor'] = _m\n";
        script << "exec(r'''" << helperSource() << "''', _m.__dict__)\n";
        script << "_m.start()\n";
        return script.str();
    }

    static std::string_view helperSource() noexcept
    {
        static constexpr std::string_view kSource = R"PY(
import ctypes
import importlib.metadata
import os
import sys
import sysconfig
import threading

_REGISTER = ctypes.PYFUNCTYPE(
    ctypes.c_uint64,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_char_p,
)(REGISTER_ADDR)
_SET_CODE_HELPER = ctypes.PYFUNCTYPE(None, ctypes.py_object)(SET_CODE_HELPER_ADDR)
_PY_START = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int)(PY_START_ADDR)
_PY_RESUME = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int)(PY_RESUME_ADDR)
_PY_THROW = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_THROW_ADDR)
_PY_RETURN = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_RETURN_ADDR)
_PY_YIELD = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_YIELD_ADDR)
_PY_UNWIND = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_UNWIND_ADDR)
_BOOT_RESET = ctypes.PYFUNCTYPE(None, ctypes.c_uint64)(BOOT_RESET_ADDR)
_BOOT_PUSH = ctypes.PYFUNCTYPE(None, ctypes.c_uint64, ctypes.c_uint64)(BOOT_PUSH_ADDR)
_STATUS = ctypes.PYFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)(STATUS_ADDR)
_FAILURE = ctypes.PYFUNCTYPE(None, ctypes.c_char_p)(FAILURE_ADDR)

_cache = {}
_module_files = {}
_tool_id = None
_plugin_root = os.path.normcase(os.path.abspath(os.path.join('plugins', '.local')))
_plugin_sources = {}
try:
    for _entry_point in importlib.metadata.entry_points(group='endstone'):
        _module = _entry_point.value.partition(':')[0].split('.', 1)[0]
        if _module:
            _plugin_sources.setdefault(_module, _entry_point.name)
except BaseException:
    pass
_paths = sysconfig.get_paths()
_stdlib_root = os.path.normcase(os.path.abspath(_paths.get('stdlib', ''))) if _paths.get('stdlib') else ''
_purelib_root = os.path.normcase(os.path.abspath(_paths.get('purelib', ''))) if _paths.get('purelib') else ''
_platlib_root = os.path.normcase(os.path.abspath(_paths.get('platlib', ''))) if _paths.get('platlib') else ''


def _norm(path):
    try:
        return os.path.normcase(os.path.abspath(path))
    except BaseException:
        return str(path)


def _under(path, root):
    if not root:
        return False
    try:
        return os.path.commonpath((path, root)) == root
    except (ValueError, OSError):
        return False


def _refresh_modules():
    for name, module in tuple(sys.modules.items()):
        filename = getattr(module, '__file__', None)
        if filename:
            _module_files.setdefault(_norm(filename), name)


def _module_for(filename):
    normalized = _norm(filename)
    module = _module_files.get(normalized)
    if module is None:
        _refresh_modules()
        module = _module_files.get(normalized)
    if module is None:
        stem = os.path.basename(normalized)
        module = stem.rsplit('.', 1)[0] if stem else '<unknown>'
    return normalized, module


def _category(filename, module):
    if _under(filename, _plugin_root):
        top = module.split('.', 1)[0]
        fallback = top[9:] if top.startswith('endstone_') else top
        source = _plugin_sources.get(top, fallback.replace('_', '-'))
        return 0, source
    if module == 'endstone' or module.startswith('endstone.'):
        return 3, ''
    if _under(filename, _purelib_root) or _under(filename, _platlib_root):
        return 2, ''
    if _under(filename, _stdlib_root):
        return 1, ''
    return 4, ''


def _code_id(code):
    key = id(code)
    cached = _cache.get(key)
    if cached is not None and cached[0] is code:
        return cached[1]
    filename, module = _module_for(code.co_filename)
    category, source = _category(filename, module)
    code_id = int(_REGISTER(
        filename.encode('utf-8', 'replace'),
        module.encode('utf-8', 'replace'),
        code.co_name.encode('utf-8', 'replace'),
        code.co_qualname.encode('utf-8', 'replace'),
        int(code.co_firstlineno),
        int(category),
        source.encode('utf-8', 'replace'),
    ))
    # Hold the code object strongly for the session so its address cannot be
    # recycled and a reloaded function can never inherit an older CodeId.
    _cache[key] = (code, code_id)
    return code_id


def _bootstrap_frame(native_id, frame):
    chain = []
    cursor = frame
    while cursor is not None:
        module_name = cursor.f_globals.get('__name__', '')
        code = cursor.f_code
        if module_name != '_endstone_spark_monitor' and not (
            code.co_name == '<module>' and code.co_filename == '<string>'
        ):
            chain.append(code)
        cursor = cursor.f_back
    _BOOT_RESET(int(native_id))
    for code in reversed(chain):
        _BOOT_PUSH(int(native_id), _code_id(code))
    return chain


def _bootstrap():
    _refresh_modules()
    native_by_ident = {
        thread.ident: thread.native_id
        for thread in threading.enumerate()
        if thread.ident is not None and getattr(thread, 'native_id', None) is not None
    }
    for ident, frame in sys._current_frames().items():
        native_id = native_by_ident.get(ident)
        if native_id is not None:
            _bootstrap_frame(native_id, frame)


def _monitoring_events(monitoring):
    events = monitoring.events
    return (
        events.PY_START,
        events.PY_RESUME,
        events.PY_THROW,
        events.PY_RETURN,
        events.PY_YIELD,
        events.PY_UNWIND,
    )


def _cleanup_monitoring(monitoring, tool_id):
    failures = []
    try:
        monitoring.set_events(tool_id, 0)
    except BaseException as exc:
        failures.append(exc)
    for event in _monitoring_events(monitoring):
        try:
            monitoring.register_callback(tool_id, event, None)
        except BaseException as exc:
            failures.append(exc)
    try:
        monitoring.free_tool_id(tool_id)
    except BaseException as exc:
        failures.append(exc)
    for exc in failures:
        _FAILURE(str(exc).encode('utf-8', 'replace'))
    return not failures


def start():
    global _tool_id
    monitoring = getattr(sys, 'monitoring', None)
    if monitoring is None:
        _STATUS(-1, b'sys.monitoring is unavailable')
        return
    for candidate in (monitoring.PROFILER_ID, 3, 4):
        try:
            if monitoring.get_tool(candidate) is None:
                monitoring.use_tool_id(candidate, 'endstone-spark')
                _tool_id = candidate
                break
        except (ValueError, RuntimeError):
            continue
    if _tool_id is None:
        _STATUS(-1, b'no free sys.monitoring tool id (tried 2, 3, 4)')
        return

    callbacks = tuple(zip(_monitoring_events(monitoring), (
        _PY_START,
        _PY_RESUME,
        _PY_THROW,
        _PY_RETURN,
        _PY_YIELD,
        _PY_UNWIND,
    )))
    try:
        _SET_CODE_HELPER(_code_id)
        for event, callback in callbacks:
            monitoring.register_callback(_tool_id, event, callback)
        _bootstrap()
        mask = 0
        for event, _callback in callbacks:
            mask |= event
        monitoring.set_events(_tool_id, mask)
    except BaseException as exc:
        cleanup_ok = _cleanup_monitoring(monitoring, _tool_id)
        if not cleanup_ok:
            # A partially registered ctypes callback points into the unloadable
            # Spark image. Continuing would make a later plugin unload unsafe.
            os.abort()
        _tool_id = None
        _STATUS(-1, str(exc).encode('utf-8', 'replace'))
        return
    _STATUS(1, f'PEP669 tool id {_tool_id}'.encode('ascii'))


def stop():
    global _tool_id
    if _tool_id is None:
        return
    monitoring = sys.monitoring
    if not _cleanup_monitoring(monitoring, _tool_id):
        # Never return control to the unload path with a callable old-DLL thunk
        # still owned by sys.monitoring.
        os.abort()
    _tool_id = None
    _cache.clear()
    _module_files.clear()
    _STATUS(0, b'stopped')
)PY";
        return kSource;
    }

    static EndstonePythonAttribution *activeBackend() noexcept
    {
        return active_backend_.load(std::memory_order_acquire);
    }

    static PythonCodeId registerThunk(const char *filename, const char *module, const char *function_name,
                                      const char *qualname, int first_line, int category,
                                      const char *plugin_source) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        return backend != nullptr
                 ? backend->registerCode(filename, module, function_name, qualname, first_line, category, plugin_source)
                 : kInvalidPythonCodeId;
    }

    static void setCodeIdHelperThunk(PyObject *helper) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        if (backend != nullptr) {
            backend->code_id_helper_ = helper;
        }
    }

    static void pyStartThunk(PyObject *code, int) noexcept { dispatchDirectEvent(PythonExecutionEvent::Start, code); }
    static void pyResumeThunk(PyObject *code, int) noexcept { dispatchDirectEvent(PythonExecutionEvent::Resume, code); }
    static void pyThrowThunk(PyObject *code, int, PyObject *) noexcept
    {
        dispatchDirectEvent(PythonExecutionEvent::Throw, code);
    }
    static void pyReturnThunk(PyObject *code, int, PyObject *) noexcept
    {
        dispatchDirectEvent(PythonExecutionEvent::Return, code);
    }
    static void pyYieldThunk(PyObject *code, int, PyObject *) noexcept
    {
        dispatchDirectEvent(PythonExecutionEvent::Yield, code);
    }
    static void pyUnwindThunk(PyObject *code, int, PyObject *) noexcept
    {
        dispatchDirectEvent(PythonExecutionEvent::Unwind, code);
    }

    static std::uint64_t callbackNativeThreadId() noexcept
    {
        thread_local const std::uint64_t native_tid = currentNativeThreadId();
        return native_tid;
    }

    static void dispatchDirectEvent(PythonExecutionEvent event, PyObject *code) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        if (backend == nullptr || code == nullptr) {
            return;
        }
        const PythonCodeId code_id = backend->codeIdForObject(code);
        if (code_id == kInvalidPythonCodeId) {
            backend->callback_failures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        backend->event_counts_[static_cast<std::size_t>(event)].fetch_add(1, std::memory_order_relaxed);
        backend->shadow_.onEvent(callbackNativeThreadId(), event, code_id);
    }

    PythonCodeId codeIdForObject(PyObject *code) noexcept
    {
        if (code == nullptr || code_id_helper_ == nullptr || api_.object_call_one_arg == nullptr ||
            api_.long_as_unsigned_long_long == nullptr || api_.dec_ref == nullptr || api_.err_clear == nullptr) {
            return kInvalidPythonCodeId;
        }

        thread_local EventCodeCache cache;
        const std::uint64_t epoch = callback_epoch_.load(std::memory_order_relaxed);
        if (cache.owner != this || cache.epoch != epoch) {
            cache.owner = this;
            cache.epoch = epoch;
            for (EventCodeCacheEntry &entry : cache.entries) {
                entry = {};
            }
        }

        const std::uintptr_t object = reinterpret_cast<std::uintptr_t>(code);
        const std::size_t index =
            static_cast<std::size_t>(((object >> 4) ^ (object >> 13) ^ (object >> 25)) & (cache.entries.size() - 1));
        EventCodeCacheEntry &entry = cache.entries[index];
        if (entry.object == object && entry.code_id != kInvalidPythonCodeId) {
            return entry.code_id;
        }

        code_cache_misses_.fetch_add(1, std::memory_order_relaxed);
        PyObject *result = api_.object_call_one_arg(code_id_helper_, code);
        if (result == nullptr) {
            api_.err_clear();
            return kInvalidPythonCodeId;
        }
        const unsigned long long raw_id = api_.long_as_unsigned_long_long(result);
        api_.dec_ref(result);
        if (raw_id == static_cast<unsigned long long>(-1) || raw_id == kInvalidPythonCodeId) {
            api_.err_clear();
            return kInvalidPythonCodeId;
        }
        entry.object = object;
        entry.code_id = static_cast<PythonCodeId>(raw_id);
        return entry.code_id;
    }

    static void bootstrapResetThunk(std::uint64_t native_tid) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        if (backend != nullptr) {
            backend->shadow_.bootstrap(native_tid, nullptr, 0);
        }
    }

    static void bootstrapPushThunk(std::uint64_t native_tid, PythonCodeId code_id) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        if (backend == nullptr || code_id == kInvalidPythonCodeId) {
            return;
        }
        PythonStackProvider::Snapshot snapshot;
        if (!backend->shadow_.snapshot(native_tid, snapshot)) {
            return;
        }
        if (snapshot.depth >= snapshot.codes.size()) {
            backend->shadow_.onEvent(native_tid, PythonExecutionEvent::Start, code_id);
            return;
        }
        snapshot.codes[snapshot.depth++] = code_id;
        backend->shadow_.bootstrap(native_tid, snapshot.codes.data(), snapshot.depth);
    }

    static void statusThunk(int status, const char *message) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        if (backend == nullptr) {
            return;
        }
        std::scoped_lock lock(backend->registry_mutex_);
        if (status > 0) {
            backend->monitoring_active_.store(true, std::memory_order_release);
            backend->unavailable_reason_.clear();
        }
        else if (status == 0) {
            backend->monitoring_active_.store(false, std::memory_order_release);
        }
        else {
            backend->monitoring_active_.store(false, std::memory_order_release);
            backend->unavailable_reason_ = message != nullptr ? message : "unknown PEP 669 initialization error";
        }
    }

    static void callbackFailureThunk(const char *message) noexcept
    {
        (void)message;
        EndstonePythonAttribution *backend = activeBackend();
        if (backend != nullptr) {
            // Failure reporting is an exceptional event callback path. Keep it
            // allocation-free and lock-free; exportState reports the atomic count.
            backend->callback_failures_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    PythonCodeId registerCode(const char *filename, const char *module, const char *function_name, const char *qualname,
                              int first_line, int category, const char *plugin_source) noexcept
    {
        try {
            std::scoped_lock lock(registry_mutex_);
            if (codes_.size() >= kCodeRegistryCapacity) {
                unknown_export_code_ids_.fetch_add(1, std::memory_order_relaxed);
                return kInvalidPythonCodeId;
            }
            PythonCodeMetadata metadata;
            metadata.code_id = static_cast<PythonCodeId>(codes_.size()) + 1;
            metadata.filename = filename != nullptr ? filename : "<unknown>";
            metadata.module = module != nullptr && module[0] != '\0' ? module : "<unknown>";
            metadata.function_name = function_name != nullptr ? function_name : "<unknown>";
            metadata.qualname = qualname != nullptr && qualname[0] != '\0' ? qualname : metadata.function_name;
            metadata.first_line = first_line >= 0 ? first_line : -1;
            if (category < static_cast<int>(PythonCodeCategory::Plugin) ||
                category > static_cast<int>(PythonCodeCategory::Unknown)) {
                metadata.category = PythonCodeCategory::Unknown;
            }
            else {
                metadata.category = static_cast<PythonCodeCategory>(category);
            }
            if (metadata.category == PythonCodeCategory::Plugin && plugin_source != nullptr) {
                metadata.plugin_source = plugin_source;
            }
            category_counts_[static_cast<std::size_t>(metadata.category)].fetch_add(1, std::memory_order_relaxed);
            codes_.push_back(std::move(metadata));
            return codes_.back().code_id;
        }
        catch (...) {
            callback_failures_.fetch_add(1, std::memory_order_relaxed);
            return kInvalidPythonCodeId;
        }
    }

    PythonApi api_;
    PythonShadowStack shadow_;
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex registry_mutex_;
    std::vector<PythonCodeMetadata> codes_;
    std::string backend_ = "none";
    std::string python_version_;
    std::string unavailable_reason_;
    PyObject *code_id_helper_ = nullptr;  // Borrowed from _endstone_spark_monitor while monitoring is active.
    std::atomic<std::uint64_t> callback_epoch_{1};
    std::atomic<bool> supported_{false};
    std::atomic<bool> monitoring_active_{false};
    std::array<std::atomic<std::uint64_t>, 6> event_counts_{};
    std::array<std::atomic<std::uint64_t>, 5> category_counts_{};
    std::atomic<std::uint64_t> attribution_samples_{0};
    std::atomic<std::uint64_t> native_only_samples_{0};
    std::atomic<std::uint64_t> boundary_misses_{0};
    std::atomic<std::uint64_t> unknown_export_code_ids_{0};
    std::atomic<std::uint64_t> callback_failures_{0};
    std::atomic<std::uint64_t> code_cache_misses_{0};

    inline static std::atomic<EndstonePythonAttribution *> active_backend_{nullptr};
};

}  // namespace spark::endstone_adapter

#endif  // ENDSTONE_SPARK_ENDSTONE_PYTHON_ATTRIBUTION_H

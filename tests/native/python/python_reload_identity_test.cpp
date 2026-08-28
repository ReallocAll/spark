#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "platform/endstone/python_attribution.h"

namespace {

using spark::PythonAttributionExport;
using spark::endstone_adapter::EndstonePythonAttribution;

bool expect(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "python reload identity test: " << message << '\n';
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
    int run(const char *script) const { return run_(script, nullptr); }
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

}  // namespace

int main()
{
#ifdef _WIN32
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
    bool ok = expect(bridge.start(diagnostic), "bridge start failed");
    const PythonAttributionExport initial = bridge.exportState();
    if (!initial.diagnostics.supported) {
        bridge.stop();
        ok &= expect(runtime.finalize() == 0, "Py_FinalizeEx failed for fallback runtime");
        return ok ? 0 : 1;
    }

    static constexpr char kScript[] = R"PY(
_source = 'def reload_identity_target():\n    return 7\n'
_first = {}
_second = {}
exec(compile(_source, 'plugins/.local/reload_identity.py', 'exec'), _first)
exec(compile(_source, 'plugins/.local/reload_identity.py', 'exec'), _second)
_first_code = _first['reload_identity_target'].__code__
_second_code = _second['reload_identity_target'].__code__
assert _first_code is not _second_code
assert _first_code == _second_code
assert hash(_first_code) == hash(_second_code)
assert _first['reload_identity_target']() == 7
assert _second['reload_identity_target']() == 7
)PY";

    ok &= expect(runtime.run(kScript) == 0, "reload identity Python workload failed");
    const PythonAttributionExport state = bridge.exportState();
    const auto reload_codes = static_cast<std::size_t>(std::ranges::count_if(state.codes, [](const auto &metadata) {
        return metadata.qualname == "reload_identity_target";
    }));
    ok &= expect(reload_codes == 2,
                 "structurally-equal code objects from separate loads reused one CodeId instead of two identities");

    bridge.stop();
    ok &= expect(runtime.finalize() == 0, "Py_FinalizeEx failed");
    return ok ? 0 : 1;
#endif
}

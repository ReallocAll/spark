#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "platform/endstone/python_attribution.h"
#include "proto/proto_reader.h"
#include "proto/sampler_data.h"

namespace {

class PythonRuntime {
public:
    bool open(const char *path)
    {
#ifdef _WIN32
        handle_ = LoadLibraryA(path);
        const auto symbol = [this](const char *name) {
            return GetProcAddress(handle_, name);
        };
#else
        handle_ = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        const auto symbol = [this](const char *name) {
            return dlsym(handle_, name);
        };
#endif
        if (handle_ == nullptr) {
            return false;
        }
        initialize_ = reinterpret_cast<void (*)()>(symbol("Py_Initialize"));
        finalize_ = reinterpret_cast<int (*)()>(symbol("Py_FinalizeEx"));
        run_ = reinterpret_cast<int (*)(const char *, void *)>(symbol("PyRun_SimpleStringFlags"));
        return initialize_ != nullptr && finalize_ != nullptr && run_ != nullptr;
    }

    void initialize() const { initialize_(); }
    [[nodiscard]] int finalize() const { return finalize_(); }
    [[nodiscard]] bool run(const std::string &script) const { return run_(script.c_str(), nullptr) == 0; }

    ~PythonRuntime()
    {
        if (handle_ != nullptr) {
#ifdef _WIN32
            FreeLibrary(handle_);
#else
            dlclose(handle_);
#endif
        }
    }

private:
#ifdef _WIN32
    HMODULE handle_ = nullptr;
#else
    void *handle_ = nullptr;
#endif
    void (*initialize_)() = nullptr;
    int (*finalize_)() = nullptr;
    int (*run_)(const char *, void *) = nullptr;
};

struct FilenameCase {
    std::string_view raw;
    std::string_view safe;
    std::string_view module;
};

constexpr std::array Filenames{
    FilenameCase{.raw = R"(C:\ownerSentinel\one\main.py)", .safe = "main.py", .module = "main"},
    FilenameCase{.raw = "/ownerSentinel/two/main.py", .safe = "main.py", .module = "main"},
    FilenameCase{.raw = R"(\\ownerSentinel\share\pkg\main.py)", .safe = "main.py", .module = "main"},
    FilenameCase{.raw = "ownerSentinel/relative/main.py", .safe = "main.py", .module = "main"},
    FilenameCase{
        .raw = "plugins/.local/ownerSentinel/package/main.py", .safe = "main.py", .module = "safe_package.main"},
    FilenameCase{.raw = "C:main.py", .safe = "main.py", .module = "main"},
    FilenameCase{.raw = "<string>", .safe = "<string>", .module = "<string>"},
    FilenameCase{.raw = "<stdin>", .safe = "<stdin>", .module = "<stdin>"},
    FilenameCase{.raw = "<unknown>", .safe = "<unknown>", .module = "<unknown>"},
    FilenameCase{
        .raw = "<frozen importlib._bootstrap>", .safe = "<frozen importlib._bootstrap>", .module = "<frozen importlib"},
    FilenameCase{.raw = "<ownerSentinel/private/path>", .safe = "<virtual>", .module = "<virtual>"},
    FilenameCase{.raw = "<frozen ownerSentinel/path>", .safe = "<virtual>", .module = "<virtual>"},
    FilenameCase{.raw = "<frozen invalid..module>", .safe = "<virtual>", .module = "<virtual>"},
    FilenameCase{.raw = "", .safe = "<unknown>", .module = "<unknown>"},
    FilenameCase{.raw = "ownerSentinel/directory/", .safe = "<unknown>", .module = "<unknown>"},
    FilenameCase{.raw = R"(C:\ownerSentinel\directory\)", .safe = "<unknown>", .module = "<unknown>"},
    FilenameCase{.raw = ".", .safe = "<unknown>", .module = "<unknown>"},
    FilenameCase{.raw = "..", .safe = "<unknown>", .module = "<unknown>"},
};

std::string registrationScript()
{
    std::string script = R"PY(
import sys, gc, weakref
_m = sys.modules['_endstone_spark_monitor']
sys.monitoring.set_events(_m._tool_id, 0)
import types, posixpath
_original_os = _m.os
_m.os = types.SimpleNamespace(path=posixpath)
try:
    for _foreign_path in ('C:\\ownerSentinel\\foreign\\fresh.py', 'C:fresh.py',
                          '\\\\ownerSentinel\\share\\fresh.py'):
        assert _m._module_for(_foreign_path)[1] == 'fresh'
finally:
    _m.os = _original_os
_filenames = [
)PY";
    for (const auto &item : Filenames) {
        script += spark::pythonJsonString(item.raw) + ",\n";
    }
    script += R"PY(]
_m._module_files[_m._norm(_filenames[4])] = 'safe_package.main'
_privacy_codes = []
for _filename in _filenames:
    _code = compile('def privacy_target():\n    return 7\n', _filename, 'exec').co_consts[0]
    assert _m._code_id(_code) != 0
    _privacy_codes.append(_code)
assert _privacy_codes[0] == _privacy_codes[1]
assert _privacy_codes[0] is not _privacy_codes[1]
assert _m._code_id(_privacy_codes[0]) != _m._code_id(_privacy_codes[1])
)PY";
    return script;
}

std::vector<std::string_view> messages(std::string_view bytes, int target)
{
    spark::ProtoReader reader(bytes);
    std::vector<std::string_view> result;
    int field = 0;
    int wire = 0;
    while (reader.nextField(field, wire)) {
        if (field == target) {
            assert(wire == 2);
            result.push_back(reader.readString());
        }
        else {
            reader.skip(wire);
        }
    }
    assert(reader.valid());
    return result;
}

void checkExport(spark::endstone_adapter::EndstonePythonAttribution &bridge)
{
    spark::setGlobalPythonStackProvider(&bridge);
    spark::ProfileMetadata metadata;
    spark::setGlobalPythonStackProvider(nullptr);
    metadata.mode = spark::ProfileMode::Allocation;
    std::array<spark::PythonCodeId, Filenames.size()> ids{};
    for (const auto &[id, code] : metadata.python_codes) {
        if (code.qualname != "privacy_target") {
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < Filenames.size(); ++i) {
            if (code.filename == Filenames[i].raw) {
                assert(ids[i] == 0);
                ids[i] = id;
                found = true;
                assert(code.first_line == 1);
                assert(code.module == Filenames[i].module);
                if (i == 4) {
                    assert(code.category == spark::PythonCodeCategory::Plugin);
                    assert(code.plugin_source == "safe-package");
                }
                break;
            }
        }
        assert(found);
    }
    spark::CallTree tree;
    tree.log({spark::pythonFrameKey(ids[1]), spark::pythonFrameKey(ids[0])}, 0, 7);
    tree.log({spark::pythonFrameKey(ids[0])}, 0, 3);
    for (std::size_t i = 2; i < ids.size(); ++i) {
        assert(ids[i] != 0);
        tree.log({spark::pythonFrameKey(ids[i])}, 0, i + 1);
    }
    assert(ids[0] != 0 && ids[1] != 0 && ids[0] != ids[1]);
    const std::string profile = spark::buildSamplerData(metadata, tree, {});
    for (const std::string_view forbidden : {"ownerSentinel", "C:", "plugins/.local", "directory/", "\\"}) {
        assert(profile.find(forbidden) == std::string::npos);
    }
    const auto threads = messages(profile, 2);
    assert(threads.size() == 1);
    const auto nodes = messages(threads.front(), 3);
    assert(nodes.size() == ids.size());
    std::map<std::string, std::size_t> by_descriptor;
    std::vector<std::vector<std::uint64_t>> children;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        spark::ProtoReader reader(nodes[index]);
        std::string class_name;
        std::string method;
        std::string descriptor;
        std::uint64_t line = 0;
        double weight = 0;
        children.emplace_back();
        int field = 0;
        int wire = 0;
        while (reader.nextField(field, wire)) {
            if (field == 3 || field == 4 || field == 7) {
                assert(wire == 2);
                const std::string value(reader.readString());
                if (field == 3) {
                    class_name = value;
                }
                else if (field == 4) {
                    method = value;
                }
                else {
                    descriptor = value;
                }
            }
            else if (field == 6) {
                assert(wire == 0);
                line = reader.readVarint();
            }
            else if (field == 8) {
                assert(wire == 2);
                const auto packed = reader.readString();
                assert(packed.size() == sizeof(weight));
                std::memcpy(&weight, packed.data(), sizeof(weight));
            }
            else if (field == 9) {
                assert(wire == 2);
                auto packed = reader.readMessage();
                while (!packed.eof()) {
                    children.back().push_back(packed.readVarint());
                }
                assert(packed.valid());
            }
            else {
                assert(false);
            }
        }
        assert(reader.valid() && line == 1 && method == "privacy_target");
        bool found = false;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::string expected = std::string(Filenames[i].safe) + " [CodeId " + std::to_string(ids[i]) + "]";
            if (descriptor == expected) {
                assert(class_name == "[Python] " + std::string(Filenames[i].module));
                assert(weight == (i == 0 ? 10.0 : i == 1 ? 7.0 : static_cast<double>(i + 1)));
                found = true;
                break;
            }
        }
        assert(found && by_descriptor.emplace(descriptor, index).second);
    }
    const auto first = by_descriptor.at("main.py [CodeId " + std::to_string(ids[0]) + "]");
    const auto second = by_descriptor.at("main.py [CodeId " + std::to_string(ids[1]) + "]");
    assert(children[first] == std::vector<std::uint64_t>{second});
    assert(children[second].empty());
    assert(profile == spark::buildSamplerData(metadata, tree, {}));
}

constexpr auto CapacityScript = R"PY(
_real_register = _m._REGISTER
_real_module_for = _m._module_for
_calls = [0, 0]
def _count_register(*args):
    _calls[0] += 1
    return _real_register(*args)
def _count_module(filename):
    _calls[1] += 1
    return _real_module_for(filename)
_m._REGISTER = _count_register
_m._module_for = _count_module
_valid = _privacy_codes[0]
_valid_id = _m._code_id(_valid)
_valid_ref = weakref.ref(_valid)
while True:
    _code = compile('pass', '<string>', 'exec')
    _ref = weakref.ref(_code)
    if _m._code_id(_code) == 0:
        break
assert _m._admission_closed
assert len(_m._cache) == 131072
assert all(value[1] != 0 for value in _m._cache.values())
del _code
gc.collect()
assert _ref() is None
_before = tuple(_calls)
for _index in range(1000):
    _code = compile('pass', 'ownerSentinel/rejected.py', 'exec')
    _ref = weakref.ref(_code)
    assert _m._code_id(_code) == 0
    del _code
    assert _ref() is None
assert tuple(_calls) == _before
assert len(_m._cache) == 131072
assert _m._code_id(_valid) == _valid_id
_m._BOOT_RESET(_m.threading.get_native_id())
_m._PY_START(_valid, 0)
del _privacy_codes, _valid
gc.collect()
assert _valid_ref() is not None
)PY";

}  // namespace

int main()
{
    const char *path = std::getenv("SPARK_TEST_LIBPYTHON");
    if (path == nullptr || path[0] == '\0') {
        std::cerr << "SPARK_TEST_LIBPYTHON must name a Python >= 3.12 runtime\n";
        return 2;
    }
    PythonRuntime runtime;
    if (!runtime.open(path)) {
        std::cerr << "could not load the requested Python runtime\n";
        return 2;
    }
    runtime.initialize();
    spark::endstone_adapter::EndstonePythonAttribution bridge;
    std::string diagnostic;
    assert(bridge.start(diagnostic));
    if (!bridge.active()) {
        std::cerr << diagnostic << '\n';
        bridge.stop();
        (void)runtime.finalize();
        return 2;
    }
    assert(runtime.run(registrationScript()));
    checkExport(bridge);
    std::cout << "Python filename protobuf and cross-platform module fallback passed\n";
    assert(runtime.run(CapacityScript));
    const auto saturated = bridge.exportState();
    assert(saturated.codes.size() == 131072);
    assert(saturated.diagnostics.unknown_code_ids == 1);
    spark::PythonStackProvider::Snapshot snapshot;
    assert(bridge.snapshot(spark::currentNativeThreadId(), snapshot));
    assert(snapshot.depth == 1);
    assert(saturated.codes.at(snapshot.codes[0] - 1).filename == Filenames[0].raw);
    bridge.stop();
    assert(!bridge.active());
    assert(runtime.run("gc.collect(); assert _valid_ref() is None; assert not _m._cache"));
    bridge.stop();
    assert(bridge.start(diagnostic) && bridge.active());
    assert(runtime.run(R"PY(
_m = sys.modules['_endstone_spark_monitor']
sys.monitoring.set_events(_m._tool_id, 0)
assert not _m._admission_closed
_code = compile('pass', '<string>', 'exec')
_new_id = _m._code_id(_code)
assert 0 < _new_id < 131072
assert _m._code_id(_code) == _new_id
_m._BOOT_RESET(_m.threading.get_native_id())
_m._PY_START(_code, 0)
_real_register = _m._REGISTER
_m._REGISTER = lambda *args: 0
_rejected = compile('pass', '<string>', 'exec')
_ref = weakref.ref(_rejected)
assert _m._code_id(_rejected) == 0
del _rejected
assert _ref() is None
_m._REGISTER = _real_register
assert _m._code_id(compile('pass', '<string>', 'exec')) == 0
assert _m._code_id(_code) == _new_id
)PY"));
    assert(bridge.snapshot(spark::currentNativeThreadId(), snapshot));
    assert(snapshot.depth == 1);
    const auto restarted = bridge.exportState();
    assert(restarted.codes.at(snapshot.codes[0] - 1).filename == "<string>");
    assert(restarted.codes.at(snapshot.codes[0] - 1).qualname == "<module>");
    bridge.stop();
    assert(runtime.finalize() == 0);
    std::cout << "Python native capacity, collection, cached identity, stop/restart and rejection passed\n";
}

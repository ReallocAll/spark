from pathlib import Path

source_path = Path("src/core/profiler/native_attribution.cpp")
test_path = Path("tests/core/profiler/native_plugin_attribution_test.cpp")
source = source_path.read_text(encoding="utf-8")
test = test_path.read_text(encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


old_observers = '''constexpr std::array KPythonAttributionObserverMethods{
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyStartThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyResumeThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyThrowThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyReturnThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyYieldThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyUnwindThunk"),
};
'''
new_observers = '''constexpr std::array KPythonAttributionObserverMethods{
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyStartThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyResumeThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyThrowThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyReturnThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyYieldThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyUnwindThunk"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyStartNativeCallback"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyResumeNativeCallback"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyThrowNativeCallback"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyReturnNativeCallback"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyYieldNativeCallback"),
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::pyUnwindNativeCallback"),
    // The tiny per-event wrappers may tail-call this shared helper. Treat the
    // helper itself as an observer boundary so optimized builds cannot expose
    // Spark's own monitoring branch when the wrapper frame is elided.
    std::string_view("spark::endstone_adapter::EndstonePythonAttribution::nativeEventCallback"),
};
'''
source = replace_once(source, old_observers, new_observers, "observer method table")

old_frames = '''    const spark::FrameKey observer_thunk = frame(5, 0x500000, 0x400);
    const spark::FrameKey user_ctypes_parent = frame(5, 0x500000, 0x500);
'''
new_frames = '''    const spark::FrameKey observer_thunk = frame(5, 0x500000, 0x400);
    const spark::FrameKey observer_native_wrapper = frame(5, 0x500000, 0x410);
    const spark::FrameKey observer_native_shared = frame(5, 0x500000, 0x420);
    const spark::FrameKey user_ctypes_parent = frame(5, 0x500000, 0x500);
'''
test = replace_once(test, old_frames, new_frames, "observer frame declarations")

old_resolved = '''    resolved.emplace(user_ctypes_parent,
                     spark::ResolvedFrame{.class_name = "plugin-a.dll", .method_name = "userCtypesCaller"});
'''
new_resolved = '''    resolved.emplace(
        observer_native_wrapper,
        spark::ResolvedFrame{
            .class_name = "spark",
            .method_name =
                "spark::endstone_adapter::EndstonePythonAttribution::pyStartNativeCallback(_object*, _object*)"});
    resolved.emplace(
        observer_native_shared,
        spark::ResolvedFrame{
            .class_name = "spark",
            .method_name =
                "spark::endstone_adapter::EndstonePythonAttribution::nativeEventCallback(spark::PythonExecutionEvent, _object*)"});
    resolved.emplace(user_ctypes_parent,
                     spark::ResolvedFrame{.class_name = "plugin-a.dll", .method_name = "userCtypesCaller"});
'''
test = replace_once(test, old_resolved, new_resolved, "native observer resolved frames")

old_logs = '''    observer_tree.log({observer_thunk, observer_ffi, observer_ctypes, observer_parent, root_frame}, 3, 7);
    observer_tree.log({user_native, user_ffi, user_ctypes, user_ctypes_parent, root_frame}, 3, 11);
'''
new_logs = '''    observer_tree.log({observer_thunk, observer_ffi, observer_ctypes, observer_parent, root_frame}, 3, 7);
    observer_tree.log({observer_native_wrapper, observer_parent, root_frame}, 3, 5);
    observer_tree.log({observer_native_shared, observer_parent, root_frame}, 3, 6);
    observer_tree.log({user_native, user_ffi, user_ctypes, user_ctypes_parent, root_frame}, 3, 11);
'''
test = replace_once(test, old_logs, new_logs, "native observer call-tree cases")

old_asserts = '''    assert(std::ranges::find(observer_keys, observer_thunk) == observer_keys.end());
    assert(std::ranges::find(observer_keys, observer_ffi) == observer_keys.end());
'''
new_asserts = '''    assert(std::ranges::find(observer_keys, observer_thunk) == observer_keys.end());
    assert(std::ranges::find(observer_keys, observer_native_wrapper) == observer_keys.end());
    assert(std::ranges::find(observer_keys, observer_native_shared) == observer_keys.end());
    assert(std::ranges::find(observer_keys, observer_ffi) == observer_keys.end());
'''
test = replace_once(test, old_asserts, new_asserts, "native observer filter assertions")

source_path.write_text(source, encoding="utf-8")
test_path.write_text(test, encoding="utf-8")

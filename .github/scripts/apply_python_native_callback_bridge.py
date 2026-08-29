from pathlib import Path

source_path = Path("src/platform/endstone/python_attribution.h")
test_path = Path("tests/native/python/python_attribution_runtime_test.cpp")
text = source_path.read_text(encoding="utf-8")
test = test_path.read_text(encoding="utf-8")


def replace_once(value: str, old: str, new: str, label: str) -> str:
    count = value.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected one match, found {count}")
    return value.replace(old, new, 1)


text = replace_once(
    text,
    "    using LongAsUnsignedLongLongFn = unsigned long long (*)(PyObject *);\n"
    "    using DecRefFn = void (*)(PyObject *);\n"
    "    using ErrClearFn = void (*)();\n\n"
    "    struct PythonApi {\n",
    "    using LongAsUnsignedLongLongFn = unsigned long long (*)(PyObject *);\n"
    "    using DecRefFn = void (*)(PyObject *);\n"
    "    using ErrClearFn = void (*)();\n"
    "    using PyCFunction = PyObject *(*)(PyObject *, PyObject *);\n\n"
    "    // PyMethodDef and METH_VARARGS are public Stable-ABI surface. Keep\n"
    "    // the public layout locally so Spark can still resolve libpython at runtime.\n"
    "    struct PyMethodDef {\n"
    "        const char *name;\n"
    "        PyCFunction method;\n"
    "        int flags;\n"
    "        const char *doc;\n"
    "    };\n\n"
    "    using CFunctionNewExFn = PyObject *(*)(PyMethodDef *, PyObject *, PyObject *);\n"
    "    using ImportAddModuleFn = PyObject *(*)(const char *);\n"
    "    using ObjectSetAttrStringFn = int (*)(PyObject *, const char *, PyObject *);\n"
    "    using TupleGetItemFn = PyObject *(*)(PyObject *, std::intptr_t);\n"
    "    using IncRefFn = void (*)(PyObject *);\n\n"
    "    struct PythonApi {\n",
    "public CPython callback API typedefs",
)

text = replace_once(
    text,
    "        ObjectCallOneArgFn object_call_one_arg = nullptr;\n"
    "        LongAsUnsignedLongLongFn long_as_unsigned_long_long = nullptr;\n"
    "        DecRefFn dec_ref = nullptr;\n"
    "        ErrClearFn err_clear = nullptr;\n",
    "        ObjectCallOneArgFn object_call_one_arg = nullptr;\n"
    "        LongAsUnsignedLongLongFn long_as_unsigned_long_long = nullptr;\n"
    "        CFunctionNewExFn cfunction_new_ex = nullptr;\n"
    "        ImportAddModuleFn import_add_module = nullptr;\n"
    "        ObjectSetAttrStringFn object_set_attr_string = nullptr;\n"
    "        TupleGetItemFn tuple_get_item = nullptr;\n"
    "        IncRefFn inc_ref = nullptr;\n"
    "        DecRefFn dec_ref = nullptr;\n"
    "        ErrClearFn err_clear = nullptr;\n",
    "PythonApi fields",
)

text = replace_once(
    text,
    "        api_.long_as_unsigned_long_long =\n"
    "            reinterpret_cast<LongAsUnsignedLongLongFn>(symbol(\"PyLong_AsUnsignedLongLong\"));\n"
    "        api_.dec_ref = reinterpret_cast<DecRefFn>(symbol(\"Py_DecRef\"));\n"
    "        api_.err_clear = reinterpret_cast<ErrClearFn>(symbol(\"PyErr_Clear\"));\n"
    "        return api_.get_version != nullptr && api_.gil_ensure != nullptr && api_.gil_release != nullptr &&\n"
    "               api_.run_simple_string != nullptr && api_.object_call_one_arg != nullptr &&\n"
    "               api_.long_as_unsigned_long_long != nullptr && api_.dec_ref != nullptr && api_.err_clear != nullptr;\n",
    "        api_.long_as_unsigned_long_long =\n"
    "            reinterpret_cast<LongAsUnsignedLongLongFn>(symbol(\"PyLong_AsUnsignedLongLong\"));\n"
    "        api_.cfunction_new_ex = reinterpret_cast<CFunctionNewExFn>(symbol(\"PyCFunction_NewEx\"));\n"
    "        api_.import_add_module = reinterpret_cast<ImportAddModuleFn>(symbol(\"PyImport_AddModule\"));\n"
    "        api_.object_set_attr_string = reinterpret_cast<ObjectSetAttrStringFn>(symbol(\"PyObject_SetAttrString\"));\n"
    "        api_.tuple_get_item = reinterpret_cast<TupleGetItemFn>(symbol(\"PyTuple_GetItem\"));\n"
    "        api_.inc_ref = reinterpret_cast<IncRefFn>(symbol(\"Py_IncRef\"));\n"
    "        api_.dec_ref = reinterpret_cast<DecRefFn>(symbol(\"Py_DecRef\"));\n"
    "        api_.err_clear = reinterpret_cast<ErrClearFn>(symbol(\"PyErr_Clear\"));\n"
    "        return api_.get_version != nullptr && api_.gil_ensure != nullptr && api_.gil_release != nullptr &&\n"
    "               api_.run_simple_string != nullptr && api_.object_call_one_arg != nullptr &&\n"
    "               api_.long_as_unsigned_long_long != nullptr && api_.cfunction_new_ex != nullptr &&\n"
    "               api_.import_add_module != nullptr && api_.object_set_attr_string != nullptr &&\n"
    "               api_.tuple_get_item != nullptr && api_.inc_ref != nullptr && api_.dec_ref != nullptr &&\n"
    "               api_.err_clear != nullptr;\n",
    "resolve Stable ABI callback symbols",
)

event_address_lines = [
    '        script << "_m.PY_START_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyStartThunk) << "\\n";\n',
    '        script << "_m.PY_RESUME_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyResumeThunk) << "\\n";\n',
    '        script << "_m.PY_THROW_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyThrowThunk) << "\\n";\n',
    '        script << "_m.PY_RETURN_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyReturnThunk) << "\\n";\n',
    '        script << "_m.PY_YIELD_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyYieldThunk) << "\\n";\n',
    '        script << "_m.PY_UNWIND_ADDR = " << reinterpret_cast<std::uintptr_t>(&pyUnwindThunk) << "\\n";\n',
]
text = replace_once(
    text,
    event_address_lines[0],
    '        script << "_m.INSTALL_NATIVE_CALLBACKS_ADDR = "\n'
    '               << reinterpret_cast<std::uintptr_t>(&installNativeCallbacksThunk) << "\\n";\n',
    "native callback installer address",
)
for line in event_address_lines[1:]:
    text = replace_once(text, line, "", f"remove event address {line.strip()}")

event_wrappers = [
    '_PY_START = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int)(PY_START_ADDR)\n',
    '_PY_RESUME = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int)(PY_RESUME_ADDR)\n',
    '_PY_THROW = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_THROW_ADDR)\n',
    '_PY_RETURN = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_RETURN_ADDR)\n',
    '_PY_YIELD = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_YIELD_ADDR)\n',
    '_PY_UNWIND = ctypes.PYFUNCTYPE(None, ctypes.py_object, ctypes.c_int, ctypes.py_object)(PY_UNWIND_ADDR)\n',
]
for line in event_wrappers:
    text = replace_once(text, line, "", f"remove ctypes event wrapper {line.split(' =', 1)[0]}")
text = replace_once(
    text,
    '_SET_CODE_HELPER = ctypes.PYFUNCTYPE(None, ctypes.py_object)(SET_CODE_HELPER_ADDR)\n',
    '_SET_CODE_HELPER = ctypes.PYFUNCTYPE(None, ctypes.py_object)(SET_CODE_HELPER_ADDR)\n'
    '_INSTALL_NATIVE_CALLBACKS = ctypes.PYFUNCTYPE(ctypes.c_int)(INSTALL_NATIVE_CALLBACKS_ADDR)\n',
    "startup installer wrapper",
)

text = replace_once(
    text,
    '    for candidate in (monitoring.PROFILER_ID, 3, 4):\n',
    "    if _INSTALL_NATIVE_CALLBACKS() != 0:\n"
    "        _STATUS(-1, b'failed to install native PEP 669 callbacks')\n"
    "        return\n"
    "    for candidate in (monitoring.PROFILER_ID, 3, 4):\n",
    "install native callbacks before monitoring tool acquisition",
)

marker = "    static void pyStartThunk(PyObject *code, int) noexcept { dispatchDirectEvent(PythonExecutionEvent::Start, code); }\n"
native_bridge = """    static int installNativeCallbacksThunk() noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        return backend != nullptr ? backend->installNativeCallbacks() : -1;
    }

    static PyObject *nativeEventCallback(PythonExecutionEvent event, PyObject *args) noexcept
    {
        EndstonePythonAttribution *backend = activeBackend();
        if (backend == nullptr || args == nullptr || backend->api_.tuple_get_item == nullptr ||
            backend->api_.inc_ref == nullptr) {
            return nullptr;
        }
        PyObject *code = backend->api_.tuple_get_item(args, 0);
        if (code == nullptr) {
            backend->callback_failures_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        dispatchDirectEvent(event, code);
        // sys.monitoring only treats the dedicated DISABLE sentinel specially.
        // A new reference to an already-provided argument avoids allocating a result.
        backend->api_.inc_ref(code);
        return code;
    }

    static PyObject *pyStartNativeCallback(PyObject *, PyObject *args) noexcept
    {
        return nativeEventCallback(PythonExecutionEvent::Start, args);
    }
    static PyObject *pyResumeNativeCallback(PyObject *, PyObject *args) noexcept
    {
        return nativeEventCallback(PythonExecutionEvent::Resume, args);
    }
    static PyObject *pyThrowNativeCallback(PyObject *, PyObject *args) noexcept
    {
        return nativeEventCallback(PythonExecutionEvent::Throw, args);
    }
    static PyObject *pyReturnNativeCallback(PyObject *, PyObject *args) noexcept
    {
        return nativeEventCallback(PythonExecutionEvent::Return, args);
    }
    static PyObject *pyYieldNativeCallback(PyObject *, PyObject *args) noexcept
    {
        return nativeEventCallback(PythonExecutionEvent::Yield, args);
    }
    static PyObject *pyUnwindNativeCallback(PyObject *, PyObject *args) noexcept
    {
        return nativeEventCallback(PythonExecutionEvent::Unwind, args);
    }

    int installNativeCallbacks() noexcept
    {
        if (api_.cfunction_new_ex == nullptr || api_.import_add_module == nullptr ||
            api_.object_set_attr_string == nullptr || api_.dec_ref == nullptr || api_.err_clear == nullptr) {
            return -1;
        }

        PyObject *module = api_.import_add_module("_endstone_spark_monitor");
        if (module == nullptr) {
            api_.err_clear();
            return -1;
        }

        static constexpr int kMethVarargs = 0x0001;
        static PyMethodDef methods[] = {
            {"_spark_py_start", &pyStartNativeCallback, kMethVarargs, nullptr},
            {"_spark_py_resume", &pyResumeNativeCallback, kMethVarargs, nullptr},
            {"_spark_py_throw", &pyThrowNativeCallback, kMethVarargs, nullptr},
            {"_spark_py_return", &pyReturnNativeCallback, kMethVarargs, nullptr},
            {"_spark_py_yield", &pyYieldNativeCallback, kMethVarargs, nullptr},
            {"_spark_py_unwind", &pyUnwindNativeCallback, kMethVarargs, nullptr},
        };
        static constexpr std::array<const char *, 6> names = {
            "_PY_START", "_PY_RESUME", "_PY_THROW", "_PY_RETURN", "_PY_YIELD", "_PY_UNWIND",
        };

        for (std::size_t i = 0; i < names.size(); ++i) {
            PyObject *callback = api_.cfunction_new_ex(&methods[i], nullptr, nullptr);
            if (callback == nullptr) {
                api_.err_clear();
                return -1;
            }
            const int result = api_.object_set_attr_string(module, names[i], callback);
            api_.dec_ref(callback);
            if (result != 0) {
                api_.err_clear();
                return -1;
            }
        }
        return 0;
    }

""" + marker
text = replace_once(text, marker, native_bridge, "native PyCFunction bridge")

test = replace_once(
    test,
    "import types\n",
    "import types\nimport _endstone_spark_monitor as _spark_monitor\n",
    "import native monitor module",
)
snap = "_SNAP = ctypes.PYFUNCTYPE(None)(SNAPSHOT_ADDRESS)\n"
test = replace_once(
    test,
    snap,
    "assert type(_spark_monitor._PY_START).__name__ == 'builtin_function_or_method'\n"
    "assert type(_spark_monitor._PY_RETURN).__name__ == 'builtin_function_or_method'\n"
    + snap,
    "verify native callback objects",
)

source_path.write_text(text, encoding="utf-8")
test_path.write_text(test, encoding="utf-8")

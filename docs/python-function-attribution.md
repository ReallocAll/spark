# Python function attribution

Spark can enrich its existing statistical CPU samples with the currently executing Python call chain when Endstone embeds CPython 3.12 or newer.

## Runtime model

The implementation deliberately separates Python execution tracking from CPU accounting:

1. PEP 669 `sys.monitoring` lifecycle callbacks maintain a per-native-thread shadow stack of monotonic `PythonCodeId` values.
2. Code-object metadata is resolved on the cold registration path. Sampling never dereferences Python frames or objects.
3. The native statistical sampler captures its normal native stack first, then attempts a bounded seqlock snapshot of the matching Python shadow stack.
4. A concurrent/inconsistent Python snapshot is dropped without dropping the native CPU sample.
5. Export converts Python code IDs to explicit `[Python] <module>` pseudo frames with qualname, source filename and first line while preserving the existing Spark protobuf/viewer schema.

The sampler does not acquire the GIL, call the Python C API, take a normal mutex, perform filesystem work, or symbolize Python objects.

The shadow stack is bounded to 256 visible frames per Python thread. Deeper execution remains safe: overflow depth is counted and diagnostics expose the maximum observed depth and truncation events. A process-wide fixed table supports up to 256 concurrently registered native Python threads per profiling session.

## Lifecycle events

Production monitoring enables only the PEP 669 events needed to describe Python execution-frame lifetime:

- enter/re-enter: `PY_START`, `PY_RESUME`, `PY_THROW`
- leave/suspend: `PY_RETURN`, `PY_YIELD`, `PY_UNWIND`

`LINE`, `INSTRUCTION` and the legacy `PyEval_SetProfile` API are not used.

At profiler start, currently executing Python stacks are bootstrapped once from public runtime APIs so late attach does not begin from an empty stack. Python threads created after monitoring starts perform the same one-time bootstrap on their first lifecycle callback; subsequent events stay on the bounded CodeId push/pop path.

Monitoring is enabled only for an active CPU profiler session. Export freezes the attribution state before monitoring is disabled, so code metadata remains valid through serialization.

## Python version policy

- Python 3.12, 3.13 and 3.14: PEP 669 function attribution is supported.
- Python 3.11: Spark keeps the existing native profiler and reports Python function attribution as unavailable with a diagnostic requiring Python 3.12 or newer.

Spark does not inspect `_PyInterpreterFrame`, use private `PyThreadState` layout, replace the eval frame (PEP 523), patch CPython, or add a second 3.11 profiling state machine.

## Attribution metadata

Each registered code object records:

- source filename
- module
- function name and qualified name
- `co_firstlineno`
- category: Endstone plugin, Endstone framework/binding, stdlib, external dependency, or unknown
- Endstone plugin source/entry-point name when applicable

The full Python caller chain is retained when execution enters stdlib or a dependency, so a plugin remains visible as the originating caller.

## Diagnostics

Export metadata includes backend/version/support state, event counts, registered threads, maximum and overflow depth, snapshot attempts/failures, attributed/native-only samples, native/Python boundary misses, thread mismatches, unknown IDs, code-category counts, cache hits/misses and monitoring callback failures.

These counters are intended both for production diagnosis and for the real-BDS validation/performance harness in `ReallocAll/bds-test-lab`.

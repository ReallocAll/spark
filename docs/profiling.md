# Profiling details

Spark samples native stacks at a bounded interval, aggregates them off the
capture path, and exports the result in spark's existing protobuf format. A
profile can be uploaded to bytebin or saved as a raw `.sparkprofile` file.
Execution weights represent elapsed sampled microseconds. Allocation weights
represent sampled requested bytes.

## Reading a profile

The spark viewer's call tree and flame graph place callers above callees.
`Total` is inclusive sampled time or bytes for a frame and its children;
`Self` is attributed to that frame. Percentages are shares of the selected
thread or root. They do not certify that a guessed symbol name is exact.

Native frames can appear as:

```text
bedrock_server.Level::_subTick()                        resolved symbol
bedrock_server.0x116d77e (str?: Level - tick redstone)() tentative runtime guess
bedrock_server.0x123456 (vtable?: Level::<virtual>)()   tentative runtime guess
bedrock_server.0x654321()                               unresolved RVA
```

Resolved PDB or dynamic symbols take priority and replace the RVA. A runtime
guess keeps the RVA and identifies its evidence: `rtti` is a verified runtime
type, `vtable` is a class and virtual-table slot, `str` is a referenced semantic
string, and `thunk` is a verified jump wrapper. A `?` means the evidence is
useful but cannot identify an exact member. Conflicting or unsafe evidence is
omitted. Guesses are deterministic and are made at export time, away from the
sampling path.

The metadata pages can include the BDS executable SHA-256 and version, loaded
plugins where the host exposes them, profiler options, rolling TPS/MSPT/CPU
windows, and sampling, queue, unwind, or allocation-hook drops. The executable
hash lets an analyst select a matching binary without uploading the binary
contents. `--not-combined` keeps separate complete trees for selected threads
even when they have the same name.

LeviLamina exports its host and game version, native-mod metadata, player count,
uptime, aggregate ping, common statistics, behavior-pack metadata, and world
metadata for the three vanilla dimensions. World entries include regions and
loaded chunks, with entity-type counts in world metadata. World gauges report
entity and loaded-chunk totals; actor and player IDs are used internally to
deduplicate and reconcile observations. Chunk discard callbacks and snapshot
reconciliation remove stale observations, while scans prune expired dimension
references. Tile/block-entity counts and gamerules are not exposed by the LL
adapter. Endstone provides its corresponding host-specific fields through its
adapter.

## Execution sampling

Linux uses a dedicated sampler thread and `SIGPROF`; the handler captures a raw
trace through cpptrace's async-signal-safe path. Dynamic symbols are resolved
with `dladdr`. Unresolved frames in the stripped BDS executable retain
`module+0xRVA` and may receive conservative guesses from ELF unwind metadata,
Itanium RTTI, vtables, and decoded instructions.

Windows suspends one selected target thread per interval, retains its current
instruction address, and walks callers with `StackWalk64`. Frames can resolve
against the shipped PDB. If caller unwinding fails, the partial sample is kept
instead of being discarded. After a suspension, the sampler retries
`ResumeThread` up to 32 times. If a live target still cannot be restored, the
process is terminated rather than leaving BDS suspended.

Samples enter a bounded queue and are aggregated on a background thread.
Symbolization, call-tree construction, compression, and network I/O stay out of
the server tick and capture paths. Multi-thread execution profiles treat the
interval as a global stack-walk budget and rotate fairly through matching
threads. Weights use the measured elapsed time between captures and exclude the
target thread's own suspension.

## Native allocation profiling

`--alloc` samples successful native allocation requests on Linux x86-64 and
Windows x64. Linux redirects supported ELF allocator imports. Windows redirects
supported UCRT and heap import slots through Spark-owned process-lifetime
Permanent-IAT gateways. Hooks enqueue bounded records; symbolization and
aggregation happen later.

Each covered thread has an independent randomized byte-sampling phase and a
non-reused session identity. Without `--thread`, allocation profiling covers all
covered process threads. Exact-name and regular-expression selectors use the same
case-insensitive full-name matching rules as execution profiles. A safe
aggregation stage applies those selectors, so hook callbacks do not allocate,
construct strings, evaluate regular expressions, or query thread names.

`--alloc-live-only` follows sampled allocations through realloc and free calls,
including releases from another thread, and exports only allocations still live
at export time. It is useful for retained-memory and leak investigation; repeat
profiles are needed to distinguish growth from legitimate long-lived state.

Coverage is limited to the supported allocator entry points and imports. Static
CRT copies, private or inlined allocators, arenas and object pools that bypass a
covered entry point, direct virtual-memory calls, and memory mappings are not
sampled. A Linux module loaded and unloaded between rescans can also escape
coverage.

The hook path never blocks allocator threads. Fixed queues, live records, thread
roots, module entries, and call-tree nodes have bounded capacity. Export metadata
reports capacities, high-water marks, overflow merging, drops, hook coverage,
and whether the profile is incomplete. Sample and byte totals describe records
accepted after the configured filters; process-wide hook and lifecycle counters
are labelled separately.

Linux gateway groups are permanent for the server process and are capped at 256
groups over that process's lifetime. Published groups are not reused after
retirement, so exhausting the cap or encountering incompatible resident gateway
code requires a server restart. Windows gateways remain safe after plugin
shutdown by falling through to the original allocator once Spark closes handler
admission and drains admitted callbacks.

## Statistics, live viewing, and recovery

The statistics service continuously retains up to 15 minutes of completed ticks
and one-second process/system CPU observations in fixed-capacity histories. The
`tps` and `health` commands, profile metadata, and live-viewer windows read from
the same service.

`/spark profiler open` connects the active execution or allocation profile to a
spark WebSocket relay. Initial sampler data and later rolling windows are
uploaded asynchronously; standalone statistics are sent every 10 seconds. A
normal allocation live viewer is cumulative from session start. An
`--alloc-live-only` viewer shows sampled allocations still retained at each
update. The viewer remains live until the profile stops, is cancelled, or times
out.

Viewer clients authenticate with RSA2048-SHA256 public-key signatures. A client
whose key is not in `trusted-viewers.json` is held pending until an operator
approves its displayed ID with `trust-viewer`.

During an active profile, a segmented recovery journal records module, thread,
sample, and tick records below `profiles/recovery/`. CRC32-validated records and
durable segment writes allow a truncated tail to be replayed after an unclean
shutdown. On the next startup, a supported unclean session is replayed and saved
as a `.sparkprofile`; clean sessions are discarded. Allocation live-only
sessions are not recovered because the journal does not contain their full
free/realloc lifecycle state. A failed save leaves the journal for another
attempt.

A watchdog records stall-begin and stall-end events when the main-thread
heartbeat stops for more than five seconds. It does not call host APIs or stop a
profiler. Shutdown uses bounded quiescence checks for health, export, viewer,
and native work; the host bootstrap aborts before unloading when quiescence is
not proven.

## Python attribution on Endstone

On Endstone with CPython 3.12 or newer, execution profiles can include the
currently executing Python plugin call chain. Python 3.11 remains native-only.
The sampler never acquires the GIL or dereferences Python objects: public PEP
669 callbacks maintain bounded per-thread shadow stacks, and export maps stable
code IDs to Python pseudo-frames. See [Python function attribution](python-function-attribution.md)
for the runtime, fallback, and diagnostic details.

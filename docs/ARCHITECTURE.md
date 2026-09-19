# Architecture

spark for Bedrock is a native statistical profiler for Bedrock Dedicated Server
(BDS). The shared application samples native execution and allocation stacks,
aggregates them, exports spark-compatible profiles, and maintains rolling
statistics independently of a profile session. Host adapters connect that core
to Endstone or to the experimental LeviLamina module.

## Source tree

```text
src/
  application/                # host-independent orchestration and commands
    activity/                 #   activity log command
    command/                  #   command registry and sender interface
    health/                   #   TPS, ping, health, and dashboard commands
    placeholder/              #   Endstone PlaceholderAPI formatting
    profiler/                 #   profiler service, export, and live viewer
    tick_monitor/             #   long-tick monitor
    spark_application.h/.cpp  #   central application container
    platform_capabilities.h   #   dispatcher, metadata, and notifier interfaces
  core/                       # host-independent services
    activity/                 #   bounded activity log
    command/                  #   argument and flag parsing
    config/                   #   TOML config and trusted viewers
    metadata/                 #   safe server.properties and behavior-pack data
    profiler/                 #   profile orchestration and grouping
    recovery/                 #   journal, replay, and stall watchdog
    stats/                    #   rolling metrics, network, ping, and system data
    ws/                       #   crypto, WebSocket protocol, and live viewer
  native/                     # sampler, symbol guesser, allocation hooks, Python bridge
  platform/
    endstone/                 # Endstone adapters and optional PlaceholderAPI integration
    levilamina/               # experimental Windows x64 LeviLamina adapter/module
  proto/                      # spark protobuf serialization
  net/                        # gzip, bytebin, WebSocket transport, profile files
  plugin.cpp                  # Endstone lifecycle and bootstrap
  spark_constants.h           # project version
```

`src/platform/levilamina/` contains the native module entry point in
`spark_mod.cpp`, the host-independent application bridge in
`application_bridge.cpp`, and typed host adapters in `adapters.cpp`. It is
compiled only when `SPARK_BUILD_LEVILAMINA=ON`. That target is experimental,
source-build-only Windows x64 support for BDS 1.26.20.x and LeviLamina 26.20.7;
it does not imply parity with the Endstone adapter.

## Layering

The CMake targets enforce dependency direction:

```text
spark_profiling_time  <- monotonic clocks and profiling-window alignment
spark_native          <- sampler, symbol guesser, allocation hooks
spark_core            <- config, profiler, statistics, recovery, proto, network
spark_application     <- commands, services, export, host capability interfaces
spark_papi_integration <- optional Endstone PlaceholderAPI adapter
spark                 <- Endstone adapters and src/plugin.cpp
spark_levilamina      <- LeviLamina module objects, bridge, and host SDK inputs
```

The shared path is `platform adapter -> application -> core -> native ->
profiling_time`. `spark_application`, `spark_core`, and `spark_native` do not
include host SDK headers. Endstone's API is confined to `src/platform/endstone/`
and `src/plugin.cpp`; LeviLamina's SDK is confined to `src/platform/levilamina/`
and its isolated CMake target.

## Application and host boundaries

`SparkApplication` owns the profiler service, health service, activity log, tick
monitor, statistics service, recovery journal, and trusted viewer state. It
receives three host capabilities:

- `MainThreadDispatcher` posts work back to the host's server thread.
- `ProfileMetadataProvider` supplies version, player, resource, and host metadata.
- `ResultNotifier` delivers command and background-operation results.

The Endstone bootstrap in `src/plugin.cpp` constructs those adapters, starts the
application, schedules tick forwarding, and registers the optional PAPI
expansion. The LeviLamina bootstrap in `src/platform/levilamina/spark_mod.cpp`
waits for `ServerStartedEvent`, creates the bridge with LeviLamina data/config
directories, forwards tick events, and registers the raw `/spark` command at
`GameDirectors` permission.

LeviLamina's typed metadata adapter currently supplies game/loader versions,
player count, uptime, and no plugin or world records. `worldGaugesAvailable()`
and `playerPingProvider()` are deliberately false/null. Endstone's adapter has
additional public APIs for fields that are available on that host; the shared
application does not assume those fields exist.

## Execution sampler

The sampler captures selected native thread stacks at a bounded interval. Linux
uses `SIGPROF` with cpptrace's safe raw-trace path. Windows suspends a target,
walks it with `StackWalk64`, and retries `ResumeThread` up to 32 times. If a live
target cannot be restored, the process is terminated before it can remain
suspended. Captures enter bounded queues and are aggregated on a background
thread.

## Allocation profiler

Allocation sampling redirects supported allocator imports on Linux x86-64 and
Windows x64. Hook callbacks are reentrancy-safe, bounded, and free of blocking,
symbolization, and unbounded allocation. Linux gateway code is process-lifetime
code; Windows uses Spark-owned Permanent-IAT gateways. Export reports hook
coverage, queue pressure, lifecycle drops, and incomplete data explicitly.

## Symbolization

Resolved platform symbols take priority. Unresolved frames in the BDS main
executable may receive deterministic runtime guesses from unwind metadata, RTTI,
vtables, thunks, and decoded string references. Guesses retain their RVA and
identify evidence strength. The running executable is the product-time source;
debug databases and IDA data are not runtime dependencies.

## Statistics and recovery

The statistics service keeps bounded rolling TPS, MSPT, CPU, player-count, ping,
network, and host-gauge histories independently of profiling. Commands, profile
metadata, health reports, and live viewer windows read the same snapshots.

`RecoveryWriter` journals module, thread, sample, and tick records through a
bounded queue. `StallWatchdog` records main-thread stall begin/end events without
calling host APIs or stopping a profile. On startup, `RecoveryPlayer` can replay
an unclean supported session into a local profile.

## Live viewer and shutdown

`ViewerSocket` manages the WebSocket relay, while export workers perform gzip and
bytebin uploads away from the server tick. RSA2048-SHA256 signatures and
`TrustedViewersState` authenticate viewer clients.

Sampling, health, export, viewer, and native backend work use bounded shutdown
waits. If quiescence is not proven before the deadline, the host bootstrap stops
before unloading the module. A timeout is not treated as evidence that unload is
safe.

## Dependencies

Conan supplies cpptrace, concurrentqueue, zlib, expected-lite, libcurl,
tomlplusplus, and nlohmann_json. Linux also uses OpenSSL and libc++/libc++abi.
CMake fetches the pinned distorm decoder for strict x86-64 instruction-boundary
decoding. Endstone builds fetch its pinned public API and PAPI headers. The
LeviLamina target uses explicitly supplied SDK/runtime/prelink inputs and does
not fetch or package those external host files.

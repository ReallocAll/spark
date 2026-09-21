# spark for Bedrock

spark for Bedrock is a native performance profiler for Bedrock Dedicated Server
(BDS). It samples native execution and allocation call stacks, builds
[spark](https://spark.lucko.me/) profiles, and opens them in the standard spark
viewer. The project has host adapters for [Endstone](https://endstone.dev/) and
[LeviLamina](https://github.com/LiteLDev/LeviLamina).

The Endstone plugin is available on Windows and Linux. The LeviLamina module is
the Windows x64 source-build target for BDS 1.26.20.x and LeviLamina 26.20.7.
See [host support](#host-support) and [building](docs/building.md) for setup details.

Profiles use spark's existing protobuf format, upload protocol, and web viewer;
credit for those parts belongs to [lucko/spark](https://github.com/lucko/spark).

## Host support

| Host | Status | Output | Notes |
| --- | --- | --- | --- |
| Endstone | Supported plugin path | `endstone_spark.dll` or `endstone_spark.so` | Windows and Linux; native execution and allocation profiling on x64 hosts |
| LeviLamina | Supported source-build target | `levilamina_spark.dll` + `manifest.json` (optional matching PDB) | Windows x64, BDS 1.26.20.x / LeviLamina 26.20.7; tile/block-entity counts and gamerules are unavailable |

The LeviLamina build produces one native module, `levilamina_spark.dll`, with its
manifest and optional matching `levilamina_spark.pdb`. It supplies server and native-mod
metadata, aggregate player ping, uptime, TPS/MSPT history, CPU and health data,
and validated world, region, and chunk metadata for the three vanilla dimensions,
including entity-type data plus entity and loaded-chunk gauges. Tile/block-entity
counts and gamerules remain unavailable. Active behavior-pack metadata uses the
same selected-pack discovery rules as the Endstone adapter. Uptime follows the
BDS process lifetime and does not reset when the module is unloaded and loaded
again. The LeviLamina `/spark` command is permission-gated at the `GameDirectors`
level. Use the LL lifecycle commands described in [the command reference](docs/commands.md#levilamina-module-lifecycle);
chunk discard callbacks and snapshot reconciliation remove stale chunk/entity
observations, scans prune expired dimension references, and quiescent module
unload closes the subscriptions before removing the module.
Its build and limitations are described in [building](docs/building.md#levilamina-target).

## Install the Endstone plugin

1. Install Endstone using its [official installation guide](https://endstone.dev/latest/getting-started/installation/).
2. Download the matching `endstone_spark.dll` (Windows) or `endstone_spark.so`
   (Linux) from the [latest Release](https://github.com/EndstoneMC/spark/releases/latest).
3. Copy the library into the server's `plugins/` directory.
4. Start or restart BDS. Endstone should load `spark` and create its data folder.

```text
plugins/
  endstone_spark.dll   # Windows
  endstone_spark.so    # Linux
```

Use a full restart when upgrading the library. The plugin writes `config.toml`,
`trusted-viewers.json`, `activity.json`, and profiles below its data directory;
see [configuration](docs/configuration.md) for the keys and paths.

## Install the LeviLamina module

For a matching BDS 1.26.20.x and LeviLamina 26.20.7 installation, download the
`levilamina_spark.dll` and `manifest.json` from the [latest
Release](https://github.com/EndstoneMC/spark/releases/latest). The matching
PDB is optional and is useful for Windows symbol debugging. Copy the DLL and
manifest into the same LeviLamina native-mod directory; if you downloaded the
PDB, keep it beside the DLL. Start or restart BDS so LL can load the module.

The module creates its configuration directory and data directory through
LeviLamina. It stores `config.toml`, `trusted-viewers.json`, `activity.json`,
and profiles there; see [configuration](docs/configuration.md) for the file
roles. The `/spark` command and the world metadata features are available after
the module is enabled.

Each LL Release publishes the DLL, manifest, and PDB as separate assets; the PDB
is optional for installation and is useful for Windows symbol debugging. No
combined archive is required.

## Capture the first profile

Run the command as a player or console sender with the required permission:

```text
/spark profiler start --timeout 30
```

After 30 seconds the profile is finalized and uploaded. Open the printed URL in
the spark viewer. To keep the raw `.sparkprofile` locally instead, use:

```text
/spark profiler start --timeout 30 --save-to-file
```

The local file is written below `plugins/spark/profiles/`; drag it onto
<https://spark.lucko.me/>. If an upload fails, spark also preserves the raw
profile locally. The [command reference](docs/commands.md) covers allocation
profiles, thread selection, filtering, live viewing, and health reports.

## Common tasks

```text
/spark profiler info
/spark profiler stop
/spark profiler cancel
/spark profiler open
/spark tps
/spark health show
/spark ping
/spark activity
/spark tickmonitor
```

Background sampling runs every 10 ms by default. Starting a foreground profile
pauses it. It resumes after an explicit stop or upload, but stays paused after
cancel or timeout.

spark creates `config.toml` on first start. If a setting is invalid, it reports
the error and uses defaults for that run without overwriting your file.

## Documentation

- [Commands](docs/commands.md) — aliases, permissions, profiler flags, health, activity, and tick monitoring.
- [Configuration](docs/configuration.md) — TOML keys, defaults, environment overrides, and trusted viewers.
- [Building](docs/building.md) — Endstone and LeviLamina builds, offline tests, ABI requirements, and host inputs.
- [Profiling details](docs/profiling.md) — profile interpretation, native symbol guesses, allocation coverage, live viewer, and crash recovery.
- [Architecture](docs/ARCHITECTURE.md) — layers, host adapters, and shutdown/sampling boundaries.
- [Python function attribution](docs/python-function-attribution.md) — Endstone CPython 3.12+ attribution details.
- [Behavior pack metadata](docs/BEHAVIOR_PACK_METADATA.md) — Endstone metadata compatibility fallback.

## License

GPLv3, matching spark. See [LICENSE](LICENSE).

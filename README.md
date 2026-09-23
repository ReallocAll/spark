# spark for Bedrock

spark for Bedrock is a native performance profiler for Bedrock Dedicated Server
(BDS). It samples native execution and allocation call stacks, creates standard
spark profiles, and uploads them to or opens them in the spark viewer. Host
adapters are available for [Endstone](https://endstone.dev/) and
[LeviLamina](https://github.com/LiteLDev/LeviLamina).

Profiles use spark's existing protobuf format, upload protocol, and web viewer;
credit for those parts belongs to [lucko/spark](https://github.com/lucko/spark).

## Host support

| Host | Type | Platforms |
| --- | --- | --- |
| Endstone | Plugin | Windows and Linux |
| LeviLamina | Module | Windows x64 |

See [building](docs/building.md) and the [latest Release](https://github.com/EndstoneMC/spark/releases/latest)
for host setup, version requirements, and limitations.

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

Spark writes the local file under the plugin data directory in `profiles/`;
drag it onto <https://spark.lucko.me/>. If an upload fails, spark also preserves
the raw profile locally. The [command reference](docs/commands.md) covers allocation
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

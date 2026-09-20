# Commands

All commands are entered as `/spark ...`. Endstone registers the umbrella
permissions `endstone.command.spark` and `spark`, plus one permission per
command: `spark.profiler`, `spark.tps`, `spark.ping`, `spark.health`,
`spark.activity`, and `spark.tickmonitor`. Operators receive these permissions
by default. An umbrella permission grants access to every spark command.

LeviLamina registers one native `/spark` command at the `GameDirectors`
permission level. It forwards the same command grammar, but does not expose the
Endstone per-command permission nodes.

## LeviLamina module lifecycle

The LL loader manages the single native module through the normal server-thread
path:

```text
/ll unload spark
/ll load spark
/ll reload spark
/ll reactivate spark
```

`unload` and `load` can physically remove and restore `levilamina_spark.dll`
after a quiescent session. Stop and save a profile first when its data must be
preserved; unloading does not export profiles automatically. `reload` and
`reactivate` remain experimental while their combinations are being validated.
These operations do not support arbitrary-thread reentry. If unload is refused,
Spark cannot force it; use diagnostics or a normal server restart.

## Command names and aliases

| Primary command | Aliases | Purpose |
| --- | --- | --- |
| `profiler` | `sampler` | Start, stop, inspect, cancel, or open an execution or allocation profile |
| `tps` | `cpu` | Show rolling TPS, MSPT percentiles, and CPU usage |
| `ping` | — | Show player ping RTT statistics |
| `health` | `healthreport`, `ht` | Show or upload a health report, or open the live dashboard |
| `activity` | `activitylog`, `log` | Show recent profile and health activity |
| `tickmonitor` | `tickmonitoring` | Report unusually long ticks |

Unknown or empty input prints help. Subcommands and flags are case-insensitive.

## Profiling

### Start, stop, and inspect

```text
/spark profiler start [flags]
/spark profiler stop [--save-to-file] [--comment <text>]
/spark profiler upload
/spark profiler info
/spark profiler cancel
```

`upload` is a compatibility alias for `stop`. Stopping finalizes and uploads a
profile unless `--save-to-file` was supplied. `cancel` stops the session without
exporting a profile. If an upload fails, spark saves the raw protobuf profile in
the profile directory and reports its path.

The execution default is a 4 ms interval. The allocation default is 524287
requested bytes per sampling interval. A timeout must be greater than 10 seconds;
without `--timeout`, a session runs until `stop` or `cancel`.

### Start flags

| Flag | Meaning |
| --- | --- |
| `--interval <value>` | Execution sampling interval in milliseconds (`1`–`1000`), or allocation interval in requested bytes (`1`–`524287`) with `--alloc`. `0` selects the mode's default. |
| `--timeout <seconds>` | Stop and export automatically after a whole number of seconds. The value must be greater than `10`. |
| `--only-ticks-over <ms>` | Keep samples only from ticks longer than this positive whole number of milliseconds. |
| `--comment <text>` | Attach a note to the exported profile. Quote text containing spaces. |
| `--save-to-file` | Save a `.sparkprofile` below `plugins/spark/profiles/` instead of uploading it. |
| `--thread <name>` | Select a case-insensitive exact thread name. Repeat to select several names. |
| `--thread *` | Select every BDS process thread and keep separate roots. This cannot be combined with another `--thread` or `--regex`. |
| `--regex` | Treat each `--thread` value as a case-insensitive full-match regular expression. At least one pattern is required. |
| `--not-combined` | Export each sampled thread as its own viewer root. |
| `--combine-all` | Merge all sampled threads into one viewer root. |
| `--ignore-sleeping` | Execution profiles only: skip threads that are idle at capture time. |
| `--alloc` | Profile sampled native allocation call stacks, weighted by requested bytes. |
| `--alloc-live-only` | Track sampled allocations through realloc/free and export only allocations still live. This implies `--alloc`. |

`--combine-all` and `--not-combined` cannot be used together. Allocation
profiles include all covered process threads when no thread selector is given.
Thread selection is applied by the safe aggregation stage, so allocator hooks do
not run regular expressions or query thread names.

### Live viewer

```text
/spark profiler open
/spark profiler trust-viewer --id <client id>
```

`open` connects the running profile to the spark live viewer. Sampler data is
uploaded on rolling windows and statistics are sent separately. Viewer clients
are authenticated by public key. An untrusted client receives a pending client
ID; approve it with `trust-viewer` to persist its key in `trusted-viewers.json`.

## Statistics and health

```text
/spark tps
/spark ping [--player <name>]
/spark health
/spark health show [--memory] [--network]
/spark health upload
/spark health trust-viewer --id <client id>
```

`/spark tps` reports TPS over 5 s, 10 s, 1 min, 5 min, and 15 min; MSPT
distributions over 10 s, 1 min, and 5 min; and process/system CPU over 10 s,
1 min, and 15 min. Early in server startup, it labels the shorter history that
is actually available.

`/spark ping` reports current player RTT minimum, median, p95, and maximum, plus
the rolling 15-minute average of the median. `--player` filters by
case-insensitive player name. LeviLamina supplies aggregate player ping through
its host adapter.

`/spark health` opens the live dashboard. `health show` prints the local report;
`--memory` adds process virtual memory, thread count, and swap/page-file data;
`--network` includes every interface, including interfaces with a zero current
rate. `health upload` uploads the same statistics and resource data as a spark
`HealthData` report. Health dashboard clients use the same trusted viewer flow as
live profiling; static health uploads do not require viewer approval.

## Activity and tick monitoring

```text
/spark activity [--page <number>]
/spark tickmonitor
/spark tickmonitor --threshold <percent>
/spark tickmonitor --threshold-tick <ms>
```

Activity is persisted in `activity.json` and shows four entries per page. URL
entries expire after 60 days; saved-file entries remain until removed. The log
records who started an operation, when it happened, and its resulting URL or
path.

The first `/spark tickmonitor` command establishes a 120-tick baseline and
reports ticks more than 100% above it. Use `--threshold <percent>` for a
different positive relative threshold, or `--threshold-tick <ms>` for a positive
absolute threshold. Run the command again to disable monitoring. The two
threshold flags are mutually exclusive.

## Placeholders on Endstone

When Endstone PlaceholderAPI is installed, spark registers the optional
`spark` expansion. It exposes TPS, MSPT, and process/system CPU values such as
`{spark:tps_5s}`, `{spark:tickduration_1m}`, and `{spark:cpu_process_15m}`.
PlaceholderAPI is optional and is not part of the LeviLamina adapter.

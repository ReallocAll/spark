# Configuration

Spark creates `config.toml` in its host data area on first start. On Endstone,
that is the plugin data directory, normally `plugins/spark/`. On LeviLamina,
`config.toml` is in the mod configuration directory and profiles, activity, and
trusted-viewer state use the mod data directory.

The file is user-owned. Spark writes the template only when the file is absent;
it does not rewrite an existing file during normal operation. Missing keys use
their defaults and unknown keys are ignored. If parsing, typing, validation, or
an endpoint check fails, spark reports the error and uses all defaults for that
startup while preserving the file byte-for-byte.

## TOML keys

| Key | Type | Default | Accepted values or effect |
| --- | --- | --- | --- |
| `viewerUrl` | string | `"https://spark.lucko.me/"` | HTTP(S) viewer base URL. A trailing `/` is added when needed. |
| `bytebinUrl` | string | `"https://spark-usercontent.lucko.me/"` | HTTP(S) bytebin endpoint for profile and health uploads. |
| `bytesocksHost` | string | `"spark-usersockets.lucko.me"` | WebSocket authority with optional port; no scheme, path, query, fragment, user, or password. |
| `backgroundProfiler` | bool | `true` | Start the profiler-independent background execution sampler. |
| `backgroundProfilerInterval` | integer | `10` | Background sampling interval in milliseconds, from `1` through `1000`. |
| `backgroundProfilerThreadGrouper` | string | `"by-pool"` | `by-pool`, `by-name`, or `as-one`. |
| `backgroundProfilerThreadDumper` | string | `"default"` | `default` for the server thread, or `all` for all covered process threads. |
| `allocationRateMetrics` | bool | `true` | Keep the count-only native allocation-rate counter active when no allocation profile owns the hooks. Explicit `--alloc` profiles remain available when this is `false`. |
| `serverPropertiesAdditionalKeys` | string | `""` | Comma-separated, administrator-reviewed keys appended to the built-in safe allowlist. |
| `disableResponseBroadcast` | bool | `false` | Send result notifications only to the originating player. |

`serverPropertiesAdditionalKeys` accepts at most 64 unique keys, each at most
128 characters. Keys may contain letters, digits, `-`, `_`, and `.`. Names known
to be sensitive, and names containing `password`, `passcode`, `token`, `secret`,
`credential`, or `private-key`, remain blocked.

For a small change, edit the generated template and restart the server. The
background profiler and allocation-rate counter are configured at startup; an
explicit profiling command can still override the execution or allocation
interval for that session.

## Environment overrides

The native plugin accepts these Java-compatible environment variables:

```text
SPARK_VIEWERURL
SPARK_BYTEBINURL
SPARK_BYTESOCKSHOST
SPARK_BACKGROUNDPROFILER
SPARK_BACKGROUNDPROFILERINTERVAL
SPARK_BACKGROUNDPROFILERTHREADGROUPER
SPARK_BACKGROUNDPROFILERTHREADDUMPER
SPARK_ALLOCATIONRATEMETRICS
SPARK_DISABLERESPONSEBROADCAST
```

Environment values override TOML values in memory and are never written back to
the file. Boolean values follow Java's `Boolean.parseBoolean` rule: only a
case-insensitive `true` is true. An interval environment value must be an
integer; malformed interval text is ignored so the TOML value remains in use.
Endpoint, thread-mode, and out-of-range interval values are validated together
with the TOML values and can make startup fall back to defaults. There is no
environment variable for `serverPropertiesAdditionalKeys`.

The optional Python attribution diagnostic switch is separate from the TOML
file:

```text
SPARK_PYTHON_ATTRIBUTION_MODE=auto       # default
SPARK_PYTHON_ATTRIBUTION_MODE=off
SPARK_PYTHON_ATTRIBUTION_MODE=shadow-only
```

Unknown values are ignored with a warning. Python attribution is an Endstone
runtime feature; LeviLamina does not expose the Endstone Python integration.
See [Python function attribution](python-function-attribution.md).

## Trusted live viewers

Approved live-viewer and health-dashboard public keys are stored separately in
`trusted-viewers.json` as a JSON array of base64-encoded X.509 keys. The
`trust-viewer` command appends to this file and does not rewrite `config.toml`.
The same trust list is used by the profiler live viewer and health dashboard.

## Files created by spark

| File or directory | Purpose |
| --- | --- |
| `config.toml` | User-owned endpoints and profiler settings |
| `trusted-viewers.json` | Approved viewer public keys |
| `activity.json` | Recent upload and saved-profile activity |
| `profiles/` | Local `.sparkprofile` files and the recovery journal |
| `profiles/recovery/` | Segmented crash-recovery journal while a profile is active |

The exact root differs by host as described above. A profile contains only the
metadata collected by the active host adapter and configured safe allowlist; it
does not include secrets, credentials, private keys, or the BDS executable.

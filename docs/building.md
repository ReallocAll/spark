# Building

The repository builds the shared profiler and application layers on their own,
the Endstone plugin on Windows or Linux, and an isolated LeviLamina module on
Windows x64. The normal build uses CMake, Ninja, Conan 2, and Clang. The first
configure may fetch the pinned public Endstone API and PlaceholderAPI headers,
so network access is required unless those sources are supplied locally.

For host setup, follow Endstone's [installation guide](https://endstone.dev/latest/getting-started/installation/)
and its [C++ project setup](https://next.endstone.dev/docs/api/cpp/getting-started/setup).
Those pages explain the MSVC environment on Windows and the Clang/libc++ ABI
requirement on Linux.

## Prerequisites

- CMake 3.29 or newer.
- Ninja and Conan 2 (`python -m pip install "conan>=2,<3"`).
- Clang 18 or newer. Use `clang-cl` with the MSVC frontend on Windows; use
  Clang with libc++ and libc++abi on Linux.
- Windows: Visual Studio Build Tools with the Windows SDK and Clang tools.
- Linux: libc++, libc++abi, and the static `libc++.a` and `libc++abi.a` archives
  used by the default symbol fixture.
- Python 3.12 or newer for release tooling and native Python runtime tests.

The checked-in `.conan2/profiles/default` selects the expected compiler. Do not
run `conan profile detect` over it.

## Endstone plugin and shared libraries

Install dependencies and configure a `RelWithDebInfo` build:

```shell
python -m pip install "conan>=2,<3"
conan install . --build=missing
cmake -S . -B build -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" \
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
cmake --build build
```

On Windows, run from an environment where `clang-cl` can find the MSVC
toolchain and Windows SDK. The plugin build fetches the pinned Endstone v0.11.11
public API and Endstone PAPI headers v0.1.0. It produces
`endstone_spark.dll` on Windows and `endstone_spark.so` on Linux.

The equivalent generated-preset commands are:

```shell
cmake --preset conan-relwithdebinfo
cmake --build --preset conan-relwithdebinfo
```

If only the common profiler, application, native, protobuf, and network layers
are needed, disable the host plugin and its fetched host APIs:

```shell
cmake -S . -B build -G Ninja \
  "-DCMAKE_TOOLCHAIN_FILE=build/RelWithDebInfo/generators/conan_toolchain.cmake" \
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo" \
  -DENDSTONE_SPARK_BUILD_PLUGIN=OFF
cmake --build build
```

Set `-DENDSTONE_SPARK_BUILD_SELFTEST=OFF` when building only the plugin. The
plugin and self-test options are independent: `ENDSTONE_SPARK_BUILD_PLUGIN=OFF`
avoids fetching Endstone and PAPI, while `ENDSTONE_SPARK_BUILD_SELFTEST=OFF`
omits the offline test tools.

## Tests

Run the offline suite after building:

```shell
ctest --test-dir build --output-on-failure
```

On Linux, leave `ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS=ON` (the default) for the
production-sampler, unload, and legacy gateway tests. Set it to `OFF` only when
those isolated tests are not needed. The focused tools are:

```text
build/spark_selftest --seconds=1
build/spark_selftest --statistics-only
build/spark_selftest --allocation-only
```

Use `.exe` paths on Windows. Python runtime tests need the matching shared
Python library through `SPARK_TEST_LIBPYTHON` or
`-DSPARK_TEST_LIBPYTHON:FILEPATH=...`. Linux may additionally run the optional
CPython 3.11 fallback with `SPARK_TEST_LIBPYTHON_311`; it does not replace the
required 3.12-or-newer runtime for Python attribution.

## LeviLamina experimental target

`SPARK_BUILD_LEVILAMINA` is disabled by default. It is an experimental,
source-build-only Windows x64 target for BDS 1.26.20.x with LeviLamina 26.20.7
inputs. It is not a published stable package and the host adapter does not yet
have full feature parity: ping, plugin-list metadata, world metadata, and world
gauges are unavailable. The raw `/spark` command requires LeviLamina's
`GameDirectors` permission level.

CMake does not download or prepare the LeviLamina SDK, runtime, BDS data, or
prelink tool. Supply every external input explicitly:

- a pinned LeviLamina 26.20.7 SDK root;
- the matching `LeviLamina.dll` and PDB;
- Bedrock runtime data for prelink;
- pinned prelink 0.7.1;
- the pinned `SymbolProvider.cpp` source;
- `llvm-dlltool`; and
- the repository's `tools/levilamina/spark-levilamina-imports.json` allowlist.

The SDK must contain the LeviLamina headers and dependency headers checked by
`cmake/LeviLamina.cmake`. CMake verifies the expected-lite header and
SymbolProvider source hashes, checks every required path, and fails closed when
an input is absent or does not match the pinned source.

From a Windows x64 `clang-cl` environment, use paths appropriate to the local
SDK and runtime layout:

```powershell
$sdk = 'C:\path\to\levilamina-sdk'
$runtime = 'C:\path\to\levilamina-runtime'
$allowlist = (Resolve-Path 'tools/levilamina/spark-levilamina-imports.json').Path

conan install . --build=missing -of build-ll
cmake -S . -B build-ll -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=build-ll/build/RelWithDebInfo/generators/conan_toolchain.cmake" `
  "-DCMAKE_BUILD_TYPE=RelWithDebInfo" `
  '-DENDSTONE_SPARK_BUILD_PLUGIN=OFF' `
  '-DENDSTONE_SPARK_BUILD_SELFTEST=OFF' `
  '-DSPARK_BUILD_LEVILAMINA=ON' `
  "-DSPARK_LL_SDK_ROOT=$sdk" `
  "-DSPARK_LL_RUNTIME_DLL=$runtime\plugins\LeviLamina\LeviLamina.dll" `
  "-DSPARK_LL_RUNTIME_PDB=$runtime\plugins\LeviLamina\LeviLamina.pdb" `
  "-DSPARK_LL_RUNTIME_DATA=$sdk\runtime-data\bedrock_runtime_data" `
  "-DSPARK_LL_PRELINK=$sdk\tools\prelink\prelink.exe" `
  "-DSPARK_LL_SYMBOLPROVIDER_SOURCE=$sdk\sources\symbolprovider\src\SymbolProvider.cpp" `
  "-DSPARK_LL_IMPORT_ALLOWLIST=$allowlist"
cmake --build build-ll --config RelWithDebInfo
ctest --test-dir build-ll -C RelWithDebInfo --output-on-failure
```

The target synthesizes a named import library from the runtime DLL/PDB and
allowlist, runs prelink against the Bedrock runtime data, and writes the native
module to `build-ll/bin/spark/spark.dll` with `manifest.json` beside it. The
generated import library, receipt, prelink output, map, and PDB remain in the
build tree. Install `spark.dll` and its manifest according to the LeviLamina
loader's native-mod layout for the matching runtime.

The LeviLamina CTest entries include the import-generator self-test, callback
protocol test, and cleanup-deadline test. They validate build-time and lifecycle
contracts; they do not establish full runtime feature parity with Endstone.

## ABI and local source reuse

Endstone plugins execute inside the BDS process. Match the host's compiler ABI,
C++ standard library, and runtime: use `clang-cl` with the MSVC ABI on Windows,
and Clang with an ABI-compatible libc++ on Linux. Do not pass STL objects across
the plugin boundary from an incompatible runtime.

For local Endstone iteration, CMake accepts
`-DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path>` to reuse an existing checkout. Keep
the fetched API at the version expected by this source tree.

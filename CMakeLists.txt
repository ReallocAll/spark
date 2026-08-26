cmake_minimum_required(VERSION 3.29)
project(spark VERSION 0.5.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENDSTONE_SPARK_BUILD_SELFTEST "Build the offline sampler self-test tool" ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# --- dependencies (from conan) ---
find_package(cpptrace REQUIRED)
find_package(concurrentqueue REQUIRED)
find_package(ZLIB REQUIRED)
find_package(CURL REQUIRED)
find_package(tomlplusplus REQUIRED)
if (NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif ()
find_package(Threads REQUIRED)

include(FetchContent)

# --- x86-64 instruction decoder ---
# funchook 1.1.3 is not available from Conan Center. Fetch the same upstream
# version used by Endstone. Its small distorm dependency provides the strict
# instruction-boundary decoder used by both native symbol guessers.
if (WIN32 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
    set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_STATIC OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
    set(FUNCHOOK_DISASM distorm CACHE STRING "" FORCE)
    FetchContent_Declare(funchook
            GIT_REPOSITORY https://github.com/kubo/funchook.git
            GIT_TAG v1.1.3
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(funchook)
endif ()

# --- Endstone: header-only API + endstone_add_plugin(), fetched standalone ---
# Endstone's root CMake returns right after its include/ target when consumed as a
# subproject, so this only pulls the headers, not the server. For local iteration,
# pass -DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path> to reuse an existing checkout.
set(ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY "https://github.com/EndstoneMC/endstone.git" CACHE STRING
        "Repository providing the Endstone public plugin API")
set(ENDSTONE_SPARK_ENDSTONE_GIT_TAG "develop" CACHE STRING
        "Git ref providing the Endstone public plugin API")
FetchContent_Declare(endstone
        GIT_REPOSITORY ${ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY}
        GIT_TAG ${ENDSTONE_SPARK_ENDSTONE_GIT_TAG})
FetchContent_MakeAvailable(endstone)

# PAPI is optional at runtime; only its public headers are fetched.
set(ENDSTONE_SPARK_PAPI_GIT_REPOSITORY "https://github.com/EndstoneMC/papi.git" CACHE STRING
        "Repository providing the public Endstone PAPI headers")
set(ENDSTONE_SPARK_PAPI_GIT_TAG "main" CACHE STRING
        "Git ref providing the public Endstone PAPI headers")
FetchContent_Declare(endstone_papi_headers
        GIT_REPOSITORY ${ENDSTONE_SPARK_PAPI_GIT_REPOSITORY}
        GIT_TAG ${ENDSTONE_SPARK_PAPI_GIT_TAG}
        GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(endstone_papi_headers)
add_library(spark_papi_headers INTERFACE)
target_include_directories(spark_papi_headers INTERFACE "${endstone_papi_headers_SOURCE_DIR}/include")
target_link_libraries(spark_papi_headers INTERFACE endstone::endstone)

if (WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()

# The Windows allocation profiler patches IAT slots to this process-lifetime
# trampoline module instead of pointing them directly into the unloadable plugin.
if (WIN32)
    add_library(spark_allocation_shim SHARED
            src/native/alloc/windows_allocation_shim.cpp)
    target_include_directories(spark_allocation_shim PRIVATE src)
    target_compile_definitions(spark_allocation_shim PRIVATE NOMINMAX)
    set_target_properties(spark_allocation_shim PROPERTIES OUTPUT_NAME "spark_allocation_shim")
endif ()

# --- shared profiling time primitives (no native or core dependencies) ---
add_library(spark_profiling_time STATIC
        src/core/util/monotonic_time.cpp
        src/core/profiler/profiling_window.cpp)
target_include_directories(spark_profiling_time PUBLIC src)
set_target_properties(spark_profiling_time PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- native backend: sampler, symbol guesser, allocation hooks (no Endstone) ---
add_library(spark_native STATIC
        src/native/alloc/allocation_profile_aggregation.cpp
        src/native/alloc/allocation_thread_filter.cpp
        src/native/sampler/call_tree.cpp
        src/native/sampler/sampler.cpp
        src/native/sampler/sampler_aggregation.cpp
        src/native/sampler/thread_selector.cpp
        src/native/sampler/thread_info.cpp
        src/native/symbol/symbol_guess_dwarf.cpp
        src/native/symbol/symbol_guess_evidence.cpp
        src/native/symbol/symbolicate.cpp
        src/native/symbol/symbol_guess.cpp)
if (WIN32)
    target_compile_definitions(spark_native PRIVATE NOMINMAX)
    target_sources(spark_native PRIVATE
            src/native/symbol/dbghelp_manager.cpp
            src/native/symbol/symbol_guess_windows.cpp
            src/native/symbol/symbol_guess_windows_support.cpp
            src/native/symbol/symbol_guess_windows_core.cpp
            src/native/symbol/symbol_guess_windows_rtti.cpp
            src/native/symbol/symbol_guess_windows_strings.cpp
            src/native/symbol/symbol_guess_windows_misc.cpp
            src/native/sampler/capture_windows.cpp
            src/native/alloc/allocation_sampler_windows.cpp
            src/native/alloc/windows_iat_funchook_compat.cpp
            src/native/alloc/windows_iat_hooks.cpp
            src/native/alloc/windows_iat_hooks_windows.cpp
            src/native/alloc/windows_thread_suspension.cpp)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_sources(spark_native PRIVATE
            src/native/sampler/capture_linux.cpp
            src/native/alloc/allocation_sampler_linux.cpp
            src/native/alloc/elf_import_hooks.cpp)
else ()
    target_sources(spark_native PRIVATE
            src/native/sampler/capture_linux.cpp
            src/native/alloc/allocation_sampler_stub.cpp)
endif ()
target_include_directories(spark_native PUBLIC src)
target_link_libraries(spark_native PUBLIC spark_profiling_time cpptrace::cpptrace concurrentqueue::concurrentqueue
        Threads::Threads)
set_target_properties(spark_native PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_definitions(spark_native PRIVATE _GNU_SOURCE)
    target_compile_options(spark_native PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()
if (WIN32)
    target_include_directories(spark_native PRIVATE "${funchook_SOURCE_DIR}/distorm/include")
    target_link_libraries(spark_native PUBLIC dbghelp psapi advapi32 distorm)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_include_directories(spark_native PRIVATE "${funchook_SOURCE_DIR}/distorm/include")
    target_link_libraries(spark_native PUBLIC distorm ${CMAKE_DL_LIBS})
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(spark_native PUBLIC ${CMAKE_DL_LIBS})
endif ()

# --- platform-independent core: profiler, stats, proto, net (no Endstone) ---
add_library(spark_core STATIC
        src/core/config/spark_config.cpp
        src/core/config/spark_config_environment.cpp
        src/core/config/trusted_viewers.cpp
        src/core/recovery/journal_reader.cpp
        src/core/recovery/recovery_player.cpp
        src/core/recovery/recovery_writer.cpp
        src/core/recovery/stall_watchdog.cpp
        src/core/util/base64.cpp
        src/core/util/format.cpp
        src/core/util/state_file.cpp
        src/core/util/world_region.cpp
        src/core/metadata/server_properties.cpp
        src/core/metadata/gamerule_defaults.cpp
        src/core/activity/activity_log.cpp
        src/core/command/arguments.cpp
        src/core/profiler/profiler.cpp
        src/core/profiler/native_attribution.cpp
        src/core/profiler/profiler_export.cpp
        src/core/profiler/profiler_recovery.cpp
        src/core/stats/executable_hash.cpp
        src/core/stats/metrics_history.cpp
        src/core/stats/statistics_service.cpp
        src/core/stats/statistics_service_queries.cpp
        src/core/stats/statistics_profile_windows.cpp
        src/core/stats/ping_statistics.cpp
        src/core/stats/network_monitor.cpp
        src/core/stats/tick_monitor.cpp
        src/core/stats/system_stats.cpp
        src/core/ws/ws_proto.cpp
        src/core/ws/viewer_socket_lifecycle.cpp
        src/core/ws/viewer_socket_send.cpp
        src/core/ws/viewer_socket_receive.cpp
        src/proto/health_data.cpp
        src/proto/metrics_proto.cpp
        src/proto/sampler_data.cpp
        src/proto/statistics_proto.cpp
        src/net/gzip.cpp
        src/net/bytebin.cpp
        src/net/profile_file.cpp
        src/net/websocket.cpp)
target_include_directories(spark_core PUBLIC src)
if (WIN32)
    target_sources(spark_core PRIVATE
            src/core/stats/network_monitor_windows.cpp
            src/core/ws/crypto_cng.cpp)
    target_link_libraries(spark_core PUBLIC spark_native ZLIB::ZLIB CURL::libcurl tomlplusplus::tomlplusplus bcrypt)
else ()
    target_sources(spark_core PRIVATE
            src/core/stats/network_monitor_linux.cpp
            src/core/ws/crypto.cpp)
    target_link_libraries(spark_core PUBLIC spark_native ZLIB::ZLIB CURL::libcurl tomlplusplus::tomlplusplus OpenSSL::Crypto)
endif ()
if (WIN32)
    target_compile_definitions(spark_core PUBLIC NOMINMAX _WIN32_WINNT=0x0600 NTDDI_VERSION=0x06000000)
    target_link_libraries(spark_core PUBLIC iphlpapi)
endif ()
set_target_properties(spark_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark_core PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()

# --- application layer: command dispatch, session orchestration, export (no Endstone) ---
add_library(spark_application STATIC
        src/application/activity/activity_command.cpp
        src/application/command/command_registry.cpp
        src/application/command/profiler_action_resolver.cpp
        src/application/health/health_command.cpp
        src/application/health/health_command_report.cpp
        src/application/health/health_report.cpp
        src/application/monitoring/monitoring_schedule.cpp
        src/application/health/health_dashboard.cpp
        src/application/health/health_dashboard_worker.cpp
        src/application/health/health_dashboard_connection.cpp
        src/application/placeholder/spark_placeholder_resolver.cpp
        src/application/profiler/profile_exporter.cpp
        src/application/profiler/live_statistics_payload.cpp
        src/application/profiler/live_viewer_schedule.cpp
        src/application/profiler/profiler_open_orchestrator.cpp
        src/application/profiler/profiler_service.cpp
        src/application/profiler/profiler_service_commands.cpp
        src/application/profiler/profiler_start_options.cpp
        src/application/profiler/profiler_timeout.cpp
        src/application/profiler/viewer_update_worker.cpp
        src/application/spark_application.cpp
        src/application/tick_monitor/tick_monitor_command.cpp)
target_include_directories(spark_application PUBLIC src)
target_link_libraries(spark_application PUBLIC spark_core)
set_target_properties(spark_application PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark_application PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()

add_library(spark_papi_integration STATIC
        src/platform/endstone/papi_integration.cpp)
target_include_directories(spark_papi_integration PUBLIC src)
target_link_libraries(spark_papi_integration PUBLIC spark_application spark_papi_headers)
set_target_properties(spark_papi_integration PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark_papi_integration PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
endif ()

# --- the Endstone plugin (links its application layer + Endstone adapters) ---
endstone_add_plugin(spark
        src/platform/endstone/adapters.cpp
        src/platform/endstone/world_gauge_provider.cpp
        src/platform/endstone/native_plugin_attribution.cpp
        src/plugin.cpp)
target_include_directories(spark PRIVATE src)
target_link_libraries(spark PRIVATE spark_application spark_papi_integration)
# Localize every symbol from the static archives (zlib, cpptrace, libunwind, libc++)
# so they can't interpose with Endstone core's own copies in the BDS process. Only
# init_endstone_plugin (default visibility via the macro) stays exported.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(spark PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    target_link_options(spark PRIVATE "-Wl,--exclude-libs,ALL")
endif ()

# --- offline self-test (no BDS needed) ---
if (ENDSTONE_SPARK_BUILD_SELFTEST)
    enable_testing()
    add_subdirectory(tests)
endif ()

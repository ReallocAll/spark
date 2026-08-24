if (MSVC)
    add_compile_options(/UNDEBUG)
else ()
    add_compile_options(-UNDEBUG)
endif ()

add_executable(spark_selftest
        selftest.cpp
        selftest_internal.cpp
        selftest_support.cpp
        selftest_basic.cpp
        selftest_capture.cpp
        selftest_sampler.cpp
        selftest_live.cpp
        selftest_viewer.cpp
        selftest_async.cpp
        selftest_statistics.cpp
        selftest_allocation_common.cpp
        selftest_allocation_selection.cpp
        selftest_allocation_sampling.cpp
        selftest_allocation_pressure.cpp
        selftest_allocation_lifecycle.cpp
        selftest_allocation_linux.cpp)
target_link_libraries(spark_selftest PRIVATE spark_application)
set_target_properties(spark_selftest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(spark_allocation_benchmark allocation_benchmark.cpp)
target_compile_definitions(
        spark_allocation_benchmark PRIVATE
        SPARK_ALLOCATION_BENCHMARK_CURRENT)
target_link_libraries(spark_allocation_benchmark PRIVATE spark_core)
set_target_properties(spark_allocation_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(spark_symbol_guess_evidence_test native/symbol/symbol_guess_evidence_test.cpp)
target_link_libraries(spark_symbol_guess_evidence_test PRIVATE spark_core)
set_target_properties(spark_symbol_guess_evidence_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_symbol_guess_evidence_test COMMAND spark_symbol_guess_evidence_test)

add_executable(spark_sampler_bounded_queue_test native/sampler/bounded_queue_test.cpp)
target_link_libraries(spark_sampler_bounded_queue_test PRIVATE spark_core)
set_target_properties(spark_sampler_bounded_queue_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_sampler_bounded_queue_test COMMAND spark_sampler_bounded_queue_test)

add_executable(spark_sampler_bounded_aggregation_test native/sampler/bounded_aggregation_test.cpp)
target_link_libraries(spark_sampler_bounded_aggregation_test PRIVATE spark_core)
set_target_properties(spark_sampler_bounded_aggregation_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_sampler_bounded_aggregation_test COMMAND spark_sampler_bounded_aggregation_test)

add_executable(spark_allocation_bounded_event_queue_test native/alloc/bounded_event_queue_test.cpp)
target_include_directories(spark_allocation_bounded_event_queue_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(spark_allocation_bounded_event_queue_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_allocation_bounded_event_queue_test COMMAND spark_allocation_bounded_event_queue_test)

add_executable(spark_allocation_profile_aggregation_test native/alloc/allocation_profile_aggregation_test.cpp)
target_link_libraries(spark_allocation_profile_aggregation_test PRIVATE spark_core)
set_target_properties(spark_allocation_profile_aggregation_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_allocation_profile_aggregation_test COMMAND spark_allocation_profile_aggregation_test)

add_executable(spark_ping_statistics_test core/stats/ping_statistics_test.cpp)
target_link_libraries(spark_ping_statistics_test PRIVATE spark_core)
set_target_properties(spark_ping_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_ping_statistics_test COMMAND spark_ping_statistics_test)

add_executable(spark_network_monitor_test core/stats/network_monitor_test.cpp)
target_link_libraries(spark_network_monitor_test PRIVATE spark_core)
set_target_properties(spark_network_monitor_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_network_monitor_test COMMAND spark_network_monitor_test)

add_executable(spark_arguments_test core/command/arguments_test.cpp)
target_link_libraries(spark_arguments_test PRIVATE spark_core)
set_target_properties(spark_arguments_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_arguments_test COMMAND spark_arguments_test)

add_executable(spark_placeholder_statistics_test core/stats/placeholder_statistics_test.cpp)
target_link_libraries(spark_placeholder_statistics_test PRIVATE spark_core)
set_target_properties(spark_placeholder_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_placeholder_statistics_test COMMAND spark_placeholder_statistics_test)

add_executable(spark_metrics_history_test core/stats/metrics_history_test.cpp)
target_link_libraries(spark_metrics_history_test PRIVATE spark_core)
set_target_properties(spark_metrics_history_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_metrics_history_test COMMAND spark_metrics_history_test)

add_executable(spark_proto_statistics_test proto/statistics_proto_test.cpp)
target_link_libraries(spark_proto_statistics_test PRIVATE spark_core)
set_target_properties(spark_proto_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_proto_statistics_test COMMAND spark_proto_statistics_test)

add_executable(spark_sampler_statistics_test proto/sampler_statistics_test.cpp)
target_link_libraries(spark_sampler_statistics_test PRIVATE spark_core)
set_target_properties(spark_sampler_statistics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_sampler_statistics_test COMMAND spark_sampler_statistics_test)

add_executable(spark_proto_health_data_test proto/health_data_test.cpp)
target_link_libraries(spark_proto_health_data_test PRIVATE spark_core)
set_target_properties(spark_proto_health_data_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_proto_health_data_test COMMAND spark_proto_health_data_test)

add_executable(spark_proto_metrics_test proto/metrics_proto_test.cpp)
target_link_libraries(spark_proto_metrics_test PRIVATE spark_core)
set_target_properties(spark_proto_metrics_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_proto_metrics_test COMMAND spark_proto_metrics_test)

add_executable(spark_activity_log_test core/activity/activity_log_test.cpp)
target_link_libraries(spark_activity_log_test PRIVATE spark_core)
set_target_properties(spark_activity_log_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_activity_log_test COMMAND spark_activity_log_test)

add_executable(spark_activity_command_test application/activity/activity_command_test.cpp)
target_link_libraries(spark_activity_command_test PRIVATE spark_application)
set_target_properties(spark_activity_command_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_activity_command_test COMMAND spark_activity_command_test)

add_executable(spark_viewer_update_worker_test application/profiler/viewer_update_worker_test.cpp)
target_link_libraries(spark_viewer_update_worker_test PRIVATE spark_application)
set_target_properties(spark_viewer_update_worker_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_viewer_update_worker_test COMMAND spark_viewer_update_worker_test)

add_executable(spark_profiler_timeout_test application/profiler/profiler_timeout_test.cpp)
target_link_libraries(spark_profiler_timeout_test PRIVATE spark_application)
set_target_properties(spark_profiler_timeout_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_timeout_test COMMAND spark_profiler_timeout_test)

add_executable(spark_profiler_timeout_integration_test application/profiler/profiler_timeout_integration_test.cpp)
target_link_libraries(spark_profiler_timeout_integration_test PRIVATE spark_application)
set_target_properties(spark_profiler_timeout_integration_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_timeout_integration_test COMMAND spark_profiler_timeout_integration_test)

add_executable(spark_profiler_service_failure_test application/profiler/profiler_service_failure_test.cpp)
target_link_libraries(spark_profiler_service_failure_test PRIVATE spark_application)
set_target_properties(spark_profiler_service_failure_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_service_failure_test COMMAND spark_profiler_service_failure_test)

add_executable(spark_health_dashboard_test application/health/health_dashboard_test.cpp)
target_link_libraries(spark_health_dashboard_test PRIVATE spark_application)
set_target_properties(spark_health_dashboard_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_dashboard_test COMMAND spark_health_dashboard_test)

add_executable(spark_health_command_routing_test application/health/health_command_routing_test.cpp)
target_link_libraries(spark_health_command_routing_test PRIVATE spark_application)
set_target_properties(spark_health_command_routing_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_command_routing_test COMMAND spark_health_command_routing_test)

add_executable(spark_health_command_failure_test application/health/health_command_failure_test.cpp)
target_link_libraries(spark_health_command_failure_test PRIVATE spark_application)
set_target_properties(spark_health_command_failure_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_command_failure_test COMMAND spark_health_command_failure_test)

add_executable(spark_health_report_test application/health/health_report_test.cpp)
target_link_libraries(spark_health_report_test PRIVATE spark_application)
set_target_properties(spark_health_report_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_health_report_test COMMAND spark_health_report_test)

add_executable(spark_profiler_start_options_test application/profiler/profiler_start_options_test.cpp)
target_link_libraries(spark_profiler_start_options_test PRIVATE spark_application)
set_target_properties(spark_profiler_start_options_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_start_options_test COMMAND spark_profiler_start_options_test)

add_executable(spark_profiler_open_orchestrator_test application/profiler/profiler_open_orchestrator_test.cpp)
target_link_libraries(spark_profiler_open_orchestrator_test PRIVATE spark_application)
set_target_properties(spark_profiler_open_orchestrator_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_open_orchestrator_test COMMAND spark_profiler_open_orchestrator_test)

add_executable(spark_placeholder_resolver_test application/placeholder/spark_placeholder_resolver_test.cpp)
target_link_libraries(spark_placeholder_resolver_test PRIVATE spark_application)
set_target_properties(spark_placeholder_resolver_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_placeholder_resolver_test COMMAND spark_placeholder_resolver_test)

add_executable(spark_papi_integration_test platform/endstone/papi_integration_test.cpp)
target_link_libraries(spark_papi_integration_test PRIVATE spark_papi_integration)
set_target_properties(spark_papi_integration_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_papi_integration_test COMMAND spark_papi_integration_test)

add_executable(spark_thread_grouper_test core/profiler/thread_grouper_test.cpp)
target_link_libraries(spark_thread_grouper_test PRIVATE spark_core)
set_target_properties(spark_thread_grouper_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_thread_grouper_test COMMAND spark_thread_grouper_test)

add_executable(spark_native_plugin_attribution_test core/profiler/native_plugin_attribution_test.cpp)
target_link_libraries(spark_native_plugin_attribution_test PRIVATE spark_core)
set_target_properties(spark_native_plugin_attribution_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_native_plugin_attribution_test COMMAND spark_native_plugin_attribution_test)

add_executable(spark_profiler_export_test core/profiler/profiler_export_test.cpp)
target_link_libraries(spark_profiler_export_test PRIVATE spark_core)
set_target_properties(spark_profiler_export_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_export_test COMMAND spark_profiler_export_test)

add_executable(spark_included_ticks_test core/profiler/included_ticks_test.cpp)
target_link_libraries(spark_included_ticks_test PRIVATE spark_core)
set_target_properties(spark_included_ticks_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_included_ticks_test COMMAND spark_included_ticks_test)

add_executable(spark_config_test core/config/spark_config_test.cpp)
target_link_libraries(spark_config_test PRIVATE spark_core)
set_target_properties(spark_config_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_config_test COMMAND spark_config_test)

add_executable(spark_trusted_viewers_test core/config/trusted_viewers_test.cpp)
target_link_libraries(spark_trusted_viewers_test PRIVATE spark_core)
set_target_properties(spark_trusted_viewers_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_trusted_viewers_test COMMAND spark_trusted_viewers_test)

add_executable(spark_crypto_test core/ws/crypto_test.cpp)
target_link_libraries(spark_crypto_test PRIVATE spark_core)
set_target_properties(spark_crypto_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_crypto_test COMMAND spark_crypto_test)

add_executable(spark_ws_proto_test core/ws/ws_proto_test.cpp)
target_link_libraries(spark_ws_proto_test PRIVATE spark_core)
set_target_properties(spark_ws_proto_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_ws_proto_test COMMAND spark_ws_proto_test)

add_executable(spark_websocket_lifecycle_test net/websocket_lifecycle_test.cpp)
target_link_libraries(spark_websocket_lifecycle_test PRIVATE spark_core)
set_target_properties(spark_websocket_lifecycle_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_websocket_lifecycle_test COMMAND spark_websocket_lifecycle_test)

add_executable(spark_stall_watchdog_test core/recovery/stall_watchdog_test.cpp)
target_link_libraries(spark_stall_watchdog_test PRIVATE spark_core)
set_target_properties(spark_stall_watchdog_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_stall_watchdog_test COMMAND spark_stall_watchdog_test)

add_executable(spark_journal_test
        core/recovery/journal_test_main.cpp
        core/recovery/journal_test_support.cpp
        core/recovery/journal_format_test.cpp
        core/recovery/journal_writer_reader_test.cpp
        core/recovery/recovery_player_test.cpp
        core/recovery/recovery_rolling_test.cpp)
target_link_libraries(spark_journal_test PRIVATE spark_core)
set_target_properties(spark_journal_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_test COMMAND spark_journal_test)

add_executable(spark_journal_reader_bounds_test core/recovery/journal_reader_bounds_test.cpp)
target_link_libraries(spark_journal_reader_bounds_test PRIVATE spark_core)
set_target_properties(spark_journal_reader_bounds_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_reader_bounds_test COMMAND spark_journal_reader_bounds_test)

add_executable(spark_recovery_writer_queue_test core/recovery/recovery_writer_queue_test.cpp)
target_link_libraries(spark_recovery_writer_queue_test PRIVATE spark_core)
set_target_properties(spark_recovery_writer_queue_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_writer_queue_test COMMAND spark_recovery_writer_queue_test)

add_executable(spark_journal_format_bounds_test core/recovery/journal_format_bounds_test.cpp)
target_link_libraries(spark_journal_format_bounds_test PRIVATE spark_core)
set_target_properties(spark_journal_format_bounds_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_journal_format_bounds_test COMMAND spark_journal_format_bounds_test)

add_executable(spark_recovery_save_failure_test application/recovery/recovery_save_failure_test.cpp)
target_link_libraries(spark_recovery_save_failure_test PRIVATE spark_application)
set_target_properties(spark_recovery_save_failure_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_save_failure_test COMMAND spark_recovery_save_failure_test)

add_executable(spark_recovery_corrupt_test application/recovery/recovery_corrupt_test.cpp)
target_link_libraries(spark_recovery_corrupt_test PRIVATE spark_application)
set_target_properties(spark_recovery_corrupt_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_recovery_corrupt_test COMMAND spark_recovery_corrupt_test)

add_executable(spark_profiler_upload_alias_test application/command/profiler_upload_alias_test.cpp)
target_link_libraries(spark_profiler_upload_alias_test PRIVATE spark_application)
set_target_properties(spark_profiler_upload_alias_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_profiler_upload_alias_test COMMAND spark_profiler_upload_alias_test)

add_executable(spark_command_registry_test application/command/command_registry_test.cpp)
target_link_libraries(spark_command_registry_test PRIVATE spark_application)
set_target_properties(spark_command_registry_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_command_registry_test COMMAND spark_command_registry_test)

add_executable(spark_world_region_test core/util/world_region_test.cpp)
target_link_libraries(spark_world_region_test PRIVATE spark_core)
set_target_properties(spark_world_region_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_world_region_test COMMAND spark_world_region_test)

add_executable(spark_monotonic_time_test core/util/monotonic_time_test.cpp)
target_link_libraries(spark_monotonic_time_test PRIVATE spark_core)
set_target_properties(spark_monotonic_time_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_monotonic_time_test COMMAND spark_monotonic_time_test)

add_executable(spark_server_properties_test core/metadata/server_properties_test.cpp)
target_link_libraries(spark_server_properties_test PRIVATE spark_core)
set_target_properties(spark_server_properties_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_server_properties_test COMMAND spark_server_properties_test)

add_executable(spark_symbol_guess_dwarf_test native/symbol/symbol_guess_dwarf_test.cpp)
target_link_libraries(spark_symbol_guess_dwarf_test PRIVATE spark_core)
set_target_properties(spark_symbol_guess_dwarf_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_test(NAME spark_symbol_guess_dwarf_test COMMAND spark_symbol_guess_dwarf_test)

if (WIN32)
    add_executable(spark_symbol_guess_windows_test
            native/symbol/symbol_guess_windows_test.cpp
            native/symbol/symbol_guess_windows_test_support.cpp
            native/symbol/symbol_guess_windows_test_core.cpp
            native/symbol/symbol_guess_windows_test_rtti.cpp
            native/symbol/symbol_guess_windows_test_strings.cpp
            native/symbol/symbol_guess_windows_test_misc.cpp
            native/symbol/symbol_guess_windows_test_evaluator.cpp)
    target_link_libraries(spark_symbol_guess_windows_test PRIVATE spark_core)
    set_target_properties(spark_symbol_guess_windows_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_symbol_guess_windows_test COMMAND spark_symbol_guess_windows_test)

    add_library(spark_windows_allocation_fixture MODULE
            native/alloc/windows_allocation_fixture.cpp)
    set_target_properties(spark_windows_allocation_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(spark_selftest spark_windows_allocation_fixture)
    target_compile_definitions(
            spark_selftest PRIVATE
            SPARK_WINDOWS_ALLOCATION_FIXTURE_PATH="$<TARGET_FILE:spark_windows_allocation_fixture>")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    add_executable(spark_symbol_guess_linux_test native/symbol/symbol_guess_linux_test.cpp)
    target_link_libraries(spark_symbol_guess_linux_test PRIVATE spark_core)
    set_target_properties(spark_symbol_guess_linux_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_symbol_guess_linux_test COMMAND spark_symbol_guess_linux_test)

    add_library(spark_hidden_symbol_fixture MODULE native/symbol/hidden_symbol_fixture.cpp)
    target_compile_options(spark_hidden_symbol_fixture PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    set_target_properties(spark_hidden_symbol_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

    add_executable(spark_hidden_symbol_test native/symbol/hidden_symbol_test.cpp)
    target_link_libraries(spark_hidden_symbol_test PRIVATE spark_core ${CMAKE_DL_LIBS})
    target_compile_definitions(
            spark_hidden_symbol_test PRIVATE
            SPARK_HIDDEN_SYMBOL_FIXTURE_PATH="$<TARGET_FILE:spark_hidden_symbol_fixture>")
    add_dependencies(spark_hidden_symbol_test spark_hidden_symbol_fixture)
    set_target_properties(spark_hidden_symbol_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_hidden_symbol_test COMMAND spark_hidden_symbol_test)

    add_library(spark_elf_hook_fixture MODULE native/alloc/elf_hook_fixture.cpp)
    set_target_properties(spark_elf_hook_fixture PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(spark_selftest spark_elf_hook_fixture)
    target_compile_definitions(
            spark_selftest PRIVATE
            _GNU_SOURCE
            SPARK_ELF_HOOK_FIXTURE_PATH="$<TARGET_FILE:spark_elf_hook_fixture>")
    target_compile_definitions(spark_profiler_export_test PRIVATE _GNU_SOURCE)

    add_executable(spark_capture_linux_lifecycle_test native/sampler/capture_linux_lifecycle_test.cpp)
    target_compile_definitions(spark_capture_linux_lifecycle_test PRIVATE _GNU_SOURCE)
    target_link_libraries(spark_capture_linux_lifecycle_test PRIVATE spark_core ${CMAKE_DL_LIBS})
    set_target_properties(spark_capture_linux_lifecycle_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_capture_linux_lifecycle_test COMMAND spark_capture_linux_lifecycle_test)
endif ()

add_test(NAME spark_selftest COMMAND spark_selftest --seconds=1)

# --- Python tests (no compilation needed) ---
add_test(NAME spark_architecture_boundaries_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_architecture_boundaries.py)

add_test(NAME spark_release_changelog_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_release_changelog.py)

add_test(NAME spark_profile_evaluator_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_profile_evaluator.py)

add_test(NAME spark_workflow_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_workflows.py)

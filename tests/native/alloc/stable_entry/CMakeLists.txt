cmake_minimum_required(VERSION 3.24)
project(spark_windows_stable_entry_experiment LANGUAGES C CXX)

if (NOT WIN32)
    message(FATAL_ERROR "stable-entry native experiment requires Windows")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
set(FUNCHOOK_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(FUNCHOOK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_INSTALL OFF CACHE BOOL "" FORCE)
set(FUNCHOOK_DISASM distorm CACHE STRING "" FORCE)
FetchContent_Declare(funchook
        GIT_REPOSITORY https://github.com/kubo/funchook.git
        GIT_TAG v1.1.3
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(funchook)

get_filename_component(SPARK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

set(STABLE_ENTRY_ENGINE_SOURCES
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_experiment.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_atomic.cpp")

add_executable(windows_stable_entry_atomic_test
        ${STABLE_ENTRY_ENGINE_SOURCES}
        "${SPARK_ROOT}/tests/native/alloc/windows_stable_entry_atomic_test.cpp")
target_include_directories(windows_stable_entry_atomic_test PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_stable_entry_atomic_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_stable_entry_atomic_test PRIVATE /W4 /WX /UNDEBUG)
target_link_libraries(windows_stable_entry_atomic_test PRIVATE funchook-static)

add_executable(windows_stable_entry_lifecycle_probe
        ${STABLE_ENTRY_ENGINE_SOURCES}
        "${SPARK_ROOT}/tests/native/alloc/windows_stable_entry_lifecycle_probe.cpp")
target_include_directories(windows_stable_entry_lifecycle_probe PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_stable_entry_lifecycle_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_stable_entry_lifecycle_probe PRIVATE /W4 /WX /UNDEBUG)
target_link_libraries(windows_stable_entry_lifecycle_probe PRIVATE funchook-static)

add_executable(windows_stable_entry_relocation_probe
        "${SPARK_ROOT}/tests/native/alloc/windows_stable_entry_relocation_probe.cpp")
target_include_directories(windows_stable_entry_relocation_probe PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_stable_entry_relocation_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_stable_entry_relocation_probe PRIVATE /W4 /WX /UNDEBUG)
target_link_libraries(windows_stable_entry_relocation_probe PRIVATE funchook-static)

add_executable(windows_stable_entry_alignment_test
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_experiment.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_resolver.cpp"
        "${SPARK_ROOT}/tests/native/alloc/windows_stable_entry_alignment_test.cpp")
target_include_directories(windows_stable_entry_alignment_test PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_stable_entry_alignment_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_stable_entry_alignment_test PRIVATE /W4 /WX /UNDEBUG)

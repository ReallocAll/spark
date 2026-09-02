cmake_minimum_required(VERSION 3.24)
project(spark_windows_permanent_gateway_research LANGUAGES C CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Windows permanent-gateway research requires Windows")
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

set(STABLE_ENTRY_ENGINE
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_experiment.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_atomic.cpp")

add_executable(windows_stable_entry_p0_diagnostic
        ${STABLE_ENTRY_ENGINE}
        "${SPARK_ROOT}/tests/native/alloc/permanent_gateway/windows_stable_entry_p0_diagnostic.cpp")
target_include_directories(windows_stable_entry_p0_diagnostic PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_stable_entry_p0_diagnostic PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_stable_entry_p0_diagnostic PRIVATE /W4 /WX /UNDEBUG)
target_link_libraries(windows_stable_entry_p0_diagnostic PRIVATE funchook-static)

add_executable(windows_stable_entry_p0_context_diagnostic
        ${STABLE_ENTRY_ENGINE}
        "${SPARK_ROOT}/tests/native/alloc/permanent_gateway/windows_stable_entry_p0_context_diagnostic.cpp")
target_include_directories(windows_stable_entry_p0_context_diagnostic PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_stable_entry_p0_context_diagnostic PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_stable_entry_p0_context_diagnostic PRIVATE /W4 /WX /UNDEBUG)
target_link_libraries(windows_stable_entry_p0_context_diagnostic PRIVATE funchook-static)

add_library(permanent_gateway_engine STATIC
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_experiment.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_atomic.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_permanent_gateway_experiment.cpp")
target_include_directories(permanent_gateway_engine PUBLIC "${SPARK_ROOT}/src")
target_compile_definitions(permanent_gateway_engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(permanent_gateway_engine PRIVATE /W4 /WX)
target_link_libraries(permanent_gateway_engine PUBLIC funchook-static)

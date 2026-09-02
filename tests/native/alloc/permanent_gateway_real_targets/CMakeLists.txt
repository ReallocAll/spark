cmake_minimum_required(VERSION 3.24)
project(spark_windows_permanent_gateway_real_targets LANGUAGES C CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Windows permanent-gateway real-target probe requires Windows")
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

get_filename_component(SPARK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../" ABSOLUTE)

set(PERMANENT_GATEWAY_SOURCE
        "${SPARK_ROOT}/src/native/alloc/windows_permanent_gateway_experiment.cpp")
set_source_files_properties(${PERMANENT_GATEWAY_SOURCE}
        PROPERTIES COMPILE_DEFINITIONS "atomicCompareExchange8=atomicCompareExchange8Quiesced")

add_executable(windows_permanent_gateway_real_targets
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_experiment.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_stable_entry_atomic.cpp"
        "${SPARK_ROOT}/src/native/alloc/windows_permanent_gateway_guarded_publish.cpp"
        ${PERMANENT_GATEWAY_SOURCE}
        windows_permanent_gateway_real_targets.cpp)
target_include_directories(windows_permanent_gateway_real_targets PRIVATE "${SPARK_ROOT}/src")
target_compile_definitions(windows_permanent_gateway_real_targets PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_permanent_gateway_real_targets PRIVATE /W4 /WX /wd4324 /UNDEBUG)
target_link_libraries(windows_permanent_gateway_real_targets PRIVATE funchook-static)

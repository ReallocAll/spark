cmake_minimum_required(VERSION 3.24)
project(spark_windows_etw_heap_probe LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "Windows ETW heap probe requires Windows")
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(windows_etw_heap_fixture etw_heap_fixture.cpp)
target_compile_definitions(windows_etw_heap_fixture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(windows_etw_heap_fixture PRIVATE /W4 /WX /UNDEBUG)

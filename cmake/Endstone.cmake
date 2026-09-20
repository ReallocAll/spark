# --- Endstone: header-only API + endstone_add_plugin(), fetched standalone ---
# Endstone's root CMake returns right after its include/ target when consumed as a
# subproject, so this only pulls the headers, not the server. For local iteration,
# pass -DFETCHCONTENT_SOURCE_DIR_ENDSTONE=<path> to reuse an existing checkout.
set(ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY "https://github.com/EndstoneMC/endstone.git" CACHE STRING
        "Repository providing the Endstone public plugin API")
set(ENDSTONE_SPARK_ENDSTONE_GIT_TAG "v0.11.11" CACHE STRING
        "Git ref providing the Endstone v0.11.11 public plugin API")
FetchContent_Declare(endstone
        GIT_REPOSITORY ${ENDSTONE_SPARK_ENDSTONE_GIT_REPOSITORY}
        GIT_TAG ${ENDSTONE_SPARK_ENDSTONE_GIT_TAG})
FetchContent_MakeAvailable(endstone)

# PAPI is optional at runtime; only its public headers are fetched.
set(ENDSTONE_SPARK_PAPI_GIT_REPOSITORY "https://github.com/EndstoneMC/papi.git" CACHE STRING
        "Repository providing the public Endstone PAPI headers")
set(ENDSTONE_SPARK_PAPI_GIT_TAG "v0.1.0" CACHE STRING
        "Git ref providing the public Endstone PAPI headers")
FetchContent_Declare(endstone_papi_headers
        GIT_REPOSITORY ${ENDSTONE_SPARK_PAPI_GIT_REPOSITORY}
        GIT_TAG ${ENDSTONE_SPARK_PAPI_GIT_TAG})
FetchContent_MakeAvailable(endstone_papi_headers)
add_library(spark_papi_headers INTERFACE)
target_include_directories(spark_papi_headers INTERFACE "${endstone_papi_headers_SOURCE_DIR}/include")
target_link_libraries(spark_papi_headers INTERFACE endstone::endstone)

if (WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    enable_language(RC)
    configure_file(src/platform/endstone/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" @ONLY)
endif ()

function(spark_add_endstone_plugin)
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
        src/platform/endstone/plugin.cpp)
    target_include_directories(spark PRIVATE src)
    target_link_libraries(spark PRIVATE spark_application spark_papi_integration)
    target_compile_definitions(spark PRIVATE ENDSTONE_SPARK_ENDSTONE_API_0_11=1)
    if (TARGET spark_linux_permanent_gateway)
        install(TARGETS spark LIBRARY DESTINATION . COMPONENT spark_linux_runtime)
        add_custom_target(spark_linux_archive
                COMMAND ${CMAKE_COMMAND}
                        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
                        "-DCONFIG=$<CONFIG>"
                        "-DOUTPUT_DIR=$<TARGET_FILE_DIR:spark>"
                        "-DPYTHON=${Python3_EXECUTABLE}"
                        "-DVERIFIER=${CMAKE_CURRENT_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageLinuxRuntime.cmake"
                DEPENDS spark VERBATIM)
    endif ()
    if (WIN32)
        target_sources(spark PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
    endif ()
# Localize every symbol from the static archives (zlib, cpptrace, libunwind, libc++)
# so they can't interpose with Endstone core's own copies in the BDS process. Only
# init_endstone_plugin (default visibility via the macro) stays exported.
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(spark PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
        target_link_options(spark PRIVATE "-Wl,--exclude-libs,ALL")
        if (TARGET spark_linux_permanent_gateway)
            add_custom_command(TARGET spark POST_BUILD
                    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py"
                            elf "$<TARGET_FILE:spark>" VERBATIM)
        endif ()
    endif ()
endfunction()

include_guard(GLOBAL)

include(FetchContent)
enable_language(C)

# Pin the decoder revision so dependency changes remain independent from
# instruction-decoding behavior changes.
set(ENDSTONE_SPARK_DISTORM_GIT_TAG "ab59d6e193948cfa5d1482fb6c7e64870e9e93b9" CACHE STRING
        "Pinned distorm revision used by Spark's x86-64 symbol guessers")

function(endstone_spark_add_distorm)
    if (TARGET distorm)
        return()
    endif ()

    FetchContent_Declare(distorm_source
            GIT_REPOSITORY https://github.com/gdabah/distorm.git
            GIT_TAG ${ENDSTONE_SPARK_DISTORM_GIT_TAG})
    FetchContent_MakeAvailable(distorm_source)

    set(distorm_src_dir "${distorm_source_SOURCE_DIR}/src")
    add_library(distorm STATIC
            "${distorm_src_dir}/decoder.c"
            "${distorm_src_dir}/distorm.c"
            "${distorm_src_dir}/instructions.c"
            "${distorm_src_dir}/insts.c"
            "${distorm_src_dir}/mnemonics.c"
            "${distorm_src_dir}/operands.c"
            "${distorm_src_dir}/prefix.c"
            "${distorm_src_dir}/textdefs.c")
    set_target_properties(distorm PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(distorm PUBLIC "${distorm_source_SOURCE_DIR}/include")

    if (MSVC)
        # prefix.c contains non-ASCII text that triggers MSVC warning C4819.
        set_source_files_properties("${distorm_src_dir}/prefix.c" PROPERTIES COMPILE_OPTIONS "/wd4819")
    elseif (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(distorm PRIVATE -fvisibility=hidden)
    endif ()
endfunction()

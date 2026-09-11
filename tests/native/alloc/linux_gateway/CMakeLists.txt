set(gateway_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/runtime")
get_target_property(gateway_sources spark_linux_permanent_gateway SOURCES)
list(TRANSFORM gateway_sources PREPEND "${CMAKE_SOURCE_DIR}/")
add_library(spark_linux_gateway_test_backend STATIC ${gateway_sources})
target_include_directories(spark_linux_gateway_test_backend PUBLIC "${CMAKE_SOURCE_DIR}/src"
        PRIVATE "${CMAKE_BINARY_DIR}/generated" "${SPARK_LIBUNWIND_INCLUDE_DIR}")
target_compile_definitions(spark_linux_gateway_test_backend PUBLIC SPARK_GATEWAY_TESTING=1
        PRIVATE _GNU_SOURCE SPARK_GATEWAY_HANDLE_TESTING)
target_compile_options(spark_linux_gateway_test_backend PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(spark_linux_gateway_test_backend PUBLIC cpptrace::cpptrace libunwind::unwind ${CMAKE_DL_LIBS} pthread)
set_target_properties(spark_linux_gateway_test_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(spark_linux_gateway_fixture SHARED fixture.cpp)
target_include_directories(spark_linux_gateway_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src" "${SPARK_LIBUNWIND_INCLUDE_DIR}")
target_link_libraries(spark_linux_gateway_fixture PRIVATE spark_linux_gateway_test_backend)
target_link_options(spark_linux_gateway_fixture PRIVATE "-Wl,--exclude-libs,ALL")
target_compile_options(spark_linux_gateway_fixture PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
        -fno-optimize-sibling-calls)
set_target_properties(spark_linux_gateway_fixture PROPERTIES PREFIX "" OUTPUT_NAME endstone_spark
        LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
add_library(spark_linux_gateway_production_fixture SHARED fixture.cpp)
target_include_directories(spark_linux_gateway_production_fixture PRIVATE "${SPARK_LIBUNWIND_INCLUDE_DIR}")
target_link_libraries(spark_linux_gateway_production_fixture PRIVATE spark_linux_permanent_gateway cpptrace::cpptrace)
target_link_options(spark_linux_gateway_production_fixture PRIVATE "-Wl,--exclude-libs,ALL")
target_compile_options(spark_linux_gateway_production_fixture PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
        -fno-optimize-sibling-calls)
set_target_properties(spark_linux_gateway_production_fixture PROPERTIES PREFIX "" OUTPUT_NAME endstone_spark
        LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}/production")
foreach (domain A B)
    add_library(spark_linux_gateway_provider_${domain} SHARED provider.cpp)
    target_compile_definitions(spark_linux_gateway_provider_${domain} PRIVATE "PROVIDER_DOMAIN='${domain}'")
    set_target_properties(spark_linux_gateway_provider_${domain} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
    add_library(spark_linux_gateway_consumer_${domain} SHARED consumer.cpp)
    target_link_libraries(spark_linux_gateway_consumer_${domain} PRIVATE spark_linux_gateway_provider_${domain})
    set_target_properties(spark_linux_gateway_consumer_${domain} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
endforeach ()
add_library(spark_linux_gateway_provider_C SHARED provider.cpp)
target_compile_definitions(spark_linux_gateway_provider_C PRIVATE "PROVIDER_DOMAIN='C'")
set_target_properties(spark_linux_gateway_provider_C PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
target_link_libraries(spark_linux_gateway_provider_A PRIVATE "-Wl,--no-as-needed" spark_linux_gateway_provider_C)

add_library(spark_linux_gateway_dependent_provider SHARED provider.cpp provider_dependency.cpp)
target_compile_definitions(spark_linux_gateway_dependent_provider PRIVATE "PROVIDER_DOMAIN='D'")
target_link_libraries(spark_linux_gateway_dependent_provider PRIVATE spark_linux_gateway_fixture)
add_library(spark_linux_gateway_unlinked_provider SHARED provider.cpp provider_unlinked.cpp)
target_compile_definitions(spark_linux_gateway_unlinked_provider PRIVATE "PROVIDER_DOMAIN='U'")
add_library(spark_linux_gateway_lookup_provider SHARED provider.cpp provider_lookup.cpp)
target_compile_options(spark_linux_gateway_lookup_provider PRIVATE -fno-optimize-sibling-calls)
target_compile_definitions(spark_linux_gateway_lookup_provider PRIVATE "PROVIDER_DOMAIN='L'")
target_link_libraries(spark_linux_gateway_lookup_provider PRIVATE "-Wl,--no-as-needed"
        spark_linux_gateway_provider_B ${CMAKE_DL_LIBS})
foreach (provider dependent unlinked lookup)
    set_target_properties(spark_linux_gateway_${provider}_provider PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
endforeach ()
foreach (provider unlinked lookup)
    add_test(NAME spark_linux_gateway_${provider}_artifact
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py" provider
                    "$<TARGET_FILE:spark_linux_gateway_${provider}_provider>")
endforeach ()

foreach (host gcc llvm)
    add_executable(spark_linux_gateway_test_${host} permanent_gateway_test.cpp)
    target_include_directories(spark_linux_gateway_test_${host} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(spark_linux_gateway_test_${host} PRIVATE _GNU_SOURCE SPARK_GATEWAY_TESTING=1
            SPARK_GATEWAY_HOST="${host}"
            SPARK_GATEWAY_PRODUCTION_FIXTURE="$<TARGET_FILE:spark_linux_gateway_production_fixture>"
            SPARK_GATEWAY_FIXTURE="$<TARGET_FILE:spark_linux_gateway_fixture>"
            SPARK_GATEWAY_PROVIDER="$<TARGET_FILE:spark_linux_gateway_provider_A>"
            SPARK_GATEWAY_TRANSITIVE_PROVIDER="$<TARGET_FILE:spark_linux_gateway_provider_C>"
            SPARK_GATEWAY_DEPENDENT_PROVIDER="$<TARGET_FILE:spark_linux_gateway_dependent_provider>"
            SPARK_GATEWAY_UNLINKED_PROVIDER="$<TARGET_FILE:spark_linux_gateway_unlinked_provider>"
            SPARK_GATEWAY_LOOKUP_PROVIDER="$<TARGET_FILE:spark_linux_gateway_lookup_provider>"
            SPARK_GATEWAY_DYNAMIC_PROVIDER="$<TARGET_FILE:spark_linux_gateway_provider_B>")
    target_link_libraries(spark_linux_gateway_test_${host} PRIVATE "-Wl,--no-as-needed" spark_linux_gateway_provider_A)
    if (host STREQUAL "llvm")
        target_link_libraries(spark_linux_gateway_test_${host} PRIVATE c++ c++abi unwind gcc_s)
    else ()
        target_link_libraries(spark_linux_gateway_test_${host} PRIVATE gcc_s c++ c++abi unwind)
    endif ()
    target_link_libraries(spark_linux_gateway_test_${host} PRIVATE ${CMAKE_DL_LIBS} pthread)
    target_link_options(spark_linux_gateway_test_${host} PRIVATE -rdynamic)
    add_dependencies(spark_linux_gateway_test_${host} spark_linux_gateway_fixture spark_linux_gateway_provider_B
            spark_linux_gateway_production_fixture spark_linux_gateway_dependent_provider
            spark_linux_gateway_unlinked_provider spark_linux_gateway_lookup_provider)
    foreach (mode basic gates tls exhaustion provider registry rollback cancellation quarantine)
        add_test(NAME spark_linux_permanent_gateway_${host}_${mode}
                COMMAND spark_linux_gateway_test_${host} ${mode})
        set_tests_properties(spark_linux_permanent_gateway_${host}_${mode} PROPERTIES TIMEOUT 120)
    endforeach ()
    foreach (mode original_null original_anonymous original_spark original_nonexecutable
            provider_dependent provider_transitive provider_unlinked_now provider_unlinked_lazy
            provider_lookup_unused provider_lookup_resolved)
        add_test(NAME spark_linux_permanent_gateway_${host}_${mode} COMMAND spark_linux_gateway_test_${host} ${mode})
        set_tests_properties(spark_linux_permanent_gateway_${host}_${mode} PROPERTIES TIMEOUT 30)
    endforeach ()
    add_test(NAME spark_linux_gateway_${host}_arena_dump COMMAND spark_linux_gateway_test_${host} arena
            "${CMAKE_BINARY_DIR}/gateway-arena-${host}")
    set_tests_properties(spark_linux_gateway_${host}_arena_dump PROPERTIES FIXTURES_SETUP "arena_${host}")
    add_test(NAME spark_linux_gateway_${host}_arena_verify
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py" arena
                    "${CMAKE_BINARY_DIR}/gateway-arena-${host}")
    set_tests_properties(spark_linux_gateway_${host}_arena_verify PROPERTIES FIXTURES_REQUIRED "arena_${host}")
endforeach ()

add_executable(spark_linux_gateway_mixed_test permanent_gateway_test.cpp)
foreach (property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES LINK_OPTIONS)
    get_target_property(mixed_${property} spark_linux_gateway_test_gcc ${property})
    set_property(TARGET spark_linux_gateway_mixed_test PROPERTY ${property} "${mixed_${property}}")
endforeach ()
target_compile_definitions(spark_linux_gateway_mixed_test PRIVATE SPARK_GATEWAY_MIXED_HOST=1)
add_dependencies(spark_linux_gateway_mixed_test spark_linux_gateway_test_gcc)
add_test(NAME spark_linux_gateway_mixed_host_rejection COMMAND spark_linux_gateway_mixed_test mixed)

add_executable(spark_linux_gateway_nonpie_test permanent_gateway_test.cpp)
foreach (property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_LIBRARIES)
    get_target_property(nonpie_${property} spark_linux_gateway_test_gcc ${property})
    set_property(TARGET spark_linux_gateway_nonpie_test PROPERTY ${property} "${nonpie_${property}}")
endforeach ()
target_link_options(spark_linux_gateway_nonpie_test PRIVATE -no-pie -rdynamic)
set_target_properties(spark_linux_gateway_nonpie_test PROPERTIES OUTPUT_NAME spark_linux_gateway_test_nonpie)
add_dependencies(spark_linux_gateway_nonpie_test spark_linux_gateway_test_gcc)
foreach (mode basic provider)
    add_test(NAME spark_linux_gateway_nonpie_${mode} COMMAND spark_linux_gateway_nonpie_test ${mode})
endforeach ()

add_library(spark_linux_admission_parser_fixture SHARED provider.cpp fixture.cpp)
target_compile_definitions(spark_linux_admission_parser_fixture PRIVATE "PROVIDER_DOMAIN='P'" SPARK_GATEWAY_PARSER_FIXTURE=1)
target_link_options(spark_linux_admission_parser_fixture PRIVATE -nostartfiles "-Wl,--exclude-libs,ALL"
        "-Wl,-z,relro,-z,now" "-Wl,--hash-style=both")
add_executable(spark_linux_admission_parser_test admission_parser_test.cpp)
target_include_directories(spark_linux_admission_parser_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(spark_linux_admission_parser_test PRIVATE _GNU_SOURCE)
target_link_libraries(spark_linux_admission_parser_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(spark_linux_admission_parser_test spark_linux_admission_parser_fixture)
add_library(spark_linux_dependency_fixture SHARED dependency_fixture.cpp)
add_executable(spark_linux_dependency_root dependency_fixture.cpp)
target_compile_definitions(spark_linux_dependency_root PRIVATE SPARK_DEPENDENCY_ROOT)
target_link_options(spark_linux_dependency_root PRIVATE -no-pie)
add_test(NAME spark_linux_dependency_policy
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/dependency_policy_test.py"
                "$<TARGET_FILE:spark_linux_admission_parser_test>" "$<TARGET_FILE:spark_linux_dependency_fixture>"
                "$<TARGET_FILE:spark_linux_dependency_root>")
add_test(NAME spark_linux_admission_malformed
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/malformed_admission_test.py"
                "$<TARGET_FILE:spark_linux_admission_parser_test>" "$<TARGET_FILE:spark_linux_admission_parser_fixture>")
add_test(NAME spark_linux_archive_rejections
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py" negative)

add_executable(spark_linux_elf_domain_test elf_domain_test.cpp
        "${CMAKE_SOURCE_DIR}/src/native/alloc/elf_import_hooks.cpp")
target_include_directories(spark_linux_elf_domain_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(spark_linux_elf_domain_test PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(spark_linux_elf_domain_test PRIVATE _GNU_SOURCE
        SPARK_GATEWAY_PROVIDER_A="$<TARGET_FILE:spark_linux_gateway_provider_A>"
        SPARK_GATEWAY_PROVIDER_B="$<TARGET_FILE:spark_linux_gateway_provider_B>"
        SPARK_GATEWAY_CONSUMER_A="$<TARGET_FILE:spark_linux_gateway_consumer_A>"
        SPARK_GATEWAY_CONSUMER_B="$<TARGET_FILE:spark_linux_gateway_consumer_B>")
add_dependencies(spark_linux_elf_domain_test spark_linux_gateway_consumer_A spark_linux_gateway_consumer_B)
add_test(NAME spark_linux_elf_domain_test COMMAND spark_linux_elf_domain_test)
add_library(spark_linux_global_interposer SHARED global_interposer.cpp)
add_dependencies(spark_linux_elf_domain_test spark_linux_global_interposer)
add_test(NAME spark_linux_global_interposer_test
        COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=$<TARGET_FILE:spark_linux_global_interposer>"
                SPARK_EXPECT_INTERPOSER=1 "$<TARGET_FILE:spark_linux_elf_domain_test>")
if (ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS)
    include(sampler_fixture.cmake)
endif ()

if (TARGET spark_native)
    add_executable(spark_linux_sampler_churn_test sampler_churn_test.cpp)
    target_link_libraries(spark_linux_sampler_churn_test PRIVATE spark_native)
    set_target_properties(spark_linux_sampler_churn_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_test(NAME spark_linux_sampler_churn_test COMMAND spark_linux_sampler_churn_test)
    set_tests_properties(spark_linux_sampler_churn_test PROPERTIES TIMEOUT 90)
endif ()

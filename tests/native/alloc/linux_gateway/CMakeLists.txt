set(gateway_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/runtime")
option(ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS "Build the production Linux sampler DSO lifecycle fixture" OFF)
if (ENDSTONE_SPARK_GATEWAY_SAMPLER_TESTS)
    include("${CMAKE_CURRENT_LIST_DIR}/sampler_fixture.cmake")
endif ()
set(gateway_test_identity "${spark_gateway_hash}:fixture:capacity64:gates")
string(SHA256 gateway_test_hash "${gateway_test_identity}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/spark_gateway_compatibility.h"
        CONTENT "#define SPARK_GATEWAY_COMPATIBILITY \"${gateway_test_hash}\"\n#define SPARK_GATEWAY_EXPECTED_CAPACITY 64\n")
add_library(spark_linux_gateway_test_helper SHARED "${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.cpp")
target_include_directories(spark_linux_gateway_test_helper PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(spark_linux_gateway_test_helper PRIVATE _GNU_SOURCE SPARK_GATEWAY_TESTING=1 SPARK_GATEWAY_CAPACITY=64 SPARK_GATEWAY_HANDLE_TESTING)
target_compile_options(spark_linux_gateway_test_helper PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden
        -fno-optimize-sibling-calls -fno-omit-frame-pointer -fasynchronous-unwind-tables)
target_link_libraries(spark_linux_gateway_test_helper PRIVATE ${CMAKE_DL_LIBS} pthread)
target_link_options(spark_linux_gateway_test_helper PRIVATE
        "-Wl,--hash-style=both"
        -nostartfiles "-Wl,--exclude-libs,ALL" "-Wl,-z,relro,-z,now"
        "-Wl,--version-script=${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.map")
set_target_properties(spark_linux_gateway_test_helper PROPERTIES OUTPUT_NAME spark_allocation_gateway_v1
        LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}/.spark-native")

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/incompatible/spark_gateway_compatibility.h"
        CONTENT "#define SPARK_GATEWAY_COMPATIBILITY \"incompatible-${gateway_test_hash}\"\n#define SPARK_GATEWAY_EXPECTED_CAPACITY 64\n")
add_library(spark_linux_gateway_incompatible SHARED "${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.cpp")
target_include_directories(spark_linux_gateway_incompatible PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/incompatible")
target_compile_definitions(spark_linux_gateway_incompatible PRIVATE _GNU_SOURCE SPARK_GATEWAY_CAPACITY=64)
target_compile_options(spark_linux_gateway_incompatible PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(spark_linux_gateway_incompatible PRIVATE ${CMAKE_DL_LIBS} pthread)
target_link_options(spark_linux_gateway_incompatible PRIVATE
        -nostartfiles "-Wl,--exclude-libs,ALL" "-Wl,-z,relro,-z,now"
        "-Wl,--version-script=${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.map")
set_target_properties(spark_linux_gateway_incompatible PROPERTIES OUTPUT_NAME spark_allocation_gateway_v1
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/incompatible-runtime/.spark-native")

add_library(spark_linux_gateway_external_import SHARED "${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.cpp")
target_include_directories(spark_linux_gateway_external_import PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(spark_linux_gateway_external_import PRIVATE _GNU_SOURCE SPARK_GATEWAY_CAPACITY=64
        SPARK_GATEWAY_EXTERNAL_IMPORT_TESTING)
target_compile_options(spark_linux_gateway_external_import PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(spark_linux_gateway_external_import PRIVATE ${CMAKE_DL_LIBS} pthread)
target_link_options(spark_linux_gateway_external_import PRIVATE -nostartfiles "-Wl,--exclude-libs,ALL" "-Wl,-z,relro,-z,now"
        "-Wl,--version-script=${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.map")
set_target_properties(spark_linux_gateway_external_import PROPERTIES OUTPUT_NAME spark_allocation_gateway_v1
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/external-import-runtime/.spark-native")
add_library(spark_linux_gateway_ifunc_provider SHARED ifunc_provider.cpp)
add_library(spark_linux_gateway_ifunc_helper SHARED "${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.cpp")
target_include_directories(spark_linux_gateway_ifunc_helper PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(spark_linux_gateway_ifunc_helper PRIVATE _GNU_SOURCE SPARK_GATEWAY_CAPACITY=64
        SPARK_GATEWAY_EXTERNAL_IMPORT_TESTING)
target_compile_options(spark_linux_gateway_ifunc_helper PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(spark_linux_gateway_ifunc_helper PRIVATE spark_linux_gateway_ifunc_provider ${CMAKE_DL_LIBS} pthread)
target_link_options(spark_linux_gateway_ifunc_helper PRIVATE -nostartfiles "-Wl,--exclude-libs,ALL" "-Wl,-z,relro,-z,now"
        "-Wl,--version-script=${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway.map")
set_target_properties(spark_linux_gateway_ifunc_helper PROPERTIES OUTPUT_NAME spark_allocation_gateway_v1
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ifunc-runtime/.spark-native")

add_library(spark_linux_gateway_fixture SHARED fixture.cpp
        "${CMAKE_SOURCE_DIR}/src/native/alloc/linux_allocation_gateway_client.cpp")
target_include_directories(spark_linux_gateway_fixture PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(spark_linux_gateway_fixture PRIVATE _GNU_SOURCE SPARK_GATEWAY_HANDLE_TESTING)
target_compile_options(spark_linux_gateway_fixture PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
        -fno-optimize-sibling-calls -fno-omit-frame-pointer)
target_link_libraries(spark_linux_gateway_fixture PRIVATE ${CMAKE_DL_LIBS} pthread)
set_target_properties(spark_linux_gateway_fixture PROPERTIES PREFIX "" OUTPUT_NAME endstone_spark
        LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
add_dependencies(spark_linux_gateway_fixture spark_linux_gateway_test_helper)

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
add_library(spark_linux_gateway_unlinked_provider SHARED provider.cpp provider_unlinked.cpp)
target_compile_definitions(spark_linux_gateway_unlinked_provider PRIVATE "PROVIDER_DOMAIN='U'")
set_target_properties(spark_linux_gateway_unlinked_provider PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")
add_library(spark_linux_gateway_lookup_provider SHARED provider.cpp provider_lookup.cpp)
target_compile_definitions(spark_linux_gateway_lookup_provider PRIVATE "PROVIDER_DOMAIN='L'")
target_compile_options(spark_linux_gateway_lookup_provider PRIVATE -fno-optimize-sibling-calls)
target_link_libraries(spark_linux_gateway_lookup_provider PRIVATE "-Wl,--no-as-needed" spark_linux_gateway_provider_B ${CMAKE_DL_LIBS})
set_target_properties(spark_linux_gateway_lookup_provider PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")

add_library(spark_linux_gateway_dependent_provider SHARED provider.cpp provider_dependency.cpp)
target_compile_definitions(spark_linux_gateway_dependent_provider PRIVATE "PROVIDER_DOMAIN='D'")
target_link_libraries(spark_linux_gateway_dependent_provider PRIVATE spark_linux_gateway_fixture)
set_target_properties(spark_linux_gateway_dependent_provider PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${gateway_fixture_root}")

add_executable(spark_linux_gateway_test gateway_test.cpp)
target_include_directories(spark_linux_gateway_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_options(spark_linux_gateway_test PRIVATE -fPIC -fno-omit-frame-pointer -fno-optimize-sibling-calls)
target_link_libraries(spark_linux_gateway_test PRIVATE "-Wl,--no-as-needed" spark_linux_gateway_provider_A
        spark_linux_gateway_ifunc_provider ${CMAKE_DL_LIBS} pthread)
target_link_options(spark_linux_gateway_test PRIVATE -rdynamic)
target_compile_definitions(spark_linux_gateway_test PRIVATE _GNU_SOURCE
        SPARK_GATEWAY_FIXTURE="$<TARGET_FILE:spark_linux_gateway_fixture>"
        SPARK_GATEWAY_HELPER="$<TARGET_FILE:spark_linux_gateway_test_helper>"
        SPARK_GATEWAY_INCOMPATIBLE="$<TARGET_FILE:spark_linux_gateway_incompatible>"
        SPARK_GATEWAY_DEPENDENT_PROVIDER="$<TARGET_FILE:spark_linux_gateway_dependent_provider>"
        SPARK_GATEWAY_DYNAMIC_PROVIDER="$<TARGET_FILE:spark_linux_gateway_provider_B>"
        SPARK_GATEWAY_TRANSITIVE_PROVIDER="$<TARGET_FILE:spark_linux_gateway_provider_C>"
        SPARK_GATEWAY_UNLINKED_PROVIDER="$<TARGET_FILE:spark_linux_gateway_unlinked_provider>"
        SPARK_GATEWAY_LOOKUP_PROVIDER="$<TARGET_FILE:spark_linux_gateway_lookup_provider>"
        SPARK_GATEWAY_EXTERNAL_IMPORT="$<TARGET_FILE:spark_linux_gateway_external_import>"
        SPARK_GATEWAY_IFUNC_HELPER="$<TARGET_FILE:spark_linux_gateway_ifunc_helper>"
        SPARK_GATEWAY_PROVIDER="$<TARGET_FILE:spark_linux_gateway_provider_A>")
add_dependencies(spark_linux_gateway_test spark_linux_gateway_fixture spark_linux_gateway_provider_A spark_linux_gateway_provider_B)
add_dependencies(spark_linux_gateway_test spark_linux_gateway_incompatible)
add_dependencies(spark_linux_gateway_test spark_linux_gateway_dependent_provider)
add_dependencies(spark_linux_gateway_test spark_linux_gateway_unlinked_provider spark_linux_gateway_external_import)
add_dependencies(spark_linux_gateway_test spark_linux_gateway_ifunc_helper)
add_dependencies(spark_linux_gateway_test spark_linux_gateway_lookup_provider)
add_executable(spark_linux_gateway_nonpie_test gateway_test.cpp)
foreach (property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
    get_target_property(nonpie_${property} spark_linux_gateway_test ${property})
    set_property(TARGET spark_linux_gateway_nonpie_test PROPERTY ${property} "${nonpie_${property}}")
endforeach ()
target_link_options(spark_linux_gateway_nonpie_test PRIVATE -no-pie -rdynamic)
target_compile_definitions(spark_linux_gateway_nonpie_test PRIVATE SPARK_EXPECT_NONPIE)
set_target_properties(spark_linux_gateway_nonpie_test PROPERTIES OUTPUT_NAME spark_linux_gateway_test_nonpie)
add_dependencies(spark_linux_gateway_nonpie_test spark_linux_gateway_test)
foreach (mode transitive ifunc_positive)
    add_test(NAME spark_linux_gateway_nonpie_${mode} COMMAND spark_linux_gateway_nonpie_test ${mode})
endforeach ()
foreach (mode lifetime tls budget layout plain concurrent incompatible namespace provider_rejection late_closed multiple dynamic_rejection binding_rejection)
    add_test(NAME spark_linux_gateway_${mode} COMMAND spark_linux_gateway_test ${mode})
    set_tests_properties(spark_linux_gateway_${mode} PROPERTIES TIMEOUT 90)
endforeach ()
foreach (mode transitive unlinked unlinked_lazy provider_lookup dynamic_transitive external_import external_preloaded ifunc_positive ifunc_spark)
    add_test(NAME spark_linux_gateway_${mode} COMMAND spark_linux_gateway_test ${mode})
endforeach ()
foreach (provider unlinked lookup)
    add_test(NAME spark_linux_gateway_${provider}_artifact
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py" provider
                "$<TARGET_FILE:spark_linux_gateway_${provider}_provider>")
endforeach ()
foreach (point 1 2 3 10 11 12 13 14 15 16 20)
    add_test(NAME spark_linux_gateway_handle_fault_${point} COMMAND spark_linux_gateway_test handle_fault ${point})
endforeach ()
add_executable(spark_linux_admission_parser_test admission_parser_test.cpp)
target_include_directories(spark_linux_admission_parser_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(spark_linux_admission_parser_test PRIVATE _GNU_SOURCE)
target_link_libraries(spark_linux_admission_parser_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(spark_linux_admission_parser_test spark_linux_gateway_test_helper)
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
                "$<TARGET_FILE:spark_linux_admission_parser_test>" "$<TARGET_FILE:spark_linux_gateway_test_helper>")

add_executable(spark_linux_elf_domain_test elf_domain_test.cpp "${CMAKE_SOURCE_DIR}/src/native/alloc/elf_import_hooks.cpp")
target_include_directories(spark_linux_elf_domain_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(spark_linux_elf_domain_test PRIVATE _GNU_SOURCE
        SPARK_GATEWAY_PROVIDER_A="$<TARGET_FILE:spark_linux_gateway_provider_A>"
        SPARK_GATEWAY_PROVIDER_B="$<TARGET_FILE:spark_linux_gateway_provider_B>"
        SPARK_GATEWAY_CONSUMER_A="$<TARGET_FILE:spark_linux_gateway_consumer_A>"
        SPARK_GATEWAY_CONSUMER_B="$<TARGET_FILE:spark_linux_gateway_consumer_B>")
target_link_libraries(spark_linux_elf_domain_test PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(spark_linux_elf_domain_test spark_linux_gateway_consumer_A spark_linux_gateway_consumer_B)
add_test(NAME spark_linux_elf_domain_test COMMAND spark_linux_elf_domain_test)
add_library(spark_linux_global_interposer SHARED global_interposer.cpp)
add_dependencies(spark_linux_elf_domain_test spark_linux_global_interposer)
add_test(NAME spark_linux_global_interposer_test
        COMMAND ${CMAKE_COMMAND} -E env "LD_PRELOAD=$<TARGET_FILE:spark_linux_global_interposer>"
                SPARK_EXPECT_INTERPOSER=1 "$<TARGET_FILE:spark_linux_elf_domain_test>")
add_test(NAME spark_linux_archive_rejection
        COMMAND ${Python3_EXECUTABLE} -O "${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py" negative)

if (ENDSTONE_SPARK_GATEWAY_TEST_ONLY)
    install(TARGETS spark_linux_gateway_fixture LIBRARY DESTINATION . COMPONENT spark_linux_runtime)
    install(TARGETS spark_linux_gateway_test_helper LIBRARY DESTINATION .spark-native COMPONENT spark_linux_runtime)
    add_custom_target(spark_linux_archive
            COMMAND ${CMAKE_COMMAND} "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DCONFIG=$<CONFIG>"
                    "-DOUTPUT_DIR=${gateway_fixture_root}" "-DPYTHON=${Python3_EXECUTABLE}"
                    "-DVERIFIER=${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py"
                    -P "${CMAKE_SOURCE_DIR}/cmake/PackageLinuxRuntime.cmake"
            DEPENDS spark_linux_gateway_fixture spark_linux_gateway_test_helper VERBATIM)
    add_test(NAME spark_linux_gateway_archive
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/native/alloc/verify_linux_gateway.py" exercise
                    "${CMAKE_COMMAND}" "${CMAKE_BINARY_DIR}"
                    "${gateway_fixture_root}/endstone_spark-linux-x86_64.tar.gz"
                    "$<TARGET_FILE:spark_linux_gateway_test>")
endif ()

if (TARGET spark_native)
    add_executable(spark_linux_sampler_churn_test sampler_churn_test.cpp)
    target_link_libraries(spark_linux_sampler_churn_test PRIVATE spark_native)
    set_target_properties(spark_linux_sampler_churn_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(spark_linux_sampler_churn_test spark_allocation_gateway_v1)
    add_test(NAME spark_linux_sampler_churn_test COMMAND spark_linux_sampler_churn_test)
    set_tests_properties(spark_linux_sampler_churn_test PROPERTIES TIMEOUT 90)
endif ()

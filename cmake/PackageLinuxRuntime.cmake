cmake_minimum_required(VERSION 3.29)

foreach (required BUILD_DIR CONFIG OUTPUT_DIR PYTHON VERIFIER)
    if (NOT DEFINED ${required})
        message(FATAL_ERROR "Missing Linux packaging argument: ${required}")
    endif ()
endforeach ()

set(stage "${OUTPUT_DIR}/spark-linux-runtime-stage")
if (IS_SYMLINK "${stage}")
    message(FATAL_ERROR "Linux runtime staging root is a symlink")
endif ()
if (EXISTS "${stage}")
    file(GLOB_RECURSE previous LIST_DIRECTORIES TRUE "${stage}/*")
    foreach (entry IN LISTS previous)
        if (IS_SYMLINK "${entry}")
            message(FATAL_ERROR "Linux runtime staging contains a symlink: ${entry}")
        endif ()
    endforeach ()
endif ()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}"
        --component spark_linux_runtime --prefix "${stage}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PYTHON}" "${VERIFIER}" stage "${stage}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${OUTPUT_DIR}/endstone_spark-linux-x86_64.tar.gz"
        --format=gnutar endstone_spark.so .spark-native/libspark_allocation_gateway_v1.so
        WORKING_DIRECTORY "${stage}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PYTHON}" "${VERIFIER}" archive "${OUTPUT_DIR}/endstone_spark-linux-x86_64.tar.gz"
        COMMAND_ERROR_IS_FATAL ANY)

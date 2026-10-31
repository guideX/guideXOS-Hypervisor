if(NOT DEFINED DEMO OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "DEMO and OUTPUT_DIR are required")
endif()

set(input_file "${OUTPUT_DIR}/ppc64-phase15-stdio-input.txt")
set(output_file "${OUTPUT_DIR}/ppc64-phase15-stdio-output.txt")
set(error_file "${OUTPUT_DIR}/ppc64-phase15-stdio-error.txt")
set(expected "phase15 redirected guest echo\n")
file(WRITE "${input_file}" "${expected}")

execute_process(
    COMMAND "${DEMO}" --max-steps 100000
    INPUT_FILE "${input_file}"
    OUTPUT_FILE "${output_file}"
    ERROR_FILE "${error_file}"
    RESULT_VARIABLE result
    TIMEOUT 30
)

if(NOT result EQUAL 0)
    file(READ "${error_file}" diagnostic)
    message(FATAL_ERROR "PPC64 redirected stdio demo returned ${result}: ${diagnostic}")
endif()
file(READ "${output_file}" actual)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "PPC64 redirected stdio output mismatch; expected [${expected}], got [${actual}]")
endif()

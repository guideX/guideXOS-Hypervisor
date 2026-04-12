cmake_minimum_required(VERSION 3.20)
cmake_minimum_required(VERSION 3.15)
project(IA64Emulator VERSION 1.0 LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows-only project
if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only")
endif()

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(CORE_SOURCES
    src/core/cpu.cpp
    src/core/cpu_core.cpp
)

set(DECODER_SOURCES
    src/decoder/decoder.cpp
)

set(MEMORY_SOURCES
    src/memory/memory.cpp
    src/mmu/mmu.cpp
)

set(LOADER_SOURCES
    src/loader/loader.cpp
    src/loader/bootstrap.cpp
)

set(ABI_SOURCES
    src/abi/abi.cpp
)

set(ALL_SOURCES
    ${CORE_SOURCES}
    ${DECODER_SOURCES}
    ${MEMORY_SOURCES}
    ${LOADER_SOURCES}
    ${ABI_SOURCES}
)

# Header files (for IDE organization)
set(HEADERS
    include/cpu_state.h
    include/cpu.h
    include/decoder.h
    include/memory.h
    include/loader.h
    include/bootstrap.h
    include/abi.h
    include/logger.h
    include/IDecoder.h
    include/ICPU.h
    include/IMemory.h
)

# Main executable
add_executable(ia64emu
    main.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

# Set warning level for MSVC
if(MSVC)
    target_compile_options(ia64emu PRIVATE /W4)
    # Disable specific warnings if needed
    # target_compile_options(ia64emu PRIVATE /wd4996)
else()
    target_compile_options(ia64emu PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Test executable
add_executable(ia64emu_tests
    tests/test_main.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(ia64emu_tests PRIVATE /W4)
else()
    target_compile_options(ia64emu_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

# CPU execution test
add_executable(test_cpu_execution
    tests/test_cpu_execution.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_cpu_execution PRIVATE /W4)
else()
    target_compile_options(test_cpu_execution PRIVATE -Wall -Wextra -Wpedantic)
endif()

# MOV_IMM test
add_executable(test_mov_imm
    tests/test_mov_imm.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_mov_imm PRIVATE /W4)
else()
    target_compile_options(test_mov_imm PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Watchpoint test
add_executable(test_watchpoint
    tests/test_watchpoint.cpp
    ${CORE_SOURCES}
    ${DECODER_SOURCES}
    ${MEMORY_SOURCES}
    ${LOADER_SOURCES}
    ${ABI_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_watchpoint PRIVATE /W4)
else()
    target_compile_options(test_watchpoint PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Debug harness executable
add_executable(debug_harness
    debug_harness.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(debug_harness PRIVATE /W4)
else()
    target_compile_options(debug_harness PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Bootstrap test
add_executable(test_bootstrap
    tests/test_bootstrap.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_bootstrap PRIVATE /W4)
else()
    target_compile_options(test_bootstrap PRIVATE -Wall -Wextra -Wpedantic)
endif()


# Enable testing
enable_testing()
add_test(NAME BasicTests COMMAND ia64emu_tests)

# Installation rules (optional)
install(TARGETS ia64emu DESTINATION bin)
install(TARGETS ia64emu_tests DESTINATION bin)

# Print configuration summary
message(STATUS "")
message(STATUS "IA-64 Emulator Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "")

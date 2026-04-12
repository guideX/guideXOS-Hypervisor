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

set(IO_SOURCES
    src/io/Console.cpp
    src/io/Timer.cpp
    src/io/InterruptController.cpp
)

set(VM_SOURCES
    src/vm/VirtualMachine.cpp
    src/vm/CPUContext.cpp
)

set(SYSCALL_SOURCES
    src/syscall/SyscallDispatcher.cpp
)

set(PROFILER_SOURCES
    src/profiler/Profiler.cpp
)

set(FUZZER_SOURCES
    src/fuzzer/Fuzzer.cpp
)

set(ALL_SOURCES
    ${CORE_SOURCES}
    ${DECODER_SOURCES}
    ${MEMORY_SOURCES}
    ${LOADER_SOURCES}
    ${ABI_SOURCES}
    ${IO_SOURCES}
    ${VM_SOURCES}
    ${SYSCALL_SOURCES}
    ${PROFILER_SOURCES}
    ${FUZZER_SOURCES}
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
    include/SyscallDispatcher.h
    include/CPUContext.h
    include/IVirtualMachine.h
    include/VirtualMachine.h
    include/Watchpoint.h
    include/Console.h
    include/Timer.h
    include/InterruptController.h
    include/Profiler.h
    include/Fuzzer.h
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

# Syscall dispatcher test
add_executable(test_syscall_dispatcher
    tests/test_syscall_dispatcher.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_syscall_dispatcher PRIVATE /W4)
else()
    target_compile_options(test_syscall_dispatcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

# IO Devices test
add_executable(test_io_devices
    tests/test_io_devices.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

target_compile_definitions(test_io_devices PRIVATE IA64EMU_IO_TEST_STANDALONE=1)

if(MSVC)
    target_compile_options(test_io_devices PRIVATE /W4)
else()
    target_compile_options(test_io_devices PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Debugger test
add_executable(test_debugger
    tests/test_debugger.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_debugger PRIVATE /W4)
else()
    target_compile_options(test_debugger PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Profiler test
add_executable(test_profiler
    tests/test_profiler.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_profiler PRIVATE /W4)
else()
    target_compile_options(test_profiler PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Fuzzer test
add_executable(test_fuzzer
    tests/test_fuzzer.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_fuzzer PRIVATE /W4)
else()
    target_compile_options(test_fuzzer PRIVATE -Wall -Wextra -Wpedantic)
endif()


# Enable testing
enable_testing()
add_test(NAME BasicTests COMMAND ia64emu_tests)
add_test(NAME SyscallTests COMMAND test_syscall_dispatcher)
add_test(NAME DebuggerTests COMMAND test_debugger)
add_test(NAME ProfilerTests COMMAND test_profiler)
add_test(NAME FuzzerTests COMMAND test_fuzzer)

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

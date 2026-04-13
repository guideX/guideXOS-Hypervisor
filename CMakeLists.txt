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

set(ISA_SOURCES
    src/isa/IA64ISAPlugin.cpp
    src/isa/ISAPluginRegistry.cpp
    src/isa/ExampleISAPlugin.cpp
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
    src/loader/dynamic_linker.cpp
    src/loader/crt0.cpp
)

set(ABI_SOURCES
    src/abi/abi.cpp
)

set(LIBC_SOURCES
    src/libc/libc_bridge.cpp
)

set(IO_SOURCES
    src/io/Console.cpp
    src/io/ConsoleOutputBuffer.cpp
    src/io/Timer.cpp
    src/io/InterruptController.cpp
    src/io/InitramfsDevice.cpp
)

set(VM_SOURCES
    src/vm/VirtualMachine.cpp
    src/vm/CPUContext.cpp
    src/vm/VMManager.cpp
    src/vm/VMBootStateMachine.cpp
    src/vm/SimpleCPUScheduler.cpp
)

set(STORAGE_SOURCES
    src/storage/RawDiskDevice.cpp
    src/storage/VirtualBlockDevice.cpp
    src/storage/FilesystemMount.cpp
    src/storage/SimpleReadOnlyFS.cpp
    src/storage/PathResolver.cpp
    src/storage/VirtualDiskManager.cpp
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

set(API_SOURCES
    src/api/RestAPIServer.cpp
)

set(WEBUI_SOURCES
    src/webui/WebUIServer.cpp
)

set(ALL_SOURCES
    ${CORE_SOURCES}
    ${ISA_SOURCES}
    ${DECODER_SOURCES}
    ${MEMORY_SOURCES}
    ${LOADER_SOURCES}
    ${ABI_SOURCES}
    ${LIBC_SOURCES}
    ${IO_SOURCES}
    ${VM_SOURCES}
    ${STORAGE_SOURCES}
    ${SYSCALL_SOURCES}
    ${PROFILER_SOURCES}
    ${FUZZER_SOURCES}
    ${API_SOURCES}
    ${WEBUI_SOURCES}
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
    include/libc_bridge.h
    include/dynamic_linker.h
    include/crt0.h
    include/logger.h
    include/IDecoder.h
    include/ICPU.h
    include/IMemory.h
    include/IISA.h
    include/IA64ISAPlugin.h
    include/ISAPluginRegistry.h
    include/ExampleISAPlugin.h
    include/SyscallDispatcher.h
    include/CPUContext.h
    include/IVirtualMachine.h
    include/VirtualMachine.h
    include/VMConfiguration.h
    include/VMMetadata.h
    include/VMManager.h
    include/IStorageDevice.h
    include/RawDiskDevice.h
    include/VirtualBlockDevice.h
    include/IFilesystem.h
    include/FilesystemMount.h
    include/SimpleReadOnlyFS.h
    include/PathResolver.h
    include/VirtualDiskManager.h
    include/Watchpoint.h
    include/Console.h
    include/Timer.h
    include/InterruptController.h
    include/Profiler.h
    include/Fuzzer.h
    include/APITypes.h
    include/APIServer.h
    include/APISerializer.h
    include/RestAPIServer.h
    include/IPCAPIServer.h
    include/WebSocketServer.h
    include/WebUIServer.h
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

# ISA Plugin test
add_executable(test_isa_plugin
    tests/test_isa_plugin.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_isa_plugin PRIVATE /W4)
else()
    target_compile_options(test_isa_plugin PRIVATE -Wall -Wextra -Wpedantic)
endif()

# VMManager test
add_executable(test_vmmanager
    tests/test_vmmanager.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_vmmanager PRIVATE /W4)
else()
    target_compile_options(test_vmmanager PRIVATE -Wall -Wextra -Wpedantic)
endif()


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

# VM Isolation test
add_executable(test_vm_isolation
    tests/test_vm_isolation.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(test_vm_isolation PRIVATE /W4)
else()
    target_compile_options(test_vm_isolation PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Web UI Example
add_executable(webui_example
    examples/webui_example.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(webui_example PRIVATE /W4)
else()
    target_compile_options(webui_example PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Virtual Disk Example
add_executable(virtual_disk_example
    examples/virtual_disk_example.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(virtual_disk_example PRIVATE /W4)
else()
    target_compile_options(virtual_disk_example PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Virtual Devices Example
add_executable(virtual_devices_example
    examples/virtual_devices_example.cpp
    ${ALL_SOURCES}
    ${HEADERS}
)

if(MSVC)
    target_compile_options(virtual_devices_example PRIVATE /W4)
else()
    target_compile_options(virtual_devices_example PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Copy web files to build directory
add_custom_target(copy_web_files ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/web
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/web
    COMMENT "Copying web UI files to build directory"
)

add_dependencies(webui_example copy_web_files)


# Enable testing
enable_testing()
add_test(NAME BasicTests COMMAND ia64emu_tests)
add_test(NAME SyscallTests COMMAND test_syscall_dispatcher)
add_test(NAME DebuggerTests COMMAND test_debugger)
add_test(NAME ProfilerTests COMMAND test_profiler)
add_test(NAME FuzzerTests COMMAND test_fuzzer)
add_test(NAME VMIsolationTests COMMAND test_vm_isolation)

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

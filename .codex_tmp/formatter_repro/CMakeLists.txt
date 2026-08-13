cmake_minimum_required(VERSION 3.20)
project(formatter_repro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${CMAKE_CURRENT_LIST_DIR}/../../include)

add_executable(formatter_repro
    formatter_repro.cpp
    ../../src/core/cpu.cpp
    ../../src/decoder/decoder.cpp
    ../../src/decoder/atype_decoder.cpp
    ../../src/decoder/itype_decoder.cpp
    ../../src/decoder/mtype_decoder.cpp
    ../../src/decoder/btype_decoder.cpp
    ../../src/decoder/ftype_decoder.cpp
    ../../src/decoder/lx_decoder.cpp
    ../../src/decoder/xtype_decoder.cpp
    ../../src/decoder/riscv_decoder.cpp
    ../../src/memory/memory.cpp
    ../../src/mmu/mmu.cpp
)

if(MSVC)
    target_compile_options(formatter_repro PRIVATE /W4 /FIerrno.h)
    target_compile_definitions(formatter_repro PRIVATE _CRT_DECLARE_NONSTDC_NAMES=1 _CRT_NONSTDC_NO_WARNINGS=1)
endif()

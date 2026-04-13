#include "bootstrap.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace ia64;

void TestKernelBootstrapConstants() {
    std::cout << "Testing kernel bootstrap constants..." << std::endl;
    
    // Verify kernel-specific constants
    assert(BootstrapConstants::KERNEL_STACK_SIZE == 16 * 1024);
    assert(BootstrapConstants::KERNEL_STACK_TOP == 0xA000000000000ULL);
    
    // Verify kernel PSR has CPL=0
    assert((BootstrapConstants::INITIAL_KERNEL_PSR & BootstrapConstants::PSR_CPL) == 0);
    
    // Verify kernel RSC has privilege level 0
    assert((BootstrapConstants::INITIAL_KERNEL_RSC & BootstrapConstants::RSC_PL_USER) == 0);
    
    std::cout << "  ? Kernel bootstrap constants verified" << std::endl;
}

void TestKernelBootstrapConfig() {
    std::cout << "Testing kernel bootstrap configuration..." << std::endl;
    
    KernelBootstrapConfig config;
    
    // Verify defaults
    assert(config.entryPoint == 0);
    assert(config.stackTop == BootstrapConstants::KERNEL_STACK_TOP);
    assert(config.stackSize == BootstrapConstants::KERNEL_STACK_SIZE);
    assert(config.backingStoreBase == BootstrapConstants::DEFAULT_BACKING_STORE_BASE);
    assert(config.backingStoreSize == BootstrapConstants::DEFAULT_BACKING_STORE_SIZE);
    assert(config.globalPointer == 0);
    assert(config.bootParamAddress == 0);
    assert(config.commandLineAddress == 0);
    assert(config.memoryMapAddress == 0);
    assert(config.memoryMapSize == 0);
    assert(config.pageTableBase == 0);
    assert(config.efiSystemTable == 0);
    assert(config.enableVirtualAddressing == false);
    
    std::cout << "  ? Default kernel configuration is correct" << std::endl;
}

void TestKernelStackSetup() {
    std::cout << "Testing kernel stack setup..." << std::endl;
    
    Memory memory;
    KernelBootstrapConfig config;
    config.stackTop = 0xA000000000000ULL;
    config.stackSize = 16 * 1024;
    
    uint64_t stackPointer = SetupKernelStack(memory, config);
    
    // Verify stack pointer is 16-byte aligned
    assert((stackPointer & 15) == 0);
    
    // Verify it's at or below stackTop
    assert(stackPointer <= config.stackTop);
    
    std::cout << "  ? Kernel stack pointer is 16-byte aligned: 0x" 
              << std::hex << stackPointer << std::dec << std::endl;
}

void TestKernelGeneralRegisters() {
    std::cout << "Testing kernel general register initialization..." << std::endl;
    
    CPUState cpu;
    cpu.Reset();
    
    uint64_t stackPointer = 0xA000000000000ULL;
    uint64_t globalPointer = 0xE000000000600000ULL;
    uint64_t bootParamAddr = 0x100000ULL;
    
    InitializeKernelGeneralRegisters(cpu, stackPointer, globalPointer, bootParamAddr);
    
    // Verify special registers
    assert(cpu.GetGR(0) == 0);                  // r0 always 0
    assert(cpu.GetGR(1) == globalPointer);      // r1 = gp (kernel)
    assert(cpu.GetGR(12) == stackPointer);      // r12 = sp (kernel stack)
    assert(cpu.GetGR(13) == 0);                 // r13 = tp (per-CPU data, 0 for now)
    assert(cpu.GetGR(28) == bootParamAddr);     // r28 = boot parameter address
    
    // Verify other static registers are 0
    for (size_t i = 2; i < 32; ++i) {
        if (i == 12 || i == 13 || i == 28) continue;
        assert(cpu.GetGR(i) == 0);
    }
    
    // Verify stacked registers are 0
    for (size_t i = 32; i < 128; ++i) {
        assert(cpu.GetGR(i) == 0);
    }
    
    std::cout << "  ? Kernel general registers initialized correctly" << std::endl;
    std::cout << "  ? Boot parameter address in r28: 0x" 
              << std::hex << cpu.GetGR(28) << std::dec << std::endl;
}

void TestKernelApplicationRegisters() {
    std::cout << "Testing kernel application register initialization..." << std::endl;
    
    CPUState cpu;
    cpu.Reset();
    
    KernelBootstrapConfig config;
    config.backingStoreBase = 0x80000000000ULL;
    config.backingStoreSize = 2 * 1024 * 1024;
    
    InitializeKernelApplicationRegisters(cpu, config);
    
    // Verify critical ARs for kernel mode
    assert(cpu.GetAR(BootstrapConstants::AR_RSC) == BootstrapConstants::INITIAL_KERNEL_RSC);
    assert(cpu.GetAR(BootstrapConstants::AR_BSP) == config.backingStoreBase);
    assert(cpu.GetAR(BootstrapConstants::AR_BSPSTORE) == config.backingStoreBase);
    assert(cpu.GetAR(BootstrapConstants::AR_RNAT) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_FPSR) == 0x0009804C0270033FULL);
    assert(cpu.GetAR(BootstrapConstants::AR_PFS) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_LC) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_EC) == 0);
    
    // Verify kernel registers (AR.KR0-AR.KR7) are initialized
    for (size_t i = 0; i < 8; ++i) {
        assert(cpu.GetAR(i) == 0);
    }
    
    std::cout << "  ? Kernel application registers initialized correctly" << std::endl;
}

void TestKernelBootstrapPhysicalAddressing() {
    std::cout << "Testing kernel bootstrap with physical addressing..." << std::endl;
    
    Memory memory;
    CPUState cpu;
    
    KernelBootstrapConfig config;
    config.entryPoint = 0x100000;  // 1 MB physical
    config.globalPointer = 0x600000;
    config.bootParamAddress = 0x10000;  // Boot params at 64 KB
    config.enableVirtualAddressing = false;  // Physical mode
    
    uint64_t stackPointer = InitializeKernelBootstrapState(cpu, memory, config);
    
    // Verify CPU state
    assert(cpu.GetIP() == config.entryPoint);
    assert(cpu.GetGR(1) == config.globalPointer);
    assert(cpu.GetGR(12) == stackPointer);
    assert(cpu.GetGR(28) == config.bootParamAddress);
    assert(cpu.GetPR(0) == true);
    assert(cpu.GetCFM() == 0);
    
    // Verify PSR is in kernel mode (CPL=0)
    uint64_t psr = cpu.GetPSR();
    assert((psr & BootstrapConstants::PSR_CPL) == 0);
    
    // Verify PSR has physical addressing (translation bits disabled)
    assert((psr & BootstrapConstants::PSR_IC) == 0);  // No interruption collection initially
    assert((psr & BootstrapConstants::PSR_DT) == 0);  // No data translation
    assert((psr & BootstrapConstants::PSR_RT) == 0);  // No register translation
    assert((psr & BootstrapConstants::PSR_IT) == 0);  // No instruction translation
    
    std::cout << "  ? Kernel initialized in physical addressing mode" << std::endl;
    std::cout << "  ? Entry point: 0x" << std::hex << cpu.GetIP() << std::dec << std::endl;
    std::cout << "  ? PSR: 0x" << std::hex << psr << std::dec << " (CPL=0, physical mode)" << std::endl;
}

void TestKernelBootstrapVirtualAddressing() {
    std::cout << "Testing kernel bootstrap with virtual addressing..." << std::endl;
    
    Memory memory;
    CPUState cpu;
    
    KernelBootstrapConfig config;
    config.entryPoint = 0xE000000000100000ULL;  // Kernel virtual address
    config.globalPointer = 0xE000000000600000ULL;
    config.bootParamAddress = 0x10000;
    config.pageTableBase = 0x50000;
    config.enableVirtualAddressing = true;  // Virtual mode
    
    uint64_t stackPointer = InitializeKernelBootstrapState(cpu, memory, config);
    
    // Verify CPU state
    assert(cpu.GetIP() == config.entryPoint);
    assert(cpu.GetGR(1) == config.globalPointer);
    assert(cpu.GetGR(12) == stackPointer);
    assert(cpu.GetGR(28) == config.bootParamAddress);
    
    // Verify PSR has virtual addressing enabled
    uint64_t psr = cpu.GetPSR();
    assert((psr & BootstrapConstants::PSR_CPL) == 0);  // Still kernel mode
    assert((psr & BootstrapConstants::PSR_IC) != 0);   // Interruption collection enabled
    assert((psr & BootstrapConstants::PSR_DT) != 0);   // Data translation enabled
    assert((psr & BootstrapConstants::PSR_RT) != 0);   // Register translation enabled
    assert((psr & BootstrapConstants::PSR_IT) != 0);   // Instruction translation enabled
    
    std::cout << "  ? Kernel initialized in virtual addressing mode" << std::endl;
    std::cout << "  ? Entry point: 0x" << std::hex << cpu.GetIP() << std::dec << std::endl;
    std::cout << "  ? PSR: 0x" << std::hex << psr << std::dec << " (CPL=0, virtual mode)" << std::endl;
}

void TestKernelBootstrapWithBootParams() {
    std::cout << "Testing kernel bootstrap with boot parameters..." << std::endl;
    
    Memory memory;
    CPUState cpu;
    
    // Setup boot parameter structure in memory
    uint64_t bootParamAddr = 0x10000;
    uint64_t cmdLineAddr = 0x11000;
    uint64_t memMapAddr = 0x12000;
    
    // Write dummy boot parameters
    memory.write<uint64_t>(bootParamAddr, 0xDEADBEEF);  // Magic number
    memory.write<uint64_t>(bootParamAddr + 8, cmdLineAddr);
    memory.write<uint64_t>(bootParamAddr + 16, memMapAddr);
    
    KernelBootstrapConfig config;
    config.entryPoint = 0x100000;
    config.bootParamAddress = bootParamAddr;
    config.commandLineAddress = cmdLineAddr;
    config.memoryMapAddress = memMapAddr;
    config.memoryMapSize = 4096;
    
    uint64_t stackPointer = InitializeKernelBootstrapState(cpu, memory, config);
    
    // Verify r28 points to boot params
    assert(cpu.GetGR(28) == bootParamAddr);
    
    // Verify we can read boot params from memory
    uint64_t magic = memory.read<uint64_t>(bootParamAddr);
    assert(magic == 0xDEADBEEF);
    
    std::cout << "  ? Boot parameters accessible via r28" << std::endl;
    std::cout << "  ? Boot param address: 0x" << std::hex << bootParamAddr << std::dec << std::endl;
    std::cout << "  ? Command line address: 0x" << std::hex << cmdLineAddr << std::dec << std::endl;
    std::cout << "  ? Memory map address: 0x" << std::hex << memMapAddr << std::dec << std::endl;
}

void TestKernelBootstrapComparisonWithUserMode() {
    std::cout << "Testing kernel vs user mode differences..." << std::endl;
    
    Memory memory;
    CPUState kernelCpu, userCpu;
    
    // Kernel bootstrap
    KernelBootstrapConfig kernelConfig;
    kernelConfig.entryPoint = 0x100000;
    kernelConfig.globalPointer = 0x600000;
    kernelConfig.bootParamAddress = 0x10000;
    
    uint64_t kernelSp = InitializeKernelBootstrapState(kernelCpu, memory, kernelConfig);
    
    // User bootstrap
    BootstrapConfig userConfig;
    userConfig.entryPoint = 0x400000;
    userConfig.globalPointer = 0x600000;
    userConfig.argv = {"program"};
    
    uint64_t userSp = InitializeBootstrapState(userCpu, memory, userConfig);
    
    // Compare PSR privilege levels
    uint64_t kernelPsr = kernelCpu.GetPSR();
    uint64_t userPsr = userCpu.GetPSR();
    
    // Kernel should be CPL=0
    assert((kernelPsr & BootstrapConstants::PSR_CPL) == 0);
    
    // User should be CPL=3
    assert((userPsr & BootstrapConstants::PSR_CPL) == BootstrapConstants::PSR_CPL);
    
    // Compare AR.RSC privilege levels
    uint64_t kernelRsc = kernelCpu.GetAR(BootstrapConstants::AR_RSC);
    uint64_t userRsc = userCpu.GetAR(BootstrapConstants::AR_RSC);
    
    // Extract privilege level from RSC (bits 2-3)
    uint64_t kernelRscPl = (kernelRsc >> 2) & 3;
    uint64_t userRscPl = (userRsc >> 2) & 3;
    
    assert(kernelRscPl == 0);  // Kernel PL=0
    assert(userRscPl == 3);    // User PL=3
    
    // Verify r28 is set for kernel but not user
    assert(kernelCpu.GetGR(28) == kernelConfig.bootParamAddress);
    assert(userCpu.GetGR(28) == 0);
    
    std::cout << "  ? Kernel PSR CPL: " << ((kernelPsr >> 32) & 3) << std::endl;
    std::cout << "  ? User PSR CPL: " << ((userPsr >> 32) & 3) << std::endl;
    std::cout << "  ? Kernel RSC PL: " << kernelRscPl << std::endl;
    std::cout << "  ? User RSC PL: " << userRscPl << std::endl;
    std::cout << "  ? Kernel has boot params in r28, user does not" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "IA-64 Kernel Bootstrap State Initializer Tests" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << std::endl;
    
    try {
        TestKernelBootstrapConstants();
        std::cout << std::endl;
        
        TestKernelBootstrapConfig();
        std::cout << std::endl;
        
        TestKernelStackSetup();
        std::cout << std::endl;
        
        TestKernelGeneralRegisters();
        std::cout << std::endl;
        
        TestKernelApplicationRegisters();
        std::cout << std::endl;
        
        TestKernelBootstrapPhysicalAddressing();
        std::cout << std::endl;
        
        TestKernelBootstrapVirtualAddressing();
        std::cout << std::endl;
        
        TestKernelBootstrapWithBootParams();
        std::cout << std::endl;
        
        TestKernelBootstrapComparisonWithUserMode();
        std::cout << std::endl;
        
        std::cout << "==================================================" << std::endl;
        std::cout << "All kernel bootstrap tests passed! ?" << std::endl;
        std::cout << "==================================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << std::endl;
        std::cerr << "==================================================" << std::endl;
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        std::cerr << "==================================================" << std::endl;
        return 1;
    }
}

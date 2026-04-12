#include "bootstrap.h"
#include "cpu_state.h"
#include "memory.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

using namespace ia64;

// Test helper function to verify memory content
bool VerifyMemoryString(Memory& memory, uint64_t address, const std::string& expected) {
    std::vector<uint8_t> buffer(expected.size() + 1);
    for (size_t i = 0; i < expected.size() + 1; ++i) {
        buffer[i] = memory.read<uint8_t>(address + i);
    }
    return std::memcmp(buffer.data(), expected.c_str(), expected.size() + 1) == 0;
}

void TestBootstrapConstants() {
    std::cout << "Testing bootstrap constants..." << std::endl;
    
    // Verify critical constants
    assert(BootstrapConstants::PAGE_SIZE == 4096);
    assert(BootstrapConstants::STACK_ALIGNMENT == 16);
    assert(BootstrapConstants::SP_REGISTER == 12);
    assert(BootstrapConstants::GP_REGISTER == 1);
    assert(BootstrapConstants::TP_REGISTER == 13);
    
    std::cout << "  ? Bootstrap constants verified" << std::endl;
}

void TestAuxiliaryVectorBuilder() {
    std::cout << "Testing auxiliary vector builder..." << std::endl;
    
    BootstrapConfig config;
    config.entryPoint = 0x400000;
    config.programHeaderAddr = 0x400040;
    config.programHeaderEntrySize = 56;
    config.programHeaderCount = 4;
    config.baseAddress = 0x400000;
    
    auto auxv = BuildDefaultAuxiliaryVector(config);
    
    // Verify essential auxiliary vectors are present
    bool foundPageSize = false;
    bool foundEntry = false;
    bool foundPhdr = false;
    
    for (const auto& aux : auxv) {
        if (aux.type == static_cast<uint64_t>(AuxVecType::AT_PAGESZ)) {
            foundPageSize = true;
            assert(aux.value == BootstrapConstants::PAGE_SIZE);
        }
        if (aux.type == static_cast<uint64_t>(AuxVecType::AT_ENTRY)) {
            foundEntry = true;
            assert(aux.value == 0x400000);
        }
        if (aux.type == static_cast<uint64_t>(AuxVecType::AT_PHDR)) {
            foundPhdr = true;
            assert(aux.value == 0x400040);
        }
    }
    
    assert(foundPageSize);
    assert(foundEntry);
    assert(foundPhdr);
    assert(auxv.size() > 5);  // Should have multiple entries
    
    std::cout << "  ? Auxiliary vector contains " << auxv.size() << " entries" << std::endl;
}

void TestStackLayout() {
    std::cout << "Testing stack layout..." << std::endl;
    
    Memory memory;
    
    BootstrapConfig config;
    config.entryPoint = 0x400000;
    config.argv = {"program", "arg1", "arg2"};
    config.envp = {"VAR1=value1", "VAR2=value2"};
    config.programHeaderAddr = 0x400040;
    config.programHeaderEntrySize = 56;
    config.programHeaderCount = 4;
    config.baseAddress = 0;
    
    uint64_t stackPointer = SetupInitialStack(memory, config);
    
    // Verify stack pointer is 16-byte aligned
    assert((stackPointer & 15) == 0);
    std::cout << "  ? Stack pointer is 16-byte aligned: 0x" 
              << std::hex << stackPointer << std::dec << std::endl;
    
    // Verify argc
    uint64_t argc = memory.read<uint64_t>(stackPointer);
    assert(argc == 3);
    std::cout << "  ? argc = " << argc << std::endl;
    
    // Verify argv pointers
    uint64_t argv0Ptr = memory.read<uint64_t>(stackPointer + 8);
    uint64_t argv1Ptr = memory.read<uint64_t>(stackPointer + 16);
    uint64_t argv2Ptr = memory.read<uint64_t>(stackPointer + 24);
    uint64_t argvNull = memory.read<uint64_t>(stackPointer + 32);
    
    assert(argv0Ptr != 0);
    assert(argv1Ptr != 0);
    assert(argv2Ptr != 0);
    assert(argvNull == 0);
    
    // Verify argv strings
    assert(VerifyMemoryString(memory, argv0Ptr, "program"));
    assert(VerifyMemoryString(memory, argv1Ptr, "arg1"));
    assert(VerifyMemoryString(memory, argv2Ptr, "arg2"));
    std::cout << "  ? argv strings verified" << std::endl;
    
    // Verify envp pointers
    uint64_t envp0Ptr = memory.read<uint64_t>(stackPointer + 40);
    uint64_t envp1Ptr = memory.read<uint64_t>(stackPointer + 48);
    uint64_t envpNull = memory.read<uint64_t>(stackPointer + 56);
    
    assert(envp0Ptr != 0);
    assert(envp1Ptr != 0);
    assert(envpNull == 0);
    
    // Verify envp strings
    assert(VerifyMemoryString(memory, envp0Ptr, "VAR1=value1"));
    assert(VerifyMemoryString(memory, envp1Ptr, "VAR2=value2"));
    std::cout << "  ? envp strings verified" << std::endl;
    
    // Verify auxiliary vector is present (first entry after envp)
    uint64_t auxvBase = stackPointer + 64;
    uint64_t firstAuxType = memory.read<uint64_t>(auxvBase);
    uint64_t firstAuxValue = memory.read<uint64_t>(auxvBase + 8);
    
    // Should not be AT_NULL (which would be 0)
    assert(firstAuxType != 0);
    std::cout << "  ? Auxiliary vector present" << std::endl;
}

void TestRegisterInitialization() {
    std::cout << "Testing register initialization..." << std::endl;
    
    CPUState cpu;
    cpu.Reset();
    
    uint64_t stackPointer = 0x7FFFFFF0000ULL;
    uint64_t globalPointer = 0x600000;
    
    InitializeGeneralRegisters(cpu, stackPointer, globalPointer);
    
    // Verify special registers
    assert(cpu.GetGR(0) == 0);  // r0 always 0
    assert(cpu.GetGR(1) == globalPointer);  // r1 = gp
    assert(cpu.GetGR(12) == stackPointer);  // r12 = sp
    assert(cpu.GetGR(13) == 0);  // r13 = tp (0 for single-threaded)
    
    // Verify other static registers are 0
    for (size_t i = 2; i < 32; ++i) {
        if (i == 12 || i == 13) continue;
        assert(cpu.GetGR(i) == 0);
    }
    
    // Verify stacked registers are 0
    for (size_t i = 32; i < 128; ++i) {
        assert(cpu.GetGR(i) == 0);
    }
    
    std::cout << "  ? General registers initialized correctly" << std::endl;
}

void TestPredicateAndBranchRegisters() {
    std::cout << "Testing predicate and branch register initialization..." << std::endl;
    
    CPUState cpu;
    cpu.Reset();
    
    InitializePredicateAndBranchRegisters(cpu);
    
    // Verify PR0 is true (hardwired)
    assert(cpu.GetPR(0) == true);
    
    // Verify all other predicates are false
    for (size_t i = 1; i < 64; ++i) {
        assert(cpu.GetPR(i) == false);
    }
    
    // Verify all branch registers are 0
    for (size_t i = 0; i < 8; ++i) {
        assert(cpu.GetBR(i) == 0);
    }
    
    std::cout << "  ? Predicate and branch registers initialized correctly" << std::endl;
}

void TestApplicationRegisters() {
    std::cout << "Testing application register initialization..." << std::endl;
    
    CPUState cpu;
    cpu.Reset();
    
    BootstrapConfig config;
    config.backingStoreBase = 0x80000000000ULL;
    config.backingStoreSize = 2 * 1024 * 1024;
    
    InitializeApplicationRegisters(cpu, config);
    
    // Verify critical ARs
    assert(cpu.GetAR(BootstrapConstants::AR_RSC) == BootstrapConstants::INITIAL_RSC);
    assert(cpu.GetAR(BootstrapConstants::AR_BSP) == config.backingStoreBase);
    assert(cpu.GetAR(BootstrapConstants::AR_BSPSTORE) == config.backingStoreBase);
    assert(cpu.GetAR(BootstrapConstants::AR_RNAT) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_FPSR) == 0x0009804C0270033FULL);
    assert(cpu.GetAR(BootstrapConstants::AR_PFS) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_LC) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_EC) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_UNAT) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_CCV) == 0);
    assert(cpu.GetAR(BootstrapConstants::AR_CSD) == 0);
    
    std::cout << "  ? Application registers initialized correctly" << std::endl;
}

void TestFullBootstrap() {
    std::cout << "Testing full bootstrap initialization..." << std::endl;
    
    Memory memory;
    CPUState cpu;
    
    BootstrapConfig config;
    config.entryPoint = 0x400000;
    config.globalPointer = 0x600000;
    config.argv = {"test_program", "--flag", "value"};
    config.envp = {"HOME=/home/user", "PATH=/usr/bin"};
    config.programHeaderAddr = 0x400040;
    config.programHeaderEntrySize = 56;
    config.programHeaderCount = 4;
    config.baseAddress = 0x400000;
    
    uint64_t stackPointer = InitializeBootstrapState(cpu, memory, config);
    
    // Verify CPU state
    assert(cpu.GetIP() == config.entryPoint);
    assert(cpu.GetGR(1) == config.globalPointer);
    assert(cpu.GetGR(12) == stackPointer);
    assert(cpu.GetPR(0) == true);
    assert(cpu.GetCFM() == 0);
    assert(cpu.GetPSR() == BootstrapConstants::INITIAL_PSR);
    
    // Verify stack pointer alignment
    assert((stackPointer & 15) == 0);
    
    // Verify stack content
    uint64_t argc = memory.read<uint64_t>(stackPointer);
    assert(argc == 3);
    
    uint64_t argv0Ptr = memory.read<uint64_t>(stackPointer + 8);
    assert(VerifyMemoryString(memory, argv0Ptr, "test_program"));
    
    std::cout << "  ? Full bootstrap completed successfully" << std::endl;
    std::cout << "  ? Entry point: 0x" << std::hex << cpu.GetIP() << std::dec << std::endl;
    std::cout << "  ? Stack pointer: 0x" << std::hex << stackPointer << std::dec << std::endl;
    std::cout << "  ? Global pointer: 0x" << std::hex << cpu.GetGR(1) << std::dec << std::endl;
}

void TestDefaultConfiguration() {
    std::cout << "Testing default bootstrap configuration..." << std::endl;
    
    BootstrapConfig config;
    
    // Verify defaults
    assert(config.entryPoint == 0);
    assert(config.stackTop == BootstrapConstants::DEFAULT_STACK_TOP);
    assert(config.stackSize == BootstrapConstants::DEFAULT_STACK_SIZE);
    assert(config.backingStoreBase == BootstrapConstants::DEFAULT_BACKING_STORE_BASE);
    assert(config.backingStoreSize == BootstrapConstants::DEFAULT_BACKING_STORE_SIZE);
    assert(config.globalPointer == 0);
    assert(config.argv.size() == 1);
    assert(config.argv[0] == "program");
    assert(config.envp.empty());
    assert(config.auxv.empty());
    
    std::cout << "  ? Default configuration is correct" << std::endl;
}

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "IA-64 Linux ABI Bootstrap State Initializer Tests" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << std::endl;
    
    try {
        TestBootstrapConstants();
        std::cout << std::endl;
        
        TestDefaultConfiguration();
        std::cout << std::endl;
        
        TestAuxiliaryVectorBuilder();
        std::cout << std::endl;
        
        TestStackLayout();
        std::cout << std::endl;
        
        TestRegisterInitialization();
        std::cout << std::endl;
        
        TestPredicateAndBranchRegisters();
        std::cout << std::endl;
        
        TestApplicationRegisters();
        std::cout << std::endl;
        
        TestFullBootstrap();
        std::cout << std::endl;
        
        std::cout << "==================================================" << std::endl;
        std::cout << "All tests passed! ?" << std::endl;
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

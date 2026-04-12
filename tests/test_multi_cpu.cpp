#include <iostream>
#include <vector>
#include <cstdint>
#include "../include/VirtualMachine.h"

using namespace ia64;

// Helper to create a simple NOP program
std::vector<uint8_t> createNOPProgram(size_t numBundles) {
    std::vector<uint8_t> program;
    
    // Each bundle is 16 bytes, all zeros for NOP
    for (size_t i = 0; i < numBundles; i++) {
        for (int j = 0; j < 16; j++) {
            program.push_back(0x00);
        }
    }
    
    return program;
}

int main() {
    std::cout << "IA-64 Multi-CPU Test\n";
    std::cout << "====================\n\n";

    try {
        // Create VM with 4 CPUs
        const size_t numCPUs = 4;
        VirtualMachine vm(1024 * 1024, numCPUs);  // 1MB memory, 4 CPUs
        std::cout << "Virtual Machine created with " << numCPUs << " CPUs\n";

        // Initialize VM
        if (!vm.init()) {
            std::cerr << "Failed to initialize VM\n";
            return 1;
        }
        std::cout << "Virtual Machine initialized\n\n";

        // Verify CPU count
        std::cout << "CPU Count: " << vm.getCPUCount() << "\n";
        std::cout << "Active CPU: " << vm.getActiveCPUIndex() << "\n\n";

        // Create a simple test program
        std::vector<uint8_t> program = createNOPProgram(4);  // 4 bundles

        // Load program into VM memory at different addresses for each CPU
        uint64_t baseAddr = 0x1000;
        if (!vm.loadProgram(program.data(), program.size(), baseAddr)) {
            std::cerr << "Failed to load program\n";
            return 1;
        }
        std::cout << "Loaded " << program.size() << " bytes at 0x" 
                  << std::hex << baseAddr << std::dec << "\n\n";

        // Set different entry points for each CPU to demonstrate isolation
        for (size_t i = 0; i < numCPUs; i++) {
            uint64_t entryPoint = baseAddr + (i * 0x100);  // Different starting points
            vm.setIP(static_cast<int>(i), entryPoint);
            std::cout << "CPU " << i << " entry point: 0x" 
                      << std::hex << entryPoint << std::dec << "\n";
        }
        std::cout << "\n";

        // Write different values to r1 on each CPU to demonstrate isolation
        for (size_t i = 0; i < numCPUs; i++) {
            uint64_t value = 0x1000 + (i * 0x100);  // Different values per CPU
            vm.writeGR(static_cast<int>(i), 1, value);  // Write to r1
            std::cout << "CPU " << i << " r1 = 0x" 
                      << std::hex << value << std::dec << "\n";
        }
        std::cout << "\n";

        // Verify register isolation
        std::cout << "Verifying register isolation:\n";
        for (size_t i = 0; i < numCPUs; i++) {
            uint64_t value = vm.readGR(static_cast<int>(i), 1);
            uint64_t expected = 0x1000 + (i * 0x100);
            std::cout << "CPU " << i << " r1 = 0x" 
                      << std::hex << value << std::dec;
            if (value == expected) {
                std::cout << " ? (expected 0x" << std::hex << expected << std::dec << ")\n";
            } else {
                std::cout << " ? (expected 0x" << std::hex << expected << std::dec << ")\n";
            }
        }
        std::cout << "\n";

        // Verify IP isolation
        std::cout << "Verifying IP isolation:\n";
        for (size_t i = 0; i < numCPUs; i++) {
            uint64_t ip = vm.getIP(static_cast<int>(i));
            uint64_t expected = baseAddr + (i * 0x100);
            std::cout << "CPU " << i << " IP = 0x" 
                      << std::hex << ip << std::dec;
            if (ip == expected) {
                std::cout << " ? (expected 0x" << std::hex << expected << std::dec << ")\n";
            } else {
                std::cout << " ? (expected 0x" << std::hex << expected << std::dec << ")\n";
            }
        }
        std::cout << "\n";

        // Execute a few steps to verify scheduler switches between CPUs
        std::cout << "Executing 12 steps (should rotate through all CPUs):\n";
        std::cout << "-----------------------------------------------------\n";
        for (int i = 0; i < 12; i++) {
            int beforeCPU = vm.getActiveCPUIndex();
            bool shouldContinue = vm.step();
            int afterCPU = vm.getActiveCPUIndex();
            
            std::cout << "Step " << (i + 1) << ": CPU " << afterCPU;
            if (beforeCPU != afterCPU) {
                std::cout << " (switched from CPU " << beforeCPU << ")";
            }
            std::cout << "\n";
            
            if (!shouldContinue) {
                std::cout << "VM halted.\n";
                break;
            }
        }
        std::cout << "\n";

        // Test CPU switching
        std::cout << "Testing manual CPU switching:\n";
        std::cout << "Current active CPU: " << vm.getActiveCPUIndex() << "\n";
        
        if (vm.setActiveCPU(2)) {
            std::cout << "Switched to CPU 2\n";
            std::cout << "Active CPU: " << vm.getActiveCPUIndex() << "\n";
            std::cout << "CPU 2 IP: 0x" << std::hex << vm.getIP() << std::dec << "\n";
            std::cout << "CPU 2 r1: 0x" << std::hex << vm.readGR(1) << std::dec << "\n";
        } else {
            std::cout << "Failed to switch to CPU 2\n";
        }
        std::cout << "\n";

        // Dump final state
        std::cout << "Final VM State:\n";
        std::cout << "===============\n";
        vm.dump();

        std::cout << "\n? Multi-CPU test completed successfully!\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

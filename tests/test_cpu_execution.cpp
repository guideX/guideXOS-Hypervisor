#include <iostream>
#include <vector>
#include <cstdint>
#include "../include/VirtualMachine.h"

using namespace ia64;

int main() {
    std::cout << "IA-64 VM Execution Loop Test\n";
    std::cout << "=============================\n\n";

    try {
        // Initialize Virtual Machine (1MB)
        VirtualMachine vm(1024 * 1024);
        std::cout << "Virtual Machine created (1MB)\n";

        // Initialize VM
        if (!vm.init()) {
            std::cerr << "Failed to initialize VM\n";
            return 1;
        }
        std::cout << "Virtual Machine initialized\n\n";

        // Create a simple test program
        // Bundle format: [template:5][slot0:41][slot1:41][slot2:41] = 128 bits = 16 bytes
        // For simplicity, we'll create bundles with NOP instructions (all zeros)
        
        std::vector<uint8_t> program = {
            // Bundle 0: Three NOPs (MII template = 0x00)
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            
            // Bundle 1: Three NOPs  
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };

        // Load program into VM memory at address 0x1000
        uint64_t startAddr = 0x1000;
        if (!vm.loadProgram(program.data(), program.size(), startAddr)) {
            std::cerr << "Failed to load program\n";
            return 1;
        }
        std::cout << "Loaded " << program.size() << " bytes at 0x" 
                  << std::hex << startAddr << std::dec << "\n\n";

        // Set entry point
        vm.setEntryPoint(startAddr);
        std::cout << "Entry point set to 0x" << std::hex << startAddr << std::dec << "\n\n";

        // Execute instructions
        std::cout << "Beginning execution...\n";
        std::cout << "---------------------\n\n";

        // Execute 6 steps (2 bundles * 3 instructions each)
        for (int i = 0; i < 6; i++) {
            std::cout << "Step " << (i + 1) << ":\n";
            bool shouldContinue = vm.step();
            std::cout << "\n";
            
            if (!shouldContinue) {
                std::cout << "VM halted.\n";
                break;
            }
        }

        std::cout << "\nExecution complete.\n";
        std::cout << "\nFinal VM State:\n";
        std::cout << "===============\n";
        vm.dump();

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

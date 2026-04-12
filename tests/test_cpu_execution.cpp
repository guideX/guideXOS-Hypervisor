#include <iostream>
#include <vector>
#include <cstdint>
#include "../include/cpu.h"
#include "../include/cpu_state.h"
#include "../include/decoder.h"
#include "../include/memory.h"

using namespace ia64;

int main() {
    std::cout << "IA-64 CPU Execution Loop Test\n";
    std::cout << "==============================\n\n";

    try {
        // Initialize memory system (1MB)
        Memory memory(1024 * 1024);
        std::cout << "Memory system initialized (1MB)\n";

        // Initialize decoder
        InstructionDecoder decoder;
        std::cout << "Instruction decoder initialized\n";

        // Initialize CPU
        CPU cpu(memory, decoder);
        std::cout << "CPU initialized\n\n";

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

        // Load program into memory at address 0x1000
        uint64_t startAddr = 0x1000;
        memory.Write(startAddr, program.data(), program.size());
        std::cout << "Loaded " << program.size() << " bytes at 0x" 
                  << std::hex << startAddr << std::dec << "\n\n";

        // Set IP to start of program
        cpu.setIP(startAddr);
        std::cout << "IP set to 0x" << std::hex << startAddr << std::dec << "\n\n";

        // Execute instructions
        std::cout << "Beginning execution...\n";
        std::cout << "---------------------\n\n";

        // Execute 6 steps (2 bundles * 3 instructions each)
        for (int i = 0; i < 6; i++) {
            std::cout << "Step " << (i + 1) << ":\n";
            bool shouldContinue = cpu.step();
            std::cout << "\n";
            
            if (!shouldContinue) {
                std::cout << "CPU halted.\n";
                break;
            }
        }

        std::cout << "\nExecution complete.\n";
        std::cout << "\nFinal CPU State:\n";
        std::cout << "================\n";
        cpu.dump();

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

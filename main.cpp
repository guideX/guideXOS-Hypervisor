#include <iostream>
#include <vector>
#include <cstdint>
#include "cpu_state.h"
#include "decoder.h"
#include "memory.h"

int main(int argc, char* argv[]) {
    std::cout << "IA-64 Emulator - Windows Edition\n";
    std::cout << "=================================\n\n";

    try {
        // Initialize CPU state
        ia64::CPUState cpu;
        std::cout << "CPU initialized\n";

        // Initialize memory system (16MB for demo)
        ia64::MemorySystem memory(16 * 1024 * 1024);
        std::cout << "Memory system initialized (16MB)\n";

        // Initialize decoder
        ia64::InstructionDecoder decoder;
        std::cout << "Instruction decoder initialized\n\n";

        // Create a fake instruction buffer with IA-64 bundles
        // Each bundle is 128 bits (16 bytes): template (5 bits) + 3 instructions (41 bits each)
        // For demo: NOP bundles (template 0x00, all NOPs)
        std::vector<uint8_t> programCode = {
            // Bundle 1: NOP bundle
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            
            // Bundle 2: NOP bundle
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        };

        // Load program into memory at address 0x10000
        uint64_t loadAddress = 0x10000;
        memory.Write(loadAddress, programCode.data(), programCode.size());
        std::cout << "Loaded " << programCode.size() << " bytes at 0x" 
                  << std::hex << loadAddress << std::dec << "\n\n";

        // Set instruction pointer
        cpu.SetIP(loadAddress);

        // Execute bundles
        std::cout << "Beginning execution...\n";
        std::cout << "----------------------\n";

        size_t bundlesExecuted = 0;
        const size_t maxBundles = 2;

        while (bundlesExecuted < maxBundles) {
            uint64_t currentIP = cpu.GetIP();
            
            // Fetch bundle (16 bytes)
            std::vector<uint8_t> bundleData(16);
            memory.Read(currentIP, bundleData.data(), 16);

            // Decode bundle
            auto bundle = decoder.DecodeBundle(bundleData.data());
            
            std::cout << "Bundle " << bundlesExecuted + 1 
                      << " @ 0x" << std::hex << currentIP << std::dec << ":\n";
            std::cout << "  Template: 0x" << std::hex << static_cast<int>(bundle.templateType) << std::dec << "\n";
            std::cout << "  Instructions: " << bundle.instructions.size() << "\n";

            // Execute each instruction in the bundle
            for (size_t i = 0; i < bundle.instructions.size(); ++i) {
                const auto& insn = bundle.instructions[i];
                std::cout << "    [" << i << "] " << insn.GetDisassembly() << "\n";
                
                // Execute instruction
                insn.Execute(cpu, memory);
            }

            // Advance IP by 16 bytes (one bundle)
            cpu.SetIP(currentIP + 16);
            bundlesExecuted++;
            std::cout << "\n";
        }

        std::cout << "Execution complete. Executed " << bundlesExecuted << " bundles.\n";
        std::cout << "\nFinal CPU State:\n";
        cpu.Dump();

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

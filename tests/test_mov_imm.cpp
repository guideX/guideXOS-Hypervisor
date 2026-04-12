#include <iostream>
#include <vector>
#include <cstdint>
#include "../include/cpu.h"
#include "../include/cpu_state.h"
#include "../include/decoder.h"
#include "../include/memory.h"

using namespace ia64;

// Helper to encode a simple instruction for testing
// This matches the simplified encoding in DecodeInstruction:
// Bits [40:37] = major opcode (4 bits)
//   0x0 = NOP
//   0x1 = MOV immediate  
//   0x2 = MOV register
//   0x3 = ADD
// Bits [36:30] = destination register (7 bits)
// Bits [29:23] = source register (7 bits)
// Bits [22:0]  = immediate value (23 bits)
uint64_t encodeNop() {
    return 0;
}

uint64_t encodeMovImm(uint8_t dst, uint32_t immediate) {
    uint64_t opcode = 1ULL;  // MOV_IMM
    uint64_t instr = (opcode << 37) | ((uint64_t)(dst & 0x7F) << 30) | (immediate & 0x7FFFFF);
    return instr;
}

uint64_t encodeMovReg(uint8_t dst, uint8_t src) {
    uint64_t opcode = 2ULL;  // MOV_GR
    uint64_t instr = (opcode << 37) | ((uint64_t)(dst & 0x7F) << 30) | ((uint64_t)(src & 0x7F) << 23);
    return instr;
}

uint64_t encodeAdd(uint8_t dst, uint8_t src1, uint8_t src2) {
    uint64_t opcode = 3ULL;  // ADD
    uint64_t instr = (opcode << 37) | ((uint64_t)(dst & 0x7F) << 30) | ((uint64_t)(src1 & 0x7F) << 23) | ((uint64_t)(src2 & 0x7F) << 16);
    return instr;
}

// Helper to encode a bundle
// Bundle layout: [template:5][slot0:41][slot1:41][slot2:41] = 128 bits = 16 bytes
void encodeBundle(uint8_t* out, uint8_t tmpl, uint64_t slot0, uint64_t slot1, uint64_t slot2) {
    // Clear output
    for (int i = 0; i < 16; i++) out[i] = 0;
    
    // Template (bits 0-4)
    out[0] = tmpl & 0x1F;
    
    // Slot 0 (bits 5-45) - 41 bits starting at bit 5
    uint64_t s0 = slot0 & 0x1FFFFFFFFFFULL;  // Mask to 41 bits
    out[0] |= (s0 << 5) & 0xFF;
    out[1] = (s0 >> 3) & 0xFF;
    out[2] = (s0 >> 11) & 0xFF;
    out[3] = (s0 >> 19) & 0xFF;
    out[4] = (s0 >> 27) & 0xFF;
    out[5] = (s0 >> 35) & 0xFF;
    
    // Slot 1 (bits 46-86) - 41 bits starting at bit 46
    uint64_t s1 = slot1 & 0x1FFFFFFFFFFULL;
    out[5] |= (s1 << 6) & 0xFF;
    out[6] = (s1 >> 2) & 0xFF;
    out[7] = (s1 >> 10) & 0xFF;
    out[8] = (s1 >> 18) & 0xFF;
    out[9] = (s1 >> 26) & 0xFF;
    out[10] = (s1 >> 34) & 0xFF;
    
    // Slot 2 (bits 87-127) - 41 bits starting at bit 87
    uint64_t s2 = slot2 & 0x1FFFFFFFFFFULL;
    out[10] |= (s2 << 7) & 0xFF;
    out[11] = (s2 >> 1) & 0xFF;
    out[12] = (s2 >> 9) & 0xFF;
    out[13] = (s2 >> 17) & 0xFF;
    out[14] = (s2 >> 25) & 0xFF;
    out[15] = (s2 >> 33) & 0xFF;
}

int main() {
    std::cout << "IA-64 CPU MOV_IMM Test\n";
    std::cout << "======================\n\n";

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

        // Create a test program with MOV_IMM instructions
        std::vector<uint8_t> program(32);  // 2 bundles = 32 bytes
        
        // Bundle 0: Three MOV_IMM instructions (MII template = 0x00)
        // mov r1 = 0x1234
        // mov r2 = 0x5678
        // mov r3 = 0x9ABC
        uint8_t bundle0[16];
        encodeBundle(bundle0, 0x00, 
                     encodeMovImm(1, 0x1234),
                     encodeMovImm(2, 0x5678),
                     encodeMovImm(3, 0x9ABC));
        
        // Bundle 1: MOV register and ADD
        // mov r4 = r1
        // add r5 = r1, r2
        // nop
        uint8_t bundle1[16];
        encodeBundle(bundle1, 0x00,
                     encodeMovReg(4, 1),
                     encodeAdd(5, 1, 2),
                     encodeNop());

        // Copy bundles into program
        for (int i = 0; i < 16; i++) {
            program[i] = bundle0[i];
            program[i + 16] = bundle1[i];
        }

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
        std::cout << "=====================\n\n";

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
        std::cout << "\nFinal Register Values:\n";
        std::cout << "======================\n";
        std::cout << "r1 = 0x" << std::hex << cpu.getState().GetGR(1) << std::dec << " (expected 0x1234)\n";
        std::cout << "r2 = 0x" << std::hex << cpu.getState().GetGR(2) << std::dec << " (expected 0x5678)\n";
        std::cout << "r3 = 0x" << std::hex << cpu.getState().GetGR(3) << std::dec << " (expected 0x9ABC)\n";
        std::cout << "r4 = 0x" << std::hex << cpu.getState().GetGR(4) << std::dec << " (expected 0x1234)\n";
        std::cout << "r5 = 0x" << std::hex << cpu.getState().GetGR(5) << std::dec << " (expected 0x68AC = 0x1234 + 0x5678)\n";

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

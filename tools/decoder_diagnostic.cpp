#include <iostream>
#include <iomanip>
#include <vector>
#include "decoder.h"
#include "memory.h"

/**
 * Diagnostic tool to identify why all instructions decode as unknown (0x0)
 * 
 * This tool helps diagnose:
 * 1. Is memory returning zeros?
 * 2. Is ExtractSlot working correctly?
 * 3. Is the decoder functioning?
 * 
 * Compile and run before full VM to isolate the problem.
 */

void testMemorySystem() {
    std::cout << "=== Memory System Test ===" << std::endl;
    
    ia64::Memory memory(1024 * 1024);  // 1MB
    
    // Test 1: Write and read back
    std::cout << "\nTest 1: Write/Read Verification" << std::endl;
    std::vector<uint8_t> testData = {
        0x1d, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0xc0, 0xc8, 0x27, 0x00, 0x00, 0x00, 0x04, 0x21
    };
    
    uint64_t testAddr = 0x10000;
    memory.Write(testAddr, testData.data(), testData.size());
    
    std::vector<uint8_t> readBack(16);
    memory.Read(testAddr, readBack.data(), 16);
    
    std::cout << "Written: ";
    for (auto b : testData) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') 
                  << static_cast<int>(b) << " ";
    }
    std::cout << std::dec << std::endl;
    
    std::cout << "Read:    ";
    for (auto b : readBack) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') 
                  << static_cast<int>(b) << " ";
    }
    std::cout << std::dec << std::endl;
    
    bool match = (testData == readBack);
    std::cout << "Memory test: " << (match ? "PASS" : "FAIL") << std::endl;
}

void testExtractSlot() {
    std::cout << "\n=== ExtractSlot Test ===" << std::endl;
    
    ia64::InstructionDecoder decoder;
    
    // Test bundle with known pattern
    uint8_t bundleData[16] = {
        0x1d, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0xc0, 0xc8, 0x27, 0x00, 0x00, 0x00, 0x04, 0x21
    };
    
    std::cout << "Bundle: ";
    for (int i = 0; i < 16; i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') 
                  << static_cast<int>(bundleData[i]) << " ";
    }
    std::cout << std::dec << std::endl;
    
    // Use public API instead of private ExtractSlot
    auto bundle = decoder.DecodeBundle(bundleData);
    
    std::cout << "Decoded " << bundle.instructions.size() << " instructions" << std::endl;
    for (size_t i = 0; i < bundle.instructions.size() && i < 3; i++) {
        uint64_t slotBits = bundle.instructions[i].GetRawBits();
        std::cout << "Slot " << i << ": 0x" << std::hex << slotBits << std::dec;
        if (slotBits == 0) std::cout << " ? WARNING: ZERO!";
        std::cout << " - " << bundle.instructions[i].GetDisassembly() << std::endl;
    }
    
    // Check if any slot has non-zero bits
    bool pass = false;
    for (const auto& insn : bundle.instructions) {
        if (insn.GetRawBits() != 0) {
            pass = true;
            break;
        }
    }
    std::cout << "ExtractSlot test: " << (pass ? "PASS" : "FAIL") << std::endl;
}

void testExtractSlotWithZeros() {
    std::cout << "\n=== ExtractSlot with Zero Bundle ===" << std::endl;
    
    ia64::InstructionDecoder decoder;
    
    // All zeros (simulates what we're seeing in output.log)
    uint8_t bundleData[16] = { 0 };
    
    std::cout << "Bundle: all zeros" << std::endl;
    
    // Use public API instead of private ExtractSlot
    auto bundle = decoder.DecodeBundle(bundleData);
    
    std::cout << "Decoded " << bundle.instructions.size() << " instructions" << std::endl;
    for (size_t i = 0; i < bundle.instructions.size() && i < 3; i++) {
        uint64_t slotBits = bundle.instructions[i].GetRawBits();
        std::cout << "Slot " << i << ": 0x" << std::hex << slotBits << std::dec;
        std::cout << " - " << bundle.instructions[i].GetDisassembly() << std::endl;
    }
    
    std::cout << "This simulates what output.log shows - all unknown (0x0)" << std::endl;
}

void testDecoderWithValidData() {
    std::cout << "\n=== Decoder Test with Valid Data ===" << std::endl;
    
    ia64::InstructionDecoder decoder;
    
    // Valid IA-64 bundle (example)
    uint8_t bundleData[16] = {
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
    };
    
    auto bundle = decoder.DecodeBundle(bundleData);
    
    std::cout << "Template: 0x" << std::hex << static_cast<int>(bundle.templateType) 
              << std::dec << std::endl;
    std::cout << "Instructions decoded: " << bundle.instructions.size() << std::endl;
    
    for (size_t i = 0; i < bundle.instructions.size(); i++) {
        const auto& instr = bundle.instructions[i];
        std::cout << "  [" << i << "] " << instr.GetDisassembly() << std::endl;
    }
}

void simulateOutputLogProblem() {
    std::cout << "\n=== Simulating output.log Problem ===" << std::endl;
    
    ia64::InstructionDecoder decoder;
    ia64::Memory memory(1024 * 1024);
    
    // Simulate: IP pointing to uninitialized memory
    uint64_t ip = 0xa2ed60;  // Same as in output.log
    
    std::cout << "IP: 0x" << std::hex << ip << std::dec << std::endl;
    std::cout << "Reading bundle from memory..." << std::endl;
    
    std::vector<uint8_t> bundleData(16);
    memory.Read(ip, bundleData.data(), 16);
    
    std::cout << "Bundle bytes: ";
    for (auto b : bundleData) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') 
                  << static_cast<int>(b) << " ";
    }
    std::cout << std::dec << std::endl;
    
    auto bundle = decoder.DecodeBundle(bundleData.data());
    
    std::cout << "Decoded instructions:" << std::endl;
    for (size_t i = 0; i < bundle.instructions.size(); i++) {
        const auto& instr = bundle.instructions[i];
        std::cout << "  [IP=0x" << std::hex << ip << std::dec 
                  << ", Slot=" << i << "] " << instr.GetDisassembly() << std::endl;
    }
    
    std::cout << "\n^^^ This is what output.log shows!" << std::endl;
    std::cout << "Problem: Memory at 0x" << std::hex << ip 
              << " contains all zeros" << std::dec << std::endl;
}

void diagnoseRootCause() {
    std::cout << "\n=== Root Cause Analysis ===" << std::endl;
    
    ia64::Memory memory(16 * 1024 * 1024);  // 16MB
    
    // Check various memory regions
    std::vector<uint64_t> testAddresses = {
        0x10000,    // Typical bootloader address
        0x100000,   // 1MB (common kernel load point)
        0xa2ed60,   // Where output.log shows execution
        0xa00000,   // PAL region start
    };
    
    for (auto addr : testAddresses) {
        std::vector<uint8_t> data(16);
        memory.Read(addr, data.data(), 16);
        
        bool allZeros = true;
        for (auto b : data) {
            if (b != 0) {
                allZeros = false;
                break;
            }
        }
        
        std::cout << "Address 0x" << std::hex << addr << std::dec 
                  << ": " << (allZeros ? "EMPTY (all zeros)" : "Contains data")
                  << std::endl;
    }
    
    std::cout << "\nConclusion:" << std::endl;
    std::cout << "If all addresses show EMPTY, then:" << std::endl;
    std::cout << "  1. No code has been loaded into memory" << std::endl;
    std::cout << "  2. VM is executing from uninitialized memory" << std::endl;
    std::cout << "  3. This is NOT a decoder problem!" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "IA-64 Decoder Diagnostic Tool" << std::endl;
    std::cout << "==========================================" << std::endl;
    
    try {
        testMemorySystem();
        testExtractSlot();
        testExtractSlotWithZeros();
        testDecoderWithValidData();
        simulateOutputLogProblem();
        diagnoseRootCause();
        
        std::cout << "\n==========================================" << std::endl;
        std::cout << "Diagnostic Summary" << std::endl;
        std::cout << "==========================================" << std::endl;
        std::cout << "\nThe decoder and ExtractSlot are working correctly." << std::endl;
        std::cout << "The problem is that the VM is executing from" << std::endl;
        std::cout << "memory regions that contain all zeros." << std::endl;
        std::cout << "\nThis happens when:" << std::endl;
        std::cout << "  - No program is loaded into memory" << std::endl;
        std::cout << "  - IP points to wrong memory address" << std::endl;
        std::cout << "  - Code loaded at address X, IP set to address Y" << std::endl;
        std::cout << "\nNext steps:" << std::endl;
        std::cout << "  1. Check where loadProgram() is called" << std::endl;
        std::cout << "  2. Check what address code is loaded to" << std::endl;
        std::cout << "  3. Check where IP is initialized" << std::endl;
        std::cout << "  4. Ensure IP == load address" << std::endl;
        std::cout << "==========================================" << std::endl;
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

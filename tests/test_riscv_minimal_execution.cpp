#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

#include ""riscv_cpu.h""
#include ""riscv_decoder.h""

using riscv::RV64CPU;
using riscv::RV64CPUState;

int main() {
    std::cout << ""RISC-V minimal execution test\n"";

    // Test 1: CPU initialization and reset
    RV64CPU cpu;
    cpu.reset();
    
    // Verify PC is initialized to 0
    assert(cpu.getIP() == 0);
    
    // Verify x0 reads as zero (hardwired)
    assert(cpu.readGR(0) == 0);
    
    // Test writing to x0 - should be ignored
    cpu.writeGR(0, 12345);
    assert(cpu.readGR(0) == 0);
    
    // Test x1-x31 can be written and read
    cpu.writeGR(1, 0x123456789ABCDEF0ULL);
    assert(cpu.readGR(1) == 0x123456789ABCDEF0ULL);
    
    cpu.writeGR(31, 0xFEDCBA9876543210ULL);
    assert(cpu.readGR(31) == 0xFEDCBA9876543210ULL);
    
    // Test PC set/get
    cpu.setIP(0x1000);
    assert(cpu.getIP() == 0x1000);
    
    // Test reset behavior
    cpu.reset();
    assert(cpu.getIP() == 0);
    assert(cpu.readGR(0) == 0); // x0 should still be zero
    assert(cpu.readGR(1) == 0); // other registers should be zero after reset
    
    // Test step behavior - it should not crash and return true
    bool stepped = cpu.step();
    assert(stepped == true);
    
    std::cout << ""RISC-V minimal execution test passed\n"";
    return 0;
}

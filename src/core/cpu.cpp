#include "cpu_state.h"
#include <iostream>
#include <iomanip>
#include <cstring>

namespace ia64 {

CPUState::CPUState() {
    Reset();
}

void CPUState::Reset() {
    // Clear all general registers
    gr_.fill(0);
    gr_nat_.fill(false);
    
    // Clear all floating-point registers
    for (auto& fr : fr_) {
        fr.fill(0);
    }
    
    // Initialize predicate registers
    pr_.fill(false);
    pr_[0] = true;  // PR0 is always true
    
    // Clear branch registers
    br_.fill(0);
    
    // Clear application registers
    ar_.fill(0);
    
    // Initialize special registers
    ip_ = 0;
    cfm_ = 0;
    psr_ = 0;
}

uint64_t CPUState::GetGR(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    // GR0 is always 0
    return (index == 0) ? 0 : gr_[index];
}

void CPUState::SetGR(size_t index, uint64_t value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    // GR0 is read-only (always 0)
    if (index != 0) {
        gr_[index] = value;
        gr_nat_[index] = false;
    }
}

bool CPUState::GetGRNaT(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    return (index == 0) ? false : gr_nat_[index];
}

void CPUState::SetGRNaT(size_t index, bool value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    if (index != 0) {
        gr_nat_[index] = value;
    }
}

void CPUState::GetFR(size_t index, uint8_t* out) const {
    if (index >= NUM_FLOAT_REGISTERS) {
        throw std::out_of_range("Floating-point register index out of range");
    }
    std::memcpy(out, fr_[index].data(), 16);
}

void CPUState::SetFR(size_t index, const uint8_t* value) {
    if (index >= NUM_FLOAT_REGISTERS) {
        throw std::out_of_range("Floating-point register index out of range");
    }
    std::memcpy(fr_[index].data(), value, 16);
}

bool CPUState::GetPR(size_t index) const {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    return pr_[index];
}

void CPUState::SetPR(size_t index, bool value) {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    // PR0 is always true
    if (index != 0) {
        pr_[index] = value;
    }
}

uint64_t CPUState::GetBR(size_t index) const {
    if (index >= NUM_BRANCH_REGISTERS) {
        throw std::out_of_range("Branch register index out of range");
    }
    return br_[index];
}

void CPUState::SetBR(size_t index, uint64_t value) {
    if (index >= NUM_BRANCH_REGISTERS) {
        throw std::out_of_range("Branch register index out of range");
    }
    br_[index] = value;
}

uint64_t CPUState::GetAR(size_t index) const {
    if (index >= NUM_APPLICATION_REGISTERS) {
        throw std::out_of_range("Application register index out of range");
    }
    return ar_[index];
}

void CPUState::SetAR(size_t index, uint64_t value) {
    if (index >= NUM_APPLICATION_REGISTERS) {
        throw std::out_of_range("Application register index out of range");
    }
    ar_[index] = value;
}

void CPUState::Dump() const {
    std::cout << "CPU State Dump:\n";
    std::cout << "---------------\n";
    
    // Instruction pointer
    std::cout << "IP:  0x" << std::hex << std::setw(16) << std::setfill('0') << ip_ << std::dec << "\n";
    std::cout << "CFM: 0x" << std::hex << std::setw(16) << std::setfill('0') << cfm_ << std::dec << "\n";
    std::cout << "PSR: 0x" << std::hex << std::setw(16) << std::setfill('0') << psr_ << std::dec << "\n\n";
    
    // General registers (show first 16)
    std::cout << "General Registers (first 16):\n";
    for (size_t i = 0; i < 16; ++i) {
        std::cout << "  GR" << std::setw(3) << i << ": 0x" 
                  << std::hex << std::setw(16) << std::setfill('0') << gr_[i] << std::dec << "\n";
    }
    
    // Predicate registers (first 8)
    std::cout << "\nPredicate Registers (first 8):\n  ";
    for (size_t i = 0; i < 8; ++i) {
        std::cout << "PR" << i << "=" << (pr_[i] ? "1" : "0") << " ";
    }
    std::cout << "\n";
    
    // Branch registers
    std::cout << "\nBranch Registers:\n";
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) {
        std::cout << "  BR" << i << ": 0x" 
                  << std::hex << std::setw(16) << std::setfill('0') << br_[i] << std::dec << "\n";
    }
}

} // namespace ia64

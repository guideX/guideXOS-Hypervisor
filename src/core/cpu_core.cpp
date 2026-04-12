#include "cpu.h"
#include "cpu.h"
#include "decoder.h"
#include "memory.h"
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace ia64 {

// ============================================================================
// RegisterFile Implementation
// ============================================================================

RegisterFile::RegisterFile(size_t totalRegs, size_t staticRegs)
    : registers_(totalRegs, 0), staticRegs_(staticRegs) {
    if (staticRegs > totalRegs) {
        throw std::invalid_argument("staticRegs cannot exceed totalRegs");
    }
}

uint64_t RegisterFile::Read(size_t logicalIndex, uint64_t rotationBase) const {
    if (logicalIndex >= registers_.size()) {
        throw std::out_of_range("Register index out of range");
    }
    
    size_t physical = MapToPhysical(logicalIndex, rotationBase);
    return registers_[physical];
}

void RegisterFile::Write(size_t logicalIndex, uint64_t value, uint64_t rotationBase) {
    if (logicalIndex >= registers_.size()) {
        throw std::out_of_range("Register index out of range");
    }
    
    size_t physical = MapToPhysical(logicalIndex, rotationBase);
    registers_[physical] = value;
}

uint64_t RegisterFile::ReadPhysical(size_t physicalIndex) const {
    if (physicalIndex >= registers_.size()) {
        throw std::out_of_range("Physical register index out of range");
    }
    return registers_[physicalIndex];
}

void RegisterFile::WritePhysical(size_t physicalIndex, uint64_t value) {
    if (physicalIndex >= registers_.size()) {
        throw std::out_of_range("Physical register index out of range");
    }
    registers_[physicalIndex] = value;
}

void RegisterFile::Reset() {
    std::fill(registers_.begin(), registers_.end(), 0);
}

size_t RegisterFile::MapToPhysical(size_t logical, uint64_t rotationBase) const {
    // Static registers (0 to staticRegs_-1) are never rotated
    if (logical < staticRegs_) {
        return logical;
    }
    
    // Rotating registers use the rotation base
    // Physical = static_size + ((logical - static_size + rotation_base) % rotating_size)
    size_t rotatingSize = registers_.size() - staticRegs_;
    size_t offset = (logical - staticRegs_ + rotationBase) % rotatingSize;
    return staticRegs_ + offset;
}

// ============================================================================
// CPU Implementation
// ============================================================================

CPU::CPU(IMemory& memory, IDecoder& decoder)
    : state_(), 
      memory_(memory), 
      decoder_(decoder),
      currentSlot_(0),
      bundleValid_(false) {
    reset();
}

CPU::~CPU() {
    // Nothing to clean up
}

void CPU::reset() {
    // Reset CPU state (registers, IP, etc.)
    state_.Reset();
    
    // Reset bundle tracking
    currentSlot_ = 0;
    bundleValid_ = false;
    currentBundle_ = Bundle();
    
    // IA-64 specific initialization:
    // - GR0 is hardwired to 0 (enforced in CPUState)
    // - PR0 is hardwired to 1 (enforced in CPUState)
    // - IP starts at 0 (can be set by loader)
    // - CFM is 0 (no active register frame)
    // - PSR starts in a default state
    
    // Set a reasonable default PSR (all interrupts disabled, supervisor mode)
    // Bit layout is complex, but for emulation we can start simple
    state_.SetPSR(0);
}

bool CPU::step() {
    // Fetch bundle if needed (every 3 instructions, or if invalid)
    if (!bundleValid_ || currentSlot_ >= currentBundle_.instructions.size()) {
        fetchBundle();
        currentSlot_ = 0;
    }
    
    // Check if we have instructions to execute
    if (currentBundle_.instructions.empty()) {
        std::cerr << "Warning: Empty bundle at IP 0x" << std::hex << state_.GetIP() << std::dec << "\n";
        return false;  // Halt execution
    }
    
    // Get the current instruction
    const InstructionEx& instr = currentBundle_.instructions[currentSlot_];
    
    // Log instruction execution
    std::cout << "[IP=0x" << std::hex << state_.GetIP() << std::dec 
              << ", Slot=" << currentSlot_ << "] "
              << instr.GetDisassembly() << std::endl;
    
    // Execute the instruction
    executeInstruction(instr);
    
    // Move to next slot
    currentSlot_++;
    
    // If we've executed all slots in the bundle, advance IP to next bundle
    if (currentSlot_ >= currentBundle_.instructions.size()) {
        // Each bundle is 16 bytes (128 bits)
        state_.SetIP(state_.GetIP() + 16);
        bundleValid_ = false;
        
        // Check for stop bit (instruction group boundary)
        // In real IA-64, stop bits affect parallel execution
        // For this emulator, we just note it for future implementation
        if (currentBundle_.hasStop) {
            // Stop bit indicates end of instruction group
            // In a real implementation, this would affect:
            // - Register dependency checking
            // - Parallel execution scheduling
            // - Memory ordering
            // For now, we just continue sequentially
        }
    }
    
    return true;  // Continue execution
}

void CPU::executeInstruction(const InstructionEx& instr) {
    // Safe execution with error handling
    try {
        // InstructionEx already has Execute() method that handles execution
        // It also handles predication internally if needed
        instr.Execute(state_, memory_);
    } catch (const std::exception& e) {
        // Handle execution errors safely - don't crash
        std::cerr << "Error executing instruction at IP 0x" << std::hex << state_.GetIP() << std::dec
                  << ": " << e.what() << std::endl;
        std::cerr << "Instruction: " << instr.GetDisassembly() << std::endl;
        std::cerr << "Treating as NOP and continuing..." << std::endl;
    }
}

void CPU::fetchBundle() {
    // Read 16 bytes (128 bits) from memory at current IP
    uint64_t ip = state_.GetIP();
    uint8_t bundleData[16];
    
    try {
        memory_.Read(ip, bundleData, 16);
    } catch (const std::exception& e) {
        std::cerr << "Memory fetch error at IP 0x" << std::hex << ip << std::dec 
                  << ": " << e.what() << "\n";
        bundleValid_ = false;
        return;
    }
    
    // Decode the bundle
    currentBundle_ = decoder_.DecodeBundle(bundleData);
    bundleValid_ = true;
}

size_t CPU::applyRegisterRotation(size_t logicalReg, char regType) const {
    // IA-64 register rotation applies to:
    // - General registers GR32-GR127 (stacked registers)
    // - Floating-point registers FR32-FR127
    // - Predicate registers PR16-PR63 (rotating predicates)
    //
    // Static registers are never rotated:
    // - GR0-GR31
    // - FR0-FR31
    // - PR0-PR15
    
    switch (regType) {
        case 'G':  // General register
            if (logicalReg < 32) {
                return logicalReg;  // Static, no rotation
            } else {
                // Apply GR rotation base (extracted from CFM)
                uint8_t rrb = getGRRotationBase();
                size_t rotatingSize = 96;  // GR32-GR127
                size_t offset = (logicalReg - 32 + rrb) % rotatingSize;
                return 32 + offset;
            }
            
        case 'F':  // Floating-point register
            if (logicalReg < 32) {
                return logicalReg;  // Static, no rotation
            } else {
                uint8_t rrb = getFRRotationBase();
                size_t rotatingSize = 96;  // FR32-FR127
                size_t offset = (logicalReg - 32 + rrb) % rotatingSize;
                return 32 + offset;
            }
            
        case 'P':  // Predicate register
            if (logicalReg < 16) {
                return logicalReg;  // Static, no rotation
            } else {
                uint8_t rrb = getPRRotationBase();
                size_t rotatingSize = 48;  // PR16-PR63
                size_t offset = (logicalReg - 16 + rrb) % rotatingSize;
                return 16 + offset;
            }
            
        default:
            return logicalReg;  // Unknown type, no rotation
    }
}

bool CPU::checkPredicate(size_t predicateReg) const {
    if (predicateReg >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register out of range");
    }
    
    // Apply rotation to predicate register number
    size_t physical = applyRegisterRotation(predicateReg, 'P');
    
    // Return the predicate value
    return state_.GetPR(physical);
}

// Register access methods (delegates to CPUState)

uint64_t CPU::readGR(size_t index) const {
    // Apply rotation for stacked registers
    size_t physical = applyRegisterRotation(index, 'G');
    return state_.GetGR(physical);
}

void CPU::writeGR(size_t index, uint64_t value) {
    size_t physical = applyRegisterRotation(index, 'G');
    state_.SetGR(physical, value);
}

bool CPU::readPR(size_t index) const {
    size_t physical = applyRegisterRotation(index, 'P');
    return state_.GetPR(physical);
}

void CPU::writePR(size_t index, bool value) {
    size_t physical = applyRegisterRotation(index, 'P');
    state_.SetPR(physical, value);
}

uint64_t CPU::readBR(size_t index) const {
    // Branch registers are never rotated
    return state_.GetBR(index);
}

void CPU::writeBR(size_t index, uint64_t value) {
    state_.SetBR(index, value);
}

uint64_t CPU::getIP() const {
    return state_.GetIP();
}

void CPU::setIP(uint64_t addr) {
    state_.SetIP(addr);
    bundleValid_ = false;  // Invalidate current bundle on IP change
}

uint64_t CPU::getCFM() const {
    return state_.GetCFM();
}

void CPU::setCFM(uint64_t value) {
    state_.SetCFM(value);
}

// CFM (Current Frame Marker) layout:
// Bits 0-6:   SOF (Size of Frame) - total registers in frame
// Bits 7-13:  SOL (Size of Locals) - local registers
// Bits 14-17: SOR (Size of Rotating) - rotating registers / 8
// Bits 18-24: RRB.gr - general register rotation base
// Bits 25-31: RRB.fr - FP register rotation base  
// Bits 32-37: RRB.pr - predicate register rotation base

uint8_t CPU::getGRRotationBase() const {
    uint64_t cfm = state_.GetCFM();
    return (cfm >> 18) & 0x7F;  // Bits 18-24 (7 bits)
}

uint8_t CPU::getFRRotationBase() const {
    uint64_t cfm = state_.GetCFM();
    return (cfm >> 25) & 0x7F;  // Bits 25-31 (7 bits)
}

uint8_t CPU::getPRRotationBase() const {
    uint64_t cfm = state_.GetCFM();
    return (cfm >> 32) & 0x3F;  // Bits 32-37 (6 bits)
}

void CPU::dump() const {
    state_.Dump();
    
    // Additional CPU-specific state
    std::cout << "\nCPU Core State:\n";
    std::cout << "  Current Bundle Valid: " << (bundleValid_ ? "Yes" : "No") << "\n";
    std::cout << "  Current Slot: " << currentSlot_ << "\n";
    
    // CFM breakdown
    uint64_t cfm = state_.GetCFM();
    uint8_t sof = cfm & 0x7F;
    uint8_t sol = (cfm >> 7) & 0x7F;
    uint8_t sor = (cfm >> 14) & 0xF;
    
    std::cout << "\nCFM Breakdown:\n";
    std::cout << "  SOF (Size of Frame):   " << static_cast<int>(sof) << "\n";
    std::cout << "  SOL (Size of Locals):  " << static_cast<int>(sol) << "\n";
    std::cout << "  SOR (Size of Rotating): " << static_cast<int>(sor * 8) << "\n";
    std::cout << "  RRB.gr: " << static_cast<int>(getGRRotationBase()) << "\n";
    std::cout << "  RRB.fr: " << static_cast<int>(getFRRotationBase()) << "\n";
    std::cout << "  RRB.pr: " << static_cast<int>(getPRRotationBase()) << "\n";
}

} // namespace ia64

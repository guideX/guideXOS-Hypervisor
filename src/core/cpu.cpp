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

    // Reset explicit RSE view
    rse_ = RSEState();
    
    // Initialize special registers
    ip_ = 0;
    cfm_ = 0;
    psr_ = 0;
}

uint64_t CPUState::GetGR(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    // GR0 is always 0.  The remaining logical register number is mapped
    // through the active rotating GR region in CFM.
    return (index == 0) ? 0 : gr_[MapGR(index)];
}

void CPUState::SetGR(size_t index, uint64_t value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    // GR0 is read-only (always 0)
    if (index != 0) {
        const size_t physical = MapGR(index);
        gr_[physical] = value;
        gr_nat_[physical] = false;
    }
}

bool CPUState::GetGRNaT(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    return (index == 0) ? false : gr_nat_[MapGR(index)];
}

void CPUState::SetGRNaT(size_t index, bool value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    if (index != 0) {
        gr_nat_[MapGR(index)] = value;
    }
}

uint64_t CPUState::GetGRPhysical(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("Physical general register index out of range");
    }
    return index == 0 ? 0 : gr_[index];
}

void CPUState::SetGRPhysical(size_t index, uint64_t value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("Physical general register index out of range");
    }
    if (index != 0) {
        gr_[index] = value;
        gr_nat_[index] = false;
    }
}

bool CPUState::GetGRNaTPhysical(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("Physical general register index out of range");
    }
    return index == 0 ? false : gr_nat_[index];
}

void CPUState::SetGRNaTPhysical(size_t index, bool value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("Physical general register index out of range");
    }
    if (index != 0) {
        gr_nat_[index] = value;
    }
}

void CPUState::GetFR(size_t index, uint8_t* out) const {
    if (index >= NUM_FLOAT_REGISTERS) {
        throw std::out_of_range("Floating-point register index out of range");
    }

    // IA-64 reserves f0 and f1 as architectural constants.  They are not
    // ordinary storage locations: f0 reads as +0.0 and f1 reads as +1.0,
    // while writes to either register are ignored.
    const size_t physical = MapFR(index);
    if (physical == 0) {
        std::memset(out, 0, 16);
        return;
    }
    if (physical == 1) {
        std::memset(out, 0, 16);
        constexpr uint64_t oneSignificand = 0x8000000000000000ULL;
        constexpr uint64_t oneExponent = 0xFFFFULL;
        std::memcpy(out, &oneSignificand, sizeof(oneSignificand));
        std::memcpy(out + 8, &oneExponent, sizeof(oneExponent));
        return;
    }

    std::memcpy(out, fr_[physical].data(), 16);
}

void CPUState::SetFR(size_t index, const uint8_t* value) {
    if (index >= NUM_FLOAT_REGISTERS) {
        throw std::out_of_range("Floating-point register index out of range");
    }

    const size_t physical = MapFR(index);
    if (physical == 0 || physical == 1) {
        return;
    }

    std::memcpy(fr_[physical].data(), value, 16);
}

void CPUState::GetFRPhysical(size_t index, uint8_t* out) const {
    if (index >= NUM_FLOAT_REGISTERS) {
        throw std::out_of_range("Physical floating-point register index out of range");
    }
    if (index == 0) {
        std::memset(out, 0, 16);
        return;
    }
    if (index == 1) {
        std::memset(out, 0, 16);
        constexpr uint64_t oneSignificand = 0x8000000000000000ULL;
        constexpr uint64_t oneExponent = 0xFFFFULL;
        std::memcpy(out, &oneSignificand, sizeof(oneSignificand));
        std::memcpy(out + 8, &oneExponent, sizeof(oneExponent));
        return;
    }
    std::memcpy(out, fr_[index].data(), 16);
}

void CPUState::SetFRPhysical(size_t index, const uint8_t* value) {
    if (index >= NUM_FLOAT_REGISTERS) {
        throw std::out_of_range("Physical floating-point register index out of range");
    }
    if (index != 0 && index != 1) {
        std::memcpy(fr_[index].data(), value, 16);
    }
}

bool CPUState::GetPR(size_t index) const {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    return index == 0 ? true : pr_[MapPR(index)];
}

void CPUState::SetPR(size_t index, bool value) {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    // PR0 is always true
    if (index != 0) {
        pr_[MapPR(index)] = value;
    }
}

bool CPUState::GetPRUnrotated(size_t index) const {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Unrotated predicate register index out of range");
    }
    return index == 0 ? true : pr_[index];
}

void CPUState::SetPRUnrotated(size_t index, bool value) {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Unrotated predicate register index out of range");
    }
    if (index != 0) {
        pr_[index] = value;
    }
}

bool CPUState::GetPRPhysical(size_t index) const {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Physical predicate register index out of range");
    }
    return index == 0 ? true : pr_[index];
}

void CPUState::SetPRPhysical(size_t index, bool value) {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Physical predicate register index out of range");
    }
    if (index != 0) {
        pr_[index] = value;
    }
}

size_t CPUState::MapGR(size_t logical) const {
    if (logical < NUM_STATIC_GR || logical >= NUM_GENERAL_REGISTERS) {
        return logical;
    }
    const size_t rotatingSize = static_cast<size_t>(GetSOR()) * 8;
    if (rotatingSize == 0 || logical >= NUM_STATIC_GR + rotatingSize) {
        return logical;
    }
    return NUM_STATIC_GR +
           ((logical - NUM_STATIC_GR + GetRRB_GR()) % rotatingSize);
}

size_t CPUState::MapFR(size_t logical) const {
    if (logical < NUM_STATIC_FR || logical >= NUM_FLOAT_REGISTERS) {
        return logical;
    }
    constexpr size_t rotatingSize = NUM_FLOAT_REGISTERS - NUM_STATIC_FR;
    return NUM_STATIC_FR +
           ((logical - NUM_STATIC_FR + GetRRB_FR()) % rotatingSize);
}

size_t CPUState::MapPR(size_t logical) const {
    if (logical < NUM_STATIC_PR || logical >= NUM_PREDICATE_REGISTERS) {
        return logical;
    }
    constexpr size_t rotatingSize = NUM_PREDICATE_REGISTERS - NUM_STATIC_PR;
    return NUM_STATIC_PR +
           ((logical - NUM_STATIC_PR + GetRRB_PR()) % rotatingSize);
}

void CPUState::RotateRegisters() {
    uint64_t cfm = cfm_;
    const size_t grSize = static_cast<size_t>(GetSOR()) * 8;
    if (grSize != 0) {
        const size_t next = (static_cast<size_t>(GetRRB_GR()) + grSize - 1) % grSize;
        cfm = (cfm & ~(0x7FULL << 18)) |
              (static_cast<uint64_t>(next) << 18);
    }

    const size_t nextFR = (static_cast<size_t>(GetRRB_FR()) + 96 - 1) % 96;
    const size_t nextPR = (static_cast<size_t>(GetRRB_PR()) + 48 - 1) % 48;
    cfm = (cfm & ~(0x7FULL << 25)) |
          (static_cast<uint64_t>(nextFR) << 25);
    cfm = (cfm & ~(0x3FULL << 32)) |
          (static_cast<uint64_t>(nextPR) << 32);
    SetCFM(cfm);
}

namespace {

ModuloLoopResult ExecuteModuloLoopStage(CPUState& cpu, bool exitSense) {
    ModuloLoopResult result;
    result.lcBefore = cpu.GetAR(65);
    result.ecBefore = cpu.GetAR(66);
    result.cfmBefore = cpu.GetCFM();
    result.pr63Before = cpu.GetPR(63);
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        if (cpu.GetPR(i)) {
            result.prMaskBefore |= (uint64_t{1} << i);
        }
    }

    if (result.lcBefore != 0) {
        cpu.SetAR(65, result.lcBefore - 1);
        cpu.SetPR(63, true);
        result.rotated = true;
        cpu.RotateRegisters();
        result.branchTaken = exitSense ? false : true;
    } else if (result.ecBefore > 1) {
        cpu.SetAR(66, result.ecBefore - 1);
        cpu.SetPR(63, false);
        result.rotated = true;
        cpu.RotateRegisters();
        result.branchTaken = exitSense ? false : true;
    } else if (result.ecBefore == 1) {
        cpu.SetAR(66, 0);
        cpu.SetPR(63, false);
        result.rotated = true;
        cpu.RotateRegisters();
        result.branchTaken = exitSense;
    } else {
        cpu.SetPR(63, false);
        result.branchTaken = exitSense;
    }

    result.lcAfter = cpu.GetAR(65);
    result.ecAfter = cpu.GetAR(66);
    result.cfmAfter = cpu.GetCFM();
    result.pr63After = cpu.GetPR(63);
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        if (cpu.GetPR(i)) {
            result.prMaskAfter |= (uint64_t{1} << i);
        }
    }
    return result;
}

} // namespace

ModuloLoopResult CPUState::ExecuteBrCTop() {
    return ExecuteModuloLoopStage(*this, false);
}

ModuloLoopResult CPUState::ExecuteBrCExit() {
    return ExecuteModuloLoopStage(*this, true);
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

    switch (index) {
        case 16:
            rse_.rsc = value;
            break;
        case 17:
            rse_.bsp = value;
            break;
        case 18:
            rse_.bspstore = value;
            break;
        case 19:
            rse_.rnat = value;
            break;
        case 64:
            rse_.pfs = value;
            rse_.cfm = value;
            rse_.sof = static_cast<uint8_t>(value & 0x7F);
            rse_.sol = static_cast<uint8_t>((value >> 7) & 0x7F);
            rse_.sor = static_cast<uint8_t>((value >> 14) & 0xF);
            break;
        default:
            break;
    }
}

void CPUState::Dump() const {
    std::cout << "CPU State Dump:\n";
    std::cout << "---------------\n";
    
    // Instruction pointer
    std::cout << "IP:  0x" << std::hex << std::setw(16) << std::setfill('0') << ip_ << std::dec << "\n";
    std::cout << "CFM: 0x" << std::hex << std::setw(16) << std::setfill('0') << cfm_ << std::dec << "\n";
    std::cout << "PSR: 0x" << std::hex << std::setw(16) << std::setfill('0') << psr_ << std::dec << "\n\n";
    std::cout << "RSE: rsc=0x" << std::hex << std::setw(16) << std::setfill('0') << rse_.rsc
              << " bsp=0x" << std::setw(16) << rse_.bsp
              << " bspstore=0x" << std::setw(16) << rse_.bspstore
              << " rnat=0x" << std::setw(16) << rse_.rnat
              << " pfs=0x" << std::setw(16) << rse_.pfs
              << " sof=" << std::dec << static_cast<unsigned>(rse_.sof)
              << " sol=" << static_cast<unsigned>(rse_.sol)
              << " sor=" << static_cast<unsigned>(rse_.sor)
              << std::dec << "\n\n";
    
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

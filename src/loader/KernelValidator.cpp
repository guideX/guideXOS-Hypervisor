#include "KernelValidator.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <cerrno>

namespace ia64 {

KernelValidator::KernelValidator()
    : minStackSize_(16 * 1024)           // 16 KB minimum stack
    , minBackingStoreSize_(16 * 1024)    // 16 KB minimum backing store
    , minMemory_(1024 * 1024)            // 1 MB minimum total memory
    , verbose_(false)
{
}

KernelRequirements KernelValidator::ValidateKernelFile(const std::string& filePath,
                                                       ValidationMode mode) {
    // Read file into buffer
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        KernelRequirements req;
        req.errors.push_back("Failed to open kernel file: " + filePath);
        return req;
    }
    
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> buffer(fileSize);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        KernelRequirements req;
        req.errors.push_back("Failed to read kernel file: " + filePath);
        return req;
    }
    
    file.close();
    
    return ValidateKernelBuffer(buffer.data(), buffer.size(), mode);
}

KernelRequirements KernelValidator::ValidateKernelBuffer(const uint8_t* buffer,
                                                        size_t size,
                                                        ValidationMode mode) {
    KernelRequirements req;
    
    // Check minimum size for ELF header
    if (size < sizeof(ELF64_Ehdr)) {
        req.errors.push_back("File too small to be a valid ELF64 binary");
        return req;
    }
    
    const ELF64_Ehdr* ehdr = reinterpret_cast<const ELF64_Ehdr*>(buffer);
    
    // Validate architecture
    ValidateArchitecture(ehdr, req);
    if (!req.isIA64 || !req.isELF64) {
        return req;  // Fatal error, can't continue
    }
    
    // Parse program headers
    std::vector<ELF64_Phdr> phdrs;
    if (ehdr->e_phoff > 0 && ehdr->e_phnum > 0) {
        // Validate program header table is within bounds
        uint64_t phdrEnd = ehdr->e_phoff + (static_cast<uint64_t>(ehdr->e_phnum) * ehdr->e_phentsize);
        if (phdrEnd > size) {
            req.errors.push_back("Program header table extends beyond file size");
            return req;
        }
        
        const uint8_t* phdrStart = buffer + ehdr->e_phoff;
        for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
            const uint8_t* phdrData = phdrStart + (i * ehdr->e_phentsize);
            ELF64_Phdr phdr;
            std::memcpy(&phdr, phdrData, sizeof(ELF64_Phdr));
            phdrs.push_back(phdr);
        }
    }
    
    // Validate entry point
    ValidateEntryPoint(ehdr, phdrs, req);
    
    // Validate segment layout
    ValidateSegmentLayout(phdrs, req);
    
    // Mode-specific validation
    if (mode >= ValidationMode::STANDARD) {
        ValidateMemoryRequirements(phdrs, req);
    }
    
    if (mode >= ValidationMode::STRICT) {
        ValidateStackRequirements(phdrs, req, mode);
    }
    
    if (mode == ValidationMode::KERNEL) {
        ValidateKernelSpecific(ehdr, phdrs, req);
    }
    
    // Summary check
    if (req.errors.empty() && req.hasValidEntryPoint && req.hasCodeSegment) {
        if (verbose_) {
            req.warnings.push_back("Kernel validation passed all checks");
        }
    }
    
    return req;
}

bool KernelValidator::CanBootKernel(const KernelRequirements& requirements) const {
    // Minimum requirements for boot:
    // 1. No critical errors
    if (!requirements.errors.empty()) {
        return false;
    }
    
    // 2. Valid entry point
    if (!requirements.hasValidEntryPoint || !requirements.entryPointAligned) {
        return false;
    }
    
    // 3. At least one code segment
    if (!requirements.hasCodeSegment) {
        return false;
    }
    
    // 4. Valid architecture
    if (!requirements.isIA64 || !requirements.isELF64) {
        return false;
    }
    
    return true;
}

std::string KernelValidator::GetValidationReport(const KernelRequirements& requirements) const {
    std::ostringstream report;
    
    report << "=== Kernel Validation Report ===\n\n";
    
    // Architecture
    report << "Architecture:\n";
    report << "  IA-64: " << (requirements.isIA64 ? "Yes" : "No") << "\n";
    report << "  ELF64: " << (requirements.isELF64 ? "Yes" : "No") << "\n";
    report << "  Executable: " << (requirements.isExecutable ? "Yes" : "No") << "\n\n";
    
    // Entry Point
    report << "Entry Point:\n";
    report << "  Valid: " << (requirements.hasValidEntryPoint ? "Yes" : "No") << "\n";
    report << "  Address: 0x" << std::hex << requirements.entryPoint << std::dec << "\n";
    report << "  Aligned: " << (requirements.entryPointAligned ? "Yes (16-byte)" : "No") << "\n";
    report << "  Executable: " << (requirements.entryPointExecutable ? "Yes" : "No") << "\n\n";
    
    // Segments
    report << "Segments:\n";
    report << "  Code Segment: " << (requirements.hasCodeSegment ? "Yes" : "No");
    if (requirements.hasCodeSegment) {
        report << " (0x" << std::hex << requirements.codeSegmentStart 
               << ", size: " << std::dec << requirements.codeSegmentSize << " bytes)";
    }
    report << "\n";
    
    report << "  Data Segment: " << (requirements.hasDataSegment ? "Yes" : "No");
    if (requirements.hasDataSegment) {
        report << " (0x" << std::hex << requirements.dataSegmentStart 
               << ", size: " << std::dec << requirements.dataSegmentSize << " bytes)";
    }
    report << "\n\n";
    
    // Stack
    report << "Stack:\n";
    report << "  Valid: " << (requirements.stackIsValid ? "Yes" : "No") << "\n";
    if (requirements.stackIsValid) {
        report << "  Top: 0x" << std::hex << requirements.stackTop << std::dec << "\n";
        report << "  Bottom: 0x" << std::hex << requirements.stackBottom << std::dec << "\n";
        report << "  Size: " << requirements.stackSize << " bytes\n";
        report << "  Aligned: " << (requirements.stackIsAligned ? "Yes (16-byte)" : "No") << "\n";
        report << "  Sufficient Space: " << (requirements.stackHasEnoughSpace ? "Yes" : "No") << "\n";
    }
    report << "\n";
    
    // Backing Store (RSE)
    report << "Backing Store (RSE):\n";
    report << "  Present: " << (requirements.hasBackingStore ? "Yes" : "No") << "\n";
    if (requirements.hasBackingStore) {
        report << "  Base: 0x" << std::hex << requirements.backingStoreBase << std::dec << "\n";
        report << "  Size: " << requirements.backingStoreSize << " bytes\n";
    }
    report << "\n";
    
    // Memory
    report << "Memory Requirements:\n";
    report << "  Total Required: " << requirements.totalMemoryRequired << " bytes\n";
    report << "  Minimum Required: " << requirements.minMemoryRequired << " bytes\n\n";
    
    // Errors
    if (!requirements.errors.empty()) {
        report << "ERRORS:\n";
        for (const auto& error : requirements.errors) {
            report << "  [ERROR] " << error << "\n";
        }
        report << "\n";
    }
    
    // Warnings
    if (!requirements.warnings.empty()) {
        report << "WARNINGS:\n";
        for (const auto& warning : requirements.warnings) {
            report << "  [WARN] " << warning << "\n";
        }
        report << "\n";
    }
    
    // Final verdict
    report << "=== Verdict ===\n";
    if (CanBootKernel(requirements)) {
        report << "Kernel image PASSED validation and can be booted.\n";
    } else {
        report << "Kernel image FAILED validation and cannot be booted.\n";
    }
    
    return report.str();
}

void KernelValidator::ValidateArchitecture(const ELF64_Ehdr* ehdr, KernelRequirements& req) {
    // Check ELF magic
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || 
        ehdr->e_ident[EI_MAG1] != ELFMAG1 || 
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || 
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        req.errors.push_back("Invalid ELF magic number");
        return;
    }
    
    // Check ELF class
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        req.errors.push_back("Not a 64-bit ELF file");
        return;
    }
    req.isELF64 = true;
    
    // Check endianness
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        req.errors.push_back("Not little-endian (required for IA-64)");
        return;
    }
    
    // Check machine type
    if (ehdr->e_machine != EM_IA_64) {
        req.errors.push_back("Not an IA-64 binary (machine type: " + 
                           std::to_string(ehdr->e_machine) + ")");
        return;
    }
    req.isIA64 = true;
    
    // Check file type
    if (ehdr->e_type != static_cast<uint16_t>(ELFType::EXEC) && 
        ehdr->e_type != static_cast<uint16_t>(ELFType::DYN)) {
        req.errors.push_back("Not an executable or shared object");
        return;
    }
    req.isExecutable = (ehdr->e_type == static_cast<uint16_t>(ELFType::EXEC));
    
    // Check version
    if (ehdr->e_version != EV_CURRENT) {
        req.warnings.push_back("Unexpected ELF version: " + std::to_string(ehdr->e_version));
    }
}

void KernelValidator::ValidateEntryPoint(const ELF64_Ehdr* ehdr,
                                        const std::vector<ELF64_Phdr>& phdrs,
                                        KernelRequirements& req) {
    req.entryPoint = ehdr->e_entry;
    
    // Entry point of 0 is invalid for kernels
    if (req.entryPoint == 0) {
        req.errors.push_back("Entry point is zero (invalid for kernel)");
        return;
    }
    
    // Check 16-byte alignment (IA-64 instruction bundle boundary)
    if ((req.entryPoint & 0xF) != 0) {
        req.errors.push_back("Entry point not aligned to 16-byte boundary");
        return;
    }
    req.entryPointAligned = true;
    
    // Verify entry point is in an executable segment
    bool foundExecutable = false;
    for (const auto& phdr : phdrs) {
        if (phdr.p_type != static_cast<uint32_t>(SegmentType::PT_LOAD)) {
            continue;
        }
        
        uint64_t segStart = phdr.p_vaddr;
        uint64_t segEnd = phdr.p_vaddr + phdr.p_memsz;
        
        if (req.entryPoint >= segStart && req.entryPoint < segEnd) {
            if ((phdr.p_flags & PF_X) != 0) {
                foundExecutable = true;
                req.entryPointExecutable = true;
                break;
            } else {
                req.errors.push_back("Entry point is in non-executable segment");
                return;
            }
        }
    }
    
    if (!foundExecutable) {
        req.errors.push_back("Entry point not found in any loadable segment");
        return;
    }
    
    req.hasValidEntryPoint = true;
}

void KernelValidator::ValidateSegmentLayout(const std::vector<ELF64_Phdr>& phdrs,
                                           KernelRequirements& req) {
    for (const auto& phdr : phdrs) {
        if (phdr.p_type != static_cast<uint32_t>(SegmentType::PT_LOAD)) {
            continue;
        }
        
        // Check for code segment (executable + readable)
        if ((phdr.p_flags & PF_X) && (phdr.p_flags & PF_R)) {
            req.hasCodeSegment = true;
            if (req.codeSegmentStart == 0 || phdr.p_vaddr < req.codeSegmentStart) {
                req.codeSegmentStart = phdr.p_vaddr;
            }
            req.codeSegmentSize += phdr.p_memsz;
        }
        
        // Check for data segment (writable)
        if ((phdr.p_flags & PF_W)) {
            req.hasDataSegment = true;
            if (req.dataSegmentStart == 0 || phdr.p_vaddr < req.dataSegmentStart) {
                req.dataSegmentStart = phdr.p_vaddr;
            }
            req.dataSegmentSize += phdr.p_memsz;
        }
        
        // Check alignment
        if (phdr.p_align > 1) {
            if ((phdr.p_align & (phdr.p_align - 1)) != 0) {
                req.errors.push_back("Segment alignment is not power of 2");
            }
            
            if ((phdr.p_vaddr % phdr.p_align) != (phdr.p_offset % phdr.p_align)) {
                req.errors.push_back("Segment address/offset not congruent modulo alignment");
            }
        }
        
        // Check file size vs memory size
        if (phdr.p_filesz > phdr.p_memsz) {
            req.errors.push_back("Segment file size exceeds memory size");
        }
    }
    
    if (!req.hasCodeSegment) {
        req.errors.push_back("No executable code segment found");
    }
}

void KernelValidator::ValidateStackRequirements(const std::vector<ELF64_Phdr>& phdrs,
                                               KernelRequirements& req,
                                               ValidationMode mode) {
    // For kernel mode, we expect the kernel to set up its own stack
    // For user mode, we look for a stack segment or assume runtime setup
    
    // Try to find explicit stack segment (GNU_STACK)
    bool foundStackSegment = false;
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == 0x6474e551) {  // PT_GNU_STACK
            foundStackSegment = true;
            req.hasStackSpace = true;
            
            // Check if stack is executable (security concern)
            if ((phdr.p_flags & PF_X) != 0) {
                req.warnings.push_back("Stack segment is executable (security risk)");
            }
            break;
        }
    }
    
    // If no explicit stack segment, kernels typically set up stack in writable segment
    if (!foundStackSegment && mode != ValidationMode::KERNEL) {
        if (req.hasDataSegment) {
            req.hasStackSpace = true;
            req.warnings.push_back("No explicit stack segment, assuming stack in data segment");
        } else {
            req.warnings.push_back("No stack segment found, kernel must set up stack manually");
        }
    } else if (mode == ValidationMode::KERNEL) {
        // Kernels always set up their own stack
        req.hasStackSpace = true;
        req.stackIsValid = true;
        req.stackIsAligned = true;
        req.stackHasEnoughSpace = true;
    }
}

void KernelValidator::ValidateMemoryRequirements(const std::vector<ELF64_Phdr>& phdrs,
                                                KernelRequirements& req) {
    uint64_t minAddr = UINT64_MAX;
    uint64_t maxAddr = 0;
    
    for (const auto& phdr : phdrs) {
        if (phdr.p_type != static_cast<uint32_t>(SegmentType::PT_LOAD)) {
            continue;
        }
        
        minAddr = std::min(minAddr, phdr.p_vaddr);
        maxAddr = std::max(maxAddr, phdr.p_vaddr + phdr.p_memsz);
        req.totalMemoryRequired += phdr.p_memsz;
    }
    
    // Add stack and backing store overhead
    req.minMemoryRequired = req.totalMemoryRequired + minStackSize_ + minBackingStoreSize_;
    
    if (req.minMemoryRequired > minMemory_) {
        if (verbose_) {
            req.warnings.push_back("Kernel requires " + std::to_string(req.minMemoryRequired) + 
                                 " bytes minimum");
        }
    }
}

void KernelValidator::ValidateKernelSpecific(const ELF64_Ehdr* ehdr,
                                            const std::vector<ELF64_Phdr>& phdrs,
                                            KernelRequirements& req) {
    // Check if entry point is in kernel address space
    if (!IsKernelAddress(req.entryPoint)) {
        req.warnings.push_back("Entry point is not in kernel address space region (0xE000000000000000+)");
    }
    
    // Check for IA-64 specific segments
    bool hasUnwindInfo = false;
    bool hasArchExt = false;
    
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == static_cast<uint32_t>(SegmentType::PT_IA_64_UNWIND)) {
            hasUnwindInfo = true;
        }
        if (phdr.p_type == static_cast<uint32_t>(SegmentType::PT_IA_64_ARCHEXT)) {
            hasArchExt = true;
        }
    }
    
    if (!hasUnwindInfo) {
        req.warnings.push_back("No IA-64 unwind information segment (debugging may be limited)");
    }
    
    // Verify segments don't overlap with reserved regions
    for (const auto& phdr : phdrs) {
        if (phdr.p_type != static_cast<uint32_t>(SegmentType::PT_LOAD)) {
            continue;
        }
        
        uint64_t segStart = phdr.p_vaddr;
        uint64_t segEnd = phdr.p_vaddr + phdr.p_memsz;
        
        // Check for overlap with user space (shouldn't happen for kernel)
        if (IsUserAddress(segStart) && IsKernelAddress(segEnd)) {
            req.errors.push_back("Segment spans user and kernel address spaces");
        }
    }
}

bool KernelValidator::IsKernelAddress(uint64_t address) const {
    return address >= KERNEL_REGION_START;
}

bool KernelValidator::IsUserAddress(uint64_t address) const {
    return address < USER_REGION_END;
}

bool KernelValidator::FindStackSegment(const std::vector<ELF64_Phdr>& phdrs,
                                      uint64_t& stackTop,
                                      uint64_t& stackSize) const {
    for (const auto& phdr : phdrs) {
        if (phdr.p_type == 0x6474e551) {  // PT_GNU_STACK
            stackTop = phdr.p_vaddr + phdr.p_memsz;
            stackSize = phdr.p_memsz;
            return true;
        }
    }
    return false;
}

} // namespace ia64

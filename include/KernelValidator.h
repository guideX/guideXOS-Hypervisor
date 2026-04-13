#pragma once

#include "loader.h"
#include <string>
#include <vector>
#include <cstdint>

namespace ia64 {

/**
 * Kernel validation results and requirements
 */
struct KernelRequirements {
    // Entry point requirements
    bool hasValidEntryPoint = false;
    bool entryPointAligned = false;
    bool entryPointExecutable = false;
    uint64_t entryPoint = 0;
    
    // Segment layout requirements
    bool hasCodeSegment = false;
    bool hasDataSegment = false;
    bool hasStackSpace = false;
    uint64_t codeSegmentStart = 0;
    uint64_t codeSegmentSize = 0;
    uint64_t dataSegmentStart = 0;
    uint64_t dataSegmentSize = 0;
    
    // Stack requirements
    bool stackIsValid = false;
    bool stackIsAligned = false;
    bool stackHasEnoughSpace = false;
    uint64_t stackTop = 0;
    uint64_t stackBottom = 0;
    uint64_t stackSize = 0;
    
    // Memory requirements
    uint64_t totalMemoryRequired = 0;
    uint64_t minMemoryRequired = 0;
    
    // Architecture requirements
    bool isIA64 = false;
    bool isELF64 = false;
    bool isExecutable = false;
    
    // Backing store (RSE) requirements for IA-64
    bool hasBackingStore = false;
    uint64_t backingStoreBase = 0;
    uint64_t backingStoreSize = 0;
    
    // Validation errors/warnings
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

/**
 * Validation mode options
 */
enum class ValidationMode {
    MINIMAL,      // Basic checks: magic, architecture, entry point
    STANDARD,     // Standard checks: includes segment layout
    STRICT,       // Strict checks: includes stack, memory limits
    KERNEL        // Kernel-specific checks: kernel address space, privileges
};

/**
 * KernelValidator - Validates kernel images for boot readiness
 * 
 * This class performs comprehensive validation of ELF64 kernel images
 * to ensure they meet minimum requirements for successful boot.
 * 
 * Validation includes:
 * - Entry point validation (alignment, executability, location)
 * - Segment layout verification (code, data, stack segments)
 * - Stack validity checks (size, alignment, location)
 * - Memory requirement verification
 * - IA-64 specific requirements (RSE backing store, register setup)
 */
class KernelValidator {
public:
    KernelValidator();
    ~KernelValidator() = default;
    
    /**
     * Validate a kernel image from file
     * 
     * @param filePath Path to the ELF kernel image
     * @param mode Validation mode (MINIMAL, STANDARD, STRICT, KERNEL)
     * @return KernelRequirements structure with validation results
     */
    KernelRequirements ValidateKernelFile(const std::string& filePath, 
                                         ValidationMode mode = ValidationMode::STANDARD);
    
    /**
     * Validate a kernel image from memory buffer
     * 
     * @param buffer Pointer to ELF kernel image data
     * @param size Size of the buffer in bytes
     * @param mode Validation mode
     * @return KernelRequirements structure with validation results
     */
    KernelRequirements ValidateKernelBuffer(const uint8_t* buffer, 
                                           size_t size,
                                           ValidationMode mode = ValidationMode::STANDARD);
    
    /**
     * Check if kernel meets minimum requirements for boot
     * 
     * @param requirements Validation results from ValidateKernel*
     * @return true if kernel can be booted, false otherwise
     */
    bool CanBootKernel(const KernelRequirements& requirements) const;
    
    /**
     * Get detailed validation report
     * 
     * @param requirements Validation results
     * @return Human-readable validation report
     */
    std::string GetValidationReport(const KernelRequirements& requirements) const;
    
    /**
     * Set minimum stack size requirement (default: 16KB)
     */
    void SetMinimumStackSize(uint64_t size) { minStackSize_ = size; }
    
    /**
     * Set minimum backing store size for RSE (default: 16KB)
     */
    void SetMinimumBackingStoreSize(uint64_t size) { minBackingStoreSize_ = size; }
    
    /**
     * Set minimum total memory requirement (default: 1MB)
     */
    void SetMinimumMemory(uint64_t size) { minMemory_ = size; }
    
    /**
     * Enable/disable verbose validation output
     */
    void SetVerbose(bool verbose) { verbose_ = verbose; }

private:
    // Validation functions
    void ValidateArchitecture(const ELF64_Ehdr* ehdr, KernelRequirements& req);
    void ValidateEntryPoint(const ELF64_Ehdr* ehdr, 
                          const std::vector<ELF64_Phdr>& phdrs,
                          KernelRequirements& req);
    void ValidateSegmentLayout(const std::vector<ELF64_Phdr>& phdrs,
                              KernelRequirements& req);
    void ValidateStackRequirements(const std::vector<ELF64_Phdr>& phdrs,
                                  KernelRequirements& req,
                                  ValidationMode mode);
    void ValidateMemoryRequirements(const std::vector<ELF64_Phdr>& phdrs,
                                   KernelRequirements& req);
    void ValidateKernelSpecific(const ELF64_Ehdr* ehdr,
                               const std::vector<ELF64_Phdr>& phdrs,
                               KernelRequirements& req);
    
    // Helper functions
    bool IsKernelAddress(uint64_t address) const;
    bool IsUserAddress(uint64_t address) const;
    bool FindStackSegment(const std::vector<ELF64_Phdr>& phdrs,
                         uint64_t& stackTop,
                         uint64_t& stackSize) const;
    
    // Configuration
    uint64_t minStackSize_;
    uint64_t minBackingStoreSize_;
    uint64_t minMemory_;
    bool verbose_;
    
    // IA-64 specific constants
    static constexpr uint64_t KERNEL_REGION_START = 0xE000000000000000ULL;
    static constexpr uint64_t USER_REGION_END = 0x0000800000000000ULL;
    static constexpr uint64_t BUNDLE_ALIGNMENT = 16;
    static constexpr uint64_t STACK_ALIGNMENT = 16;
    static constexpr uint64_t PAGE_SIZE = 4096;
};

} // namespace ia64

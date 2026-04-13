/**
 * kernel_validation_example.cpp
 * 
 * Demonstrates kernel validation and minimal kernel boot
 * for the guideXOS Hypervisor.
 * 
 * This example shows:
 * 1. Validating kernel images with different strictness levels
 * 2. Checking minimum boot requirements
 * 3. Generating validation reports
 * 4. Loading and booting a minimal IA-64 kernel
 * 5. Monitoring kernel execution with boot trace
 */

#include "VirtualMachine.h"
#include "KernelValidator.h"
#include "loader.h"
#include "BootTraceSystem.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cstring>
#include <cerrno>

using namespace ia64;

/**
 * Create a minimal test kernel in memory
 * This simulates the minimal_kernel.c compiled kernel
 */
std::vector<uint8_t> createMinimalKernelImage() {
    std::vector<uint8_t> kernel;
    
    // ELF64 Header
    ELF64_Ehdr ehdr;
    std::memset(&ehdr, 0, sizeof(ehdr));
    
    // Magic number
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = 0;  // System V ABI
    ehdr.e_ident[EI_ABIVERSION] = 0;
    
    // File type and machine
    ehdr.e_type = static_cast<uint16_t>(ELFType::EXEC);
    ehdr.e_machine = EM_IA_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_entry = 0xE000000000100000ULL;  // Kernel entry point
    ehdr.e_phoff = sizeof(ELF64_Ehdr);  // Program headers follow immediately
    ehdr.e_shoff = 0;  // No section headers for minimal kernel
    ehdr.e_flags = 0;
    ehdr.e_ehsize = sizeof(ELF64_Ehdr);
    ehdr.e_phentsize = sizeof(ELF64_Phdr);
    ehdr.e_phnum = 2;  // Code and data segments
    ehdr.e_shentsize = 0;
    ehdr.e_shnum = 0;
    ehdr.e_shstrndx = 0;
    
    // Program headers
    ELF64_Phdr phdr_code, phdr_data;
    std::memset(&phdr_code, 0, sizeof(phdr_code));
    std::memset(&phdr_data, 0, sizeof(phdr_data));
    
    // Code segment (text + rodata)
    phdr_code.p_type = static_cast<uint32_t>(SegmentType::PT_LOAD);
    phdr_code.p_flags = PF_R | PF_X;  // Read + Execute
    phdr_code.p_offset = 0x1000;  // 4KB offset in file
    phdr_code.p_vaddr = 0xE000000000100000ULL;
    phdr_code.p_paddr = 0x100000;  // Physical 1MB
    phdr_code.p_filesz = 0x2000;  // 8KB code
    phdr_code.p_memsz = 0x2000;
    phdr_code.p_align = 0x1000;  // Page aligned
    
    // Data segment (data + bss)
    phdr_data.p_type = static_cast<uint32_t>(SegmentType::PT_LOAD);
    phdr_data.p_flags = PF_R | PF_W;  // Read + Write
    phdr_data.p_offset = 0x3000;  // 12KB offset
    phdr_data.p_vaddr = 0xE000000000102000ULL;
    phdr_data.p_paddr = 0x102000;
    phdr_data.p_filesz = 0x1000;  // 4KB initialized data
    phdr_data.p_memsz = 0x2000;   // 8KB total (4KB BSS)
    phdr_data.p_align = 0x1000;
    
    // Build the kernel image
    kernel.resize(0x5000);  // 20KB total
    std::memset(kernel.data(), 0, kernel.size());
    
    // Write ELF header
    std::memcpy(kernel.data(), &ehdr, sizeof(ehdr));
    
    // Write program headers
    std::memcpy(kernel.data() + sizeof(ehdr), &phdr_code, sizeof(phdr_code));
    std::memcpy(kernel.data() + sizeof(ehdr) + sizeof(phdr_code), &phdr_data, sizeof(phdr_data));
    
    // Write minimal code (simplified IA-64 bundles)
    uint64_t* code = reinterpret_cast<uint64_t*>(kernel.data() + 0x1000);
    
    // Bundle 0: NOP instructions (simplified)
    code[0] = 0x0000000000000004ULL;  // Template + NOPs
    code[1] = 0x0000000000000000ULL;
    
    // Bundle 1: More NOPs
    code[2] = 0x0000000000000004ULL;
    code[3] = 0x0000000000000000ULL;
    
    // Add kernel signature to data section
    const char* signature = "GUIDEXOS_MINIMAL_IA64_KERNEL_V1";
    std::memcpy(kernel.data() + 0x3000, signature, strlen(signature) + 1);
    
    return kernel;
}

/**
 * Example 1: Basic Kernel Validation
 */
void example_basic_validation() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "                 Example 1: Basic Kernel Validation                            \n";
    std::cout << "================================================================================\n\n";
    
    // Create validator
    KernelValidator validator;
    validator.SetVerbose(true);
    
    // Create minimal kernel
    std::cout << "Creating minimal test kernel...\n";
    auto kernelImage = createMinimalKernelImage();
    std::cout << "Kernel image size: " << kernelImage.size() << " bytes\n\n";
    
    // Validate with MINIMAL mode
    std::cout << "Validating with MINIMAL mode...\n";
    auto requirements = validator.ValidateKernelBuffer(
        kernelImage.data(), 
        kernelImage.size(),
        ValidationMode::MINIMAL
    );
    
    // Check if can boot
    std::cout << "\nCan boot kernel? " 
              << (validator.CanBootKernel(requirements) ? "YES" : "NO") << "\n\n";
    
    // Display report
    std::cout << validator.GetValidationReport(requirements);
}

/**
 * Example 2: Strict Validation
 */
void example_strict_validation() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "                 Example 2: Strict Kernel Validation                           \n";
    std::cout << "================================================================================\n\n";
    
    KernelValidator validator;
    validator.SetVerbose(true);
    validator.SetMinimumStackSize(16 * 1024);     // 16KB
    validator.SetMinimumBackingStoreSize(16 * 1024);
    validator.SetMinimumMemory(1 * 1024 * 1024);  // 1MB
    
    auto kernelImage = createMinimalKernelImage();
    
    std::cout << "Validating with STRICT mode...\n";
    auto requirements = validator.ValidateKernelBuffer(
        kernelImage.data(),
        kernelImage.size(),
        ValidationMode::STRICT
    );
    
    std::cout << "\n" << validator.GetValidationReport(requirements);
}

/**
 * Example 3: Kernel-Specific Validation
 */
void example_kernel_specific_validation() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 3: Kernel-Specific Validation                            \n";
    std::cout << "================================================================================\n\n";
    
    KernelValidator validator;
    validator.SetVerbose(true);
    
    auto kernelImage = createMinimalKernelImage();
    
    std::cout << "Validating with KERNEL mode (checks kernel address space)...\n";
    auto requirements = validator.ValidateKernelBuffer(
        kernelImage.data(),
        kernelImage.size(),
        ValidationMode::KERNEL
    );
    
    std::cout << "\n" << validator.GetValidationReport(requirements);
}

/**
 * Example 4: Validate and Boot Minimal Kernel
 */
void example_validate_and_boot() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 4: Validate and Boot Minimal Kernel                      \n";
    std::cout << "================================================================================\n\n";
    
    // Step 1: Validate kernel
    KernelValidator validator;
    auto kernelImage = createMinimalKernelImage();
    
    std::cout << "Step 1: Validating kernel image...\n";
    auto requirements = validator.ValidateKernelBuffer(
        kernelImage.data(),
        kernelImage.size(),
        ValidationMode::KERNEL
    );
    
    if (!validator.CanBootKernel(requirements)) {
        std::cerr << "ERROR: Kernel validation failed!\n";
        std::cerr << validator.GetValidationReport(requirements);
        return;
    }
    
    std::cout << "Kernel validation PASSED\n";
    std::cout << "Entry point: 0x" << std::hex << requirements.entryPoint << std::dec << "\n";
    std::cout << "Code segment: " << requirements.codeSegmentSize << " bytes\n";
    std::cout << "Data segment: " << requirements.dataSegmentSize << " bytes\n\n";
    
    // Step 2: Create VM with sufficient memory
    std::cout << "Step 2: Creating virtual machine...\n";
    VirtualMachine vm(8 * 1024 * 1024, 1);  // 8MB memory, 1 CPU
    
    if (!vm.init()) {
        std::cerr << "ERROR: Failed to initialize VM\n";
        return;
    }
    std::cout << "VM initialized successfully\n\n";
    
    // Step 3: Load kernel into VM
    std::cout << "Step 3: Loading kernel into VM memory...\n";
    if (!vm.loadProgram(kernelImage.data(), kernelImage.size(), 0x100000)) {
        std::cerr << "ERROR: Failed to load kernel\n";
        return;
    }
    std::cout << "Kernel loaded at physical address 0x100000\n\n";
    
    // Step 4: Set entry point
    std::cout << "Step 4: Setting kernel entry point...\n";
    vm.setEntryPoint(requirements.entryPoint);
    std::cout << "Entry point set to 0x" << std::hex << requirements.entryPoint << std::dec << "\n\n";
    
    // Step 5: Execute kernel
    std::cout << "Step 5: Executing kernel...\n";
    for (int i = 0; i < 100; i++) {
        if (!vm.step()) {
            std::cout << "Kernel halted at step " << i << "\n";
            break;
        }
    }
    
    // Step 6: Display boot trace
    std::cout << "\nStep 6: Boot trace summary:\n";
    auto stats = vm.getBootTraceSystem().getStatistics();
    std::cout << "  Total events: " << stats.totalEvents << "\n";
    std::cout << "  Syscalls: " << stats.syscallCount << "\n";
    std::cout << "  Exceptions: " << stats.exceptionCount << "\n";
}

/**
 * Example 5: Invalid Kernel Detection
 */
void example_invalid_kernel_detection() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 5: Invalid Kernel Detection                              \n";
    std::cout << "================================================================================\n\n";
    
    KernelValidator validator;
    
    // Create invalid kernel (wrong magic)
    std::vector<uint8_t> invalidKernel(1024);
    std::memset(invalidKernel.data(), 0xFF, invalidKernel.size());
    
    std::cout << "Validating invalid kernel image...\n";
    auto requirements = validator.ValidateKernelBuffer(
        invalidKernel.data(),
        invalidKernel.size(),
        ValidationMode::MINIMAL
    );
    
    std::cout << "\nCan boot? " 
              << (validator.CanBootKernel(requirements) ? "YES" : "NO") << "\n\n";
    
    std::cout << "Validation errors:\n";
    for (const auto& error : requirements.errors) {
        std::cout << "  [ERROR] " << error << "\n";
    }
}

/**
 * Example 6: Real Kernel File Validation
 * (Only runs if kernel file exists)
 */
void example_real_kernel_validation() {
    std::cout << "\n";
    std::cout << "================================================================================\n";
    std::cout << "              Example 6: Real Kernel File Validation                           \n";
    std::cout << "================================================================================\n\n";
    
    const std::string kernelPath = "kernels/vmlinux";  // Example path
    
    // Check if file exists
    std::ifstream file(kernelPath);
    if (!file.good()) {
        std::cout << "Kernel file not found: " << kernelPath << "\n";
        std::cout << "(This is expected - place a real IA-64 kernel to test)\n";
        return;
    }
    
    std::cout << "Validating kernel file: " << kernelPath << "\n";
    
    KernelValidator validator;
    validator.SetVerbose(true);
    
    auto requirements = validator.ValidateKernelFile(kernelPath, ValidationMode::KERNEL);
    
    std::cout << "\n" << validator.GetValidationReport(requirements);
    
    if (validator.CanBootKernel(requirements)) {
        std::cout << "\nThis kernel can be booted in the hypervisor!\n";
    } else {
        std::cout << "\nThis kernel cannot be booted. See errors above.\n";
    }
}

/**
 * Main function
 */
int main(int argc, char* argv[]) {
    std::cout << "================================================================================\n";
    std::cout << "        guideXOS Hypervisor - Kernel Validation Example                        \n";
    std::cout << "================================================================================\n";
    
    try {
        // Run all examples
        example_basic_validation();
        example_strict_validation();
        example_kernel_specific_validation();
        example_validate_and_boot();
        example_invalid_kernel_detection();
        example_real_kernel_validation();
        
        std::cout << "\n";
        std::cout << "================================================================================\n";
        std::cout << "                    All Examples Completed Successfully                        \n";
        std::cout << "================================================================================\n";
        
    } catch (const std::exception& e) {
        std::cerr << "\nException caught: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}

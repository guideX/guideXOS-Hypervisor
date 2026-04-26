#include "VMManager.h"
#include "RawDiskDevice.h"
#include "IStorageDevice.h"
#include "ISO9660Parser.h"
#include "FATParser.h"
#include "PEParser.h"
#include "TestKernelHandler.h"
#include "logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>

namespace ia64 {

namespace {
void DrawBootStatus(VMInstance* instance,
                    const char* line1,
                    const char* line2 = nullptr,
                    const char* line3 = nullptr,
                    uint32_t titleColor = 0xFF6EE7B7) {
    if (!instance || !instance->vm || !instance->vm->getFramebufferDevice()) {
        return;
    }

    FramebufferDevice* fb = instance->vm->getFramebufferDevice();
    // Use a clearly visible dark-blue background (not near-black)
    fb->Clear(0xFF1A1A3E);
    // Title: bright cyan/green
    fb->DrawText(32, 20, "GUIDEXOS IA-64 HYPERVISOR", titleColor, 2);
    // Separator line (draw as a row of dashes)
    fb->DrawText(32, 46, "------------------------", 0xFF505070, 2);
    if (line1) {
        fb->DrawText(32, 72, line1, 0xFFFFFFFF, 2);
    }
    if (line2) {
        fb->DrawText(32, 100, line2, 0xFFFFD166, 2);
    }
    if (line3) {
        fb->DrawText(32, 128, line3, 0xFF93C5FD, 2);
    }
    // Footer hint
    fb->DrawText(32, 460, "CHECK LOGS FOR DETAILS", 0xFF606060, 1);
}
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

VMManager::VMManager()
    : vms_()
    , snapshots_()
    , defaultConfig_(VMConfiguration::createStandard("default"))
    , vmCounter_(0)
    , snapshotCounter_(0) {
    LOG_INFO("VMManager initialized");
}

VMManager::~VMManager() {
    LOG_INFO("VMManager shutting down...");
    stopAllVMs();
    vms_.clear();
    LOG_INFO("VMManager destroyed");
}

// ============================================================================
// VM Creation and Configuration
// ============================================================================

std::string VMManager::createVM(const VMConfiguration& config) {
    // Validate configuration
    std::string validationError;
    if (!config.validate(&validationError)) {
        LOG_ERROR("VM configuration validation failed: " + validationError);
        return "";
    }
    
    // Generate VM ID
    std::string vmId = generateVMId(config.name);
    
    LOG_INFO("Creating VM: " + config.name + " (ID: " + vmId + ")");
    
    try {
        // Create VM instance
        auto instance = std::make_unique<VMInstance>();
        
        // Initialize metadata
        instance->metadata.vmId = vmId;
        instance->metadata.name = config.name;
        instance->metadata.uuid = config.uuid.empty() ? vmId : config.uuid;
        instance->metadata.configuration = config;
        instance->metadata.setState(VMState::CREATED, "VM created");
        
        // Create VirtualMachine
        instance->vm = std::make_unique<VirtualMachine>(
            config.memory.memorySize,
            config.cpu.cpuCount,
            config.cpu.isaType
        );
        
        // Create storage devices
        if (!createStorageDevices(instance.get(), config.storageDevices)) {
            LOG_ERROR("Failed to create storage devices for VM: " + vmId);
            return "";
        }
        
        // Initialize VM
        if (!instance->vm->init()) {
            LOG_ERROR("Failed to initialize VM: " + vmId);
            return "";
        }
        
        instance->metadata.setState(VMState::STOPPED, "VM ready to start");
        
        // Store VM
        vms_[vmId] = std::move(instance);
        
        LOG_INFO("VM created successfully: " + vmId);
        return vmId;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception creating VM: " + std::string(e.what()));
        return "";
    }
}

bool VMManager::deleteVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    // Cannot delete running VM
    if (instance->metadata.currentState == VMState::RUNNING ||
        instance->metadata.currentState == VMState::PAUSED) {
        LOG_ERROR("Cannot delete running VM: " + vmId);
        return false;
    }
    
    LOG_INFO("Deleting VM: " + vmId);
    
    // Delete snapshots
    snapshots_.erase(vmId);
    
    // Delete VM
    vms_.erase(vmId);
    
    LOG_INFO("VM deleted: " + vmId);
    return true;
}

VMConfiguration VMManager::getVMConfiguration(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMConfiguration();
    }
    return instance->metadata.configuration;
}

bool VMManager::updateVMConfiguration(const std::string& vmId, 
                                     const VMConfiguration& config) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    // VM must be stopped to update configuration
    if (instance->metadata.currentState != VMState::STOPPED) {
        LOG_ERROR("VM must be stopped to update configuration: " + vmId);
        return false;
    }
    
    // Validate new configuration
    std::string validationError;
    if (!config.validate(&validationError)) {
        LOG_ERROR("Configuration validation failed: " + validationError);
        return false;
    }
    
    instance->metadata.configuration = config;
    instance->metadata.modifiedTime = std::chrono::system_clock::now();
    
    LOG_INFO("VM configuration updated: " + vmId);
    return true;
}

// ============================================================================
// VM Lifecycle Management
// ============================================================================

bool VMManager::startVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canStart()) {
        LOG_ERROR("Cannot start VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Starting VM: " + vmId);
    
    try {
        instance->metadata.setState(VMState::STARTING, "Starting VM");
        
        // Reset VM if it was terminated
        if (instance->metadata.previousState == VMState::TERMINATED) {
            if (!instance->vm->reset()) {
                instance->metadata.setError("Failed to reset VM");
                return false;
            }
        }
        
        // Load bootloader from boot device if configured
        const auto& bootConfig = instance->metadata.configuration.boot;
        
        // Handle direct boot mode (load kernel directly)
        if (bootConfig.directBoot && !bootConfig.kernelPath.empty()) {
            LOG_INFO("Direct boot mode enabled");
            LOG_INFO("  Kernel path: " + bootConfig.kernelPath);
            LOG_INFO("  Entry point: 0x" + std::to_string(bootConfig.entryPoint));
            
            // Load kernel file
            std::ifstream kernelFile(bootConfig.kernelPath, std::ios::binary | std::ios::ate);
            if (!kernelFile) {
                LOG_ERROR("Failed to open kernel file: " + bootConfig.kernelPath);
                instance->metadata.setError("Kernel file not found");
                return false;
            }
            
            // Get file size
            std::streamsize kernelSize = kernelFile.tellg();
            kernelFile.seekg(0, std::ios::beg);
            
            // Read kernel into memory
            std::vector<uint8_t> kernelData(kernelSize);
            if (!kernelFile.read(reinterpret_cast<char*>(kernelData.data()), kernelSize)) {
                LOG_ERROR("Failed to read kernel file");
                instance->metadata.setError("Failed to read kernel");
                return false;
            }
            
            LOG_INFO("  Kernel size: " + std::to_string(kernelSize) + " bytes");
            
            // Check if this is a test kernel
            if (TestKernelHandler::IsTestKernel(kernelData.data(), kernelData.size())) {
                LOG_INFO("? Detected test kernel");
                
                // Get framebuffer from VM (80x25 VGA text mode)
                uint16_t* framebuffer = instance->vm->getFramebuffer();
                if (framebuffer) {
                    if (TestKernelHandler::ExecuteTestKernel(kernelData.data(), 
                                                             kernelData.size(), 
                                                             framebuffer)) {
                        LOG_INFO("? Test kernel executed successfully!");
                        LOG_INFO("  Framebuffer updated with test pattern");
                        
                        // Don't set entry point for test kernels - they're already "executed"
                        // The VM will just idle
                        instance->vm->setEntryPoint(0xFFFFFFFF); // Invalid entry point (halt)
                    } else {
                        LOG_ERROR("Test kernel execution failed");
                        instance->metadata.setError("Test kernel execution failed");
                        return false;
                    }
                } else {
                    LOG_ERROR("Framebuffer not available");
                    instance->metadata.setError("Framebuffer not available");
                    return false;
                }
            } else {
                // Regular kernel - load into memory
                uint64_t loadAddress = bootConfig.entryPoint != 0 ? 
                                      bootConfig.entryPoint : 0x100000;
                
                LOG_INFO("  Loading kernel to address: 0x" + std::to_string(loadAddress));
                
                if (instance->vm->loadProgram(kernelData.data(), kernelData.size(), loadAddress)) {
                    instance->vm->setEntryPoint(loadAddress);
                    LOG_INFO("? Kernel loaded successfully");
                } else {
                    LOG_ERROR("Failed to load kernel into memory");
                    instance->metadata.setError("Failed to load kernel");
                    return false;
                }
            }
        }
        // Load from boot device if not direct boot
        else if (!bootConfig.bootDevice.empty() && !bootConfig.directBoot) {
            // Find the boot device
            IStorageDevice* bootDevice = nullptr;
            for (const auto& device : instance->storageDevices) {
                if (device->getDeviceId() == bootConfig.bootDevice) {
                    bootDevice = device.get();
                    break;
                }
            }
            
            if (bootDevice) {
                LOG_INFO("Loading bootloader from device: " + bootConfig.bootDevice);
                
                // Try to parse as ISO 9660 filesystem
                LOG_INFO("Attempting to parse ISO 9660 filesystem...");
                ISO9660Parser isoParser(bootDevice);
                
                if (isoParser.parse()) {
                    LOG_INFO("? ISO filesystem parsed successfully");
                    
                    // Find El Torito boot catalog
                    if (isoParser.findBootCatalog()) {
                        LOG_INFO("? El Torito boot catalog found");
                        
                        // Get EFI boot entry
                        const BootEntryInfo* efiEntry = isoParser.getEFIBootEntry();
                        if (efiEntry) {
                            LOG_INFO("? EFI boot entry found");
                            LOG_INFO("  Loading from LBA: " + std::to_string(efiEntry->loadLBA));
                            LOG_INFO("  Size: " + std::to_string(efiEntry->sectorCount) + " sectors");
                            
                            // Extract EFI executable from boot image (FAT filesystem)
                            std::vector<uint8_t> efiExecutable;
                            if (isoParser.extractEFIExecutable(efiExecutable)) {
                                LOG_INFO("? EFI executable extracted: " + 
                                        std::to_string(efiExecutable.size()) + " bytes");
                                
                                // Parse PE/COFF format
                                guideXOS::PEParser peParser;
                                if (peParser.parse(efiExecutable.data(), efiExecutable.size())) {
                                    LOG_INFO("? PE/COFF image parsed successfully");
                                    
                                    if (!peParser.isIA64()) {
                                        LOG_ERROR("EFI executable is not for IA-64 architecture");
                                    } else if (!peParser.isEFI()) {
                                        LOG_WARN("Executable may not be an EFI application");
                                    } else {
                                        // Load PE image properly
                                        std::vector<uint8_t> imageBuffer;
                                        uint64_t loadAddress, entryPoint;
                                        
                                        if (peParser.loadImage(imageBuffer, loadAddress, entryPoint)) {
                                            LOG_INFO("? PE image prepared for loading");
                                            LOG_INFO("  Preferred load address: 0x" + std::to_string(loadAddress));
                                            LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
                                            
                                            // Load into VM memory at preferred address
                                            if (instance->vm->loadProgram(imageBuffer.data(), 
                                                                         imageBuffer.size(), 
                                                                         loadAddress)) {
                                                // Set entry point from PE header
                                                instance->vm->setEntryPoint(entryPoint);
                                                LOG_INFO("? EFI bootloader loaded successfully");
                                                LOG_INFO("  Load address: 0x" + std::to_string(loadAddress));
                                                LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            } else {
                                                LOG_ERROR("Failed to load EFI bootloader into memory");
                                            }
                                        } else {
                                            LOG_ERROR("Failed to prepare PE image for loading");
                                        }
                                    }
                                } else {
                                    LOG_ERROR("Failed to parse EFI executable as PE/COFF");
                                }
                            } else {
                                LOG_ERROR("Failed to extract EFI executable from boot image");
                            }
                        } else {
                            LOG_WARN("No EFI boot entry found in El Torito boot catalog");
                            LOG_INFO("Attempting to find EFI bootloader in ISO filesystem...");
                            
                            // First, list root directory to see what's available
                            LOG_INFO("Listing root directory contents...");
                            isoParser.listRootDirectory();
                            
                            // Fallback: Search for BOOTIA64.EFI in the ISO filesystem
                            const char* efiPaths[] = {
                                "/EFI/BOOT/BOOTIA64.EFI",
                                "EFI/BOOT/BOOTIA64.EFI",
                                "/efi/boot/bootia64.efi",
                                "BOOTIA64.EFI",  // Try just the filename in case it's in root
                                "/BOOT/BOOTIA64.EFI"
                            };
                            
                            std::vector<uint8_t> efiExecutable;
                            bool foundEFI = false;
                            
                            for (const char* path : efiPaths) {
                                LOG_INFO("Searching for: " + std::string(path));
                                if (isoParser.extractFile(path, efiExecutable)) {
                                    LOG_INFO("? Found EFI bootloader: " + std::string(path));
                                    foundEFI = true;
                                    break;
                                }
                            }
                            
                            if (foundEFI) {
                                LOG_INFO("? EFI bootloader extracted: " + 
                                        std::to_string(efiExecutable.size()) + " bytes");
                                
                                // Parse PE/COFF format
                                guideXOS::PEParser peParser;
                                if (peParser.parse(efiExecutable.data(), efiExecutable.size())) {
                                    LOG_INFO("? PE/COFF image parsed successfully");
                                    
                                    if (!peParser.isIA64()) {
                                        LOG_ERROR("EFI executable is not for IA-64 architecture");
                                    } else if (!peParser.isEFI()) {
                                        LOG_WARN("Executable may not be an EFI application");
                                    } else {
                                        // Load PE image properly
                                        std::vector<uint8_t> imageBuffer;
                                        uint64_t loadAddress, entryPoint;
                                        
                                        if (peParser.loadImage(imageBuffer, loadAddress, entryPoint)) {
                                            LOG_INFO("? PE image prepared for loading");
                                            LOG_INFO("  Preferred load address: 0x" + std::to_string(loadAddress));
                                            LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
                                            
                                            // Load into VM memory at preferred address
                                            if (instance->vm->loadProgram(imageBuffer.data(), 
                                                                         imageBuffer.size(), 
                                                                         loadAddress)) {
                                                // Set entry point from PE header
                                                instance->vm->setEntryPoint(entryPoint);
                                                LOG_INFO("? EFI bootloader loaded successfully from filesystem");
                                                LOG_INFO("  Load address: 0x" + std::to_string(loadAddress));
                                                LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            } else {
                                                LOG_ERROR("Failed to load EFI bootloader into memory");
                                            }
                                        } else {
                                            LOG_ERROR("Failed to prepare PE image for loading");
                                        }
                                    }
                                } else {
                                    LOG_ERROR("Failed to parse EFI executable as PE/COFF");
                                }
                            } else {
                                LOG_ERROR("Could not find EFI bootloader in ISO filesystem");
                                LOG_INFO("Checking for boot image with embedded EFI bootloader...");
                                
                                // Try to find BOOT.IMG and search for EFI bootloader inside it
                                const char* bootImgPaths[] = {
                                    "/boot/boot.img",
                                    "/BOOT/BOOT.IMG",
                                    "boot/boot.img",
                                    "BOOT/BOOT.IMG",
                                    "/boot.img",
                                    "/BOOT.IMG"
                                };
                                
                                std::vector<uint8_t> bootImgData;
                                bool foundBootImg = false;
                                std::string bootImgPath;
                                
                                for (const char* path : bootImgPaths) {
                                    if (isoParser.extractFile(path, bootImgData)) {
                                        LOG_INFO("? Found boot image: " + std::string(path));
                                        foundBootImg = true;
                                        bootImgPath = path;
                                        break;
                                    }
                                }
                                
                                bool bootedFromImage = false;
                                
                                if (foundBootImg) {
                                    LOG_INFO("? Boot image extracted: " + 
                                            std::to_string(bootImgData.size()) + " bytes");
                                    LOG_INFO("  Parsing as FAT filesystem...");
                                    
                                    // Parse boot image as FAT filesystem
                                    guideXOS::FATParser fatParser;
                                    if (fatParser.parse(bootImgData.data(), bootImgData.size())) {
                                        LOG_INFO("? FAT filesystem parsed successfully");
                                        
                                        // Search for EFI bootloader inside the FAT image
                                        const char* efiPathsInFAT[] = {
                                            "/EFI/BOOT/BOOTIA64.EFI",
                                            "EFI/BOOT/BOOTIA64.EFI",
                                            "/efi/boot/bootia64.efi",
                                            "efi/boot/bootia64.efi"
                                        };
                                        
                                        guideXOS::FATFileInfo efiFileInfo;
                                        std::string foundEFIPath;
                                        
                                        for (const char* path : efiPathsInFAT) {
                                            LOG_INFO("  Searching in boot image: " + std::string(path));
                                            if (fatParser.findFile(path, efiFileInfo)) {
                                                LOG_INFO("  ?? Found EFI bootloader in boot image!");
                                                foundEFIPath = path;
                                                break;
                                            }
                                        }
                                        
                                        if (!foundEFIPath.empty()) {
                                            // Extract EFI bootloader from FAT image
                                            if (fatParser.readFile(efiFileInfo, efiExecutable)) {
                                                LOG_INFO("??? EFI bootloader extracted from boot image: " + 
                                                        std::to_string(efiExecutable.size()) + " bytes");
                                                
                                                // Parse and load the EFI executable
                                                guideXOS::PEParser peParser;
                                                if (peParser.parse(efiExecutable.data(), efiExecutable.size())) {
                                                    LOG_INFO("? PE/COFF image parsed successfully");
                                                    
                                                    if (!peParser.isIA64()) {
                                                        LOG_ERROR("EFI executable is not for IA-64 architecture");
                                                    } else if (!peParser.isEFI()) {
                                                        LOG_WARN("Executable may not be an EFI application");
                                                    } else {
                                                        // Load PE image properly
                                                        std::vector<uint8_t> imageBuffer;
                                                        uint64_t loadAddress, entryPoint;
                                                        
                                                        if (peParser.loadImage(imageBuffer, loadAddress, entryPoint)) {
                                                            LOG_INFO("? PE image prepared for loading");
                                                            
                                                            // Use ostringstream for proper hex formatting
                                                            std::ostringstream oss;
                                                            oss << "  Preferred load address: 0x" << std::hex << loadAddress << std::dec;
                                                            LOG_INFO(oss.str());
                                                            
                                                            oss.str("");
                                                            oss << "  Entry point: 0x" << std::hex << entryPoint << std::dec;
                                                            LOG_INFO(oss.str());
                                                            
                                                            LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
                                                            
                                                            // Load into VM memory
                                                            if (instance->vm->loadProgram(imageBuffer.data(), 
                                                                                         imageBuffer.size(), 
                                                                                         loadAddress)) {
                                                                instance->vm->setEntryPoint(entryPoint);
                                                                const auto& peInfo = peParser.getImageInfo();
                                                                if (peInfo.hasGlobalPointer) {
                                                                    instance->vm->getCPU().getState().SetGR(1, peInfo.globalPointer);
                                                                    oss.str("");
                                                                    oss << "  Global pointer (r1): 0x" << std::hex
                                                                        << peInfo.globalPointer << std::dec;
                                                                    LOG_INFO(oss.str());
                                                                }

                                                                // Set up minimal EFI firmware environment
                                                                // BOOTIA64.EFI entry: efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
                                                                // These are passed in r32 (in0) and r33 (in1) per IA-64 ABI.
                                                                // Without valid pointers, early EFI code (e.g. st8 [r33]=r16)
                                                                // corrupts address 0 (the loaded image itself).
                                                                constexpr uint64_t EFI_STUB_ADDR = 0x200000ULL; // above the loaded EFI image
                                                                constexpr uint64_t EFI_IMAGE_HANDLE = 0x1ULL;   // dummy non-null handle

                                                                // Write a zeroed-out minimal EFI System Table stub
                                                                {
                                                                    static const uint8_t stub[256] = {}; // all zeros
                                                                    // Write EFI_SYSTEM_TABLE signature at offset 0
                                                                    // "IBI SYST" = 0x5453595320494249
                                                                    static const uint8_t sig[] = {
                                                                        0x49,0x42,0x49,0x20,0x53,0x59,0x53,0x54
                                                                    };
                                                                    instance->vm->getMemory().Write(EFI_STUB_ADDR, stub, sizeof(stub));
                                                                    instance->vm->getMemory().Write(EFI_STUB_ADDR, sig, sizeof(sig));
                                                                    oss.str("");
                                                                    oss << "  EFI System Table stub at: 0x" << std::hex << EFI_STUB_ADDR << std::dec;
                                                                    LOG_INFO(oss.str());
                                                                }

                                                                // Set EFI entry arguments: r32=ImageHandle, r33=SystemTable
                                                                instance->vm->writeGR(0, 32, EFI_IMAGE_HANDLE);
                                                                instance->vm->writeGR(0, 33, EFI_STUB_ADDR);
                                                                LOG_INFO("  Set r32=ImageHandle=0x1, r33=EFI_SystemTable=0x200000");

                                                                LOG_INFO("??? EFI bootloader loaded successfully from boot image!");
                                                                LOG_INFO("  Source: " + bootImgPath + " -> " + foundEFIPath);
                                                                
                                                                oss.str("");
                                                                oss << "  Load address: 0x" << std::hex << loadAddress << std::dec;
                                                                LOG_INFO(oss.str());
                                                                
                                                                oss.str("");
                                                                oss << "  Entry point: 0x" << std::hex << entryPoint << std::dec;
                                                                LOG_INFO(oss.str());

                                                                DrawBootStatus(instance,
                                                                               "EFI BOOTLOADER LOADED",
                                                                               "STARTING IA64 EXECUTION",
                                                                               "SEE LOGS FOR DECODE TRACE");
                                                                
                                                                bootedFromImage = true;
                                                            } else {
                                                                LOG_ERROR("Failed to load EFI bootloader into memory");
                                                                DrawBootStatus(instance,
                                                                               "BOOT FAILED",
                                                                               "EFI LOAD INTO MEMORY FAILED",
                                                                               "SEE NATIVE LOG");
                                                            }
                                                        } else {
                                                            LOG_ERROR("Failed to prepare PE image for loading");
                                                        }
                                                    }
                                                } else {
                                                    LOG_ERROR("Failed to parse EFI executable as PE/COFF");
                                                }
                                            } else {
                                                LOG_ERROR("Failed to read EFI file from boot image");
                                            }
                                        } else {
                                            LOG_WARN("No EFI bootloader found in boot image");
                                        }
                                    } else {
                                        LOG_WARN("Boot image is not a valid FAT filesystem");
                                    }
                                }
                                
                                // If we didn't boot from image, try other fallbacks
                                if (!bootedFromImage) {
                                    LOG_INFO("This ISO does not appear to have EFI boot support.");
                                    LOG_INFO("Attempting to find IA-64 kernel for direct boot...");
                                    
                                    // Try to find the IA-64 kernel in common Debian installer locations
                                    const char* kernelPaths[] = {
                                        "/install/vmlinuz",
                                        "/install/vmlinux",
                                        "/boot/vmlinuz",
                                        "/boot/vmlinux",
                                        "install/vmlinuz",
                                        "install/vmlinux",
                                        "boot/vmlinuz",
                                        "boot/vmlinux"
                                    };
                                    
                                    std::vector<uint8_t> kernelData;
                                    bool foundKernel = false;
                                    std::string foundPath;
                                    
                                    for (const char* path : kernelPaths) {
                                        LOG_INFO("Searching for kernel: " + std::string(path));
                                        if (isoParser.extractFile(path, kernelData)) {
                                            LOG_INFO("? Found kernel: " + std::string(path));
                                            foundKernel = true;
                                            foundPath = path;
                                            break;
                                        }
                                    }
                                    
                                    if (foundKernel) {
                                        LOG_INFO("? Kernel extracted: " + 
                                                std::to_string(kernelData.size()) + " bytes");
                                        LOG_INFO("  Path: " + foundPath);
                                        
                                        // TODO: Parse ELF kernel format and load properly
                                        // For now, attempt basic loading
                                        uint64_t kernelAddress = 0x100000;  // Standard kernel load address
                                        
                                        if (instance->vm->loadProgram(kernelData.data(), 
                                                                     kernelData.size(), 
                                                                     kernelAddress)) {
                                            instance->vm->setEntryPoint(kernelAddress);
                                            LOG_INFO("? Kernel loaded at 0x" + 
                                                    std::to_string(kernelAddress));
                                            LOG_WARN("Direct kernel boot attempted - this is experimental");
                                            LOG_WARN("The kernel may require additional setup (initrd, boot parameters, etc.)");
                                        } else {
                                            LOG_ERROR("Failed to load kernel into memory");
                                        }
                                    } else {
                                        LOG_WARN("No IA-64 kernel found in standard locations");
                                        LOG_INFO("Falling back to raw boot sector loading...");
                                        
                                        // Fallback: Load first 4KB as before
                                        std::vector<uint8_t> bootSector(4096);
                                        int64_t bytesRead = bootDevice->readBytes(0, bootSector.size(), bootSector.data());
                                        
                                        if (bytesRead > 0) {
                                            uint64_t bootAddress = bootConfig.entryPoint != 0 ? 
                                                                  bootConfig.entryPoint : 0x100000;
                                            LOG_INFO("Loading raw boot data to address: 0x" + 
                                                    std::to_string(bootAddress));
                                            LOG_WARN("WARNING: Loading x86 BIOS boot code on IA-64 architecture");
                                            LOG_WARN("This will NOT work. Please use an EFI-bootable IA-64 ISO or direct kernel boot.");
                                            
                                            if (instance->vm->loadProgram(bootSector.data(), 
                                                                         static_cast<size_t>(bytesRead), 
                                                                         bootAddress)) {
                                                instance->vm->setEntryPoint(bootAddress);
                                                LOG_INFO("Raw boot data loaded, entry point: 0x" + 
                                                        std::to_string(bootAddress));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        LOG_WARN("El Torito boot catalog not found");
                        LOG_INFO("This may not be a bootable ISO");
                    }
                } else {
                    LOG_WARN("Failed to parse ISO filesystem, trying raw boot sector...");
                    
                    // Fallback: Load first 4KB as raw boot sector
                    std::vector<uint8_t> bootSector(4096);
                    int64_t bytesRead = bootDevice->readBytes(0, bootSector.size(), bootSector.data());
                    
                    if (bytesRead > 0) {
                        uint64_t bootAddress = bootConfig.entryPoint != 0 ? 
                                              bootConfig.entryPoint : 0x100000;
                        LOG_INFO("Loading raw boot data to address: 0x" + 
                                std::to_string(bootAddress));
                        
                        if (instance->vm->loadProgram(bootSector.data(), 
                                                     static_cast<size_t>(bytesRead), 
                                                     bootAddress)) {
                            instance->vm->setEntryPoint(bootAddress);
                            LOG_INFO("Raw boot data loaded, entry point: 0x" + 
                                    std::to_string(bootAddress));
                        } else {
                            LOG_ERROR("Failed to load boot data into memory");
                        }
                    } else {
                        LOG_ERROR("Failed to read boot data from device");
                    }
                }
            } else {
                LOG_WARN("Boot device not found: " + bootConfig.bootDevice);
            }
        }
        
        instance->metadata.setState(VMState::RUNNING, "VM running");
        instance->metadata.startedTime = std::chrono::system_clock::now();
        
        LOG_INFO("VM started: " + vmId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception starting VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::stopVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canStop()) {
        LOG_ERROR("Cannot stop VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Stopping VM: " + vmId);
    
    try {
        instance->metadata.setState(VMState::STOPPING, "Stopping VM");
        
        instance->vm->halt();
        
        // Update resource usage before stopping
        updateResourceUsage(instance);
        
        instance->metadata.setState(VMState::STOPPED, "VM stopped");
        instance->metadata.stoppedTime = std::chrono::system_clock::now();
        
        LOG_INFO("VM stopped: " + vmId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception stopping VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::pauseVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canPause()) {
        LOG_ERROR("Cannot pause VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Pausing VM: " + vmId);
    
    instance->metadata.setState(VMState::PAUSING, "Pausing VM");
    
    // Update resource usage before pausing
    updateResourceUsage(instance);
    
    instance->metadata.setState(VMState::PAUSED, "VM paused");
    
    LOG_INFO("VM paused: " + vmId);
    return true;
}

bool VMManager::resumeVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canResume()) {
        LOG_ERROR("Cannot resume VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Resuming VM: " + vmId);
    
    instance->metadata.setState(VMState::RESUMING, "Resuming VM");
    instance->metadata.setState(VMState::RUNNING, "VM running");
    
    LOG_INFO("VM resumed: " + vmId);
    return true;
}

bool VMManager::resetVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    LOG_INFO("Resetting VM: " + vmId);
    
    try {
        VMState previousState = instance->metadata.currentState;
        
        if (!instance->vm->reset()) {
            instance->metadata.setError("Failed to reset VM");
            return false;
        }
        
        instance->metadata.resourceUsage.reset();
        instance->metadata.clearError();
        
        // Restore previous state if it was running
        if (previousState == VMState::RUNNING) {
            instance->metadata.setState(VMState::RUNNING, "VM reset and running");
        } else {
            instance->metadata.setState(VMState::STOPPED, "VM reset");
        }
        
        LOG_INFO("VM reset: " + vmId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception resetting VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::terminateVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    LOG_INFO("Terminating VM: " + vmId);
    
    instance->metadata.setState(VMState::TERMINATING, "Terminating VM");
    
    instance->vm->halt();
    updateResourceUsage(instance);
    
    instance->metadata.setState(VMState::TERMINATED, "VM terminated");
    instance->metadata.stoppedTime = std::chrono::system_clock::now();
    
    LOG_INFO("VM terminated: " + vmId);
    return true;
}

// ============================================================================
// VM Execution Control
// ============================================================================

uint64_t VMManager::runVM(const std::string& vmId, uint64_t maxCycles) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return 0;
    }
    
    if (instance->metadata.currentState != VMState::RUNNING) {
        LOG_ERROR("VM is not running: " + vmId);
        return 0;
    }
    
    try {
        uint64_t cyclesExecuted = instance->vm->run(maxCycles);

        if (cyclesExecuted == 0 || instance->vm->getState() == VMState::ERROR) {
            std::ostringstream oss;
            oss << "STOPPED AT IP=0x" << std::hex << instance->vm->getIP() << std::dec;
            const std::string stopLine = oss.str();

            // Show a clearly red boot-failure screen
            DrawBootStatus(instance,
                           "BOOT FAILED",
                           stopLine.c_str(),
                           "CHECK NATIVE LOG FOR [SKIP] WARNINGS",
                           0xFFFF4444);

            if (instance->vm->getState() == VMState::ERROR) {
                instance->metadata.setError("VM execution stopped; see native log for [SKIP] warnings");
            }
        } else {
            // Execution ran a full batch — show a "RUNNING" heartbeat on-screen
            // so the user can see the boot is progressing (updated each RunVM call)
            std::ostringstream oss;
            oss << "CYCLES: " << instance->metadata.resourceUsage.cyclesExecuted + cyclesExecuted;
            const std::string cyclesLine = oss.str();

            std::ostringstream ipOss;
            ipOss << "IP=0x" << std::hex << instance->vm->getIP() << std::dec;
            const std::string ipLine = ipOss.str();

            DrawBootStatus(instance,
                           "EFI BOOTING",
                           cyclesLine.c_str(),
                           ipLine.c_str(),
                           0xFF6EE7B7);
        }
        
        // Update resource usage
        instance->metadata.resourceUsage.cyclesExecuted += cyclesExecuted;
        
        return cyclesExecuted;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception running VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return 0;
    }
}

bool VMManager::stepVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (instance->metadata.currentState != VMState::RUNNING) {
        LOG_ERROR("VM is not running: " + vmId);
        return false;
    }
    
    try {
        bool result = instance->vm->step();
        
        instance->metadata.resourceUsage.instructionsExecuted++;
        
        return result;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception stepping VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

// ============================================================================
// Snapshot Management
// ============================================================================

std::string VMManager::snapshotVM(const std::string& vmId,
                                  const std::string& snapshotName,
                                  const std::string& description) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return "";
    }
    
    if (!instance->metadata.canSnapshot()) {
        LOG_ERROR("Cannot snapshot VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return "";
    }
    
    LOG_INFO("Creating snapshot for VM: " + vmId + " - " + snapshotName);
    
    try {
        VMState previousState = instance->metadata.currentState;
        instance->metadata.setState(VMState::SNAPSHOTTING, "Creating snapshot");
        
        // Generate snapshot ID
        std::string snapshotId = generateSnapshotId(snapshotName);
        
        // Create snapshot metadata
        VMSnapshot snapshot;
        snapshot.snapshotId = snapshotId;
        snapshot.name = snapshotName;
        snapshot.description = description;
        snapshot.createdTime = std::chrono::system_clock::now();
        snapshot.vmState = previousState;
        snapshot.memorySize = instance->metadata.configuration.memory.memorySize;
        snapshot.vmMetadata = instance->metadata;
        
        // Store snapshot
        snapshots_[vmId].push_back(snapshot);
        instance->metadata.snapshotCount = snapshots_[vmId].size();
        
        // Restore previous state
        instance->metadata.setState(previousState, "Snapshot created");
        
        LOG_INFO("Snapshot created: " + snapshotId);
        return snapshotId;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception creating snapshot: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return "";
    }
}

bool VMManager::restoreSnapshot(const std::string& vmId, const std::string& snapshotId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    // Find snapshot
    auto snapshotIt = snapshots_.find(vmId);
    if (snapshotIt == snapshots_.end()) {
        LOG_ERROR("No snapshots found for VM: " + vmId);
        return false;
    }
    
    auto& vmSnapshots = snapshotIt->second;
    auto it = std::find_if(vmSnapshots.begin(), vmSnapshots.end(),
        [&snapshotId](const VMSnapshot& s) { return s.snapshotId == snapshotId; });
    
    if (it == vmSnapshots.end()) {
        LOG_ERROR("Snapshot not found: " + snapshotId);
        return false;
    }
    
    LOG_INFO("Restoring snapshot: " + snapshotId + " for VM: " + vmId);
    
    try {
        instance->metadata.setState(VMState::RESTORING, "Restoring snapshot");
        
        // Restore metadata from snapshot
        const VMSnapshot& snapshot = *it;
        instance->metadata = snapshot.vmMetadata;
        instance->metadata.activeSnapshot = snapshotId;
        
        // Reset VM
        if (!instance->vm->reset()) {
            instance->metadata.setError("Failed to reset VM during restore");
            return false;
        }
        
        instance->metadata.setState(VMState::STOPPED, "Snapshot restored");
        
        LOG_INFO("Snapshot restored: " + snapshotId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception restoring snapshot: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::deleteSnapshot(const std::string& vmId, const std::string& snapshotId) {
    auto snapshotIt = snapshots_.find(vmId);
    if (snapshotIt == snapshots_.end()) {
        LOG_ERROR("No snapshots found for VM: " + vmId);
        return false;
    }
    
    auto& vmSnapshots = snapshotIt->second;
    auto it = std::find_if(vmSnapshots.begin(), vmSnapshots.end(),
        [&snapshotId](const VMSnapshot& s) { return s.snapshotId == snapshotId; });
    
    if (it == vmSnapshots.end()) {
        LOG_ERROR("Snapshot not found: " + snapshotId);
        return false;
    }
    
    LOG_INFO("Deleting snapshot: " + snapshotId);
    
    vmSnapshots.erase(it);
    
    auto* instance = getVMInstance(vmId);
    if (instance) {
        instance->metadata.snapshotCount = vmSnapshots.size();
    }
    
    LOG_INFO("Snapshot deleted: " + snapshotId);
    return true;
}

std::vector<VMSnapshot> VMManager::listSnapshots(const std::string& vmId) const {
    auto it = snapshots_.find(vmId);
    if (it == snapshots_.end()) {
        return std::vector<VMSnapshot>();
    }
    return it->second;
}

// ============================================================================
// Storage Device Management
// ============================================================================

bool VMManager::attachStorage(const std::string& vmId, 
                              std::unique_ptr<IStorageDevice> device) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!device) {
        LOG_ERROR("Invalid storage device");
        return false;
    }
    
    std::string deviceId = device->getDeviceId();
    LOG_INFO("Attaching storage device " + deviceId + " to VM: " + vmId);
    
    // Check for duplicate device ID
    for (const auto& existingDevice : instance->storageDevices) {
        if (existingDevice->getDeviceId() == deviceId) {
            LOG_ERROR("Storage device already attached: " + deviceId);
            return false;
        }
    }
    
    // Connect device
    if (!device->connect()) {
        LOG_ERROR("Failed to connect storage device: " + deviceId);
        return false;
    }
    
    instance->storageDevices.push_back(std::move(device));
    
    LOG_INFO("Storage device attached: " + deviceId);
    return true;
}

bool VMManager::detachStorage(const std::string& vmId, const std::string& deviceId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    LOG_INFO("Detaching storage device " + deviceId + " from VM: " + vmId);
    
    auto it = std::find_if(instance->storageDevices.begin(), 
                          instance->storageDevices.end(),
        [&deviceId](const std::unique_ptr<IStorageDevice>& dev) {
            return dev->getDeviceId() == deviceId;
        });
    
    if (it == instance->storageDevices.end()) {
        LOG_ERROR("Storage device not found: " + deviceId);
        return false;
    }
    
    (*it)->disconnect();
    instance->storageDevices.erase(it);
    
    LOG_INFO("Storage device detached: " + deviceId);
    return true;
}

StorageDeviceInfo VMManager::getStorageInfo(const std::string& vmId, 
                                            const std::string& deviceId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return StorageDeviceInfo();
    }
    
    for (const auto& device : instance->storageDevices) {
        if (device->getDeviceId() == deviceId) {
            return device->getInfo();
        }
    }
    
    return StorageDeviceInfo();
}

// ============================================================================
// VM Query and Status
// ============================================================================

VMMetadata VMManager::getVMMetadata(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMMetadata();
    }
    return instance->metadata;
}

VMState VMManager::getVMState(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMState::ERROR;
    }
    return instance->metadata.currentState;
}

VMResourceUsage VMManager::getVMResourceUsage(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMResourceUsage();
    }
    return instance->metadata.resourceUsage;
}

bool VMManager::vmExists(const std::string& vmId) const {
    return vms_.find(vmId) != vms_.end();
}

std::vector<std::string> VMManager::listVMs() const {
    std::vector<std::string> vmIds;
    vmIds.reserve(vms_.size());
    
    for (const auto& pair : vms_) {
        vmIds.push_back(pair.first);
    }
    
    return vmIds;
}

std::vector<std::string> VMManager::listVMsByState(VMState state) const {
    std::vector<std::string> vmIds;
    
    for (const auto& pair : vms_) {
        if (pair.second->metadata.currentState == state) {
            vmIds.push_back(pair.first);
        }
    }
    
    return vmIds;
}

// ============================================================================
// Direct VM Access
// ============================================================================

VirtualMachine* VMManager::getVMDirect(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    return instance ? instance->vm.get() : nullptr;
}

const VirtualMachine* VMManager::getVMDirect(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    return instance ? instance->vm.get() : nullptr;
}

// ============================================================================
// Manager Configuration
// ============================================================================

size_t VMManager::stopAllVMs() {
    size_t count = 0;
    
    for (auto& pair : vms_) {
        if (pair.second->metadata.currentState == VMState::RUNNING ||
            pair.second->metadata.currentState == VMState::PAUSED) {
            if (stopVM(pair.first)) {
                count++;
            }
        }
    }
    
    return count;
}

std::string VMManager::getStatistics() const {
    std::ostringstream oss;
    
    oss << "VMManager Statistics:\n";
    oss << "  Total VMs: " << vms_.size() << "\n";
    
    size_t running = 0, stopped = 0, paused = 0, error = 0;
    for (const auto& pair : vms_) {
        switch (pair.second->metadata.currentState) {
            case VMState::RUNNING: running++; break;
            case VMState::STOPPED: stopped++; break;
            case VMState::PAUSED: paused++; break;
            case VMState::ERROR: error++; break;
            default: break;
        }
    }
    
    oss << "  Running: " << running << "\n";
    oss << "  Stopped: " << stopped << "\n";
    oss << "  Paused: " << paused << "\n";
    oss << "  Error: " << error << "\n";
    oss << "  Total Snapshots: " << snapshots_.size() << "\n";
    
    return oss.str();
}

// ============================================================================
// Internal Helpers
// ============================================================================

VMInstance* VMManager::getVMInstance(const std::string& vmId) {
    auto it = vms_.find(vmId);
    return it != vms_.end() ? it->second.get() : nullptr;
}

const VMInstance* VMManager::getVMInstance(const std::string& vmId) const {
    auto it = vms_.find(vmId);
    return it != vms_.end() ? it->second.get() : nullptr;
}

std::string VMManager::generateVMId(const std::string& vmName) {
    std::ostringstream oss;
    oss << vmName << "-" << std::setfill('0') << std::setw(6) << vmCounter_++;
    return oss.str();
}

std::string VMManager::generateSnapshotId(const std::string& snapshotName) {
    std::ostringstream oss;
    oss << snapshotName << "-" << std::setfill('0') << std::setw(6) 
        << snapshotCounter_++;
    return oss.str();
}

bool VMManager::canTransition(VMState from, VMState to) const {
    // Define valid state transitions
    // This can be expanded based on requirements
    
    switch (from) {
        case VMState::CREATED:
        case VMState::STOPPED:
            return to == VMState::STARTING || to == VMState::TERMINATED;
            
        case VMState::RUNNING:
            return to == VMState::PAUSING || to == VMState::STOPPING || 
                   to == VMState::SNAPSHOTTING || to == VMState::ERROR;
            
        case VMState::PAUSED:
            return to == VMState::RESUMING || to == VMState::STOPPING ||
                   to == VMState::SNAPSHOTTING;
            
        default:
            return true;  // Allow transitions from transitional states
    }
}

bool VMManager::createStorageDevices(VMInstance* instance,
                                    const std::vector<StorageConfiguration>& configs) {
    
    if (configs.empty()) {
        LOG_INFO("No storage devices to create");
        return true;
    }
    
    LOG_INFO("Creating " + std::to_string(configs.size()) + " storage device(s)");
    
    for (const auto& config : configs) {
        LOG_INFO("  Device ID: " + config.deviceId);
        LOG_INFO("  Type: " + config.deviceType);
        LOG_INFO("  Path: " + config.imagePath);
        LOG_INFO("  Read-only: " + std::string(config.readOnly ? "yes" : "no"));
        
        // Create appropriate storage device type
        std::unique_ptr<IStorageDevice> device;
        
        if (config.deviceType == "raw") {
            try {
                LOG_INFO("  Creating RawDiskDevice...");
                device = std::make_unique<RawDiskDevice>(
                    config.deviceId,
                    config.imagePath,
                    config.sizeBytes,
                    config.readOnly
                );
                LOG_INFO("  ? RawDiskDevice created");
            } catch (const std::exception& e) {
                LOG_ERROR("  ERROR creating RawDiskDevice: " + std::string(e.what()));
                return false;
            }
        } else {
            LOG_ERROR("Unsupported storage device type: " + config.deviceType);
            return false;
        }
        
        LOG_INFO("  Connecting storage device...");
        if (!device->connect()) {
            LOG_ERROR("  ERROR: Failed to connect storage device: " + config.deviceId);
            LOG_ERROR("  File path: " + config.imagePath);
            LOG_ERROR("  This usually means:");
            LOG_ERROR("    - File doesn't exist");
            LOG_ERROR("    - File is locked by another process");
            LOG_ERROR("    - Insufficient permissions");
            return false;
        }
        
        LOG_INFO("  ? Storage device connected successfully");
        instance->storageDevices.push_back(std::move(device));
    }
    
    LOG_INFO("? All storage devices created successfully");
    return true;
}

void VMManager::updateResourceUsage(VMInstance* instance) {
    if (!instance || !instance->vm) {
        return;
    }
    
    // Update memory usage
    instance->metadata.resourceUsage.memoryUsedBytes = 
        instance->metadata.configuration.memory.memorySize;
    
    // Other resource updates can be added here
}

// ============================================================================
// Console Output Access
// ============================================================================

std::vector<std::string> VMManager::getConsoleOutput(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::vector<std::string>();
    }
    return instance->vm->getConsoleOutput();
}

std::vector<std::string> VMManager::getConsoleOutput(const std::string& vmId, 
                                                     size_t startLine, 
                                                     size_t count) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::vector<std::string>();
    }
    return instance->vm->getConsoleOutput(startLine, count);
}

std::string VMManager::getRecentConsoleOutput(const std::string& vmId, 
                                              size_t maxBytes) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::string();
    }
    return instance->vm->getRecentConsoleOutput(maxBytes);
}

std::vector<std::string> VMManager::getConsoleOutputSince(const std::string& vmId, 
                                                          size_t lineNumber) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::vector<std::string>();
    }
    return instance->vm->getConsoleOutputSince(lineNumber);
}

size_t VMManager::getConsoleLineCount(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return 0;
    }
    return instance->vm->getConsoleLineCount();
}

uint64_t VMManager::getConsoleTotalBytes(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return 0;
    }
    return instance->vm->getConsoleTotalBytes();
}

bool VMManager::clearConsoleOutput(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return false;
    }
    instance->vm->clearConsoleOutput();
    return true;
}

// ============================================================================
// Framebuffer Access
// ============================================================================

bool VMManager::getFramebuffer(const std::string& vmId, 
                               uint8_t* buffer, 
                               size_t bufferSize,
                               size_t* width,
                               size_t* height) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return false;
    }
    
    const FramebufferDevice* fb = instance->vm->getFramebufferDevice();
    if (!fb) {
        return false;
    }
    
    const size_t fbWidth = fb->GetWidth();
    const size_t fbHeight = fb->GetHeight();
    const size_t fbSize = fbWidth * fbHeight * fb->GetBytesPerPixel();
    
    if (bufferSize < fbSize) {
        return false;
    }
    
    if (width) *width = fbWidth;
    if (height) *height = fbHeight;
    
    fb->CopyFramebuffer(buffer, bufferSize);
    return true;
}

bool VMManager::getFramebufferDimensions(const std::string& vmId,
                                        size_t* width,
                                        size_t* height) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return false;
    }
    
    const FramebufferDevice* fb = instance->vm->getFramebufferDevice();
    if (!fb) {
        return false;
    }
    
    if (width) *width = fb->GetWidth();
    if (height) *height = fb->GetHeight();
    
    return true;
}

} // namespace ia64

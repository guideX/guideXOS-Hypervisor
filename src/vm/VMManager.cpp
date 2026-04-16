#include "VMManager.h"
#include "VMManager.h"
#include "RawDiskDevice.h"
#include "IStorageDevice.h"
#include "logger.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace ia64 {

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
        if (!bootConfig.bootDevice.empty() && !bootConfig.directBoot) {
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
                
                // Read boot sector (first 512 bytes for BIOS, or more for EFI)
                std::vector<uint8_t> bootSector(4096); // 4KB to handle EFI boot
                if (bootDevice->read(0, bootSector.data(), bootSector.size())) {
                    // For IA-64, load to standard entry point (EFI entry is typically at 1MB)
                    uint64_t bootAddress = bootConfig.entryPoint != 0 ? bootConfig.entryPoint : 0x100000;
                    
                    LOG_INFO("Loading boot sector to address: 0x" + 
                            std::to_string(bootAddress));
                    
                    // Load boot sector into VM memory
                    if (instance->vm->loadProgram(bootSector.data(), bootSector.size(), bootAddress)) {
                        // Set entry point to bootloader
                        instance->vm->setEntryPoint(bootAddress);
                        LOG_INFO("Bootloader loaded successfully, entry point: 0x" + 
                                std::to_string(bootAddress));
                    } else {
                        LOG_ERROR("Failed to load bootloader into memory");
                    }
                } else {
                    LOG_ERROR("Failed to read boot sector from device");
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
    for (const auto& config : configs) {
        // Create appropriate storage device type
        std::unique_ptr<IStorageDevice> device;
        
        if (config.deviceType == "raw") {
            device = std::make_unique<RawDiskDevice>(
                config.deviceId,
                config.imagePath,
                config.sizeBytes,
                config.readOnly
            );
        } else {
            LOG_ERROR("Unsupported storage device type: " + config.deviceType);
            return false;
        }
        
        if (!device->connect()) {
            LOG_ERROR("Failed to connect storage device: " + config.deviceId);
            return false;
        }
        
        instance->storageDevices.push_back(std::move(device));
    }
    
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

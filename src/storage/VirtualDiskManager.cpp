#include "VirtualDiskManager.h"
#include <sstream>
#include <algorithm>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VirtualDiskManager::VirtualDiskManager()
    : devices_()
    , mounts_()
    , pathResolver_()
{
}

VirtualDiskManager::~VirtualDiskManager() {
    // Unmount all filesystems
    while (!mounts_.empty()) {
        mounts_.back()->unmount();
        mounts_.pop_back();
    }
    
    // Disconnect all devices
    devices_.clear();
}

// ============================================================================
// Device Management
// ============================================================================

std::shared_ptr<VirtualBlockDevice> VirtualDiskManager::createMemoryDisk(
    const std::string& deviceId,
    uint64_t sizeBytes,
    uint32_t sectorSize)
{
    // Check if device ID already exists
    if (devices_.find(deviceId) != devices_.end()) {
        return nullptr;
    }
    
    // Create memory-backed device
    auto device = std::make_shared<VirtualBlockDevice>(deviceId, sizeBytes, sectorSize);
    
    // Connect the device
    if (!device->connect()) {
        return nullptr;
    }
    
    // Register the device
    devices_[deviceId] = device;
    
    return device;
}

std::shared_ptr<VirtualBlockDevice> VirtualDiskManager::createFileDisk(
    const std::string& deviceId,
    const std::string& imagePath,
    uint64_t sizeBytes,
    bool readOnly,
    uint32_t sectorSize)
{
    // Check if device ID already exists
    if (devices_.find(deviceId) != devices_.end()) {
        return nullptr;
    }
    
    // Create file-backed device
    auto device = std::make_shared<VirtualBlockDevice>(
        deviceId,
        imagePath,
        VirtualBlockDevice::BackingType::DISK_IMAGE,
        sizeBytes,
        readOnly,
        sectorSize
    );
    
    // Connect the device
    if (!device->connect()) {
        return nullptr;
    }
    
    // Register the device
    devices_[deviceId] = device;
    
    return device;
}

bool VirtualDiskManager::registerDevice(std::shared_ptr<VirtualBlockDevice> device) {
    if (!device) {
        return false;
    }
    
    std::string deviceId = device->getDeviceId();
    
    // Check if already registered
    if (devices_.find(deviceId) != devices_.end()) {
        return false;
    }
    
    devices_[deviceId] = device;
    return true;
}

bool VirtualDiskManager::unregisterDevice(const std::string& deviceId) {
    auto it = devices_.find(deviceId);
    if (it == devices_.end()) {
        return false;
    }
    
    // Check if device is mounted
    for (const auto& mount : mounts_) {
        if (mount->getDevice() == it->second) {
            return false;  // Can't unregister mounted device
        }
    }
    
    devices_.erase(it);
    return true;
}

std::shared_ptr<VirtualBlockDevice> VirtualDiskManager::getDevice(const std::string& deviceId) const {
    auto it = devices_.find(deviceId);
    if (it == devices_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::shared_ptr<VirtualBlockDevice>> VirtualDiskManager::getDevices() const {
    std::vector<std::shared_ptr<VirtualBlockDevice>> result;
    result.reserve(devices_.size());
    
    for (const auto& pair : devices_) {
        result.push_back(pair.second);
    }
    
    return result;
}

// ============================================================================
// Filesystem Management
// ============================================================================

bool VirtualDiskManager::mount(
    const std::string& mountPoint,
    const std::string& deviceId,
    std::shared_ptr<IFilesystem> filesystem,
    const FilesystemMount::MountOptions& options)
{
    // Get device
    auto device = getDevice(deviceId);
    if (!device) {
        return false;
    }
    
    return mount(mountPoint, device, filesystem, options);
}

bool VirtualDiskManager::mount(
    const std::string& mountPoint,
    std::shared_ptr<IStorageDevice> device,
    std::shared_ptr<IFilesystem> filesystem,
    const FilesystemMount::MountOptions& options)
{
    if (!device || !filesystem) {
        return false;
    }
    
    // Check if mount point already exists
    if (findMount(mountPoint)) {
        return false;
    }
    
    // Create mount
    auto mount = std::make_shared<FilesystemMount>(
        mountPoint,
        filesystem,
        device,
        options
    );
    
    // Mount the filesystem
    if (!mount->mount()) {
        return false;
    }
    
    // Add to mounts list
    mounts_.push_back(mount);
    
    // Sort mounts by mount point length (longest first) for proper resolution
    std::sort(mounts_.begin(), mounts_.end(),
        [](const std::shared_ptr<FilesystemMount>& a,
           const std::shared_ptr<FilesystemMount>& b) {
            return a->getMountPoint().size() > b->getMountPoint().size();
        });
    
    return true;
}

bool VirtualDiskManager::unmount(const std::string& mountPoint) {
    auto mount = findMount(mountPoint);
    if (!mount) {
        return false;
    }
    
    // Unmount
    mount->unmount();
    
    // Remove from list
    mounts_.erase(
        std::remove(mounts_.begin(), mounts_.end(), mount),
        mounts_.end()
    );
    
    return true;
}

std::shared_ptr<FilesystemMount> VirtualDiskManager::getMount(const std::string& mountPoint) const {
    return findMount(mountPoint);
}

std::vector<std::shared_ptr<FilesystemMount>> VirtualDiskManager::getMounts() const {
    return mounts_;
}

// ============================================================================
// File Operations (High-Level API)
// ============================================================================

bool VirtualDiskManager::exists(const std::string& guestPath) {
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return false;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->exists(resolved.filesystemPath);
}

bool VirtualDiskManager::isDirectory(const std::string& guestPath) {
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return false;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->isDirectory(resolved.filesystemPath);
}

bool VirtualDiskManager::isFile(const std::string& guestPath) {
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return false;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->isFile(resolved.filesystemPath);
}

int64_t VirtualDiskManager::getFileSize(const std::string& guestPath) {
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return -1;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->getFileSize(resolved.filesystemPath);
}

bool VirtualDiskManager::getAttributes(const std::string& guestPath, FileAttributes& attrs) {
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return false;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->getAttributes(resolved.filesystemPath, attrs);
}

int64_t VirtualDiskManager::readFile(
    const std::string& guestPath,
    uint8_t* buffer,
    uint64_t size,
    uint64_t offset)
{
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return -1;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->readFile(
        resolved.filesystemPath,
        buffer,
        size,
        offset
    );
}

bool VirtualDiskManager::readFileAll(const std::string& guestPath, std::vector<uint8_t>& data) {
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return false;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->readFileAll(resolved.filesystemPath, data);
}

bool VirtualDiskManager::listDirectory(
    const std::string& guestPath,
    std::vector<DirectoryEntry>& entries)
{
    auto resolved = resolvePath(guestPath);
    
    if (!resolved.valid || !resolved.mount) {
        return false;
    }
    
    resolved.mount->incrementAccessCount();
    
    return resolved.mount->getFilesystem()->listDirectory(resolved.filesystemPath, entries);
}

// ============================================================================
// Path Resolution
// ============================================================================

PathResolver::ResolvedPath VirtualDiskManager::resolvePath(
    const std::string& guestPath,
    const std::string& currentDir)
{
    return pathResolver_.resolve(guestPath, mounts_, currentDir);
}

// ============================================================================
// Information and Statistics
// ============================================================================

std::string VirtualDiskManager::getStatistics() const {
    std::ostringstream oss;
    
    oss << "Virtual Disk Manager Statistics:\n"
        << "  Registered Devices: " << devices_.size() << "\n"
        << "  Active Mounts: " << mounts_.size() << "\n\n";
    
    oss << "Devices:\n";
    for (const auto& pair : devices_) {
        const auto& device = pair.second;
        oss << "  - " << device->getDeviceId() << ": "
            << device->getSize() << " bytes, "
            << (device->isConnected() ? "connected" : "disconnected")
            << "\n";
    }
    
    oss << "\nMounts:\n";
    for (const auto& mount : mounts_) {
        oss << "  - " << mount->getMountPoint()
            << " -> " << mount->getDevice()->getDeviceId()
            << " (" << mount->getFilesystem()->getType() << ")"
            << ", accesses: " << mount->getAccessCount()
            << "\n";
    }
    
    return oss.str();
}

std::string VirtualDiskManager::getMountInfo() const {
    std::ostringstream oss;
    
    for (const auto& mount : mounts_) {
        oss << mount->getInfo() << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// Helper Methods
// ============================================================================

std::shared_ptr<FilesystemMount> VirtualDiskManager::findMount(const std::string& mountPoint) const {
    for (const auto& mount : mounts_) {
        if (mount->getMountPoint() == mountPoint) {
            return mount;
        }
    }
    return nullptr;
}

} // namespace ia64

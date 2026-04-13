#include "FilesystemMount.h"
#include <sstream>
#include <algorithm>

namespace ia64 {

// ============================================================================
// Constructor
// ============================================================================

FilesystemMount::FilesystemMount(const std::string& mountPoint,
                                std::shared_ptr<IFilesystem> filesystem,
                                std::shared_ptr<IStorageDevice> device,
                                const MountOptions& options)
    : mountPoint_(normalizePath(mountPoint))
    , filesystem_(filesystem)
    , device_(device)
    , options_(options)
    , mounted_(false)
    , accessCount_(0)
{
}

// ============================================================================
// Destructor
// ============================================================================

FilesystemMount::~FilesystemMount() {
    if (mounted_) {
        unmount();
    }
}

// ============================================================================
// Mount Operations
// ============================================================================

bool FilesystemMount::mount() {
    if (mounted_) {
        return true;
    }
    
    if (!filesystem_ || !device_) {
        return false;
    }
    
    // Ensure device is connected
    if (!device_->isConnected()) {
        if (!device_->connect()) {
            return false;
        }
    }
    
    // Mount the filesystem on the device
    if (!filesystem_->mount(device_)) {
        return false;
    }
    
    mounted_ = true;
    return true;
}

void FilesystemMount::unmount() {
    if (!mounted_) {
        return;
    }
    
    if (filesystem_) {
        filesystem_->unmount();
    }
    
    mounted_ = false;
}

// ============================================================================
// Path Translation
// ============================================================================

bool FilesystemMount::containsPath(const std::string& guestPath) const {
    std::string normalizedGuest = normalizePath(guestPath);
    
    // Check if guest path starts with mount point
    if (normalizedGuest.size() < mountPoint_.size()) {
        return false;
    }
    
    // Exact match or prefix match with separator
    if (normalizedGuest == mountPoint_) {
        return true;
    }
    
    if (normalizedGuest.compare(0, mountPoint_.size(), mountPoint_) == 0) {
        // Check that next character is a separator
        if (normalizedGuest.size() == mountPoint_.size()) {
            return true;
        }
        if (normalizedGuest[mountPoint_.size()] == '/') {
            return true;
        }
    }
    
    return false;
}

std::string FilesystemMount::translatePath(const std::string& guestPath) const {
    if (!containsPath(guestPath)) {
        return "";
    }
    
    std::string normalizedGuest = normalizePath(guestPath);
    
    // If exact match with mount point, return root
    if (normalizedGuest == mountPoint_) {
        return "/";
    }
    
    // Remove mount point prefix
    std::string relative = normalizedGuest.substr(mountPoint_.size());
    
    // Ensure it starts with /
    if (relative.empty() || relative[0] != '/') {
        relative = "/" + relative;
    }
    
    return relative;
}

// ============================================================================
// Statistics
// ============================================================================

std::string FilesystemMount::getInfo() const {
    std::ostringstream oss;
    oss << "Filesystem Mount:\n"
        << "  Mount Point: " << mountPoint_ << "\n"
        << "  Filesystem Type: " << (filesystem_ ? filesystem_->getType() : "None") << "\n"
        << "  Device ID: " << (device_ ? device_->getDeviceId() : "None") << "\n"
        << "  Mounted: " << (mounted_ ? "Yes" : "No") << "\n"
        << "  Read-Only: " << (isReadOnly() ? "Yes" : "No") << "\n"
        << "  Access Count: " << accessCount_ << "\n";
    
    if (filesystem_ && mounted_) {
        oss << "  Total Size: " << filesystem_->getTotalSize() << " bytes\n"
            << "  Free Size: " << filesystem_->getFreeSize() << " bytes\n";
    }
    
    return oss.str();
}

// ============================================================================
// Helper Methods
// ============================================================================

std::string FilesystemMount::normalizePath(const std::string& path) {
    if (path.empty()) {
        return "/";
    }
    
    std::string result = path;
    
    // Ensure starts with /
    if (result[0] != '/') {
        result = "/" + result;
    }
    
    // Remove trailing slashes (except for root "/")
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    
    // TODO: Could add more normalization like resolving ".." and "."
    
    return result;
}

} // namespace ia64

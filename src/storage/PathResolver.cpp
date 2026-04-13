#include "PathResolver.h"
#include "FilesystemMount.h"
#include <algorithm>
#include <sstream>

namespace ia64 {

// ============================================================================
// Constructor
// ============================================================================

PathResolver::PathResolver(const ValidationOptions& options)
    : options_(options)
{
}

// ============================================================================
// Path Resolution
// ============================================================================

PathResolver::ResolvedPath PathResolver::resolve(
    const std::string& guestPath,
    const std::vector<std::shared_ptr<FilesystemMount>>& mounts,
    const std::string& currentDir) const
{
    ResolvedPath result;
    
    // Normalize the path
    result.normalizedPath = normalize(guestPath, currentDir);
    
    if (result.normalizedPath.empty()) {
        result.valid = false;
        result.errorMessage = "Invalid path: normalization failed";
        return result;
    }
    
    // Validate the normalized path
    if (!validate(result.normalizedPath, result.errorMessage)) {
        result.valid = false;
        return result;
    }
    
    // Find matching mount point
    result.mount = findMount(result.normalizedPath, mounts);
    
    if (!result.mount) {
        result.valid = false;
        result.errorMessage = "No filesystem mounted at path";
        return result;
    }
    
    // Translate to filesystem-relative path
    result.filesystemPath = result.mount->translatePath(result.normalizedPath);
    
    if (result.filesystemPath.empty()) {
        result.valid = false;
        result.errorMessage = "Path translation failed";
        return result;
    }
    
    result.valid = true;
    return result;
}

std::string PathResolver::normalize(const std::string& path,
                                   const std::string& currentDir) const
{
    if (path.empty()) {
        return "";
    }
    
    // Check for malicious characters
    if (containsMaliciousChars(path)) {
        return "";
    }
    
    // Check path length
    if (path.size() > options_.maxPathLength) {
        return "";
    }
    
    // Start with absolute path
    std::string workingPath;
    if (isAbsolute(path)) {
        workingPath = path;
    } else {
        // Make relative path absolute using current directory
        workingPath = join(currentDir, path);
    }
    
    // Split into components
    std::vector<std::string> components = split(workingPath);
    std::vector<std::string> normalized;
    
    // Process components (handle "." and "..")
    for (const auto& component : components) {
        if (component.empty() || component == ".") {
            // Skip empty and current directory references
            continue;
        } else if (component == "..") {
            // Parent directory
            if (options_.allowParentRefs) {
                if (!normalized.empty()) {
                    normalized.pop_back();
                }
                // Note: if normalized is empty, we're at root, can't go up
            } else {
                return "";  // Parent refs not allowed
            }
        } else {
            // Regular component
            if (!isValidComponent(component)) {
                return "";
            }
            normalized.push_back(component);
        }
    }
    
    // Check depth
    if (normalized.size() > options_.maxDepth) {
        return "";
    }
    
    // Build normalized path
    if (normalized.empty()) {
        return "/";
    }
    
    std::string result = "/";
    for (size_t i = 0; i < normalized.size(); ++i) {
        if (i > 0) {
            result += "/";
        }
        result += normalized[i];
    }
    
    return result;
}

bool PathResolver::validate(const std::string& path, std::string& errorMessage) const
{
    if (path.empty()) {
        errorMessage = "Empty path";
        return false;
    }
    
    if (!isAbsolute(path)) {
        errorMessage = "Path must be absolute";
        return false;
    }
    
    if (path.size() > options_.maxPathLength) {
        errorMessage = "Path too long";
        return false;
    }
    
    if (containsMaliciousChars(path)) {
        errorMessage = "Path contains invalid characters";
        return false;
    }
    
    // Check depth
    std::vector<std::string> components = split(path);
    if (components.size() > options_.maxDepth) {
        errorMessage = "Path depth exceeds maximum";
        return false;
    }
    
    // Validate each component
    for (const auto& component : components) {
        if (!isValidComponent(component)) {
            errorMessage = "Invalid path component: " + component;
            return false;
        }
    }
    
    return true;
}

bool PathResolver::escapesRoot(const std::string& path, const std::string& root) const
{
    // Normalize both paths
    std::string normalizedPath = normalize(path, "/");
    std::string normalizedRoot = normalize(root, "/");
    
    if (normalizedPath.empty() || normalizedRoot.empty()) {
        return true;  // Invalid paths are considered escaping
    }
    
    // Path should start with root
    if (normalizedPath.size() < normalizedRoot.size()) {
        return true;
    }
    
    // Check if path starts with root
    if (normalizedPath.compare(0, normalizedRoot.size(), normalizedRoot) != 0) {
        return true;
    }
    
    // If path is longer, next character should be '/'
    if (normalizedPath.size() > normalizedRoot.size() &&
        normalizedRoot != "/" &&
        normalizedPath[normalizedRoot.size()] != '/') {
        return true;
    }
    
    return false;
}

// ============================================================================
// Path Manipulation
// ============================================================================

std::string PathResolver::join(const std::string& base, const std::string& component)
{
    if (base.empty()) {
        return component;
    }
    
    if (component.empty()) {
        return base;
    }
    
    // If component is absolute, just return it
    if (isAbsolute(component)) {
        return component;
    }
    
    // Join with separator if needed
    std::string result = base;
    if (result.back() != '/') {
        result += '/';
    }
    result += component;
    
    return result;
}

std::string PathResolver::dirname(const std::string& path)
{
    if (path.empty() || path == "/") {
        return "/";
    }
    
    // Remove trailing slashes
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    
    // Find last slash
    size_t lastSlash = trimmed.find_last_of('/');
    
    if (lastSlash == std::string::npos) {
        return ".";
    }
    
    if (lastSlash == 0) {
        return "/";
    }
    
    return trimmed.substr(0, lastSlash);
}

std::string PathResolver::basename(const std::string& path)
{
    if (path.empty() || path == "/") {
        return "/";
    }
    
    // Remove trailing slashes
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') {
        trimmed.pop_back();
    }
    
    // Find last slash
    size_t lastSlash = trimmed.find_last_of('/');
    
    if (lastSlash == std::string::npos) {
        return trimmed;
    }
    
    return trimmed.substr(lastSlash + 1);
}

std::vector<std::string> PathResolver::split(const std::string& path)
{
    std::vector<std::string> components;
    
    if (path.empty()) {
        return components;
    }
    
    std::string current;
    
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                components.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    
    if (!current.empty()) {
        components.push_back(current);
    }
    
    return components;
}

bool PathResolver::isAbsolute(const std::string& path)
{
    return !path.empty() && path[0] == '/';
}

// ============================================================================
// Helper Methods
// ============================================================================

bool PathResolver::isValidComponent(const std::string& component) const
{
    if (component.empty()) {
        return false;
    }
    
    // Check for null bytes
    if (component.find('\0') != std::string::npos) {
        return false;
    }
    
    // Check for invalid characters (null, path separators in component)
    for (char c : component) {
        if (c == '\0') {
            return false;
        }
        // Note: '/' should not appear in a component (handled by split)
    }
    
    return true;
}

bool PathResolver::containsMaliciousChars(const std::string& path) const
{
    // Check for null bytes
    if (path.find('\0') != std::string::npos) {
        return true;
    }
    
    // Check for other potentially problematic characters
    // (can be extended based on security requirements)
    
    return false;
}

std::shared_ptr<FilesystemMount> PathResolver::findMount(
    const std::string& path,
    const std::vector<std::shared_ptr<FilesystemMount>>& mounts) const
{
    std::shared_ptr<FilesystemMount> bestMatch;
    size_t bestMatchLength = 0;
    
    // Find the mount with the longest matching prefix
    for (const auto& mount : mounts) {
        if (!mount || !mount->isMounted()) {
            continue;
        }
        
        if (mount->containsPath(path)) {
            size_t mountLength = mount->getMountPoint().size();
            if (mountLength > bestMatchLength) {
                bestMatch = mount;
                bestMatchLength = mountLength;
            }
        }
    }
    
    return bestMatch;
}

} // namespace ia64

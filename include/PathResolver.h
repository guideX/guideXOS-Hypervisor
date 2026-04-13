#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace ia64 {

// Forward declarations
class FilesystemMount;
class IFilesystem;

/**
 * PathResolver - Path resolution with sandboxing for virtual filesystems
 * 
 * This class provides secure path resolution that:
 * - Normalizes paths (removes "..", ".", multiple slashes)
 * - Prevents directory traversal attacks
 * - Ensures all paths stay within sandbox boundaries
 * - Translates guest paths to filesystem-relative paths
 * - Validates path components
 * 
 * Security Features:
 * - Rejects paths with ".." that escape the root
 * - Rejects absolute symlinks that point outside sandbox
 * - Rejects null bytes and other malicious characters
 * - Limits path length to prevent buffer overflows
 * - Validates each path component
 * 
 * Thread Safety:
 * - Thread-safe for read operations
 * - Stateless design (no mutable state)
 */
class PathResolver {
public:
    /**
     * Path resolution result
     */
    struct ResolvedPath {
        bool valid;                             // True if path is valid and safe
        std::string normalizedPath;             // Normalized absolute path
        std::string filesystemPath;             // Path relative to filesystem root
        std::shared_ptr<FilesystemMount> mount; // Mount point (if found)
        std::string errorMessage;               // Error message if invalid
        
        ResolvedPath()
            : valid(false)
            , normalizedPath()
            , filesystemPath()
            , mount(nullptr)
            , errorMessage()
        {}
    };
    
    /**
     * Path validation options
     */
    struct ValidationOptions {
        bool allowParentRefs;       // Allow ".." in paths (default: true, but checked)
        bool allowSymlinks;         // Allow symbolic links (default: false for safety)
        uint32_t maxPathLength;     // Maximum path length (default: 4096)
        uint32_t maxDepth;          // Maximum directory depth (default: 128)
        bool caseSensitive;         // Case-sensitive path matching (default: true)
        
        ValidationOptions()
            : allowParentRefs(true)
            , allowSymlinks(false)
            , maxPathLength(4096)
            , maxDepth(128)
            , caseSensitive(true)
        {}
    };
    
    /**
     * Constructor
     * 
     * @param options Validation options
     */
    explicit PathResolver(const ValidationOptions& options = ValidationOptions());
    
    ~PathResolver() = default;
    
    // ========================================================================
    // Path Resolution
    // ========================================================================
    
    /**
     * Resolve a guest path against a list of mounts
     * 
     * @param guestPath Path from guest (absolute or relative)
     * @param mounts List of filesystem mounts
     * @param currentDir Current directory for relative paths (default "/")
     * @return Resolution result
     */
    ResolvedPath resolve(const std::string& guestPath,
                        const std::vector<std::shared_ptr<FilesystemMount>>& mounts,
                        const std::string& currentDir = "/") const;
    
    /**
     * Normalize a path (resolve ".", "..", remove multiple slashes)
     * 
     * @param path Input path
     * @param currentDir Current directory for relative paths
     * @return Normalized absolute path, or empty string if invalid
     */
    std::string normalize(const std::string& path,
                         const std::string& currentDir = "/") const;
    
    /**
     * Validate a path for security issues
     * 
     * @param path Path to validate
     * @param errorMessage Output error message if invalid
     * @return True if path is safe
     */
    bool validate(const std::string& path, std::string& errorMessage) const;
    
    /**
     * Check if a path escapes a root directory
     * 
     * @param path Absolute normalized path
     * @param root Root directory boundary
     * @return True if path escapes root
     */
    bool escapesRoot(const std::string& path, const std::string& root) const;
    
    // ========================================================================
    // Path Manipulation
    // ========================================================================
    
    /**
     * Join path components
     * 
     * @param base Base path
     * @param component Component to append
     * @return Joined path
     */
    static std::string join(const std::string& base, const std::string& component);
    
    /**
     * Get directory name from path
     * 
     * @param path Input path
     * @return Directory portion (e.g., "/foo/bar" -> "/foo")
     */
    static std::string dirname(const std::string& path);
    
    /**
     * Get base name from path
     * 
     * @param path Input path
     * @return Base name (e.g., "/foo/bar" -> "bar")
     */
    static std::string basename(const std::string& path);
    
    /**
     * Split path into components
     * 
     * @param path Input path
     * @return Vector of path components
     */
    static std::vector<std::string> split(const std::string& path);
    
    /**
     * Check if path is absolute
     * 
     * @param path Input path
     * @return True if absolute (starts with /)
     */
    static bool isAbsolute(const std::string& path);
    
    /**
     * Get validation options
     * @return Current validation options
     */
    const ValidationOptions& getOptions() const { return options_; }
    
private:
    ValidationOptions options_;
    
    // Helper methods
    bool isValidComponent(const std::string& component) const;
    bool containsMaliciousChars(const std::string& path) const;
    std::shared_ptr<FilesystemMount> findMount(
        const std::string& path,
        const std::vector<std::shared_ptr<FilesystemMount>>& mounts) const;
};

} // namespace ia64

#pragma once

#include "IISA.h"
#include <memory>
#include <string>
#include <map>
#include <functional>

namespace ia64 {

// Forward declarations
class IDecoder;
class SyscallDispatcher;
class Profiler;

/**
 * ISAPluginRegistry - Registry for managing ISA plugins
 * 
 * This registry provides a central location for registering and creating ISA plugins.
 * It enables:
 * - Dynamic ISA selection at VM creation time
 * - Plugin discovery and enumeration
 * - Factory pattern for ISA instantiation
 * - Decoupling of VM core from specific ISA implementations
 * 
 * Design Philosophy:
 * - Singleton pattern for global access
 * - Factory functions for creating ISA instances
 * - Name-based ISA lookup
 * - Support for ISA-specific parameters
 * 
 * Usage Example:
 * ```
 * // Register an ISA plugin
 * ISAPluginRegistry::instance().registerISA("IA-64", createIA64ISA);
 * 
 * // Create an ISA instance
 * auto isa = ISAPluginRegistry::instance().createISA("IA-64", decoder, ...);
 * ```
 */
class ISAPluginRegistry {
public:
    /**
     * Get singleton instance
     */
    static ISAPluginRegistry& instance();
    
    /**
     * ISA factory function type with optional components
     * 
     * Factory functions take ISA-specific parameters and return a new ISA instance.
     * Different ISAs may require different parameters.
     */
    using ISAFactoryFunc = std::function<std::unique_ptr<IISA>(
        void* isaSpecificData
    )>;
    
    /**
     * Register an ISA plugin
     * 
     * @param name ISA name (e.g., "IA-64", "x86-64", "ARM64")
     * @param factory Factory function to create ISA instances
     * @return True if registration succeeded, false if name already exists
     */
    bool registerISA(const std::string& name, ISAFactoryFunc factory);
    
    /**
     * Unregister an ISA plugin
     * 
     * @param name ISA name to unregister
     * @return True if unregistration succeeded, false if name not found
     */
    bool unregisterISA(const std::string& name);
    
    /**
     * Check if an ISA is registered
     * 
     * @param name ISA name to check
     * @return True if ISA is registered
     */
    bool isRegistered(const std::string& name) const;
    
    /**
     * Get list of registered ISA names
     * 
     * @return Vector of registered ISA names
     */
    std::vector<std::string> getRegisteredISAs() const;
    
    /**
     * Create an ISA instance
     * 
     * @param name ISA name
     * @param isaSpecificData ISA-specific construction data
     * @return Unique pointer to ISA instance, or nullptr if name not found
     */
    std::unique_ptr<IISA> createISA(const std::string& name, void* isaSpecificData = nullptr);
    
    /**
     * Clear all registered ISAs (for testing)
     */
    void clear();
    
private:
    // Singleton - private constructor
    ISAPluginRegistry() = default;
    ~ISAPluginRegistry() = default;
    
    // Prevent copying and moving
    ISAPluginRegistry(const ISAPluginRegistry&) = delete;
    ISAPluginRegistry& operator=(const ISAPluginRegistry&) = delete;
    ISAPluginRegistry(ISAPluginRegistry&&) = delete;
    ISAPluginRegistry& operator=(ISAPluginRegistry&&) = delete;
    
    // Registry storage
    std::map<std::string, ISAFactoryFunc> registry_;
};

/**
 * ISA factory data structure for IA-64
 * 
 * This structure is passed to the IA-64 factory function to provide
 * all necessary components for creating an IA-64 ISA instance.
 */
struct IA64FactoryData {
    IDecoder* decoder;
    SyscallDispatcher* syscallDispatcher;
    Profiler* profiler;
    
    IA64FactoryData()
        : decoder(nullptr)
        , syscallDispatcher(nullptr)
        , profiler(nullptr) {}
        
    IA64FactoryData(IDecoder* dec, SyscallDispatcher* sysc = nullptr, Profiler* prof = nullptr)
        : decoder(dec)
        , syscallDispatcher(sysc)
        , profiler(prof) {}
};

/**
 * Auto-registration helper class
 * 
 * Usage:
 * ```
 * static ISAPluginRegistrar<MyISA> myISARegistrar("MyISA", myISAFactory);
 * ```
 */
template<typename ISA>
class ISAPluginRegistrar {
public:
    ISAPluginRegistrar(const std::string& name, ISAPluginRegistry::ISAFactoryFunc factory) {
        ISAPluginRegistry::instance().registerISA(name, factory);
    }
};

} // namespace ia64

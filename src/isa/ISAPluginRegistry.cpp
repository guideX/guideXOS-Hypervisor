#include "ISAPluginRegistry.h"
#include "IA64ISAPlugin.h"
#include <iostream>

namespace ia64 {

// ============================================================================
// ISAPluginRegistry Implementation
// ============================================================================

ISAPluginRegistry& ISAPluginRegistry::instance() {
    static ISAPluginRegistry instance;
    return instance;
}

bool ISAPluginRegistry::registerISA(const std::string& name, ISAFactoryFunc factory) {
    if (registry_.find(name) != registry_.end()) {
        std::cerr << "Warning: ISA '" << name << "' is already registered\n";
        return false;
    }
    
    registry_[name] = factory;
    std::cout << "Registered ISA plugin: " << name << "\n";
    return true;
}

bool ISAPluginRegistry::unregisterISA(const std::string& name) {
    auto it = registry_.find(name);
    if (it == registry_.end()) {
        return false;
    }
    
    registry_.erase(it);
    std::cout << "Unregistered ISA plugin: " << name << "\n";
    return true;
}

bool ISAPluginRegistry::isRegistered(const std::string& name) const {
    return registry_.find(name) != registry_.end();
}

std::vector<std::string> ISAPluginRegistry::getRegisteredISAs() const {
    std::vector<std::string> names;
    names.reserve(registry_.size());
    
    for (const auto& pair : registry_) {
        names.push_back(pair.first);
    }
    
    return names;
}

std::unique_ptr<IISA> ISAPluginRegistry::createISA(const std::string& name, void* isaSpecificData) {
    auto it = registry_.find(name);
    if (it == registry_.end()) {
        std::cerr << "Error: ISA '" << name << "' is not registered\n";
        return nullptr;
    }
    
    try {
        return it->second(isaSpecificData);
    } catch (const std::exception& e) {
        std::cerr << "Error creating ISA '" << name << "': " << e.what() << "\n";
        return nullptr;
    }
}

void ISAPluginRegistry::clear() {
    registry_.clear();
}

// ============================================================================
// IA-64 Plugin Auto-Registration
// ============================================================================

namespace {

// Factory function for creating IA-64 ISA instances
std::unique_ptr<IISA> ia64Factory(void* data) {
    if (!data) {
        throw std::invalid_argument("IA-64 factory requires IA64FactoryData");
    }
    
    IA64FactoryData* factoryData = static_cast<IA64FactoryData*>(data);
    
    if (!factoryData->decoder) {
        throw std::invalid_argument("IA-64 factory requires a decoder");
    }
    
    return createIA64ISA(*factoryData->decoder, 
                         factoryData->syscallDispatcher,
                         factoryData->profiler);
}

// Auto-register IA-64 plugin on startup
struct IA64AutoRegistrar {
    IA64AutoRegistrar() {
        ISAPluginRegistry::instance().registerISA("IA-64", ia64Factory);
    }
} ia64AutoRegistrar;

} // anonymous namespace

} // namespace ia64

#include "VMManager_DLL.h"
// CRITICAL: Define this BEFORE any includes to ensure DLL exports work correctly
#ifndef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_HYPERVISOR_EXPORTS
#endif

// Verify the macro is defined (compile-time check)
#ifndef GUIDEXOS_HYPERVISOR_EXPORTS
#error "GUIDEXOS_HYPERVISOR_EXPORTS must be defined for DLL compilation"
#endif

#include "VMManager_DLL.h"
#include "VMManager.h"
#include "VMConfiguration.h"
#include <string>
#include <cstring>
#include <iostream>

using namespace ia64;

// ============================================================================
// Helper Functions
// ============================================================================

static char* AllocateString(const std::string& str) {
    char* result = new char[str.length() + 1];
#ifdef _MSC_VER
    strcpy_s(result, str.length() + 1, str.c_str());
#else
    std::strcpy(result, str.c_str());
#endif
    return result;
}

// ============================================================================
// VMManager Lifecycle
// ============================================================================

GUIDEXOS_API VMManagerHandle VMManager_Create() {
    try {
        VMManager* manager = new VMManager();
        return reinterpret_cast<VMManagerHandle>(manager);
    } catch (...) {
        return nullptr;
    }
}

GUIDEXOS_API void VMManager_Destroy(VMManagerHandle manager) {
    if (manager) {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        delete mgr;
    }
}

// ============================================================================
// VM Lifecycle
// ============================================================================

GUIDEXOS_API const char* VMManager_CreateVM(
    VMManagerHandle manager,
    const char* configJson) {
    
    if (!manager) {
        std::cerr << "ERROR: VMManager_CreateVM - manager is null" << std::endl;
        return nullptr;
    }
    
    if (!configJson) {
        std::cerr << "ERROR: VMManager_CreateVM - configJson is null" << std::endl;
        return nullptr;
    }
    
    std::cout << "VMManager_CreateVM called" << std::endl;
    std::cout << "Config JSON: " << configJson << std::endl;
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        
        std::cout << "Parsing JSON configuration..." << std::endl;
        
        // Parse JSON configuration
        VMConfiguration config;
        try {
            config = VMConfiguration::fromJson(std::string(configJson));
            std::cout << "? JSON parsed successfully" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "ERROR: JSON parsing failed: " << e.what() << std::endl;
            return nullptr;
        }
        
        std::cout << "Validating configuration..." << std::endl;
        
        // Validate configuration
        std::string validationError;
        if (!config.validate(&validationError)) {
            std::cerr << "ERROR: Configuration validation failed: " << validationError << std::endl;
            return nullptr;
        }
        
        std::cout << "? Configuration valid" << std::endl;
        std::cout << "Creating VM with name: " << config.name << std::endl;
        
        // Create VM with the parsed configuration
        std::string vmId = mgr->createVM(config);
        
        if (vmId.empty()) {
            std::cerr << "ERROR: VMManager::createVM returned empty string" << std::endl;
            return nullptr;
        }
        
        std::cout << "? VM created successfully with ID: " << vmId << std::endl;
        return AllocateString(vmId);
        
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception in VMManager_CreateVM: " << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cerr << "ERROR: Unknown exception in VMManager_CreateVM" << std::endl;
        return nullptr;
    }
}

GUIDEXOS_API bool VMManager_StartVM(VMManagerHandle manager, const char* vmId) {
    if (!manager || !vmId) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->startVM(vmId);
    } catch (...) {
        return false;
    }
}

GUIDEXOS_API bool VMManager_StopVM(VMManagerHandle manager, const char* vmId) {
    if (!manager || !vmId) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->stopVM(vmId);
    } catch (...) {
        return false;
    }
}

GUIDEXOS_API bool VMManager_PauseVM(VMManagerHandle manager, const char* vmId) {
    if (!manager || !vmId) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->pauseVM(vmId);
    } catch (...) {
        return false;
    }
}

GUIDEXOS_API bool VMManager_ResetVM(VMManagerHandle manager, const char* vmId) {
    if (!manager || !vmId) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->resetVM(vmId);
    } catch (...) {
        return false;
    }
}

GUIDEXOS_API bool VMManager_DeleteVM(VMManagerHandle manager, const char* vmId) {
    if (!manager || !vmId) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->deleteVM(vmId);
    } catch (...) {
        return false;
    }
}

// ============================================================================
// VM Execution Control
// ============================================================================

GUIDEXOS_API uint64_t VMManager_RunVM(
    VMManagerHandle manager,
    const char* vmId,
    uint64_t maxCycles) {
    
    if (!manager || !vmId) {
        return 0;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        return mgr->runVM(vmId, maxCycles);
    } catch (...) {
        return 0;
    }
}

// ============================================================================
// Framebuffer Access
// ============================================================================

GUIDEXOS_API bool VMManager_GetFramebuffer(
    VMManagerHandle manager,
    const char* vmId,
    uint8_t* buffer,
    size_t bufferSize,
    int* width,
    int* height) {
    
    if (!manager || !vmId || !buffer || !width || !height) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        size_t w = 0, h = 0;
        bool result = mgr->getFramebuffer(vmId, buffer, bufferSize, &w, &h);
        *width = static_cast<int>(w);
        *height = static_cast<int>(h);
        return result;
    } catch (...) {
        return false;
    }
}

GUIDEXOS_API bool VMManager_GetFramebufferDimensions(
    VMManagerHandle manager,
    const char* vmId,
    int* width,
    int* height) {
    
    if (!manager || !vmId || !width || !height) {
        return false;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        size_t w = 0, h = 0;
        bool result = mgr->getFramebufferDimensions(vmId, &w, &h);
        *width = static_cast<int>(w);
        *height = static_cast<int>(h);
        return result;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Memory Management
// ============================================================================

GUIDEXOS_API void VMManager_FreeString(const char* str) {
    if (str) {
        delete[] str;
    }
}

#include "VMManager_DLL.h"
// Force GUIDEXOS_HYPERVISOR_EXPORTS to be defined when building DLL
#ifndef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_HYPERVISOR_EXPORTS
#endif

#include "VMManager_DLL.h"
#include "VMManager.h"
#include "VMConfiguration.h"
#include <string>
#include <cstring>

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
    
    if (!manager || !configJson) {
        return nullptr;
    }
    
    try {
        VMManager* mgr = reinterpret_cast<VMManager*>(manager);
        
        // Parse JSON configuration
        VMConfiguration config = VMConfiguration::fromJson(std::string(configJson));
        
        // Validate configuration
        std::string validationError;
        if (!config.validate(&validationError)) {
            // Log validation error
            return nullptr;
        }
        
        // Create VM with the parsed configuration
        std::string vmId = mgr->createVM(config);
        
        if (vmId.empty()) {
            return nullptr;
        }
        
        return AllocateString(vmId);
    } catch (const std::exception& e) {
        // Log error: e.what()
        return nullptr;
    } catch (...) {
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

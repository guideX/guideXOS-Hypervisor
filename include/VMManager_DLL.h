#pragma once

#include <cstdint>
#include <cstddef>

// DLL Export/Import macros
#ifdef GUIDEXOS_HYPERVISOR_EXPORTS
#define GUIDEXOS_API __declspec(dllexport)
#else
#define GUIDEXOS_API __declspec(dllimport)
#endif

// C-style exports for P/Invoke
#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef void* VMManagerHandle;
typedef void* VMHandle;

// ============================================================================
// VMManager Lifecycle
// ============================================================================

/**
 * Create a new VMManager instance
 * @return VMManager handle
 */
GUIDEXOS_API VMManagerHandle VMManager_Create();

/**
 * Destroy a VMManager instance
 * @param manager VMManager handle
 */
GUIDEXOS_API void VMManager_Destroy(VMManagerHandle manager);

// ============================================================================
// VM Lifecycle
// ============================================================================

/**
 * Create a new VM
 * @param manager VMManager handle
 * @param name VM name
 * @param memorySize Memory size in bytes
 * @param cpuCount Number of CPUs
 * @return VM ID string (caller must free with VMManager_FreeString)
 */
GUIDEXOS_API const char* VMManager_CreateVM(
    VMManagerHandle manager,
    const char* name,
    size_t memorySize,
    int cpuCount);

/**
 * Start a VM
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_StartVM(VMManagerHandle manager, const char* vmId);

/**
 * Stop a VM
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_StopVM(VMManagerHandle manager, const char* vmId);

/**
 * Pause a VM
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_PauseVM(VMManagerHandle manager, const char* vmId);

/**
 * Reset a VM
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_ResetVM(VMManagerHandle manager, const char* vmId);

/**
 * Delete a VM
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_DeleteVM(VMManagerHandle manager, const char* vmId);

// ============================================================================
// Framebuffer Access
// ============================================================================

/**
 * Get framebuffer data from a VM
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @param buffer Destination buffer (BGRA32 format)
 * @param bufferSize Size of destination buffer
 * @param width Output: framebuffer width
 * @param height Output: framebuffer height
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_GetFramebuffer(
    VMManagerHandle manager,
    const char* vmId,
    uint8_t* buffer,
    size_t bufferSize,
    int* width,
    int* height);

/**
 * Get framebuffer dimensions
 * @param manager VMManager handle
 * @param vmId VM identifier
 * @param width Output: framebuffer width
 * @param height Output: framebuffer height
 * @return true if successful
 */
GUIDEXOS_API bool VMManager_GetFramebufferDimensions(
    VMManagerHandle manager,
    const char* vmId,
    int* width,
    int* height);

// ============================================================================
// Memory Management
// ============================================================================

/**
 * Free a string allocated by the DLL
 * @param str String to free
 */
GUIDEXOS_API void VMManager_FreeString(const char* str);

#ifdef __cplusplus
}
#endif

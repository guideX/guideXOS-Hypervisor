#include <Windows.h>

/**
 * DLL Entry Point
 * 
 * Called by Windows when the DLL is loaded or unloaded.
 * Minimal implementation - no special initialization required.
 */
BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD ul_reason_for_call,
    LPVOID lpReserved
) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        // DLL is being loaded into a process
        // Disable thread library calls for performance
        DisableThreadLibraryCalls(hModule);
        break;
        
    case DLL_THREAD_ATTACH:
        // A new thread is being created (won't be called if DisableThreadLibraryCalls is used)
        break;
        
    case DLL_THREAD_DETACH:
        // A thread is exiting (won't be called if DisableThreadLibraryCalls is used)
        break;
        
    case DLL_PROCESS_DETACH:
        // DLL is being unloaded from a process
        break;
    }
    
    return TRUE;
}

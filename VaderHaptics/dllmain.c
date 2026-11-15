#include <windows.h>
#include <stdio.h>
#include "vader_hid.h"
#include "hooks_manager.h"

static HANDLE g_hProcessMutex = NULL;
static BOOL g_isPrimaryDll = FALSE;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hinstDLL);

            // Pin this proxy DLL in memory so FreeLibrary / CoFreeUnusedLibraries can never unload our hooks mid-game
            HMODULE hPinned = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (LPCWSTR)hinstDLL, &hPinned);

            char mutexName[64];
            snprintf(mutexName, sizeof(mutexName), "Local\\VaderHaptics_PID_%lu", (unsigned long)GetCurrentProcessId());
            HANDLE hMtx = CreateMutexA(NULL, FALSE, mutexName);
            if (hMtx && GetLastError() == ERROR_ALREADY_EXISTS) {
                CloseHandle(hMtx);
            } else {
                g_hProcessMutex = hMtx;
                g_isPrimaryDll = TRUE;
                Hooks_Install();
                Vader_Init();
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            if (g_isPrimaryDll) {
                if (lpReserved != NULL) {
                    // Process is terminating: do not block on thread handles or call MH_Uninitialize under Loader Lock
                    Vader_EmergencyStop();
                } else {
                    Vader_Shutdown();
                    Hooks_Uninstall();
                    if (g_hProcessMutex) {
                        CloseHandle(g_hProcessMutex);
                        g_hProcessMutex = NULL;
                    }
                }
            }
            break;
    }
    return TRUE;
}

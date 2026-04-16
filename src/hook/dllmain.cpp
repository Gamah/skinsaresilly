#include "sovereign_hook.h"
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        // Disable per-thread notifications — we only need attach/detach
        DisableThreadLibraryCalls(hinstDLL);
        SovereignHook::Install();
        break;

    case DLL_PROCESS_DETACH:
        SovereignHook::Uninstall();
        break;
    }
    return TRUE;
}

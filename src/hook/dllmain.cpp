#include "sovereign_hook.h"
#include "sas_log.h"
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID /*lpvReserved*/)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        SasLog::Init(hinstDLL);
        SasLog::Write("DLL_PROCESS_ATTACH — SovereignHook loaded into cs2.exe");
        SovereignHook::Install();
        break;

    case DLL_PROCESS_DETACH:
        SasLog::Write("DLL_PROCESS_DETACH — unloading");
        SovereignHook::Uninstall();
        SasLog::Write("Uninstall complete");
        SasLog::Close();
        break;
    }
    return TRUE;
}

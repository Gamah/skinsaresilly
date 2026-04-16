#pragma once

namespace SovereignHook {

// Called from DllMain on DLL_PROCESS_ATTACH.
void Install();

// Called from DllMain on DLL_PROCESS_DETACH.
void Uninstall();

// Skin selection is communicated via the shared memory region defined in
// shared_skin_state.h — MuseumCurator.exe writes it, the update loop reads it.

} // namespace SovereignHook

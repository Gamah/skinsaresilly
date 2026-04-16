#pragma once

namespace SovereignHook {

// Called from DllMain on DLL_PROCESS_ATTACH.
// Locates CS2's skin interface and installs the vtable hook.
void Install();

// Called from DllMain on DLL_PROCESS_DETACH.
// Restores the original vtable pointer.
void Uninstall();

// Sets which skin index to report to the renderer.
// skinIndex corresponds to CS2's internal item definition index.
void SetActiveSkin(int skinIndex);

} // namespace SovereignHook

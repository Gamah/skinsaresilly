#pragma once
// Minimal timestamped logger for SovereignHook.dll.
// Writes to <dll_dir>\SovereignHook.log (same directory as the DLL).
// Thread-safe. Flushes on every write so the last line before a crash is preserved.
//
// Implementation is in sas_log.cpp — do NOT add inline definitions here, or each
// translation unit that includes this header will get its own copy of the statics
// and Init() / Write() will operate on different objects.

#include <windows.h>

namespace SasLog {

void Init(HMODULE hDll);
void Write(const char* fmt, ...);
void Close();

} // namespace SasLog

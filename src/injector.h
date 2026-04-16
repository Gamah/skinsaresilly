#pragma once
#include <windows.h>
#include <string>

// Returns 0 if the process is not found.
DWORD FindProcessId(const wchar_t* processName);

// Injects dllPath into the process identified by pid using the
// CreateRemoteThread + LoadLibraryW pattern.
// Returns an empty string on success, or an error message on failure.
std::wstring InjectDll(DWORD pid, const std::wstring& dllPath);

#include "injector.h"
#include <tlhelp32.h>
#include <string>

DWORD FindProcessId(const wchar_t* processName)
{
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

std::wstring InjectDll(DWORD pid, const std::wstring& dllPath)
{
    // Full path including null terminator
    const SIZE_T pathSize = (dllPath.size() + 1) * sizeof(wchar_t);

    HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!process)
        return L"OpenProcess failed. Is CS2 running? (error " + std::to_wstring(GetLastError()) + L")";

    // Allocate memory inside cs2.exe for the DLL path string
    void* remoteAddr = VirtualAllocEx(process, nullptr, pathSize,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteAddr) {
        CloseHandle(process);
        return L"VirtualAllocEx failed (error " + std::to_wstring(GetLastError()) + L")";
    }

    // Write the path string into cs2.exe's address space
    if (!WriteProcessMemory(process, remoteAddr, dllPath.c_str(), pathSize, nullptr)) {
        VirtualFreeEx(process, remoteAddr, 0, MEM_RELEASE);
        CloseHandle(process);
        return L"WriteProcessMemory failed (error " + std::to_wstring(GetLastError()) + L")";
    }

    // Ask cs2.exe to call LoadLibraryW(remoteAddr) — loads our DLL
    LPTHREAD_START_ROUTINE loadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE thread = CreateRemoteThread(process, nullptr, 0,
                                       loadLibrary, remoteAddr, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remoteAddr, 0, MEM_RELEASE);
        CloseHandle(process);
        return L"CreateRemoteThread failed (error " + std::to_wstring(GetLastError()) + L")";
    }

    // Wait for LoadLibraryW to return inside cs2.exe
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    // Clean up the remote memory — the DLL is loaded, the path string is no longer needed
    VirtualFreeEx(process, remoteAddr, 0, MEM_RELEASE);
    CloseHandle(process);

    return L""; // empty = success
}

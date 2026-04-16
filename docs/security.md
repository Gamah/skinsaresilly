# Security Transparency

This document explains exactly what SkinsAreSilly does to your system.
No vague claims. No hand-waving. Every API call, explained in plain English.

**If you don't understand something here, ask someone who does before running the tool.**
**If you can't build from source, don't run pre-built binaries from strangers.**

---

## What the tool does, step by step

### Step 1 — Find CS2's process ID (`MuseumCurator.exe`)

**APIs used:** `CreateToolhelp32Snapshot`, `Process32FirstW`, `Process32NextW`

These APIs take a snapshot of all running processes and walk the list looking for `cs2.exe`. This is equivalent to `ps aux | grep cs2`. Nothing is written, nothing is modified. It is read-only enumeration.

### Step 2 — Open a handle to cs2.exe (`MuseumCurator.exe`)

**API used:** `OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid)`

This obtains a handle that allows the injector to interact with the CS2 process. Windows requires this handle for the subsequent steps. The handle is closed when injection is complete.

`PROCESS_ALL_ACCESS` is used for simplicity; a production version could use a minimal permission set (`PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD`).

### Step 3 — Allocate memory inside cs2.exe (`MuseumCurator.exe`)

**API used:** `VirtualAllocEx(processHandle, NULL, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)`

This reserves a small region of memory inside CS2's address space — just large enough to hold the path string to `SovereignHook.dll`. No game data is touched.

### Step 4 — Write the DLL path into that memory (`MuseumCurator.exe`)

**API used:** `WriteProcessMemory(processHandle, remoteAddr, dllPath, pathSize, NULL)`

Writes the string `"C:\path\to\SovereignHook.dll"` into the memory allocated in Step 3. This is the only data written to CS2's process memory by the injector. It is a path string, not executable code.

### Step 5 — Ask CS2 to load the DLL (`MuseumCurator.exe`)

**API used:** `CreateRemoteThread(processHandle, NULL, 0, LoadLibraryW, remoteAddr, 0, NULL)`

This creates a thread inside CS2 that calls Windows' own `LoadLibraryW` function with the path written in Step 4. Windows then loads `SovereignHook.dll` into CS2's process — the same mechanism used by every legitimate DLL loader, debugger, and profiler.

### Step 6 — Clean up (`MuseumCurator.exe`)

**APIs used:** `WaitForSingleObject`, `VirtualFreeEx`, `CloseHandle`

The injector waits for the load thread to finish, frees the remote memory, and closes all handles. No persistent footprint in the injector process.

---

## What SovereignHook.dll does inside CS2

### On load (`DLL_PROCESS_ATTACH`)

`SovereignHook::Install()` is called. This:

1. **Pattern-scans** CS2's loaded modules to locate the weapon/skin interface — a read-only walk of already-loaded memory pages. No writing during this phase.
2. **Installs a vtable hook** on the skin resolution function. A vtable hook replaces one function pointer in a class's virtual dispatch table with a pointer to our replacement function. The original pointer is saved so it can be restored on unload.

The hooked function intercepts the call CS2 makes when determining which skin to render for a weapon. Our replacement returns the player-selected skin index instead of the inventory-verified one. The result is local and visual only — the server never sees it, other players never see it, game state is not affected.

### On unload (`DLL_PROCESS_DETACH`)

`SovereignHook::Uninstall()` restores the original vtable pointer. CS2 returns to normal skin rendering.

---

## Third-party dependencies (fetched at build time)

SovereignHook.dll is built against two open-source libraries fetched from GitHub via CMake FetchContent. Nothing is bundled as a binary — you can audit both before building.

### a2x/cs2-dumper
- **Repo**: https://github.com/a2x/cs2-dumper
- **What it provides**: Auto-generated C++ header files (`output/client_dll.hpp`, `output/offsets.hpp`) containing the memory offsets for every CS2 data structure — weapon entity layout, inventory services, item view fields, etc.
- **How it's used**: `sovereign_hook.cpp` imports these headers and uses the offset constants to navigate weapon entity memory without hardcoded magic numbers. When CS2 updates, the community re-runs the dumper and the output headers are updated in the repo.
- **How to verify**: Read `output/client_dll.hpp` in the fetched source. Every field is a named `constexpr std::ptrdiff_t`. No executable code.

### alliedmodders/hl2sdk (cs2 branch)
- **Repo**: https://github.com/alliedmodders/hl2sdk (branch: `cs2`)
- **What it provides**: Source 2 engine interface declarations — `IEntitySystem`, `CEntityHandle`, `CBaseEntity`, etc. Used by SourceMod and MetaMod, the legitimate CS2 server plugin ecosystem.
- **How it's used**: Included for engine type definitions. Headers only — nothing compiled from this repo goes into the output DLL.
- **How to verify**: Headers only. No compiled code from this dependency is in the final DLL.

## Comparison to HLAE

This project follows the same injection pattern as [HLAE (Half-Life Advanced Effects)](https://github.com/advancedfx/advancedfx), the open-source cinematography tool used by professional CS content creators.

HLAE also:
- Injects a DLL into CS2 via `CreateRemoteThread`/`LoadLibraryW`
- Reads CS2 internal memory using offset navigation
- Requires `-insecure` mode
- Is open source

The difference is purpose: HLAE hooks rendering for camera control and demo playback; SAS modifies weapon item view data for local skin display.

Relevant HLAE source for reference:
- Injection: [`AfxHookSource2/AfxHookSource2.cpp`](https://github.com/advancedfx/advancedfx/tree/main/AfxHookSource2)

---

## What this tool does NOT do

- Does not read, exfiltrate, or log any data from your machine
- Does not contact any network endpoint
- Does not modify any files on disk (no game file patching)
- Does not affect any process other than cs2.exe
- Does not provide aim assistance, wallhacks, or any competitive advantage
- Does not touch VAC's memory — VAC will, however, detect the injected DLL if you use this on a VAC-protected server

---

## Pre-built binaries — implications

This repository ships a `bin/` directory containing compiled versions of `SovereignHook.dll` and `MuseumCurator.exe`. This is a deliberate trade-off between convenience and trust, and you should understand it before using those files.

### What shipping a binary means

A pre-built binary is an opaque executable. You cannot verify what it does by inspection alone — only by:

1. **Comparing its SHA-256 hash to the published value** (proves the file hasn't been modified after the commit, but does not prove the source compiles to that binary).
2. **Reproducing the build yourself** (proves the binary matches the source you can read).

Published hashes for the current `bin/` contents:

| File | SHA-256 |
|------|---------|
| `bin/SovereignHook.dll` | `c36810396eba17d09b9df91da85dead0badf6eac416a83172a9e33a9745838de` |
| `bin/MuseumCurator.exe` | `49166c6d5c59c6649f9e6575588710b880ca4f00bba6a0558c82c6fb0ec0c080` |

```powershell
Get-FileHash bin\SovereignHook.dll  -Algorithm SHA256
Get-FileHash bin\MuseumCurator.exe  -Algorithm SHA256
```

### What trusting this binary requires you to trust

- That this commit's source is the source that produced the binary.
- That the build machine was not compromised.
- That no toolchain component (MinGW, system libraries) injected unexpected code.
- That the GitHub repository has not been tampered with between the author's push and your clone.

**None of these are guaranteed.** This is true of every pre-built binary ever distributed, including software you use daily. The difference here is that you are injecting this DLL into a live game process with elevated privileges, so the consequences of a compromised binary are higher than average.

### Recommendation

Build from source if you have any doubt. The build is reproducible:

```bash
sudo bash setup_host.sh
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-windows.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
sha256sum SovereignHook.dll MuseumCurator.exe
```

The hashes should match the table above. If they don't, open an issue — either the build is not reproducible (a bug) or something is wrong (a security concern).

---

## Reporting concerns

If you find code in this repository that does something not described in this document, open an issue immediately. The goal of this project is full transparency. Unexplained behavior is a bug.

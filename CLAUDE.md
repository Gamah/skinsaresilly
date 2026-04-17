# CLAUDE.md

## Project

**SkinsAreSilly (SAS)** — a CS2 social-commentary tool that applies local skin overrides by injecting a DLL. Requires CS2 launched with `-insecure` (VAC disabled). No competitive advantage; visual only.

## Repo layout

```
src/
  hook/sovereign_hook.cpp   DLL payload — runs inside cs2.exe; update loop writes
                            weapon entity fallback fields every 250 ms
  hook/sovereign_hook.h
  shared_skin_state.h       IPC struct via named file-mapping (Local\SkinsAreSillyState)
  skin_catalogue.h          Static weapon/skin definitions (weaponDefIndex, paintKitId)
  skin_loader.h             Parses ByMykel skins.json; BuildStaticFallback() fallback
  inspect_api.h             WinHTTP GET to api.csgofloat.com to decode inspect links
  injector.cpp / .h         LoadLibraryW-based DLL injector
  main_ui.cpp               Dear ImGui UI — tabs, search, wear slider, inject button

bin/                        Prebuilt Windows binaries (SHA-256 hashes in README + security.md)
docs/security.md            API-level transparency doc; update whenever new syscalls/network added
toolchain-windows.cmake     MinGW cross-compile from Ubuntu to Windows
```

## Build

Cross-compiled on Ubuntu → Windows (MinGW). CMake FetchContent pulls:
- `a2x/cs2-dumper` (GIT_TAG=main) — offset headers; **stale offsets = game crash**
- `alliedmodders/hl2sdk` branch cs2 — engine type headers only
- `nlohmann/json` v3.11.3 — JSON parsing in MuseumCurator.exe only
- `dear imgui` — UI

```bash
sudo bash setup_host.sh
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-windows.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

## Key facts for future sessions

- **Crash after injection** = almost always stale cs2-dumper offsets. Delete `build/_deps/cs2_dumper-src`, re-run cmake.
- `m_pInGameMoneyServices` in `sovereign_hook.cpp` is flagged as a placeholder field name — verify against current `client_dll.hpp` before trusting the pointer chain.
- `MemRead`/`MemWrite` use bare `memcpy` — no SEH guard. A bad pointer crashes the game process, not just the hook thread.
- The IPC is a named file-mapping; MuseumCurator must be running for the hook to pick up a skin selection.
- Prebuilt `bin/` hashes must be updated in both `README.md` and `docs/security.md` after every binary rebuild.
- `docs/security.md` is the trust document — keep it accurate. Add any new syscall, file read, or network call there.

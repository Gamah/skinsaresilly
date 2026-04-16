# SkinsAreSilly (SAS)

> *"A skin is just paint on a wall. The gallery should be free to enter."*

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows-lightgrey)](https://github.com)
[![CS2: -insecure only](https://img.shields.io/badge/CS2--insecure%20only-red)](https://github.com)

---

## The Art Museum

Imagine a world-class art museum that charges a separate admission fee for every painting.
You can walk the halls, but the canvases are blank unless you pay — not to own a print, not to take it home, but simply to *see* it while you are already inside.

That is the current state of CS2 weapon skins.

Skins are **client-side cosmetics**. They do not affect gameplay. No other player benefits from your purchase; no other player is harmed by your viewing. Yet Valve has constructed a system where the *perception* of a visual effect — one that exists entirely on your own screen — requires a real-money transaction.

**SkinsAreSilly** is a social commentary project. It lets you walk through the museum. You load CS2 in `-insecure` mode (VAC disabled, competitive matchmaking unavailable), inject this tool, and your weapons render with whatever skin you choose — locally, client-side, visible only to you.

The point is not to steal. The point is to prove the absurdity of the model.

---

## The Martyr Disclaimer

**READ THIS BEFORE RUNNING ANYTHING.**

| Situation | Consequence |
|---|---|
| CS2 launched with `-insecure` + SAS injected | Safe. VAC is not active. No ban. |
| CS2 launched **without** `-insecure` + SAS injected | **Permanent VAC ban. No appeal. No exceptions.** |
| Joining *any* VAC-protected server with SAS loaded | **Permanent VAC ban.** |

The developer of this tool (Gamah) runs this on their own account and is comfortable with the consequences. The tool exists to make a point. If Valve bans the account, that ban itself becomes part of the commentary: *a company so protective of digital scarcity that it punishes users for looking at textures.*

**Do not run this on an account you care about unless you share this philosophy.**

This is not a cheat. There is no aim assistance, no wallhack, no competitive advantage. It is a texture viewer. The VAC ban risk is real and accepted, not hidden.

---

## Building

### Prerequisites

- Ubuntu 24.04 (or compatible)
- Internet access (CMake fetches Dear ImGui at configure time)

### Setup

```bash
git clone https://github.com/yourusername/skinsaresilly.git
cd skinsaresilly
sudo bash setup_host.sh
```

### Compile

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-windows.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

Output files:
- `build/SovereignHook.dll` — the hook payload, injected into cs2.exe
- `build/MuseumCurator.exe` — the UI and injector, runs on your Windows machine

### Running (Windows)

1. Launch CS2 with `-insecure` in your Steam launch options
2. Wait for CS2 to reach the main menu
3. Run `MuseumCurator.exe`
4. Read and acknowledge the disclaimer
5. Select a skin, click Inject

---

## Why Open Source?

Because the moment this project is closed-source, it becomes untrustworthy.

You should be able to read every line of code that touches your game process. If you cannot read C++, find someone who can. The source is here. Build it yourself. Do not run binaries from strangers.

See [`docs/security.md`](docs/security.md) for a plain-English explanation of every Windows API this tool calls.

---

## Support the Commentary

If you find this project interesting — as software, as philosophy, or as provocation — you can buy me a coffee.

**[buymeacoffee.com/Gamah](https://buymeacoffee.com/Gamah)**

---

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

Any fork of this project must remain open source under the same terms. You may not distribute modified closed-source versions.

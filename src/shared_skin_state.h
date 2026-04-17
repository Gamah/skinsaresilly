#pragma once
#include <windows.h>
#include <cstdint>

// Named file-mapping used as IPC between MuseumCurator.exe (writer)
// and SovereignHook.dll (reader, runs inside cs2.exe).
#define SKINSARESILLY_SHMEM_NAME L"Local\\SkinsAreSillyState"

// Maximum number of weapon slots in the shared loadout.
// One slot per unique weaponDefIndex; 64 covers all CS2 weapons with room to spare.
#define SAS_MAX_LOADOUT_ENTRIES 64

// One entry in the shared loadout: maps a weapon type to a paint kit + wear.
struct LoadoutSlot {
    int   weaponDefIndex; // CS2 item_definition_index; 0 = empty slot
    int   paintKitId;
    float wear;
};

// Full shared-memory state written by MuseumCurator and read by SovereignHook every tick.
// The hook builds a defIndex→(paintKit,wear) map from slots[] and applies it per weapon.
struct SharedLoadoutState {
    uint32_t    version;                        // incremented by UI on every change
    LoadoutSlot slots[SAS_MAX_LOADOUT_ENTRIES]; // sparse array; unused slots have weaponDefIndex==0
    // Total: 4 + (12 * 64) = 772 bytes
};

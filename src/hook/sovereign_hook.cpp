#include "sovereign_hook.h"
#include "shared_skin_state.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// cs2-dumper generated headers (fetched at build time from a2x/cs2-dumper)
//
// These are committed to the dumper repo's output/ directory and updated by
// the community after every CS2 patch. If offsets.hpp or client_dll.hpp are
// missing after cmake configure, run FetchContent_MakeAvailable again or
// check that GIT_TAG=main resolved correctly.
//
// Source: https://github.com/a2x/cs2-dumper
// ---------------------------------------------------------------------------
#include "offsets.hpp"      // dwEntityList, dwLocalPlayerController, etc.
#include "client_dll.hpp"   // C_EconItemView, CCSPlayerPawn, weapon services, etc.

// Shorter namespace aliases
namespace schemas  = cs2_dumper::schemas::client_dll;
namespace offsets  = cs2_dumper::offsets::client_dll;

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
template<typename T>
static T MemRead(uintptr_t addr)
{
    T val{};
    if (addr) std::memcpy(&val, reinterpret_cast<void*>(addr), sizeof(T));
    return val;
}

template<typename T>
static void MemWrite(uintptr_t addr, T val)
{
    if (addr) std::memcpy(reinterpret_cast<void*>(addr), &val, sizeof(T));
}

// ---------------------------------------------------------------------------
// CS2 entity handle resolution
//
// An EntityHandle is a 32-bit value: lower 15 bits = entity index,
// upper bits = serial number. The entity list is split into chunks of 512.
//
// Layout (from CS2 reverse engineering, consistent across recent builds):
//   EntityList base → array of chunk pointers
//   chunk[i]        → array of entity pointers (stride = 0x78 per slot)
//   entity ptr      → CEntityInstance* (first field of entity)
// ---------------------------------------------------------------------------
static constexpr uint32_t ENT_ENTRY_MASK  = 0x7FFF;
static constexpr uint32_t ENT_CHUNK_SIZE  = 512;
static constexpr uintptr_t ENT_PTR_STRIDE = 0x78;  // bytes per slot in chunk

static uintptr_t EntityFromHandle(uintptr_t entityListBase, uint32_t handle)
{
    if (handle == 0xFFFFFFFF) return 0; // invalid handle

    uint32_t index = handle & ENT_ENTRY_MASK;
    uint32_t chunk = index / ENT_CHUNK_SIZE;
    uint32_t slot  = index % ENT_CHUNK_SIZE;

    uintptr_t chunkPtr = MemRead<uintptr_t>(entityListBase + (chunk + 1) * 8);
    if (!chunkPtr) return 0;

    return MemRead<uintptr_t>(chunkPtr + slot * ENT_PTR_STRIDE);
}

// ---------------------------------------------------------------------------
// Skin application state
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool> g_running{false};
std::thread       g_updateThread;

// ---------------------------------------------------------------------------
// ApplySkins — reads the current skin selection from shared memory (written by
// MuseumCurator.exe), then walks the local player's weapon loadout and writes:
//
//   C_EconItemView::m_iItemDefinitionIndex  — weapon type (AK-47 = 7, AWP = 9…)
//   C_EconEntity::m_nFallbackPaintKit       — paint kit ID (Dragon Lore = 344…)
//   C_EconEntity::m_flFallbackWear          — wear float [0.0, 1.0]
//
// The fallback fields exist precisely for this use-case and are the simplest
// reliable path; no attribute array traversal required.
//
// Navigation chain (offsets from cs2-dumper client_dll.hpp):
//   client.dll base + dwLocalPlayerController → CCSPlayerController*
//     → m_pInGameMoneyServices → CCSPlayerController_InventoryServices*
//       → m_vecNetworkableLoadout[64] → entity handles
//         → weapon entity
//           → m_AttributeManager → C_AttributeContainer → m_Item → C_EconItemView
// ---------------------------------------------------------------------------
static void ApplySkins()
{
    // Read skin selection from shared memory written by MuseumCurator.exe.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, SKINSARESILLY_SHMEM_NAME);
    if (!hMap) return; // UI not running

    const auto* state = static_cast<const SharedSkinState*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedSkinState)));
    if (!state || state->weaponDefIndex == 0) {
        if (state) UnmapViewOfFile(state);
        CloseHandle(hMap);
        return;
    }

    int   desiredDefIndex = state->weaponDefIndex;
    int   paintKitId      = state->paintKitId;
    float wear            = state->wear;
    UnmapViewOfFile(state);
    CloseHandle(hMap);

    // Resolve client.dll base — we are already inside the process
    uintptr_t clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
    if (!clientBase) return;

    // Read the global pointer to the local player controller
    uintptr_t localControllerPtr = MemRead<uintptr_t>(
        clientBase + offsets::dwLocalPlayerController);
    if (!localControllerPtr) return;

    // Get the InventoryServices sub-object
    uintptr_t inventoryServicesPtr = MemRead<uintptr_t>(
        localControllerPtr +
        schemas::CCSPlayerController::m_pInGameMoneyServices); // placeholder field name
    // NOTE: the exact field name for inventory services in the current dump may differ.
    // Check client_dll.hpp for CCSPlayerController fields referencing inventory or loadout.
    if (!inventoryServicesPtr) return;

    // Read entity list base
    uintptr_t entityListBase = MemRead<uintptr_t>(
        clientBase + offsets::dwEntityList);
    if (!entityListBase) return;

    // Walk the loadout slot array (64 slots: pistols, rifles, knives, etc.)
    constexpr int LOADOUT_SLOTS = 64;
    uintptr_t loadoutArray = inventoryServicesPtr +
        schemas::CCSPlayerController_InventoryServices::m_vecNetworkableLoadout;

    for (int i = 0; i < LOADOUT_SLOTS; ++i) {
        uint32_t handle = MemRead<uint32_t>(loadoutArray + i * sizeof(uint32_t));
        if (handle == 0 || handle == 0xFFFFFFFF) continue;

        uintptr_t weaponEntity = EntityFromHandle(entityListBase, handle);
        if (!weaponEntity) continue;

        // Navigate to C_EconItemView via C_AttributeContainer.
        // m_AttributeManager is an *embedded* C_AttributeContainer (not a pointer),
        // so we add the offset directly — no pointer dereference here.
        uintptr_t attrMgrBase = weaponEntity + schemas::C_EconEntity::m_AttributeManager;

        // m_Item is likewise an embedded C_EconItemView inside C_AttributeContainer.
        uintptr_t itemViewPtr = attrMgrBase + schemas::C_AttributeContainer::m_Item;

        // Write weapon type.
        MemWrite<uint16_t>(
            itemViewPtr + schemas::C_EconItemView::m_iItemDefinitionIndex,
            static_cast<uint16_t>(desiredDefIndex));

        // Write paint kit and wear directly to the fallback fields on C_EconEntity.
        MemWrite<int32_t>(weaponEntity + schemas::C_EconEntity::m_nFallbackPaintKit, paintKitId);
        MemWrite<float>  (weaponEntity + schemas::C_EconEntity::m_flFallbackWear,    wear);
    }
}

static void UpdateLoop()
{
    while (g_running.load()) {
        ApplySkins();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace SovereignHook {

void Install()
{
    g_running.store(true);
    g_updateThread = std::thread(UpdateLoop);
}

void Uninstall()
{
    g_running.store(false);
    if (g_updateThread.joinable())
        g_updateThread.join();
}

} // namespace SovereignHook

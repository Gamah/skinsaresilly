#include "sovereign_hook.h"

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

std::atomic<int>  g_activeSkinDefIndex{0}; // CS2 item definition index for desired skin
std::atomic<bool> g_running{false};
std::thread       g_updateThread;

// ---------------------------------------------------------------------------
// ApplySkins — walks the local player's weapon loadout and overwrites the
// item definition index on each weapon's C_EconItemView.
//
// Called periodically from a background thread so the skin persists across
// weapon switches and round restarts.
//
// Navigation chain (all offsets from cs2-dumper client_dll.hpp):
//
//   client.dll base
//     + offsets::dwLocalPlayerController
//     → CCSPlayerController*
//         + schemas::CCSPlayerController_InventoryServices (InventoryServices ptr)
//         → CCSPlayerController_InventoryServices*
//             + schemas::CCSPlayerController_InventoryServices::m_vecNetworkableLoadout
//             → CHandle<C_EconItemView>[64]   (one per loadout slot)
//
// For each non-null loadout entry:
//   resolve handle → CEntityInstance (the weapon entity)
//     + schemas::C_EconEntity::m_AttributeManager
//     → CAttributeContainer*
//         + schemas::C_AttributeContainer::m_Item
//         → C_EconItemView*
//             + schemas::C_EconItemView::m_iItemDefinitionIndex  ← write here
//
// TODO — Paint kit (skin texture):
//   m_iItemDefinitionIndex selects the weapon TYPE (AK-47, AWP, etc.).
//   The paint kit (which skin pattern/finish) is stored as an attribute in the
//   item's CEconItemAttribute array. Attribute definition index 6 is "set supply
//   crate series" — the exact attr def index for paint kits needs to be confirmed
//   against the current CS2 item schema (items_game.txt). Once known, add:
//
//     uintptr_t attrBase = itemViewPtr + schemas::C_EconItemView::m_AttributeList;
//     // iterate CEconItemAttribute array, find attr def == PAINTKIT_ATTR_DEF,
//     // write desired paint kit ID to m_value_bytes (uint32 cast from float)
//
//   Reference: https://github.com/a2x/cs2-dumper output/client_dll.hpp
//              search for C_EconItemAttribute, CEconItemAttributeDefinition
// ---------------------------------------------------------------------------
static void ApplySkins()
{
    int desiredDefIndex = g_activeSkinDefIndex.load();
    if (desiredDefIndex == 0) return;

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

        // Write the desired item definition index
        // This changes the weapon's base type — for skins, also write the
        // paint kit attribute (see TODO above).
        MemWrite<uint16_t>(
            itemViewPtr + schemas::C_EconItemView::m_iItemDefinitionIndex,
            static_cast<uint16_t>(desiredDefIndex));
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

void SetActiveSkin(int itemDefIndex)
{
    g_activeSkinDefIndex.store(itemDefIndex);
}

} // namespace SovereignHook

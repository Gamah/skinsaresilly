#include "sovereign_hook.h"
#include "shared_skin_state.h"
#include "sas_log.h"

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_map>

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
// Memory helpers — bare memcpy inside the game process (no SEH guard).
// A bad address will crash the game process, not just this thread.
// Always validate pointers before calling these.
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

std::atomic<bool>     g_running{false};
std::thread           g_updateThread;
std::atomic<uint64_t> g_tickCount{0};

// ---------------------------------------------------------------------------
// ApplySkins — reads the full per-weapon loadout from shared memory (written by
// MuseumCurator.exe), then walks the local player's weapon loadout and writes:
//
//   C_EconEntity::m_nFallbackPaintKit  — paint kit ID (Dragon Lore = 344…)
//   C_EconEntity::m_flFallbackWear     — wear float [0.0, 1.0]
//
// The fallback fields exist precisely for this use-case; no attribute array
// traversal required. We do NOT write m_iItemDefinitionIndex — the game engine
// already has the correct type from its own state, and overwriting it corrupts
// the entity.
//
// Navigation chain (offsets from cs2-dumper client_dll.hpp):
//   client.dll base + dwLocalPlayerController → CCSPlayerController*
//     → m_pInventoryServices → CCSPlayerController_InventoryServices*
//       → m_vecNetworkableLoadout[64] → entity handles
//         → weapon entity
//           → m_AttributeManager → C_AttributeContainer → m_Item → C_EconItemView
//               (read m_iItemDefinitionIndex to identify weapon type)
//           → m_nFallbackPaintKit, m_flFallbackWear  (write these)
// ---------------------------------------------------------------------------
static void ApplySkins()
{
    uint64_t tick = ++g_tickCount;

    // Read the full loadout from shared memory written by MuseumCurator.exe.
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, SKINSARESILLY_SHMEM_NAME);
    if (!hMap) {
        if (tick == 1)
            SasLog::Write("tick #%llu: OpenFileMapping returned NULL — MuseumCurator not running?", tick);
        return;
    }

    const auto* state = static_cast<const SharedLoadoutState*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedLoadoutState)));
    if (!state || state->version == 0) {
        if (state) UnmapViewOfFile(state);
        CloseHandle(hMap);
        if (tick == 1)
            SasLog::Write("tick #%llu: shmem mapped but version==0 — loadout is empty (select skins first)", tick);
        return;
    }

    // Build a quick defIndex → slot lookup from the sparse slots array.
    std::unordered_map<int, LoadoutSlot> skinMap;
    for (int i = 0; i < SAS_MAX_LOADOUT_ENTRIES; ++i) {
        const auto& s = state->slots[i];
        if (s.weaponDefIndex != 0)
            skinMap[s.weaponDefIndex] = s;
    }
    uint32_t loadoutVersion = state->version;
    UnmapViewOfFile(state);
    CloseHandle(hMap);

    if (skinMap.empty()) {
        if (tick == 1)
            SasLog::Write("tick #%llu: loadout has no assigned skins yet", tick);
        return;
    }

    SasLog::Write("tick #%llu: loadout v%u — %zu weapon(s) assigned",
        tick, loadoutVersion, skinMap.size());

    // Resolve client.dll base — we are already inside the process.
    uintptr_t clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
    if (!clientBase) {
        SasLog::Write("tick #%llu: client.dll not found — aborting", tick);
        return;
    }

    // Read the global pointer to the local player controller.
    uintptr_t localControllerPtr = MemRead<uintptr_t>(
        clientBase + offsets::dwLocalPlayerController);
    if (!localControllerPtr) {
        if (tick % 20 == 1)
            SasLog::Write("tick #%llu: localControllerPtr is null — not in-game yet?", tick);
        return;
    }

    // Get the InventoryServices sub-object.
    // Field: CCSPlayerController::m_pInventoryServices (offset 0x810)
    // This is the loadout/inventory pointer, NOT m_pInGameMoneyServices (0x808).
    uintptr_t inventoryServicesPtr = MemRead<uintptr_t>(
        localControllerPtr +
        schemas::CCSPlayerController::m_pInventoryServices);
    if (!inventoryServicesPtr) {
        SasLog::Write("tick #%llu: inventoryServicesPtr is null — wrong offset? verify client_dll.hpp", tick);
        return;
    }

    // Read entity list base.
    uintptr_t entityListBase = MemRead<uintptr_t>(
        clientBase + offsets::dwEntityList);
    if (!entityListBase) {
        SasLog::Write("tick #%llu: entityListBase is null — aborting", tick);
        return;
    }

    // Walk the loadout slot array (64 slots: pistols, rifles, knives, etc.)
    constexpr int LOADOUT_SLOTS = 64;
    uintptr_t loadoutArray = inventoryServicesPtr +
        schemas::CCSPlayerController_InventoryServices::m_vecNetworkableLoadout;

    int written = 0;
    for (int i = 0; i < LOADOUT_SLOTS; ++i) {
        uint32_t handle = MemRead<uint32_t>(loadoutArray + i * sizeof(uint32_t));
        if (handle == 0 || handle == 0xFFFFFFFF) continue;

        uintptr_t weaponEntity = EntityFromHandle(entityListBase, handle);
        if (!weaponEntity) continue;

        // Read the weapon's actual defIndex from its EconItemView — do NOT write it.
        // Writing it would corrupt the entity (bug #3 fix).
        uintptr_t attrMgrBase = weaponEntity + schemas::C_EconEntity::m_AttributeManager;
        uintptr_t itemViewPtr = attrMgrBase + schemas::C_AttributeContainer::m_Item;
        int defIndex = static_cast<int>(
            MemRead<uint16_t>(itemViewPtr + schemas::C_EconItemView::m_iItemDefinitionIndex));

        auto it = skinMap.find(defIndex);
        if (it == skinMap.end()) continue; // no skin assigned for this weapon

        const LoadoutSlot& slot = it->second;
        MemWrite<int32_t>(weaponEntity + schemas::C_EconEntity::m_nFallbackPaintKit, slot.paintKitId);
        MemWrite<float>  (weaponEntity + schemas::C_EconEntity::m_flFallbackWear,    slot.wear);
        ++written;
    }

    if (written > 0 || tick % 20 == 1)
        SasLog::Write("tick #%llu: wrote to %d weapon slot(s)", tick, written);
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
    SasLog::Write("Install(): starting update thread (250ms tick)");
    g_running.store(true);
    g_updateThread = std::thread(UpdateLoop);
    SasLog::Write("Install(): thread started");
}

void Uninstall()
{
    SasLog::Write("Uninstall(): stopping update thread");
    g_running.store(false);
    if (g_updateThread.joinable())
        g_updateThread.join();
    SasLog::Write("Uninstall(): thread joined");
}

} // namespace SovereignHook

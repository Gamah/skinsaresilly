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
// dwEntityList points to CGameEntitySystem*, which inherits CEntitySystem.
// CEntitySystem layout (from hl2sdk entity2/entitysystem.h):
//   +0x00  vtable
//   +0x08  m_pCurrentManifest (IEntityResourceManifest*, private)
//   +0x10  m_EntityList (CConcreteEntityList, inline)
//
// CConcreteEntityList layout (entity2/concreteentitylist.h):
//   +0x00  m_pIdentityChunks[64]  — CEntityIdentity* per chunk (512 entries each)
//
// So chunk[i] pointer lives at: entityListBase + 0x10 + i * 8
//
// Each chunk is an array of CEntityIdentity (stride = 0x78).
// CEntityIdentity::m_pInstance (CEntityInstance*) is at offset 0x00.
// ---------------------------------------------------------------------------
static constexpr uint32_t  ENT_ENTRY_MASK  = 0x7FFF;
static constexpr uint32_t  ENT_CHUNK_SIZE  = 512;
static constexpr uintptr_t ENT_PTR_STRIDE  = 0x70;   // sizeof(CEntityIdentity) — hl2sdk layout:
                                                       // m_pNextByClass at 0x68 (+8) = 0x70 total,
                                                       // padded to 8-byte alignment. 0x78 was wrong
                                                       // and caused m_pClass to be read as m_pInstance
                                                       // for every slot > 0, producing a garbage entity
                                                       // pointer that crashed on the weapon-services walk.
static constexpr uintptr_t ENT_LIST_OFFSET = 0x10;   // CEntitySystem::m_EntityList offset

static uintptr_t EntityFromHandle(uintptr_t entityListBase, uint32_t handle)
{
    if (handle == 0xFFFFFFFF) return 0; // invalid handle

    uint32_t index = handle & ENT_ENTRY_MASK;
    uint32_t chunk = index / ENT_CHUNK_SIZE;
    uint32_t slot  = index % ENT_CHUNK_SIZE;

    // m_pIdentityChunks[chunk] lives at entityListBase + 0x10 + chunk * 8
    uintptr_t chunkPtr = MemRead<uintptr_t>(entityListBase + ENT_LIST_OFFSET + chunk * 8);
    if (!chunkPtr) return 0;

    // m_pInstance is at offset 0x00 within CEntityIdentity
    return MemRead<uintptr_t>(chunkPtr + slot * ENT_PTR_STRIDE);
}

// ---------------------------------------------------------------------------
// Skin application state
// ---------------------------------------------------------------------------
namespace {

std::atomic<bool>     g_running{false};
std::thread           g_updateThread;
std::atomic<uint64_t> g_tickCount{0};

// Per-entity last-written state used to suppress redundant readback logs.
// Key = weapon entity address.  We only log when the pre-write value diverges
// from what we wrote last time (meaning the server/prediction wiped it).
struct WrittenState { int32_t pk; float wear; uint32_t idHigh; };
std::unordered_map<uintptr_t, WrittenState> g_lastWritten;
uint32_t g_lastLoadoutVersion = 0;

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

    if (loadoutVersion != g_lastLoadoutVersion) {
        SasLog::Write("tick #%llu: loadout v%u — %zu weapon(s) assigned",
            tick, loadoutVersion, skinMap.size());
        g_lastLoadoutVersion = loadoutVersion;
    }

    // Resolve client.dll base — we are already inside the process.
    uintptr_t clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
    if (!clientBase) {
        SasLog::Write("tick #%llu: client.dll not found — aborting", tick);
        return;
    }
    // Verbose pointer chain only on tick 1 — at 64 Hz this would be 60 lines/sec.
    if (tick == 1)
        SasLog::Write("tick #%llu: clientBase=0x%llX", tick, (unsigned long long)clientBase);

    uintptr_t localControllerPtr = MemRead<uintptr_t>(
        clientBase + offsets::dwLocalPlayerController);
    if (!localControllerPtr) {
        if (tick % 60 == 1)
            SasLog::Write("tick #%llu: localControllerPtr is null — not in-game yet?", tick);
        return;
    }

    uintptr_t entityListBase = MemRead<uintptr_t>(
        clientBase + offsets::dwEntityList);
    if (!entityListBase) {
        if (tick % 60 == 1)
            SasLog::Write("tick #%llu: entityListBase is null — aborting", tick);
        return;
    }

    uint32_t pawnHandle = MemRead<uint32_t>(
        localControllerPtr + schemas::CCSPlayerController::m_hPlayerPawn);
    if (pawnHandle == 0xFFFFFFFF || pawnHandle == 0) {
        if (tick % 60 == 1)
            SasLog::Write("tick #%llu: pawn handle invalid — not spawned yet", tick);
        return;
    }

    uintptr_t pawnEntity = EntityFromHandle(entityListBase, pawnHandle);
    if (!pawnEntity) {
        if (tick % 60 == 1)
            SasLog::Write("tick #%llu: pawn handle 0x%08X resolved to null — stale list?", tick, pawnHandle);
        return;
    }

    uintptr_t weaponServicesPtr = MemRead<uintptr_t>(
        pawnEntity + schemas::C_BasePlayerPawn::m_pWeaponServices);
    if (!weaponServicesPtr) {
        if (tick % 60 == 1)
            SasLog::Write("tick #%llu: weaponServicesPtr is null", tick);
        return;
    }

    // CUtlVector layout (hl2sdk tier1/utlvector.h):
    //   +0x00  int32   m_Size          — element count  (NOT the data pointer)
    //   +0x04  int32   padding
    //   +0x08  T*      m_pMemory       — heap pointer to handle array
    //   +0x10  int32   m_nAllocationCount
    // The previous assumption (data pointer first) was wrong; m_Size=3 was
    // being used as a pointer → crash on the first handle read from address 0x3.
    uintptr_t weaponsBase    = weaponServicesPtr + schemas::CPlayer_WeaponServices::m_hMyWeapons;
    int32_t   weaponsCount   = MemRead<int32_t>  (weaponsBase + 0x00);
    uintptr_t weaponsDataPtr = MemRead<uintptr_t>(weaponsBase + 0x08);

    if (!weaponsDataPtr || weaponsCount <= 0 || weaponsCount > 64) {
        if (tick % 60 == 1)
            SasLog::Write("tick #%llu: weapon list empty or invalid (count=%d)", tick, weaponsCount);
        return;
    }

    int written = 0;
    for (int i = 0; i < weaponsCount; ++i) {
        uint32_t handle = MemRead<uint32_t>(weaponsDataPtr + i * sizeof(uint32_t));
        if (handle == 0 || handle == 0xFFFFFFFF) continue;

        uintptr_t weaponEntity = EntityFromHandle(entityListBase, handle);
        if (!weaponEntity) continue;

        uintptr_t attrMgrBase = weaponEntity + schemas::C_EconEntity::m_AttributeManager;
        uintptr_t itemViewPtr = attrMgrBase + schemas::C_AttributeContainer::m_Item;
        int defIndex = static_cast<int>(
            MemRead<uint16_t>(itemViewPtr + schemas::C_EconItemView::m_iItemDefinitionIndex));

        // Verbose per-slot logging only on tick 1 — helps diagnose entity walk
        // correctness without flooding the log at 64 Hz.
        if (tick == 1) {
            SasLog::Write("tick #%llu: slot[%d] handle=0x%08X entity=0x%llX defIndex=%d",
                tick, i, handle, (unsigned long long)weaponEntity, defIndex);
        }

        auto it = skinMap.find(defIndex);
        if (it == skinMap.end()) continue; // no skin assigned for this weapon

        const LoadoutSlot& slot = it->second;

        // Pre-write readback: read current values BEFORE overwriting them.
        // Pre-write readback diagnostic.  Compare current memory state against
        // what we wrote last time for this entity.
        //
        // Only log when values diverge (means server/prediction wiped them).
        // Silence = writes persist in memory → rendering is the bottleneck
        //           (CS2 caches material at weapon spawn, not per-frame).
        // "OVERWRITTEN" = networking/prediction is the bottleneck.
        uint32_t preIDHigh = MemRead<uint32_t>(itemViewPtr  + schemas::C_EconItemView::m_iItemIDHigh);
        int32_t  prePK     = MemRead<int32_t> (weaponEntity + schemas::C_EconEntity::m_nFallbackPaintKit);
        float    preWear   = MemRead<float>   (weaponEntity + schemas::C_EconEntity::m_flFallbackWear);

        auto lastIt = g_lastWritten.find(weaponEntity);
        if (lastIt == g_lastWritten.end()) {
            // First time seeing this entity — log initial state.
            SasLog::Write("tick #%llu: entity=0x%llX defIdx=%d first write; initial IDHigh=0x%08X pk=%d wear=%.4f",
                tick, (unsigned long long)weaponEntity, defIndex, preIDHigh, prePK, preWear);
        } else if (preIDHigh != lastIt->second.idHigh ||
                   prePK     != lastIt->second.pk     ||
                   preWear   != lastIt->second.wear) {
            SasLog::Write("tick #%llu: entity=0x%llX defIdx=%d OVERWRITTEN — got IDHigh=0x%08X pk=%d wear=%.4f, expected IDHigh=0xFFFFFFFF pk=%d",
                tick, (unsigned long long)weaponEntity, defIndex,
                preIDHigh, prePK, preWear, lastIt->second.pk);
        }

        MemWrite<uint64_t>(itemViewPtr + schemas::C_EconItemView::m_iItemIDHigh,
                           0xFFFFFFFFFFFFFFFFull);
        MemWrite<int32_t>(weaponEntity + schemas::C_EconEntity::m_nFallbackPaintKit, slot.paintKitId);
        MemWrite<float>  (weaponEntity + schemas::C_EconEntity::m_flFallbackWear,    slot.wear);

        g_lastWritten[weaponEntity] = { slot.paintKitId, slot.wear, 0xFFFFFFFFu };
        ++written;
    }

    // Heartbeat ~every 5 s so we know the loop is alive even when nothing is new.
    if (tick % 300 == 1)
        SasLog::Write("tick #%llu: alive, %d write(s) this tick", tick, written);
}

static void UpdateLoop()
{
    while (g_running.load()) {
        ApplySkins();
        // 16 ms ≈ 64 Hz — matches the CS2 server tick rate so that if the server
        // resets the fallback fields between our writes we overwrite them back
        // before the next render frame.  At 250 ms we were only "winning" 6% of
        // the time; at 16 ms we win virtually every frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
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

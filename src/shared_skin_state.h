#pragma once
#include <windows.h>

// Named file-mapping used as IPC between MuseumCurator.exe (writer)
// and SovereignHook.dll (reader, runs inside cs2.exe).
#define SKINSARESILLY_SHMEM_NAME L"Local\\SkinsAreSillyState"

struct SharedSkinState {
    int   weaponDefIndex; // 0 = no override
    int   paintKitId;
    float wear;
};

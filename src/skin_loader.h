#pragma once
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "imgui.h"
#include "nlohmann/json.hpp"
#include "skin_catalogue.h"

// ---------------------------------------------------------------------------
// Runtime skin entry — richer than the old static SkinEntry
// ---------------------------------------------------------------------------
struct DynamicSkin {
    std::string    name;            // "AK-47 | Asiimov"
    std::string    weaponId;        // "weapon_ak47"
    std::string    weaponName;      // "AK-47"
    int            weaponDefIndex = 0;
    int            paintKitId     = 0;
    float          minWear        = 0.00f;
    float          maxWear        = 1.00f;
    float          wear           = 0.07f;  // mutable; user adjusts this
    ImVec4         rarityColor    = {0.6f, 0.6f, 0.6f, 1.0f};
    std::string    rarityName;
    WeaponCategory category       = kCatAll;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline ImVec4 ParseHexColor(const std::string& hex) {
    ImVec4 c{0.6f, 0.6f, 0.6f, 1.0f};
    if (hex.size() < 7 || hex[0] != '#') return c;
    try {
        c.x = std::stoi(hex.substr(1, 2), nullptr, 16) / 255.0f;
        c.y = std::stoi(hex.substr(3, 2), nullptr, 16) / 255.0f;
        c.z = std::stoi(hex.substr(5, 2), nullptr, 16) / 255.0f;
    } catch (...) {}
    return c;
}

inline const char* WearGrade(float w) {
    if (w < 0.07f) return "FN";
    if (w < 0.15f) return "MW";
    if (w < 0.38f) return "FT";
    if (w < 0.45f) return "WW";
    return "BS";
}

// ---------------------------------------------------------------------------
// Load the ByMykel CSGO-API skins.json from disk.
// Returns an empty vector on any failure (caller should use fallback).
// ---------------------------------------------------------------------------
inline std::vector<DynamicSkin> LoadSkinsJson(const std::wstring& jsonPath) {
    std::vector<DynamicSkin> out;

    FILE* fp = _wfopen(jsonPath.c_str(), L"rb");
    if (!fp) return out;

    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize <= 0) { fclose(fp); return out; }

    std::string buf(static_cast<size_t>(fileSize), '\0');
    fread(buf.data(), 1, static_cast<size_t>(fileSize), fp);
    fclose(fp);

    nlohmann::json j;
    try { j = nlohmann::json::parse(buf); }
    catch (...) { return out; }
    if (!j.is_array()) return out;

    out.reserve(j.size());
    for (const auto& item : j) {
        try {
            if (!item.is_object()) continue;
            if (!item.contains("paint_index") || !item["paint_index"].is_number_integer()) continue;

            DynamicSkin s;
            s.name      = item.value("name", "");
            s.paintKitId = item["paint_index"].get<int>();
            if (s.name.empty() || s.paintKitId == 0) continue;

            if (item.contains("weapon") && item["weapon"].is_object()) {
                s.weaponId   = item["weapon"].value("id", "");
                s.weaponName = item["weapon"].value("name", "");
            }
            if (s.weaponId.empty()) continue;

            s.weaponDefIndex = WeaponDefIndexFromId(s.weaponId);
            if (s.weaponDefIndex == 0) continue;  // unknown / unmapped weapon

            s.minWear  = item.value("min_float", 0.00f);
            s.maxWear  = item.value("max_float", 1.00f);
            s.wear     = s.minWear;
            s.category = WeaponCategoryFromId(s.weaponId);

            if (item.contains("rarity") && item["rarity"].is_object()) {
                s.rarityName  = item["rarity"].value("name",  "");
                s.rarityColor = ParseHexColor(item["rarity"].value("color", "#999999"));
            }

            out.push_back(std::move(s));
        } catch (...) { continue; }
    }

    // Sort: weapon name, then skin name
    std::sort(out.begin(), out.end(), [](const DynamicSkin& a, const DynamicSkin& b) {
        if (a.weaponName != b.weaponName) return a.weaponName < b.weaponName;
        return a.name < b.name;
    });

    return out;
}

// ---------------------------------------------------------------------------
// Static fallback — the original curated list, used when skins.json is absent
// ---------------------------------------------------------------------------
inline std::vector<DynamicSkin> BuildStaticFallback() {
    auto make = [](const char* name, const char* wid, const char* wname,
                   int defIdx, int pk, float wear, float minW, float maxW,
                   float r, float g, float b, const char* rarity) -> DynamicSkin {
        DynamicSkin s;
        s.name           = name;
        s.weaponId       = wid;
        s.weaponName     = wname;
        s.weaponDefIndex = defIdx;
        s.paintKitId     = pk;
        s.wear           = wear;
        s.minWear        = minW;
        s.maxWear        = maxW;
        s.rarityColor    = {r, g, b, 1.0f};
        s.rarityName     = rarity;
        s.category       = WeaponCategoryFromId(wid);
        return s;
    };
    // clang-format off
    return {
        make("AWP | Dragon Lore",         "weapon_awp",             "AWP",           9, 344, 0.07f, 0.01f, 0.08f, 0.92f, 0.78f, 0.20f, "Covert"),
        make("AWP | Asiimov",             "weapon_awp",             "AWP",           9, 279, 0.18f, 0.18f, 1.00f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("AWP | Medusa",              "weapon_awp",             "AWP",           9, 345, 0.07f, 0.00f, 0.08f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("AK-47 | Fire Serpent",      "weapon_ak47",            "AK-47",         7,  27, 0.10f, 0.00f, 0.70f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("AK-47 | Asiimov",           "weapon_ak47",            "AK-47",         7, 279, 0.18f, 0.18f, 1.00f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("M4A4 | Howl",               "weapon_m4a1",            "M4A4",         16, 309, 0.07f, 0.00f, 0.44f, 0.92f, 0.37f, 0.20f, "Contraband"),
        make("M4A1-S | Asiimov",          "weapon_m4a1_silencer",   "M4A1-S",       60, 279, 0.18f, 0.18f, 1.00f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("USP-S | Kill Confirmed",    "weapon_usp_silencer",    "USP-S",        61, 345, 0.07f, 0.00f, 0.08f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("Desert Eagle | Blaze",      "weapon_deagle",          "Desert Eagle",  1,  87, 0.01f, 0.00f, 0.08f, 0.92f, 0.78f, 0.20f, "Classified"),
        make("Glock-18 | Fade",           "weapon_glock",           "Glock-18",      4,  38, 0.01f, 0.00f, 0.08f, 0.92f, 0.78f, 0.20f, "Classified"),
        make("Karambit | Doppler",        "weapon_knife_karambit",  "Karambit",    507, 420, 0.01f, 0.00f, 0.08f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("Karambit | Fade",           "weapon_knife_karambit",  "Karambit",    507,  38, 0.01f, 0.00f, 0.08f, 0.92f, 0.78f, 0.20f, "Classified"),
        make("M9 Bayonet | Doppler",      "weapon_knife_m9_bayonet","M9 Bayonet",  508, 420, 0.01f, 0.00f, 0.08f, 0.92f, 0.37f, 0.20f, "Covert"),
        make("Butterfly Knife | Fade",    "weapon_knife_butterfly", "Butterfly Knife",515,38, 0.01f, 0.00f, 0.08f, 0.92f, 0.78f, 0.20f, "Classified"),
    };
    // clang-format on
}

// ---------------------------------------------------------------------------
// Returns the directory of the running executable, with trailing backslash
// ---------------------------------------------------------------------------
inline std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    auto pos = s.rfind(L'\\');
    return (pos != std::wstring::npos) ? s.substr(0, pos + 1) : s;
}

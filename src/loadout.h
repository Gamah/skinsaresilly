#pragma once
// Per-weapon loadout: maps each weapon (by defIndex) to a skin (paintKit + wear).
// Serialised to/from loadout.json via nlohmann/json.
// No ImGui dependency — this file is safe to include from any TU.

#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include "nlohmann/json.hpp"

// One per-weapon assignment stored in the loadout.
struct LoadoutEntry {
    std::string weaponId;       // "weapon_ak47"
    std::string weaponName;     // "AK-47"
    int         weaponDefIndex = 0;
    std::string skinName;       // "AK-47 | Asiimov"
    int         paintKitId     = 0;
    float       wear           = 0.07f;
};

class Loadout {
public:
    // Load from disk. Silently starts empty if the file is missing or malformed.
    void Load(const std::wstring& path)
    {
        m_entries.clear();

        FILE* fp = _wfopen(path.c_str(), L"rb");
        if (!fp) return;

        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { fclose(fp); return; }

        std::string buf(static_cast<size_t>(sz), '\0');
        fread(buf.data(), 1, static_cast<size_t>(sz), fp);
        fclose(fp);

        try {
            auto j = nlohmann::json::parse(buf);
            if (!j.is_object()) return;
            auto& arr = j["loadout"];
            if (!arr.is_array()) return;
            for (const auto& item : arr) {
                LoadoutEntry e;
                e.weaponId       = item.value("weaponId",       "");
                e.weaponName     = item.value("weaponName",     "");
                e.weaponDefIndex = item.value("weaponDefIndex", 0);
                e.skinName       = item.value("skinName",       "");
                e.paintKitId     = item.value("paintKitId",     0);
                e.wear           = item.value("wear",           0.07f);
                if (e.weaponDefIndex == 0 || e.paintKitId == 0) continue;
                m_entries.push_back(std::move(e));
            }
        } catch (...) {
            m_entries.clear();
        }
    }

    // Write the full loadout to disk as pretty-printed JSON.
    void Save(const std::wstring& path) const
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : m_entries) {
            arr.push_back({
                {"weaponId",       e.weaponId},
                {"weaponName",     e.weaponName},
                {"weaponDefIndex", e.weaponDefIndex},
                {"skinName",       e.skinName},
                {"paintKitId",     e.paintKitId},
                {"wear",           e.wear}
            });
        }
        nlohmann::json root = {{"version", 1}, {"loadout", arr}};
        std::string out = root.dump(2);

        FILE* fp = _wfopen(path.c_str(), L"wb");
        if (!fp) return;
        fwrite(out.data(), 1, out.size(), fp);
        fclose(fp);
    }

    // Assign (or update) a skin for a weapon slot.
    void Set(const LoadoutEntry& e)
    {
        for (auto& existing : m_entries) {
            if (existing.weaponDefIndex == e.weaponDefIndex) {
                existing = e;
                return;
            }
        }
        m_entries.push_back(e);
    }

    // Remove the skin assignment for the given weapon (if any).
    void Clear(int weaponDefIndex)
    {
        m_entries.erase(
            std::remove_if(m_entries.begin(), m_entries.end(),
                [weaponDefIndex](const LoadoutEntry& e) {
                    return e.weaponDefIndex == weaponDefIndex;
                }),
            m_entries.end());
    }

    // Return the assigned skin for a weapon, or nullptr if unassigned.
    const LoadoutEntry* Get(int weaponDefIndex) const
    {
        for (const auto& e : m_entries)
            if (e.weaponDefIndex == weaponDefIndex) return &e;
        return nullptr;
    }

    const std::vector<LoadoutEntry>& Entries() const { return m_entries; }

private:
    std::vector<LoadoutEntry> m_entries;
};

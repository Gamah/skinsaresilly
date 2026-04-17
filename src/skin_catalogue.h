#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Weapon ID string -> CS2 item_definition_index  (items_game.txt)
// ---------------------------------------------------------------------------
inline int WeaponDefIndexFromId(const std::string& id) {
    static const std::unordered_map<std::string, int> kMap = {
        {"weapon_deagle",                  1},
        {"weapon_elite",                   2},
        {"weapon_fiveseven",               3},
        {"weapon_glock",                   4},
        {"weapon_ak47",                    7},
        {"weapon_aug",                     8},
        {"weapon_awp",                     9},
        {"weapon_famas",                  10},
        {"weapon_g3sg1",                  11},
        {"weapon_galilar",                13},
        {"weapon_m249",                   14},
        {"weapon_m4a1",                   16},  // M4A4
        {"weapon_mac10",                  17},
        {"weapon_p90",                    19},
        {"weapon_mp5sd",                  23},
        {"weapon_ump45",                  24},
        {"weapon_xm1014",                 25},
        {"weapon_bizon",                  26},
        {"weapon_mag7",                   27},
        {"weapon_negev",                  28},
        {"weapon_sawedoff",               29},
        {"weapon_tec9",                   30},
        {"weapon_hkp2000",                32},
        {"weapon_mp7",                    33},
        {"weapon_mp9",                    34},
        {"weapon_nova",                   35},
        {"weapon_p250",                   36},
        {"weapon_scar20",                 38},
        {"weapon_sg556",                  39},
        {"weapon_ssg08",                  40},
        {"weapon_m4a1_silencer",          60},  // M4A1-S
        {"weapon_usp_silencer",           61},  // USP-S
        {"weapon_cz75a",                  63},
        {"weapon_revolver",               64},
        {"weapon_bayonet",               500},  // Bayonet
        {"weapon_knife_flip",            505},
        {"weapon_knife_gut",             506},
        {"weapon_knife_karambit",        507},
        {"weapon_knife_m9_bayonet",      508},
        {"weapon_knife_tactical",        509},  // Huntsman
        {"weapon_knife_falchion",        512},
        {"weapon_knife_survival_bowie",  514},
        {"weapon_knife_butterfly",       515},
        {"weapon_knife_push",            516},  // Shadow Daggers
        {"weapon_knife_cord",            517},  // Paracord
        {"weapon_knife_canis",           518},  // Survival
        {"weapon_knife_ursus",           519},
        {"weapon_knife_gypsy_jackknife", 520},  // Navaja
        {"weapon_knife_outdoor",         521},  // Nomad
        {"weapon_knife_stiletto",        522},
        {"weapon_knife_widowmaker",      523},  // Talon
        {"weapon_knife_skeleton",        525},
        {"weapon_knife_kukri",           526},
    };
    auto it = kMap.find(id);
    return it != kMap.end() ? it->second : 0;
}

// ---------------------------------------------------------------------------
// Weapon categories for tab-based UI filtering
// ---------------------------------------------------------------------------
enum WeaponCategory {
    kCatAll = 0,
    kCatRifles,
    kCatPistols,
    kCatSMGs,
    kCatHeavy,
    kCatKnives,
    kCatCount
};

inline WeaponCategory WeaponCategoryFromId(const std::string& id) {
    if (id.find("knife") != std::string::npos) return kCatKnives;
    static const std::unordered_set<std::string> kRifles = {
        "weapon_ak47", "weapon_m4a1", "weapon_m4a1_silencer", "weapon_awp",
        "weapon_famas", "weapon_galilar", "weapon_aug", "weapon_sg556",
        "weapon_ssg08", "weapon_g3sg1", "weapon_scar20"
    };
    if (kRifles.count(id)) return kCatRifles;
    static const std::unordered_set<std::string> kPistols = {
        "weapon_glock", "weapon_usp_silencer", "weapon_hkp2000", "weapon_p250",
        "weapon_fiveseven", "weapon_tec9", "weapon_cz75a", "weapon_deagle",
        "weapon_revolver", "weapon_elite"
    };
    if (kPistols.count(id)) return kCatPistols;
    static const std::unordered_set<std::string> kSMGs = {
        "weapon_mac10", "weapon_mp9", "weapon_mp7", "weapon_mp5sd",
        "weapon_ump45", "weapon_bizon", "weapon_p90"
    };
    if (kSMGs.count(id)) return kCatSMGs;
    static const std::unordered_set<std::string> kHeavy = {
        "weapon_nova", "weapon_xm1014", "weapon_sawedoff", "weapon_mag7",
        "weapon_m249", "weapon_negev"
    };
    if (kHeavy.count(id)) return kCatHeavy;
    return kCatAll;  // unknown weapon — show under All but no specific tab
}

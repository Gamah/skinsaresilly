#pragma once

// ---------------------------------------------------------------------------
// CS2 skin catalogue
//
// weaponDefIndex — CS2 item_definition_index for the weapon type.
//   Source: Valve items_game.txt (canonical, stable across patches).
//
// paintKitId — CS2 paint kit ID written to C_EconEntity::m_nFallbackPaintKit.
//   Source: Valve items_game.txt, community-verified via a2x/cs2-dumper.
//
// wear — float in [0.0, 1.0]; written to C_EconEntity::m_flFallbackWear.
//   0.00–0.07 = Factory New, 0.07–0.15 = Minimal Wear, etc.
// ---------------------------------------------------------------------------

struct SkinEntry {
    const char* name;
    int         weaponDefIndex; // 0 = no override
    int         paintKitId;     // 0 = stock (no paint)
    float       wear;
};

static const SkinEntry k_skins[] = {
    // defIdx  paintKit  wear
    { "-- None (Default) --",    0,    0,   0.00f },

    // AWP (defIndex 9)
    { "AWP | Dragon Lore",       9,  344,   0.07f },
    { "AWP | Asiimov",           9,  279,   0.15f },
    { "AWP | Medusa",            9,  345,   0.07f },

    // AK-47 (defIndex 7)
    { "AK-47 | Fire Serpent",    7,   27,   0.10f },
    { "AK-47 | Asiimov",         7,  279,   0.15f },

    // M4A4 (defIndex 16)
    { "M4A4 | Howl",            16,  309,   0.07f },

    // M4A1-S (defIndex 60)
    { "M4A1-S | Asiimov",       60,  279,   0.15f },

    // USP-S (defIndex 61)
    { "USP-S | Kill Confirmed", 61,  345,   0.07f },

    // Desert Eagle (defIndex 1)
    { "Desert Eagle | Blaze",    1,   87,   0.01f },

    // Glock-18 (defIndex 4)
    { "Glock-18 | Fade",         4,   38,   0.01f },

    // Karambit (defIndex 507)
    { "Karambit | Doppler",    507,  420,   0.01f },
    { "Karambit | Fade",       507,   38,   0.01f },

    // M9 Bayonet (defIndex 508)
    { "M9 Bayonet | Doppler",  508,  420,   0.01f },

    // Butterfly Knife (defIndex 515)
    { "Butterfly Knife | Fade",515,   38,   0.01f },
};

static const int k_skinCount = static_cast<int>(sizeof(k_skins) / sizeof(k_skins[0]));

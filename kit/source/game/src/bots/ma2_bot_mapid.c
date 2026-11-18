/* Identifies the multiplayer map that is loading, for bot navigation.
 *
 * Same signal as ma2_cinematic.c: every level load opens its own
 * \Device\Harddisk0\Partition5\XXXXXXXX.cache, named ~CRC32 of the lower-case
 * "d:\<archive>" path. Unlike the level archive itself, this open always
 * reaches g_xbox_path_hook (the archive can be opened relative to a directory
 * handle). Hashes below were computed with that formula and checked against
 * the known values for cinema_2_1 (52EDEEE3), mission_04 (1EEF6E28) and the
 * front-end lobby (46229B6E).
 *
 * Chains the existing hook, so ma2_cinematic.c keeps working whichever
 * initializer runs first. Any other level cache clears the current map. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>

extern void (*g_xbox_path_hook)(const char *xbox_path);
extern void nv2a_shader_level_begin(uint32_t level);   /* kernel/nv2a_shader_cache.c */
extern void nv2a_texture_pack_level_begin(void);      /* kernel/nv2a_texture_pack.cpp: hold the first frames */
static void (*previous_hook)(const char *xbox_path);

typedef struct Ma2MapInfo {
    uint32_t    cache;      /* ~CRC32("d:\\<archive>.mgf") */
    const char *archive;    /* game_files\<archive> */
    const char *nav;        /* shipped 'Mech graph inside the archive, or NULL */
} Ma2MapInfo;

static const Ma2MapInfo maps[] = {
    {0x389B4EBEu, "boggeddown.mgf",    NULL},
    {0x9855A39Eu, "deliverance.mgf",   NULL},
    {0xEC3C5013u, "gocommando.mgf",    NULL},
    {0x8D0FB316u, "illwind.mgf",       NULL},
    {0xF026C4DEu, "inyourface.mgf",    "PATHFINDING/InYourFace_Mech.map"},
    {0x5CF1EEABu, "killingfield.mgf",  NULL},
    {0xC44F7405u, "newrivercity.mgf",  "PATHFINDING/NewRiverCity_Mech.map"},
    {0xCD1E02E4u, "nomansland.mgf",    NULL},
    {0x13FDF0A0u, "ontherocks.mgf",    NULL},
    {0x594C25AAu, "overfalls.mgf",     NULL},
    {0xD27209A4u, "provingground.mgf", NULL},
    {0x9FFC216Cu, "rushhour.mgf",      "Pathfinding/Mission16_Mech.map"},
    {0x2CB181A6u, "undercover.mgf",    NULL},
    {0xC36F38BEu, "witchcauldron.mgf", "PATHFINDING/Witch_Mech.map"},
};

static volatile int current = -1;   /* index into maps, -1 = not a MP map */

static int starts_with(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

static int level_cache(const char *name, uint32_t *hash)
{
    uint32_t value = 0;
    for (int i = 0; i < 8; i++) {
        int c = tolower((unsigned char)name[i]);
        if (c >= '0' && c <= '9') value = value * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') value = value * 16 + (uint32_t)(c - 'a' + 10);
        else return 0;
    }
    if (!starts_with(name + 8, ".cache") || name[14]) return 0;
    *hash = value;
    return 1;
}

static void ma2_bot_map_path(const char *path)
{
    const char *name = strrchr(path, '\\');
    uint32_t hash;
    name = name ? name + 1 : path;
    if (starts_with(path, "\\Device\\Harddisk0\\Partition5\\") && level_cache(name, &hash)) {
        int found = -1;
        nv2a_shader_level_begin(hash);   /* every level: its shader list (shader preloading) */
        nv2a_texture_pack_level_begin();   /* its first frames wait for HD textures and shaders */
        for (size_t i = 0; i < sizeof maps / sizeof maps[0]; i++)
            if (maps[i].cache == hash) { found = (int)i; break; }
        current = found;
    }
    if (previous_hook) previous_hook(path);
}

/* Map being loaded/played: archive file name (e.g. "inyourface.mgf") or NULL. */
const char *ma2_bot_map_archive(void)
{
    int i = current;
    return i >= 0 ? maps[i].archive : NULL;
}

/* Shipped 'Mech nav graph path inside that archive, or NULL (generate one). */
const char *ma2_bot_map_nav(void)
{
    int i = current;
    return i >= 0 ? maps[i].nav : NULL;
}

static void __cdecl ma2_bot_map_install(void)
{
    previous_hook = g_xbox_path_hook;
    g_xbox_path_hook = ma2_bot_map_path;
}

#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (__cdecl *ma2_bot_map_initializer)(void) = ma2_bot_map_install;

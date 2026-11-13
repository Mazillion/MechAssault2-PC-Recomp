/* Small startup/settings policies shared by main.c, pc_native_settings.cpp and pc_prompts.cpp.
 * Header-only (static inline, C and C++) so a console test can exercise the logic without the game. */
#ifndef MA2_STARTUP_POLICY_H
#define MA2_STARTUP_POLICY_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single instance (easy-wins B1.1). Returns 1 when this process owns the lock (the handle is kept
 * open until exit), 0 when another process holds it. Runs that set RECOMP_SAVE_DIR (the MCP harness,
 * the editor's Test) or RECOMP_ALLOW_MULTI use their own saves and are exempt: always 1. */
static inline int ma2_single_instance_acquire(const wchar_t *name, HANDLE *out)
{
    HANDLE h;
    if (out) *out = NULL;
    if (getenv("RECOMP_SAVE_DIR") || getenv("RECOMP_ALLOW_MULTI")) return 1;
    h = CreateMutexW(NULL, FALSE, name);
    if (!h) return 1;                              /* cannot tell: never block the game */
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); return 0; }
    if (out) *out = h;
    return 1;
}

/* KeyboardLayout ini value: only "legacy" is a second layout, anything else is "pc" (B1.6). */
static inline void ma2_normalise_layout(char *layout)
{
    if (_stricmp(layout, "legacy")) strcpy(layout, "pc");
}

/* Scan code to use with GetKeyNameTextW for a virtual key (B1.12). Extended keys (Insert, Delete,
 * Home, End, Page Up/Down, arrows, right Ctrl/Alt, numpad Enter/Divide) carry the 0x100 bit. */
static inline unsigned ma2_key_name_scan(int vk)
{
    unsigned scan = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
    if ((scan & 0xFF00u) == 0xE000u) scan = (scan & 0xFFu) | 0x100u;
    else scan &= 0xFFu;
    /* MapVirtualKey reports no E0 prefix for keys that share a virtual key with the numpad (Insert, Delete,
     * the arrows block), so list the extended keys explicitly. */
    if ((vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_SNAPSHOT || vk == VK_DIVIDE ||
        vk == VK_NUMLOCK || vk == VK_RCONTROL || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS)
        scan |= 0x100u;
    return scan;
}

/* Layout-aware default key for a physical key (B1.13): the virtual key the current layout gives
 * this scan code (W is on the AZERTY Z key, and so on). Falls back to the QWERTY virtual key. */
typedef unsigned (*ma2_scan_mapper)(unsigned scan);
static inline unsigned ma2_scan_to_vk_system(unsigned scan) { return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK); }
static inline int ma2_default_key_from_scan(unsigned scan, int fallback, ma2_scan_mapper mapper)
{
    unsigned vk = (mapper ? mapper : ma2_scan_to_vk_system)(scan);
    return vk > 0 && vk < 256 ? (int)vk : fallback;
}

/* Dump housekeeping (W-08): crash-<pid>.dmp and hang-<pid>.dmp, newest `keep` by write time.
 * Returns how many were deleted. */
typedef struct { char name[64]; FILETIME t; } Ma2DumpFile;
static inline int ma2_dump_newer_first(const void *a, const void *b)
{
    return -CompareFileTime(&((const Ma2DumpFile *)a)->t, &((const Ma2DumpFile *)b)->t);
}
static inline int ma2_prune_dumps(const char *dir, int keep)
{
    static const char *const kinds[2] = { "crash-*.dmp", "hang-*.dmp" };
    Ma2DumpFile files[256];
    char pattern[MAX_PATH], path[MAX_PATH];
    int n = 0, deleted = 0, i, k;
    WIN32_FIND_DATAA f;
    HANDLE h;
    for (k = 0; k < 2; k++) {
        if (snprintf(pattern, sizeof(pattern), "%s\\%s", dir, kinds[k]) >= (int)sizeof(pattern)) return 0;
        h = FindFirstFileA(pattern, &f);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strlen(f.cFileName) >= sizeof(files[0].name)) continue;
            if (n < (int)(sizeof(files) / sizeof(files[0]))) {
                strcpy(files[n].name, f.cFileName);
                files[n].t = f.ftLastWriteTime;
                n++;
            }
        } while (FindNextFileA(h, &f));
        FindClose(h);
    }
    if (n <= keep) return 0;
    qsort(files, (size_t)n, sizeof(files[0]), ma2_dump_newer_first);
    for (i = keep; i < n; i++) {
        snprintf(path, sizeof(path), "%s\\%s", dir, files[i].name);
        if (DeleteFileA(path)) deleted++;
    }
    return deleted;
}

/* Log folder housekeeping (D11). A game log is YYYYMMDD-HHMMSS-<pid>.log; names sort by time. */
static inline int ma2_is_game_log_name(const char *n)
{
    int i;
    for (i = 0; i < 8; i++) if (n[i] < '0' || n[i] > '9') return 0;
    if (n[8] != '-') return 0;
    for (i = 9; i < 15; i++) if (n[i] < '0' || n[i] > '9') return 0;
    if (n[15] != '-') return 0;
    for (i = 16; n[i] >= '0' && n[i] <= '9'; i++) {}
    return i > 16 && !strcmp(n + i, ".log");
}
static inline int ma2_log_name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
/* Deletes the oldest game logs so that at most `keep` remain. Returns how many were deleted. */
static inline int ma2_prune_logs(const char *dir, int keep)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    char (*names)[48];
    int n = 0, cap = 512, deleted = 0, i;
    WIN32_FIND_DATAA f;
    HANDLE h;
    if (keep < 0 || strlen(dir) + 8 >= sizeof(pattern)) return 0;
    names = (char (*)[48])malloc(sizeof(char[48]) * (size_t)cap);
    if (!names) return 0;
    _snprintf(pattern, sizeof(pattern), "%s\\*.log", dir);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strlen(f.cFileName) < 48 &&
                ma2_is_game_log_name(f.cFileName) && n < cap)
                strcpy(names[n++], f.cFileName);
        } while (FindNextFileA(h, &f));
        FindClose(h);
    }
    qsort(names, (size_t)n, 48, ma2_log_name_cmp);
    for (i = 0; i < n - keep; i++) {
        _snprintf(path, sizeof(path), "%s\\%s", dir, names[i]);
        path[sizeof(path) - 1] = 0;
        if (DeleteFileA(path)) deleted++;
    }
    free(names);
    return deleted;
}

#ifdef __cplusplus
}
#endif
#endif

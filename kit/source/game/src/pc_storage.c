#define COBJMACROS
#include "pc_storage.h"
#include "pc_firstrun.h"
#include "platform/pc_cpu_topo.h"
#include <windows.h>
#include <shlobj.h>
#include <winioctl.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);

extern void xbox_path_init(const char *, const char *);

static int create_directory(const char *path)
{
    int result = SHCreateDirectoryExA(NULL, path, NULL);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

static int copy_directory(const char *source, const char *destination)
{
    WIN32_FIND_DATAA entry;
    char pattern[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    int copied = 0;
    if (!create_directory(destination) ||
        snprintf(pattern, sizeof(pattern), "%s\\*", source) >= sizeof(pattern)) return -1;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    do {
        if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..") ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (snprintf(from, sizeof(from), "%s\\%s", source, entry.cFileName) >= sizeof(from) ||
            snprintf(to, sizeof(to), "%s\\%s", destination, entry.cFileName) >= sizeof(to)) {
            copied = -1; break;
        }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int count = copy_directory(from, to);
            if (count < 0) { copied = -1; break; }
            copied += count;
        } else if (CopyFileA(from, to, TRUE)) {
            copied++;
        } else if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            fprintf(stderr, "[PC-STORAGE] Cannot migrate %s (Windows error %lu)\n", from, GetLastError());
            copied = -1; break;
        }
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    return copied;
}

int ma2_pc_copy_missing(const char *source, const char *destination)
{
    char absolute_source[MAX_PATH], absolute_destination[MAX_PATH];
    DWORD attributes = GetFileAttributesA(source);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    DWORD source_length = GetFullPathNameA(source, MAX_PATH, absolute_source, NULL);
    DWORD destination_length = GetFullPathNameA(destination, MAX_PATH, absolute_destination, NULL);
    if (!source_length || source_length >= MAX_PATH ||
        !destination_length || destination_length >= MAX_PATH) return -1;
    /* A destination inside the source would make recursive copying unbounded. */
    if (!_strnicmp(absolute_source, absolute_destination, source_length) &&
        (!absolute_destination[source_length] || absolute_destination[source_length] == '\\')) return -1;
    return copy_directory(absolute_source, absolute_destination);
}

/* ---- FT-RT: automatic first-run defaults (R8-LOWEND-PERF). Applied only when this process has just
 * created the per-user pc_settings.ini from the packaged defaults; an existing file is never read or
 * changed. The decision table is in pc_firstrun.h (unit-tested). RECOMP_FIRSTRUN=0 turns it off;
 * RECOMP_FIRSTRUN_HW="vram=2048,ram=8192,cpus=8,battery=1,disk=hdd" replaces the detected hardware (test). ---- */

static const GUID k_iid_factory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID k_iid_factory6 = { 0xc1b6694f, 0xff09, 0x44a9, { 0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17 } };
static const GUID k_iid_adapter1 = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };

/* The adapter the game will pick: Windows' high-performance one (as nv2a_gpu_d3d11.c), else the
 * software-free adapter with the most dedicated memory. dxgi.dll is loaded dynamically so this file
 * links without it. */
static int detect_gpu(unsigned *vram_mb)
{
    typedef HRESULT (WINAPI *create_factory_fn)(REFIID, void **);
    int known = 0;
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    create_factory_fn create = dxgi ? (create_factory_fn)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *factory = NULL;
    *vram_mb = 0;
    if (create && SUCCEEDED(create(&k_iid_factory1, (void **)&factory)) && factory) {
        IDXGIFactory6 *preferred = NULL;
        SIZE_T best = 0;
        IDXGIFactory1_QueryInterface(factory, &k_iid_factory6, (void **)&preferred);
        for (UINT i = 0; i < 16; i++) {
            IDXGIAdapter1 *adapter = NULL;
            DXGI_ADAPTER_DESC1 d;
            HRESULT hr = preferred
                ? IDXGIFactory6_EnumAdapterByGpuPreference(preferred, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           &k_iid_adapter1, (void **)&adapter)
                : IDXGIFactory1_EnumAdapters1(factory, i, &adapter);
            if (FAILED(hr) || !adapter) break;
            memset(&d, 0, sizeof(d));
            hr = IDXGIAdapter1_GetDesc1(adapter, &d);
            IDXGIAdapter1_Release(adapter);
            if (FAILED(hr) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            if (preferred) { *vram_mb = (unsigned)(d.DedicatedVideoMemory >> 20); known = 1; break; }
            if (!known || d.DedicatedVideoMemory > best) {
                best = d.DedicatedVideoMemory;
                *vram_mb = (unsigned)(best >> 20);
                known = 1;
            }
        }
        if (preferred) IDXGIFactory6_Release(preferred);
        IDXGIFactory1_Release(factory);
    }
    return known;
}

/* Seek penalty of the volume that holds the game (an HDD has one; SSD and NVMe do not).
 * Unknown on any failure. */
static int detect_disk(const char *path_on_game_volume)
{
    char full[MAX_PATH], device[16];
    HANDLE volume;
    STORAGE_PROPERTY_QUERY query;
    DEVICE_SEEK_PENALTY_DESCRIPTOR penalty;
    DWORD returned = 0;
    int result = PC_DISK_UNKNOWN;
    DWORD n = GetFullPathNameA(path_on_game_volume, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH || full[1] != ':') return PC_DISK_UNKNOWN;
    snprintf(device, sizeof(device), "\\\\.\\%c:", full[0]);
    volume = CreateFileA(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (volume == INVALID_HANDLE_VALUE) return PC_DISK_UNKNOWN;
    memset(&query, 0, sizeof(query));
    memset(&penalty, 0, sizeof(penalty));
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(volume, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &penalty, sizeof(penalty),
                        &returned, NULL) && returned >= sizeof(penalty))
        result = penalty.IncursSeekPenalty ? PC_DISK_HDD : PC_DISK_SSD;
    CloseHandle(volume);
    return result;
}

static void hardware_override(PcHardware *hw, const char *spec)
{
    char copy[160], *token, *context = NULL;
    snprintf(copy, sizeof(copy), "%s", spec);
    for (token = strtok_s(copy, ",; ", &context); token; token = strtok_s(NULL, ",; ", &context)) {
        char *equals = strchr(token, '=');
        if (!equals) continue;
        *equals++ = 0;
        if (!_stricmp(token, "vram")) { hw->vram_mb = (unsigned)atoi(equals); hw->gpu_known = 1; }
        else if (!_stricmp(token, "novram")) hw->gpu_known = 0;
        else if (!_stricmp(token, "ram")) hw->ram_mb = (unsigned)atoi(equals);
        else if (!_stricmp(token, "cpus")) hw->logical = (unsigned)atoi(equals);
        else if (!_stricmp(token, "battery")) hw->battery = atoi(equals) != 0;
        else if (!_stricmp(token, "desktop")) {   /* desktop=2560x1440 */
            unsigned w = 0, h = 0;
            if (sscanf(equals, "%ux%u", &w, &h) == 2) { hw->desktop_w = w; hw->desktop_h = h; }
        }
        else if (!_stricmp(token, "disk"))
            hw->disk = !_stricmp(equals, "hdd") ? PC_DISK_HDD : !_stricmp(equals, "ssd") ? PC_DISK_SSD : PC_DISK_UNKNOWN;
    }
}

static void first_run_write(const char *ini, const char *section, const char *key, const char *value)
{
    if (!WritePrivateProfileStringA(section, key, value, ini))
        fprintf(stderr, "[FIRST-RUN] cannot write %s/%s (Windows error %lu)\n", section, key, GetLastError());
}

/* `defaults` is the packaged INI next to the game: its volume is the game volume. */
static void apply_first_run_defaults(const char *ini, const char *defaults)
{
    PcHardware hw;
    PcFirstRun choice;
    MEMORYSTATUSEX memory;
    SYSTEM_POWER_STATUS power;
    const char *spec = getenv("RECOMP_FIRSTRUN_HW"), *off = getenv("RECOMP_FIRSTRUN");
    char text[24];
    if (off && !strcmp(off, "0")) return;
    memset(&hw, 0, sizeof(hw));
    hw.gpu_known = detect_gpu(&hw.vram_mb);
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) hw.ram_mb = (unsigned)(memory.ullTotalPhys >> 20);
    hw.logical = pc_cpu_logical();
    if (GetSystemPowerStatus(&power)) hw.battery = power.BatteryFlag != 128 && power.BatteryFlag != 255;
    hw.disk = detect_disk(defaults);
    hw.desktop_w = (unsigned)GetSystemMetrics(SM_CXSCREEN);
    hw.desktop_h = (unsigned)GetSystemMetrics(SM_CYSCREEN);
    if (spec && *spec) hardware_override(&hw, spec);
    pc_firstrun_decide(&hw, &choice);
    { char shown[MAX_PATH * 2]; fprintf(stderr, "[FIRST-RUN] new settings file %s\n", xbox_log_path(ini, shown, sizeof shown)); }
    fprintf(stderr, "[FIRST-RUN] hardware: gpu=%s vram=%u MB ram=%u MB cpus=%u battery=%d disk=%s%s\n",
            choice.gpu_class, hw.vram_mb, hw.ram_mb, hw.logical, hw.battery,
            hw.disk == PC_DISK_SSD ? "ssd" : hw.disk == PC_DISK_HDD ? "hdd" : "unknown",
            spec && *spec ? " (RECOMP_FIRSTRUN_HW)" : "");
    if (choice.texture_quality > 0) {
        /* W-03: never pick a texture pack that is not installed (same test as the Video page). */
        const char *root = getenv("RECOMP_TEXTURE_PACK_DIR");
        int tq = choice.texture_quality;
        while (tq > 0) {
            char pack[MAX_PATH];
            snprintf(pack, sizeof(pack), "%s\\%dx\\index.json", root && *root ? root : "game_files\\texture_packs", tq == 1 ? 2 : 4);
            if (GetFileAttributesA(pack) != INVALID_FILE_ATTRIBUTES) break;
            tq--;
        }
        if (tq != choice.texture_quality) {
            fprintf(stderr, "[FIRST-RUN] TextureQuality %d -> %d (clamped: pack not installed)\n", choice.texture_quality, tq);
            choice.texture_quality = tq;
        }
    }
    if (choice.fullscreen >= 0) {
        snprintf(text, sizeof(text), "%d", choice.fullscreen);
        first_run_write(ini, "Display", "Fullscreen", text);
        snprintf(text, sizeof(text), "%d", choice.window_w);
        first_run_write(ini, "Display", "WindowWidth", text);
        snprintf(text, sizeof(text), "%d", choice.window_h);
        first_run_write(ini, "Display", "WindowHeight", text);
    }
    if (choice.texture_quality >= 0) {
        snprintf(text, sizeof(text), "%d", choice.texture_quality);
        first_run_write(ini, "Display", "TextureQuality", text);
    }
    if (choice.frame_limit >= 0) {
        snprintf(text, sizeof(text), "%d", choice.frame_limit);
        first_run_write(ini, "Display", "FrameLimit", text);
    }
    if (choice.preload_shaders >= 0) {
        snprintf(text, sizeof(text), "%d", choice.preload_shaders);
        first_run_write(ini, "Display", "PreloadShaders", text);
    }
    if (choice.audio_threads) first_run_write(ini, "Audio", "Threads", choice.audio_threads);
    fprintf(stderr, "[FIRST-RUN] chose TextureQuality=%d FrameLimit=%d PreloadShaders=%d Audio.Threads=%s "
            "Fullscreen=%d Window=%dx%d (-1 = packaged default kept)\n", choice.texture_quality, choice.frame_limit,
            choice.preload_shaders, choice.audio_threads ? choice.audio_threads : "(auto)",
            choice.fullscreen, choice.window_w, choice.window_h);
}

int ma2_pc_settings_path(const char *defaults, char *path, size_t capacity)
{
    const char *override = getenv("RECOMP_PC_SETTINGS_INI");
    if (override && *override) {
        int fits = snprintf(path, capacity, "%s", override) < (int)capacity;
        /* Test only: RECOMP_FIRSTRUN_TEST=1 creates a missing override file from the defaults and runs
         * the first-run detector on it. */
        const char *test = getenv("RECOMP_FIRSTRUN_TEST");
        if (fits && test && !strcmp(test, "1") && GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES &&
            CopyFileA(defaults, path, TRUE)) {
            SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
            apply_first_run_defaults(path, defaults);
        }
        return fits;
    }
    char root[MAX_PATH], directory[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, root)) ||
        snprintf(directory, sizeof(directory), "%s\\MechAssault2", root) >= sizeof(directory) ||
        !create_directory(directory) ||
        snprintf(path, capacity, "%s\\pc_settings.ini", directory) >= (int)capacity) return 0;
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    if (CopyFileA(defaults, path, TRUE)) {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);   /* a read-only packaged copy must stay writable */
        apply_first_run_defaults(path, defaults);          /* FT-RT: the settings file is created HERE, once */
        return 1;
    }
    if (GetFileAttributesA(defaults) == INVALID_FILE_ATTRIBUTES) return 1; /* built-in defaults */
    fprintf(stderr, "[PC-STORAGE] Cannot initialize settings (Windows error %lu)\n", GetLastError());
    return 0;
}

/* ---- FT-RT: stale cache namespaces (R12 P2-3 / open question 3). ----
 * Each pc_assets build has its own Cache\PC2-<hash> folder (RECOMP_CACHE_NAMESPACE, main.c from
 * cache.version); nothing ever removed the old ones (250-465 MB each).
 *
 * Why this cannot lose anything the build can read: the namespace is chosen once at startup and every
 * Cache access (X:, Y:, Z:, \Device\Harddisk0\Partition3-5) is mapped into Cache\<current namespace>
 * (kernel_path.c); no code reads another namespace. The mission checkpoint Z:\Missions\MASaveGame.wdf
 * therefore lives only in the namespace that was current when it was written, and a build with a
 * different namespace never sees it, with or without this cleanup. Saves are in UserData\ and
 * TitleData\, which this code never touches.
 *
 * Rule: only plain directories named PC2-* directly under <save>\Cache, never the current namespace,
 * newest `keep` (by last use) are kept, the rest are removed. Reparse points are never followed. */

static int prune_path_join(char *out, size_t capacity, const char *a, const char *b)
{
    return snprintf(out, capacity, "%s\\%s", a, b) < (int)capacity;
}

/* Deletes a directory tree without following reparse points (a link is removed, its target is not).
 * Returns the number of files removed, -1 on failure. */
static int prune_remove_tree(const char *directory)
{
    WIN32_FIND_DATAA entry;
    char pattern[MAX_PATH], child[MAX_PATH];
    int removed = 0, failed = 0;
    HANDLE search;
    if (!prune_path_join(pattern, sizeof(pattern), directory, "*")) return -1;
    search = FindFirstFileA(pattern, &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, "..")) continue;
            if (!prune_path_join(child, sizeof(child), directory, entry.cFileName)) { failed = 1; continue; }
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) failed |= !RemoveDirectoryA(child);
                else failed |= !DeleteFileA(child);
            } else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                int count = prune_remove_tree(child);
                if (count < 0) failed = 1; else removed += count;
            } else {
                if (entry.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
                if (DeleteFileA(child)) removed++; else failed = 1;
            }
        } while (FindNextFileA(search, &entry));
        FindClose(search);
    }
    if (!RemoveDirectoryA(directory)) failed = 1;
    return failed ? -1 : removed;
}

static unsigned long long prune_file_time(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    ULARGE_INTEGER t;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
    t.LowPart = data.ftLastWriteTime.dwLowDateTime;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

typedef struct PruneCandidate {
    char name[64];
    unsigned long long used;   /* newest of the folder and its Missions\MASaveGame.wdf */
} PruneCandidate;

int ma2_pc_prune_cache_namespaces(const char *cache_directory, const char *current_namespace, int keep)
{
    WIN32_FIND_DATAA entry;
    char pattern[MAX_PATH], path[MAX_PATH], checkpoint[MAX_PATH];
    PruneCandidate list[64];
    int count = 0, removed_folders = 0;
    HANDLE search;
    if (!cache_directory || !current_namespace || !*current_namespace || keep < 0) return 0;
    if (!prune_path_join(pattern, sizeof(pattern), cache_directory, "PC2-*")) return 0;
    search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) return 0;
    do {
        unsigned long long used;
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;      /* plain folders only */
        if (_strnicmp(entry.cFileName, "PC2-", 4) || !_stricmp(entry.cFileName, current_namespace)) continue;
        if (strlen(entry.cFileName) >= sizeof(list[0].name) || count >= 64) continue;
        if (!prune_path_join(path, sizeof(path), cache_directory, entry.cFileName)) continue;
        used = prune_file_time(path);
        if (snprintf(checkpoint, sizeof(checkpoint), "%s\\Missions\\MASaveGame.wdf", path) < (int)sizeof(checkpoint)) {
            unsigned long long wdf = prune_file_time(checkpoint);
            if (wdf > used) used = wdf;
        }
        snprintf(list[count].name, sizeof(list[count].name), "%s", entry.cFileName);
        list[count].used = used;
        count++;
    } while (FindNextFileA(search, &entry));
    FindClose(search);
    /* Newest first (insertion sort; at most 64). */
    for (int i = 1; i < count; i++) {
        PruneCandidate moving = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].used < moving.used) { list[j + 1] = list[j]; j--; }
        list[j + 1] = moving;
    }
    fprintf(stderr, "[CACHE-PRUNE] current namespace %s; %d older namespace(s) found, keeping the newest %d\n",
            current_namespace, count, keep);
    for (int i = 0; i < count; i++) {
        if (i < keep) {
            fprintf(stderr, "[CACHE-PRUNE] keep %s\n", list[i].name);
            continue;
        }
        if (!prune_path_join(path, sizeof(path), cache_directory, list[i].name)) continue;
        int files = prune_remove_tree(path);
        if (files >= 0) {
            removed_folders++;
            fprintf(stderr, "[CACHE-PRUNE] removed %s (%d files)\n", list[i].name, files);
        } else {
            fprintf(stderr, "[CACHE-PRUNE] could not fully remove %s (Windows error %lu); left as is\n",
                    list[i].name, GetLastError());
        }
    }
    return removed_folders;
}

static void prune_stale_namespaces(const char *save_directory, int explicit_save_directory)
{
    const char *current = getenv("RECOMP_CACHE_NAMESPACE"), *setting = getenv("RECOMP_CACHE_PRUNE");
    char cache[MAX_PATH];
    if (!current || !*current) return;                                     /* no namespace: caches sit in Cache\ itself */
    if (setting && !strcmp(setting, "0")) return;
    if (explicit_save_directory && !(setting && !strcmp(setting, "1"))) return;   /* harness folders: opt in only */
    if (!prune_path_join(cache, sizeof(cache), save_directory, "Cache")) return;
    ma2_pc_prune_cache_namespaces(cache, current, 2);
}

void ma2_pc_storage_initialize(const char *game_directory)
{
    const char *override = getenv("RECOMP_SAVE_DIR");
    if (override && *override) {
        xbox_path_init(game_directory, override);
        { char shown[MAX_PATH * 2]; fprintf(stderr, "[PC-STORAGE] Diagnostic save directory: %s\n", xbox_log_path(override, shown, sizeof shown)); }
        prune_stale_namespaces(override, 1);
        return;
    }
    char root[MAX_PATH], save_directory[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, root)) ||
        snprintf(save_directory, sizeof(save_directory), "%s\\MechAssault2\\Saved Games", root) >= sizeof(save_directory)) {
        xbox_path_init(game_directory, NULL);
        return;
    }
    xbox_path_init(game_directory, save_directory);
    const char *legacy_names[] = { "UDATA", "TDATA" };
    const char *pc_names[] = { "UserData", "TitleData" };
    for (int i = 0; i < 2; i++) {
        char destination[MAX_PATH], source[MAX_PATH];
        snprintf(destination, sizeof(destination), "%s\\%s\\4d53006b", save_directory, pc_names[i]);
        snprintf(source, sizeof(source), "%s\\%s\\4d53006b", game_directory, legacy_names[i]);
        int copied = ma2_pc_copy_missing(source, destination);
        if (copied) fprintf(stderr, "[PC-STORAGE] Legacy %s migration: %d files\n", legacy_names[i], copied);
        snprintf(source, sizeof(source), "%s\\xboxrecomp\\%s\\4d53006b", root, pc_names[i]);
        copied = ma2_pc_copy_missing(source, destination);
        if (copied) fprintf(stderr, "[PC-STORAGE] Previous runtime %s migration: %d files\n", pc_names[i], copied);
    }
    { char shown[MAX_PATH * 2]; fprintf(stderr, "[PC-STORAGE] Saves: %s\n", xbox_log_path(save_directory, shown, sizeof shown)); }
    prune_stale_namespaces(save_directory, 0);
}

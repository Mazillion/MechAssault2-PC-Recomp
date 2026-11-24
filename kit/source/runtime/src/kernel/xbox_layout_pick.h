/* Choosing where the guest address space goes when the fixed bases fail: Wine / Proton only.
 *
 * xbox_MemoryLayoutInit maps guest RAM at host 0x10000 when it can (Windows and Wine 9 can), and then at a few fixed
 * fallbacks, the last one 256 MB. Under Proton 11 (Wine 11) the low range is taken, the 256 MB base is used, and two
 * things collide: the 28 RAM mirrors run into Wine's TEB/PEB reservation at 0x7FDE0000, and the tiled aperture
 * (guest 0xF0000000 = host 0x100000000) lands on a 1 MB block Wine 11 keeps at exactly 0x100000000. The layout then
 * fails ("memory layout failed", error 487) and the game exits before its first frame.
 *
 * Under Wine, before trying 256 MB, xbox_layout_scan_base() looks at what is really mapped (VirtualQuery) and picks
 * the lowest 64 KB-aligned base for which every window the layout maps is free: base + RAM + mirrors, the contiguous
 * window (guest 0x80000000), the tiled aperture (0xF0000000), NV2A (0xFD000000), MCPX (0xFE800000) and flash
 * (0xFF000000). Windows never runs this: xbox_layout_under_wine() is false there, and the fixed bases are tried
 * exactly as before. xbox_layout_pick_base() is the pure part (tests/game/layout_pick_test.c).
 */
#ifndef XBOX_LAYOUT_PICK_H
#define XBOX_LAYOUT_PICK_H
#include <stddef.h>
#include <stdint.h>

typedef struct { uint64_t lo, hi; } XboxBusyRange;       /* [lo, hi) host addresses that are not free */

#define XBOX_PICK_ALIGN   0x10000ull                     /* allocation granularity */
#define XBOX_PICK_MIN     0x10000ull
#define XBOX_PICK_MAX     0x40000000ull                  /* bases above 1 GB push the mirrors into Wine's TEB area */

/* The guest windows the layout maps, as [offset, offset + length) from the base. */
static inline size_t xbox_layout_windows(uint64_t map_size, int mirrors, XboxBusyRange *out)
{
    uint64_t ram = map_size * (uint64_t)(1 + mirrors);
    if (ram > 0x80000000ull) ram = 0x80000000ull;        /* mirrors at or past the contiguous window are skipped anyway */
    out[0].lo = 0;           out[0].hi = ram;
    out[1].lo = 0x80000000u; out[1].hi = 0x80000000ull + 64ull * 1024 * 1024;   /* contiguous window (kernel page inside) */
    out[2].lo = 0xF0000000u; out[2].hi = 0xF0000000ull + 64ull * 1024 * 1024;   /* tiled aperture, at most 64 MB */
    out[3].lo = 0xFD000000u; out[3].hi = 0xFD000000ull + 16ull * 1024 * 1024;   /* NV2A */
    out[4].lo = 0xFE800000u; out[4].hi = 0xFE800000ull + 8ull * 1024 * 1024;    /* MCPX */
    out[5].lo = 0xFF000000u; out[5].hi = 0xFF000000ull + 1ull * 1024 * 1024;    /* flash */
    return 6;
}

static inline int xbox_layout_overlaps(const XboxBusyRange *busy, size_t n, uint64_t lo, uint64_t hi, uint64_t *busy_end)
{
    for (size_t i = 0; i < n; i++)
        if (busy[i].lo < hi && lo < busy[i].hi) { if (busy_end) *busy_end = busy[i].hi; return 1; }
    return 0;
}

/* The lowest base in [XBOX_PICK_MIN, XBOX_PICK_MAX] where no window overlaps a busy range, or 0. Pure: no OS calls. */
static inline uint64_t xbox_layout_pick_base(const XboxBusyRange *busy, size_t n, uint64_t map_size, int mirrors)
{
    XboxBusyRange win[6];
    size_t nw = xbox_layout_windows(map_size, mirrors, win);
    uint64_t base = XBOX_PICK_MIN;
    while (base <= XBOX_PICK_MAX) {
        uint64_t next = 0;
        size_t w;
        for (w = 0; w < nw; w++) {
            uint64_t end;
            if (xbox_layout_overlaps(busy, n, base + win[w].lo, base + win[w].hi, &end)) {
                uint64_t cand = end > win[w].lo ? end - win[w].lo : base + XBOX_PICK_ALIGN;   /* move the window past it */
                if (cand > next) next = cand;
                break;
            }
        }
        if (w == nw) return base;
        next = (next + XBOX_PICK_ALIGN - 1) & ~(XBOX_PICK_ALIGN - 1);
        base = next > base ? next : base + XBOX_PICK_ALIGN;
    }
    return 0;
}

#ifdef _WIN32
#include <windows.h>

/* Wine and Proton export wine_get_version from ntdll; Windows does not. */
static inline int xbox_layout_under_wine(void)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != NULL;
}

/* The base xbox_layout_pick_base() chooses for this process's address space right now, or 0. */
static inline uintptr_t xbox_layout_scan_base(uint64_t map_size, int mirrors)
{
    static XboxBusyRange busy[4096];
    size_t n = 0;
    uint64_t a = 0, top = 0x110000000ull;
    while (a < top && n < sizeof busy / sizeof busy[0]) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((LPCVOID)(uintptr_t)a, &m, sizeof m)) { busy[n].lo = a; busy[n].hi = top; n++; break; }
        uint64_t lo = (uint64_t)(uintptr_t)m.BaseAddress, hi = lo + m.RegionSize;
        if (hi <= a) break;
        if (m.State != MEM_FREE) { busy[n].lo = lo; busy[n].hi = hi; n++; }
        a = hi;
    }
    if (n == sizeof busy / sizeof busy[0]) return 0;      /* too fragmented to judge: let the caller go on as before */
    return (uintptr_t)xbox_layout_pick_base(busy, n, map_size, mirrors);
}
#endif
#endif

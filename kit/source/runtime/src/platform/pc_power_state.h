/* 1.1.8: the PC's power state for the logs ([POWER] at launch, power=/mhz= on [HEALTH]), so a report shows a PC running in
 * power-saving mode or with its clocks pulled down mid-match. Read only, nothing changes; every part that fails says
 * "unknown" and nothing waits.
 *
 *   mode  the Windows power mode slider (PowerGetEffectiveOverlayScheme, undocumented, loaded from powrprof.dll at run time):
 *         best-efficiency | balanced | better-performance | best-performance | unknown
 *   plan  the active power plan (PowerGetActiveScheme): balanced | high-performance | ultimate | power-saver for the standard
 *         plans, else "custom-{GUID}" (the plan's name is never read: an OEM or user plan can carry any text)
 *   mhz   CallNtPowerInformation(ProcessorInformation): average and lowest current MHz over the logical processors, and the
 *         highest max MHz (Windows may report the base clock as current on some CPUs)
 * pc_power_state_test(fail) makes every call fail (tests/game/power_state_test.c). */
#ifndef PC_POWER_STATE_H
#define PC_POWER_STATE_H
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { char mode[24]; char plan[48]; unsigned mhz_avg, mhz_min, mhz_max, ncpu; } pc_power_state;

static int pc_power_state_fail;                          /* tests: every Windows call fails */
static __inline void pc_power_state_test(int fail) { pc_power_state_fail = fail; }

static __inline int pc_power_guid_is(const GUID *g, unsigned long d1, unsigned short d2, unsigned short d3,
                            unsigned char a, unsigned char b, unsigned char c, unsigned char d,
                            unsigned char e, unsigned char f, unsigned char h, unsigned char i)
{
    const GUID w = {d1, d2, d3, {a, b, c, d, e, f, h, i}};
    return !memcmp(g, &w, sizeof w);
}

static __inline void pc_power_guid_text(const GUID *g, char *out, size_t n)
{
    snprintf(out, n, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* the names for the known GUIDs (pure, tested) */
static __inline const char *pc_power_mode_name(const GUID *g)
{
    static const GUID zero;
    if (!memcmp(g, &zero, sizeof zero)) return "balanced";
    if (pc_power_guid_is(g, 0x961CC777, 0x2547, 0x4F9D, 0x81, 0x74, 0x7D, 0x86, 0x18, 0x1B, 0x8A, 0x7A)) return "best-efficiency";
    if (pc_power_guid_is(g, 0x3AF9B8D9, 0x7C97, 0x431D, 0xAD, 0x78, 0x34, 0xA8, 0xBF, 0xEA, 0x43, 0x9F)) return "better-performance";
    if (pc_power_guid_is(g, 0xDED574B5, 0x45A0, 0x4F42, 0x87, 0x37, 0x46, 0x34, 0x5C, 0x09, 0xC2, 0x38)) return "best-performance";
    return "unknown";
}
static __inline void pc_power_plan_name(const GUID *g, char *out, size_t n)
{
    const char *s = NULL;
    if (pc_power_guid_is(g, 0x381B4222, 0xF694, 0x41F0, 0x96, 0x85, 0xFF, 0x5B, 0xB2, 0x60, 0xDF, 0x2E)) s = "balanced";
    else if (pc_power_guid_is(g, 0x8C5E7FDA, 0xE8BF, 0x4A96, 0x9A, 0x85, 0xA6, 0xE2, 0x3A, 0x8C, 0x63, 0x5C)) s = "high-performance";
    else if (pc_power_guid_is(g, 0xE9A42B02, 0xD5DF, 0x448D, 0xAA, 0x00, 0x03, 0xF1, 0x47, 0x49, 0xEB, 0x61)) s = "ultimate";
    else if (pc_power_guid_is(g, 0xA1841308, 0x3541, 0x4FAB, 0xBC, 0x81, 0xF7, 0x15, 0x56, 0xF2, 0x0B, 0x4A)) s = "power-saver";
    if (s) { snprintf(out, n, "%s", s); return; }
    char t[40]; pc_power_guid_text(g, t, sizeof t);
    snprintf(out, n, "custom-%s", t);
}

typedef DWORD (WINAPI *pc_overlay_fn)(GUID *);
typedef DWORD (WINAPI *pc_active_fn)(HKEY, GUID **);
typedef LONG (WINAPI *pc_ntpi_fn)(int, PVOID, ULONG, PVOID, ULONG);
typedef struct { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; } pc_ppi;

static __inline void pc_power_state_read(pc_power_state *s)
{
    static HMODULE pp; static int tried;
    memset(s, 0, sizeof *s);
    snprintf(s->mode, sizeof s->mode, "unknown"); snprintf(s->plan, sizeof s->plan, "unknown");
    if (pc_power_state_fail) return;
    if (!tried) { tried = 1; pp = LoadLibraryExW(L"powrprof.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); }
    if (!pp) return;
    pc_overlay_fn ov = (pc_overlay_fn)(void *)GetProcAddress(pp, "PowerGetEffectiveOverlayScheme");
    pc_active_fn act = (pc_active_fn)(void *)GetProcAddress(pp, "PowerGetActiveScheme");
    pc_ntpi_fn ntpi = (pc_ntpi_fn)(void *)GetProcAddress(pp, "CallNtPowerInformation");
    GUID g;
    if (ov && ov(&g) == 0) snprintf(s->mode, sizeof s->mode, "%s", pc_power_mode_name(&g));
    GUID *a = NULL;
    if (act && act(NULL, &a) == 0 && a) { pc_power_plan_name(a, s->plan, sizeof s->plan); LocalFree(a); }
    if (ntpi) {
        pc_ppi buf[256];
        SYSTEM_INFO si; GetSystemInfo(&si);
        unsigned n = si.dwNumberOfProcessors > 256 ? 256 : si.dwNumberOfProcessors;
        if (n && ntpi(11 /* ProcessorInformation */, NULL, 0, buf, (ULONG)(n * sizeof *buf)) == 0) {
            unsigned long long sum = 0; unsigned mn = ~0u, mx = 0;
            for (unsigned i = 0; i < n; i++) {
                sum += buf[i].CurrentMhz;
                if (buf[i].CurrentMhz < mn) mn = buf[i].CurrentMhz;
                if (buf[i].MaxMhz > mx) mx = buf[i].MaxMhz;
            }
            s->ncpu = n; s->mhz_avg = (unsigned)(sum / n); s->mhz_min = mn; s->mhz_max = mx;
        }
    }
}

/* "pmode=... pplan=... mhz=avg/min/max" (mhz=unknown when not read; "plan=" on [HEALTH] is the DSP plan) */
static __inline void pc_power_state_text(const pc_power_state *s, char *out, size_t n)
{
    if (s->ncpu) snprintf(out, n, "pmode=%s pplan=%s mhz=%u/%u/%u", s->mode, s->plan, s->mhz_avg, s->mhz_min, s->mhz_max);
    else snprintf(out, n, "pmode=%s pplan=%s mhz=unknown", s->mode, s->plan);
}
#endif

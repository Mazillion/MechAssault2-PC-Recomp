/* pc_cpu_topo.c -- CPU topology detection and the thread-role registry (pipeline M0, 2026-10-31).
 *
 * #included by main.c (no build edge of its own, so it ships as part of main.c.obj); see
 * pc_cpu_topo.h for the interface. main.c calls, in WinMain, before any other thread starts:
 *     pc_cpu_affinity_from_env();   RECOMP_AFFINITY=<hex> (test only): SetProcessAffinityMask
 *     pc_cpu_topo_init();           measure, fill pc_cpu_topo_api, log one [CPU-TOPO] line
 *
 * Detection only reads (GetSystemCpuSetInformation, else GetLogicalProcessorInformationEx, else the
 * affinity mask; GetSystemPowerStatus). Nothing here changes how any thread runs: the role policy
 * is an empty hook for T1-5 ([Cpu] ThreadPlacement). Every API is optional: a missing or failing call
 * falls back to the next source and is reported in the log line (source=...). */
#include "pc_cpu_topo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PcCpuTopo pc_topo;

static unsigned pc_topo_popcount64(unsigned long long v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* RECOMP_AFFINITY=<hex mask> (test only): restrict the whole process to those logical CPUs of its
 * group, e.g. 55 = 4 cores without SMT, FF = 4 cores with SMT, FF0000 = the 8 E-cores of an 8P+8E CPU. */
static void pc_cpu_affinity_from_env(void)
{
    char text[64];
    DWORD n = GetEnvironmentVariableA("RECOMP_AFFINITY", text, sizeof(text));
    DWORD_PTR process = 0, system = 0;
    unsigned long long mask;
    BOOL ok;
    if (!n || n >= sizeof(text)) return;
    mask = _strtoui64(text, NULL, 16);
    GetProcessAffinityMask(GetCurrentProcess(), &process, &system);
    ok = mask ? SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask) : FALSE;
    fprintf(stderr, "[CPU-TOPO] RECOMP_AFFINITY=%llX (test only): %s (was %llX, system %llX)%s\n", mask,
            ok ? "set" : "FAILED", (unsigned long long)process, (unsigned long long)system,
            ok ? "" : " -- the mask must be a non-empty subset of the system mask");
}

/* Distinct (group, value) pairs among the first n entries: n is at most PC_CPU_TOPO_MAX_SETS. */
static unsigned pc_topo_distinct(const unsigned *keys, unsigned n)
{
    unsigned i, j, count = 0;
    for (i = 0; i < n; i++) {
        for (j = 0; j < i && keys[j] != keys[i]; j++) {}
        if (j == i) count++;
    }
    return count;
}

typedef BOOL (WINAPI *PcGetSystemCpuSetInformation)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *PcGetProcessDefaultCpuSets)(HANDLE, PULONG, ULONG, PULONG);

/* Primary source: CPU Sets (Windows 10+). Returns 0 when unavailable. */
static int pc_topo_from_cpusets(PcCpuTopo *t, int single_group, USHORT group, unsigned long long mask)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    PcGetSystemCpuSetInformation get_sets =
        k32 ? (PcGetSystemCpuSetInformation)(void *)GetProcAddress(k32, "GetSystemCpuSetInformation") : NULL;
    PcGetProcessDefaultCpuSets get_defaults =
        k32 ? (PcGetProcessDefaultCpuSets)(void *)GetProcAddress(k32, "GetProcessDefaultCpuSets") : NULL;
    ULONG length = 0, defaults_n = 0, *defaults = NULL;
    unsigned char *buffer, *p, *end;
    unsigned core_keys[PC_CPU_TOPO_MAX_SETS], llc_keys[PC_CPU_TOPO_MAX_SETS], class_keys[PC_CPU_TOPO_MAX_SETS];
    unsigned sys_class_keys[PC_CPU_TOPO_MAX_SETS], sys_n = 0, i;
    unsigned char top = 0;
    if (!get_sets) return 0;
    get_sets(NULL, 0, &length, GetCurrentProcess(), 0);
    if (!length || !(buffer = (unsigned char *)malloc(length))) return 0;
    if (!get_sets((PSYSTEM_CPU_SET_INFORMATION)buffer, length, &length, GetCurrentProcess(), 0)) { free(buffer); return 0; }
    /* Default CPU Sets of the process (normally none) narrow it further. */
    if (get_defaults && !get_defaults(GetCurrentProcess(), NULL, 0, &defaults_n) && defaults_n &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && (defaults = (ULONG *)malloc(defaults_n * sizeof(ULONG)))) {
        if (!get_defaults(GetCurrentProcess(), defaults, defaults_n, &defaults_n)) defaults_n = 0;
    } else {
        defaults_n = 0;
    }
    t->set_count = 0;
    t->system_logical = 0;
    for (p = buffer, end = buffer + length; p + sizeof(DWORD) * 2 <= end; ) {
        PSYSTEM_CPU_SET_INFORMATION info = (PSYSTEM_CPU_SET_INFORMATION)p;
        DWORD size = info->Size;
        int usable;
        if (size < sizeof(DWORD) * 2 || p + size > end) break;
        p += size;
        if (info->Type != CpuSetInformation) continue;
        t->system_logical++;
        if (sys_n < PC_CPU_TOPO_MAX_SETS) sys_class_keys[sys_n++] = info->CpuSet.EfficiencyClass;
        if (info->CpuSet.EfficiencyClass > t->system_p_class) t->system_p_class = info->CpuSet.EfficiencyClass;
        usable = !single_group || (info->CpuSet.Group == group && info->CpuSet.LogicalProcessorIndex < 64 &&
                                   (mask >> info->CpuSet.LogicalProcessorIndex) & 1);
        /* Exclusively allocated to some other process (bit 2 without bit 4). */
        if ((info->CpuSet.AllFlags & 0x06) == 0x02) usable = 0;
        if (usable && defaults_n) {
            ULONG k;
            for (k = 0; k < defaults_n && defaults[k] != info->CpuSet.Id; k++) {}
            usable = k < defaults_n;
        }
        if (!usable) continue;
        t->logical++;
        if (t->set_count < PC_CPU_TOPO_MAX_SETS) {
            PcCpuSet *s = &t->set[t->set_count++];
            s->id = info->CpuSet.Id;
            s->group = info->CpuSet.Group;
            s->logical = info->CpuSet.LogicalProcessorIndex;
            s->core = info->CpuSet.CoreIndex;
            s->eff_class = info->CpuSet.EfficiencyClass;
            s->llc = info->CpuSet.LastLevelCacheIndex;
            s->numa = info->CpuSet.NumaNodeIndex;
            s->flags = info->CpuSet.AllFlags;
        }
    }
    free(buffer);
    free(defaults);
    if (!t->logical) return 0;
    for (i = 0; i < t->set_count; i++) {
        core_keys[i] = (unsigned)t->set[i].group << 16 | t->set[i].core;
        llc_keys[i] = (unsigned)t->set[i].group << 16 | t->set[i].llc;
        class_keys[i] = t->set[i].eff_class;
        if (t->set[i].eff_class > top) top = t->set[i].eff_class;
    }
    t->physical = pc_topo_distinct(core_keys, t->set_count);
    t->llc = pc_topo_distinct(llc_keys, t->set_count);
    t->classes = pc_topo_distinct(class_keys, t->set_count);
    t->system_classes = pc_topo_distinct(sys_class_keys, sys_n);
    t->p_class = top;
    {
        unsigned p_cores[PC_CPU_TOPO_MAX_SETS], e_cores[PC_CPU_TOPO_MAX_SETS], np = 0, ne = 0;
        for (i = 0; i < t->set_count; i++) {
            if (t->set[i].eff_class == top) { t->p_logical++; p_cores[np++] = core_keys[i]; }
            else { t->e_logical++; e_cores[ne++] = core_keys[i]; }
        }
        t->p_physical = pc_topo_distinct(p_cores, np);
        t->e_physical = pc_topo_distinct(e_cores, ne);
    }
    t->source = PC_CPU_SRC_CPUSETS;
    return 1;
}

/* Fallback: one record per core (Windows 7+; EfficiencyClass is filled in on Windows 10+). */
static int pc_topo_from_glpi(PcCpuTopo *t, int single_group, USHORT group, unsigned long long mask)
{
    DWORD length = 0;
    unsigned char *buffer, *p, *end;
    unsigned char classes[PC_CPU_TOPO_MAX_SETS], counts[PC_CPU_TOPO_MAX_SETS];
    unsigned cores = 0, class_keys[PC_CPU_TOPO_MAX_SETS], sys_class_keys[PC_CPU_TOPO_MAX_SETS], sys_n = 0, i;
    unsigned char top = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);
    if (!length || !(buffer = (unsigned char *)malloc(length))) return 0;
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &length)) {
        free(buffer);
        return 0;
    }
    t->system_logical = 0;
    for (p = buffer, end = buffer + length; p + sizeof(DWORD) * 2 <= end; ) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;
        unsigned n = 0, g;
        if (info->Size < sizeof(DWORD) * 2 || p + info->Size > end) break;
        p += info->Size;
        if (info->Relationship != RelationProcessorCore) continue;
        for (g = 0; g < info->Processor.GroupCount && g < ANYSIZE_ARRAY; g++) {
            unsigned long long m = (unsigned long long)info->Processor.GroupMask[g].Mask;
            t->system_logical += pc_topo_popcount64(m);
            if (!single_group) n += pc_topo_popcount64(m);
            else if (info->Processor.GroupMask[g].Group == group) n += pc_topo_popcount64(m & mask);
        }
        if (sys_n < PC_CPU_TOPO_MAX_SETS) sys_class_keys[sys_n++] = info->Processor.EfficiencyClass;
        if (info->Processor.EfficiencyClass > t->system_p_class) t->system_p_class = info->Processor.EfficiencyClass;
        if (!n) continue;
        t->logical += n;
        if (cores < PC_CPU_TOPO_MAX_SETS) {
            classes[cores] = info->Processor.EfficiencyClass;
            counts[cores] = (unsigned char)(n > 255 ? 255 : n);
            class_keys[cores] = classes[cores];
            if (classes[cores] > top) top = classes[cores];
            cores++;
        }
    }
    free(buffer);
    if (!t->logical) return 0;
    t->physical = cores;
    t->classes = pc_topo_distinct(class_keys, cores);
    t->system_classes = pc_topo_distinct(sys_class_keys, sys_n);
    t->p_class = top;
    for (i = 0; i < cores; i++) {
        if (classes[i] == top) { t->p_logical += counts[i]; t->p_physical++; }
        else { t->e_logical += counts[i]; t->e_physical++; }
    }
    t->llc = 0;
    t->source = PC_CPU_SRC_GLPI;
    return 1;
}

/* ── Thread roles (TH-1 registry) ─────────────────────────────── */

#define PC_THREAD_ROLE_MAX 64
typedef struct PcThreadRoleEntry {
    int role;          /* PcThreadRole */
    DWORD tid;         /* 0 = free slot */
    HANDLE thread;     /* real handle (DuplicateHandle), owned by the table */
} PcThreadRoleEntry;
static PcThreadRoleEntry pc_role_table[PC_THREAD_ROLE_MAX];
static unsigned pc_role_high;            /* slots in use are below this */
static LONG pc_role_overflow;            /* registrations dropped because the table was full */
static SRWLOCK pc_role_lock = SRWLOCK_INIT;
/* T1-0 TH-1: the caller's slot (for pc_thread_cpu_note) and per-slot processor stamp counts. */
static __declspec(thread) int pc_role_tls_slot = -1;
static unsigned long long pc_role_cpu_hist[PC_THREAD_ROLE_MAX][64];

/* Placement policy for a thread's role. Deliberately empty until T1-5 ([Cpu] ThreadPlacement=auto:
 * P-core CPU Sets for the game and executor at PC_CPU_TIER_FULL, EcoQoS for helpers on hybrid CPUs,
 * nothing below the 4-thread floor, never hard affinity). Runs on the registering thread. */
static void pc_thread_role_policy(int role)
{
    (void)role;
}

static void pc_thread_role_enter_impl(int role)
{
    DWORD tid = GetCurrentThreadId();
    unsigned i, slot = PC_THREAD_ROLE_MAX;
    if (role < 0 || role >= PC_ROLE_COUNT) role = PC_ROLE_OTHER;
    AcquireSRWLockExclusive(&pc_role_lock);
    for (i = 0; i < pc_role_high; i++)
        if (pc_role_table[i].tid == tid) break;
    if (i < pc_role_high) {
        pc_role_table[i].role = role;              /* already registered: new role */
        pc_role_tls_slot = (int)i;
    } else {
        for (i = 0; i < pc_role_high && pc_role_table[i].tid; i++) {}
        if (i < PC_THREAD_ROLE_MAX) slot = i;
        if (slot < PC_THREAD_ROLE_MAX) {
            HANDLE h = NULL;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE,
                                DUPLICATE_SAME_ACCESS)) {
                pc_role_table[slot].role = role;
                pc_role_table[slot].tid = tid;
                pc_role_table[slot].thread = h;
                memset(pc_role_cpu_hist[slot], 0, sizeof(pc_role_cpu_hist[slot]));
                pc_role_tls_slot = (int)slot;
                if (slot >= pc_role_high) pc_role_high = slot + 1;
            }
        } else {
            InterlockedIncrement(&pc_role_overflow);
        }
    }
    ReleaseSRWLockExclusive(&pc_role_lock);
    pc_thread_role_policy(role);
}

static void pc_thread_role_leave_impl(void)
{
    DWORD tid = GetCurrentThreadId();
    unsigned i;
    HANDLE h = NULL;
    AcquireSRWLockExclusive(&pc_role_lock);
    for (i = 0; i < pc_role_high; i++)
        if (pc_role_table[i].tid == tid) {
            h = pc_role_table[i].thread;
            pc_role_table[i].tid = 0;
            pc_role_table[i].thread = NULL;
            pc_role_tls_slot = -1;
            break;
        }
    while (pc_role_high && !pc_role_table[pc_role_high - 1].tid) pc_role_high--;
    ReleaseSRWLockExclusive(&pc_role_lock);
    if (h) CloseHandle(h);
}

/* For TH-1 (main.c): calls fn for every registered thread under the shared lock, so a thread's
 * handle stays open while fn uses it. fn must not register or leave. Returns the count. */
static unsigned pc_thread_role_foreach(void (*fn)(const PcThreadRoleEntry *entry, void *ctx), void *ctx)
{
    unsigned i, n = 0;
    AcquireSRWLockShared(&pc_role_lock);
    for (i = 0; i < pc_role_high; i++)
        if (pc_role_table[i].tid) {
            n++;
            if (fn) fn(&pc_role_table[i], ctx);
        }
    ReleaseSRWLockShared(&pc_role_lock);
    return n;
}

/* ── T1-0 TH-1 [PERF-CPU] + TH-4 [PERF-RATE] reporter (RECOMP_PERF_CPU=1, test only) ──────────
 * Every 5 s, on its own thread (PC_ROLE_DIAG):
 *   [PERF-CPU] proc = % of one CPU; per registered role %; the top threads by CPU (tid, role).
 *   [PERF-LP]  per logical processor busy % from QueryIdleProcessorCycleTime (TSC based): the
 *              hottest LPs as lp(core,P|E), cores with two LPs over 50 % (SMT sharing), E-core load,
 *              and where the game thread ran (its per-frame pc_thread_cpu_note stamps) with the
 *              busy % of its SMT sibling, so a game/executor pair sharing one core shows up.
 *   [PERF-RATE] ICALLs/s, guard hits/misses, kernel calls/s and the top ordinals/s, and guest CS
 *              contention: contended enters/s, ms blocked per s (all threads / game thread), and
 *              the same for the DirectSound API lock 0x3C4598.
 * Off: no thread, no stamps (cpu_note stays NULL), CS counters stay disabled. */
#include <tlhelp32.h>
#include <intrin.h>
void xbox_kernel_rate_counts(unsigned long long *total, const unsigned long long **per_ordinal, unsigned *ordinals);
void xbox_rtl_cs_rate_enable(DWORD game_tid);
void xbox_rtl_cs_rate_counts(long long out[6]);
extern volatile unsigned long long g_icall_count;

static void pc_thread_cpu_note_impl(void)
{
    int s = pc_role_tls_slot;
    PROCESSOR_NUMBER pn;
    if (s < 0 || s >= PC_THREAD_ROLE_MAX) return;
    GetCurrentProcessorNumberEx(&pn);
    if (pn.Group == 0 && pn.Number < 64) pc_role_cpu_hist[s][pn.Number]++;   /* one writer per slot */
}

static unsigned long long pc_perf_ft(FILETIME a, FILETIME b)
{
    return (((unsigned long long)a.dwHighDateTime << 32) | a.dwLowDateTime) +
           (((unsigned long long)b.dwHighDateTime << 32) | b.dwLowDateTime);
}

typedef struct PcPerfRoleCtx {
    unsigned long long role_100ns[PC_ROLE_COUNT];
    unsigned long long prev[PC_THREAD_ROLE_MAX];
    DWORD prev_tid[PC_THREAD_ROLE_MAX];
    int game_slot;
} PcPerfRoleCtx;

static void pc_perf_role_cb(const PcThreadRoleEntry *e, void *vctx)
{
    PcPerfRoleCtx *c = (PcPerfRoleCtx *)vctx;
    unsigned slot = (unsigned)(e - pc_role_table);
    FILETIME cr, ex, k, u;
    unsigned long long now;
    if (slot >= PC_THREAD_ROLE_MAX || !GetThreadTimes(e->thread, &cr, &ex, &k, &u)) return;
    now = pc_perf_ft(k, u);
    if (c->prev_tid[slot] == e->tid && now >= c->prev[slot]) c->role_100ns[e->role] += now - c->prev[slot];
    c->prev[slot] = now;
    c->prev_tid[slot] = e->tid;
    if (e->role == PC_ROLE_GAME) c->game_slot = (int)slot;
}

static const char *pc_perf_role_of(DWORD tid)
{
    unsigned i;
    for (i = 0; i < pc_role_high; i++)
        if (pc_role_table[i].tid == tid) return pc_thread_role_name(pc_role_table[i].role);
    return "-";
}

static int pc_perf_is_e(const PcCpuTopo *t, int set)
{
    return set >= 0 && t->set[set].eff_class < t->p_class;
}

/* Clock probe: a chain of dependent multiplies (3 cycles each on Golden Cove / Gracemont / Zen) timed with QPC,
 * so a CPU throttled hard (power limit, PROCHOT) shows as a drop in cpuspeed= in [PERF-CPU] -- thread cycle counts tick at
 * the TSC rate and cannot show it. ~1.5 ms on the diag thread, wherever the scheduler put it. */
static double pc_perf_clock_ghz(void)
{
    LARGE_INTEGER f, a, b;
    volatile unsigned long long sink;
    unsigned long long x = 7;
    unsigned i;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (i = 0; i < 1500000u; i++) x = x * 2862933555777941757ull + 3037000493ull;
    QueryPerformanceCounter(&b);
    sink = x; (void)sink;
    double ns = (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart;
    return ns > 0.0 ? 1500000.0 * 1000.0 / ns : 0.0;   /* steps per microsecond: a relative speed (the compiler shortens the chain) */
}

#define PC_PERF_TMAX 512
static DWORD WINAPI pc_perf_cpu_thread(LPVOID unused)
{
    static PcPerfRoleCtx rc;
    static DWORD ttid[PC_PERF_TMAX];
    static unsigned long long tprev[PC_PERF_TMAX], tdelta[PC_PERF_TMAX];
    static unsigned long long idle_prev[64], idle_now[64], hist_prev[64];
    static unsigned long long *ord_prev;
    const PcCpuTopo *t = &pc_topo;
    int lp_set[64], lp;
    unsigned long long proc_prev = 0, tsc_prev, icall_prev, kern_prev = 0;
    long long cs_prev[6], cs_now[6];
    LARGE_INTEGER qf, q_prev, q_now;
    unsigned ntid = 0, nlp = 0, n_ord = 0, i;
    FILETIME cr, ex, k, u;
    (void)unused;
    pc_thread_role_enter(PC_ROLE_DIAG);
    for (lp = 0; lp < 64; lp++) lp_set[lp] = -1;
    for (i = 0; i < t->set_count; i++)
        if (t->set[i].group == 0 && t->set[i].logical < 64) lp_set[t->set[i].logical] = (int)i;
    {
        ULONG size = sizeof(idle_prev);
        if (QueryIdleProcessorCycleTime(&size, idle_prev)) nlp = size / sizeof(idle_prev[0]);
        if (nlp > 64) nlp = 64;
    }
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q_prev);
    tsc_prev = __rdtsc();
    icall_prev = g_icall_count;
    {
        const unsigned long long *po = NULL;
        xbox_kernel_rate_counts(&kern_prev, &po, &n_ord);
        ord_prev = (unsigned long long *)calloc(n_ord ? n_ord : 1, sizeof(*ord_prev));
        if (ord_prev && po) memcpy(ord_prev, po, n_ord * sizeof(*ord_prev));
    }
    xbox_rtl_cs_rate_counts(cs_prev);
    if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) proc_prev = pc_perf_ft(k, u);
    rc.game_slot = -1;
    pc_thread_role_foreach(pc_perf_role_cb, &rc);
    for (;;) {
        char line[1600];
        size_t used;
        double wall, tscd;
        unsigned long long proc_now = 0, tsc_now;
        Sleep(5000);
        QueryPerformanceCounter(&q_now);
        tsc_now = __rdtsc();
        wall = (double)(q_now.QuadPart - q_prev.QuadPart) / (double)qf.QuadPart;
        tscd = (double)(tsc_now - tsc_prev);
        if (wall <= 0.0) wall = 1e-9;
        /* process and roles */
        if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) proc_now = pc_perf_ft(k, u);
        memset(rc.role_100ns, 0, sizeof(rc.role_100ns));
        pc_thread_role_foreach(pc_perf_role_cb, &rc);
        used = (size_t)snprintf(line, sizeof(line), "[PERF-CPU] wall=%.1fs proc=%.0f%%", wall,
                                (double)(proc_now - proc_prev) / (wall * 1e5));
        used += (size_t)snprintf(line + used, sizeof(line) - used, " cpuspeed=%.0f", pc_perf_clock_ghz());
        for (i = 0; i < PC_ROLE_COUNT && used < sizeof(line) - 64; i++)
            if (rc.role_100ns[i])
                used += (size_t)snprintf(line + used, sizeof(line) - used, " %s=%.1f%%",
                                         pc_thread_role_name((int)i), (double)rc.role_100ns[i] / (wall * 1e5));
        /* the top threads of the whole process, registered or not */
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            unsigned best[6], nb, j, q;
            DWORD pid = GetCurrentProcessId();
            for (i = 0; i < ntid; i++) tdelta[i] = 0;
            if (snap != INVALID_HANDLE_VALUE) {
                te.dwSize = sizeof(te);
                if (Thread32First(snap, &te)) do {
                    if (te.th32OwnerProcessID == pid) {
                        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                        if (h) {
                            if (GetThreadTimes(h, &cr, &ex, &k, &u)) {
                                unsigned long long now = pc_perf_ft(k, u);
                                for (j = 0; j < ntid && ttid[j] != te.th32ThreadID; j++) {}
                                if (j == ntid) {
                                    if (ntid < PC_PERF_TMAX) { ttid[ntid] = te.th32ThreadID; tprev[ntid] = now; ntid++; }
                                } else {
                                    tdelta[j] = now >= tprev[j] ? now - tprev[j] : 0;
                                    tprev[j] = now;
                                }
                            }
                            CloseHandle(h);
                        }
                    }
                    te.dwSize = sizeof(te);
                } while (Thread32Next(snap, &te));
                CloseHandle(snap);
            }
            for (nb = 0; nb < 6; nb++) {
                unsigned b = PC_PERF_TMAX;
                for (j = 0; j < ntid; j++) {
                    unsigned taken = 0;
                    for (q = 0; q < nb; q++) taken |= best[q] == j;
                    if (!taken && tdelta[j] && (b == PC_PERF_TMAX || tdelta[j] > tdelta[b])) b = j;
                }
                if (b == PC_PERF_TMAX) break;
                best[nb] = b;
            }
            used += (size_t)snprintf(line + used, sizeof(line) - used, " | top");
            for (j = 0; j < nb && used < sizeof(line) - 48; j++)
                used += (size_t)snprintf(line + used, sizeof(line) - used, " %lu(%s)=%.1f%%",
                                         (unsigned long)ttid[best[j]], pc_perf_role_of(ttid[best[j]]),
                                         (double)tdelta[best[j]] / (wall * 1e5));
        }
        fprintf(stderr, "%s\n", line);
        /* logical processors, and where the game thread ran */
        if (nlp) {
            ULONG size = sizeof(idle_now);
            double busy[64], ebusy = 0.0;
            unsigned order[64], n = 0, j;
            if (QueryIdleProcessorCycleTime(&size, idle_now)) {
                for (lp = 0; lp < (int)nlp; lp++) {
                    double d = (double)(idle_now[lp] - idle_prev[lp]);
                    busy[lp] = tscd > 0 ? 100.0 * (1.0 - d / tscd) : 0.0;
                    if (busy[lp] < 0) busy[lp] = 0;
                    if (busy[lp] > 100) busy[lp] = 100;
                    idle_prev[lp] = idle_now[lp];
                    if (lp_set[lp] >= 0) {
                        order[n++] = (unsigned)lp;
                        if (pc_perf_is_e(t, lp_set[lp])) ebusy += busy[lp];
                    }
                }
                for (i = 0; i < n; i++)
                    for (j = i + 1; j < n; j++)
                        if (busy[order[j]] > busy[order[i]]) { unsigned x = order[i]; order[i] = order[j]; order[j] = x; }
                used = (size_t)snprintf(line, sizeof(line), "[PERF-LP] hot");
                for (i = 0; i < n && i < 8; i++)
                    used += (size_t)snprintf(line + used, sizeof(line) - used, " %u(c%u%s)=%.0f%%", order[i],
                                             t->set[lp_set[order[i]]].core,
                                             pc_perf_is_e(t, lp_set[order[i]]) ? "E" : "P", busy[order[i]]);
                used += (size_t)snprintf(line + used, sizeof(line) - used, " | smt>50:");
                for (i = 0; i < n; i++) {
                    if (busy[order[i]] <= 50.0) continue;
                    for (j = i + 1; j < n; j++)
                        if (t->set[lp_set[order[j]]].core == t->set[lp_set[order[i]]].core && busy[order[j]] > 50.0 &&
                            used < sizeof(line) - 32)
                            used += (size_t)snprintf(line + used, sizeof(line) - used, " c%u(%u+%u)",
                                                     t->set[lp_set[order[i]]].core, order[i], order[j]);
                }
                used += (size_t)snprintf(line + used, sizeof(line) - used, " | ecore_sum=%.0f%%", ebusy);
                if (rc.game_slot >= 0) {
                    unsigned long long tot = 0, cnt[64], ecnt = 0, sib_w = 0;
                    unsigned g[3] = { 64, 64, 64 };
                    double sib = 0.0;
                    for (lp = 0; lp < 64; lp++) {
                        unsigned long long h = pc_role_cpu_hist[rc.game_slot][lp];
                        cnt[lp] = h >= hist_prev[lp] ? h - hist_prev[lp] : 0;
                        hist_prev[lp] = h;
                        tot += cnt[lp];
                        if (cnt[lp] && pc_perf_is_e(t, lp_set[lp])) ecnt += cnt[lp];
                    }
                    for (i = 0; i < 3; i++)
                        for (lp = 0; lp < 64; lp++) {
                            if (!cnt[lp] || (i > 0 && lp == (int)g[0]) || (i > 1 && lp == (int)g[1])) continue;
                            if (g[i] == 64 || cnt[lp] > cnt[g[i]]) g[i] = (unsigned)lp;
                        }
                    /* busy % of the SMT siblings of the LPs the game ran on, weighted by its stamps there */
                    for (lp = 0; lp < (int)nlp; lp++) {
                        if (!cnt[lp] || lp_set[lp] < 0) continue;
                        for (j = 0; j < nlp; j++)
                            if ((int)j != lp && lp_set[j] >= 0 && t->set[lp_set[j]].core == t->set[lp_set[lp]].core) {
                                sib += busy[j] * (double)cnt[lp];
                                sib_w += cnt[lp];
                            }
                    }
                    used += (size_t)snprintf(line + used, sizeof(line) - used, " | game stamps=%llu", tot);
                    for (i = 0; i < 3 && tot; i++)
                        if (g[i] < 64 && lp_set[g[i]] >= 0)
                            used += (size_t)snprintf(line + used, sizeof(line) - used, " %u(c%u%s)=%.0f%%", g[i],
                                                     t->set[lp_set[g[i]]].core, pc_perf_is_e(t, lp_set[g[i]]) ? "E" : "P",
                                                     100.0 * (double)cnt[g[i]] / (double)tot);
                    if (tot)
                        used += (size_t)snprintf(line + used, sizeof(line) - used, " on_e=%.0f%% sibling_busy=%.0f%%",
                                                 100.0 * (double)ecnt / (double)tot, sib_w ? sib / (double)sib_w : -1.0);
                }
                fprintf(stderr, "%s\n", line);
            }
        }
        /* rates */
        {
            unsigned long long ic = g_icall_count, kt = 0;
            const unsigned long long *po = NULL;
            unsigned n = 0, j, q, shown[5];
            double tick_ms = 1000.0 / (double)qf.QuadPart;
            xbox_kernel_rate_counts(&kt, &po, &n);
            xbox_rtl_cs_rate_counts(cs_now);
            used = (size_t)snprintf(line, sizeof(line), "[PERF-RATE] icall=%.0f/s kernel=%.0f/s top",
                                    (double)(ic - icall_prev) / wall, (double)(kt - kern_prev) / wall);
            if (po && ord_prev && n == n_ord) {
                for (q = 0; q < 5; q++) {
                    unsigned b = n;
                    for (j = 0; j < n; j++) {
                        unsigned r, taken = 0;
                        for (r = 0; r < q; r++) taken |= shown[r] == j;
                        if (!taken && po[j] > ord_prev[j] && (b == n || po[j] - ord_prev[j] > po[b] - ord_prev[b])) b = j;
                    }
                    if (b == n) break;
                    shown[q] = b;
                    used += (size_t)snprintf(line + used, sizeof(line) - used, " ord%u=%.0f/s", b,
                                             (double)(po[b] - ord_prev[b]) / wall);
                }
                memcpy(ord_prev, po, n * sizeof(*ord_prev));
            }
            used += (size_t)snprintf(line + used, sizeof(line) - used,
                                     " | cs contended=%.0f/s wait=%.2fms/s game_wait=%.2fms/s"
                                     " | 3C4598 contended=%.0f/s wait=%.2fms/s game_wait=%.2fms/s",
                                     (double)(cs_now[0] - cs_prev[0]) / wall,
                                     (double)(cs_now[1] - cs_prev[1]) * tick_ms / wall,
                                     (double)(cs_now[2] - cs_prev[2]) * tick_ms / wall,
                                     (double)(cs_now[3] - cs_prev[3]) / wall,
                                     (double)(cs_now[4] - cs_prev[4]) * tick_ms / wall,
                                     (double)(cs_now[5] - cs_prev[5]) * tick_ms / wall);
            fprintf(stderr, "%s\n", line);
            icall_prev = ic;
            kern_prev = kt;
            memcpy(cs_prev, cs_now, sizeof(cs_prev));
        }
        fflush(stderr);
        proc_prev = proc_now;
        tsc_prev = tsc_now;
        q_prev = q_now;
    }
}

/* main.c, on the game thread right before the guest entry point. */
static void pc_perf_cpu_start(void)
{
    const char *e = getenv("RECOMP_PERF_CPU");
    HANDLE h;
    if (!e || !*e || *e == '0') return;
    pc_cpu_topo_api.cpu_note = pc_thread_cpu_note_impl;
    xbox_rtl_cs_rate_enable(GetCurrentThreadId());
    h = CreateThread(NULL, 64 * 1024, pc_perf_cpu_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    fprintf(stderr, "[PERF-CPU] reporter on (5 s windows), game tid %lu\n", (unsigned long)GetCurrentThreadId());
}

/* ── Startup ──────────────────────────────────────────────────── */

static const char *pc_cpu_tier_name(int tier)
{
    return tier == PC_CPU_TIER_FULL ? "full" : tier == PC_CPU_TIER_SMALL ? "small" : "floor";
}

static void pc_cpu_topo_init(void)
{
    PcCpuTopo *t = &pc_topo;
    DWORD_PTR process = 0, system = 0;
    USHORT groups[8], group_n = 8, group = 0;
    int single_group = 0;
    SYSTEM_POWER_STATUS ps;
    memset(t, 0, sizeof(*t));
    t->groups = GetActiveProcessorGroupCount();
    if (!t->groups) t->groups = 1;
    /* The mask only describes the primary group; a process already spread over several groups
     * (more than 64 logical CPUs) counts every CPU Set. */
    if (GetProcessGroupAffinity(GetCurrentProcess(), &group_n, groups) && group_n == 1 &&
        GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        single_group = 1;
        group = groups[0];
        t->mask = (unsigned long long)process;
    }
    if (!pc_topo_from_cpusets(t, single_group, group, t->mask)) {
        memset(t, 0, sizeof(*t));
        t->groups = GetActiveProcessorGroupCount();
        if (!t->groups) t->groups = 1;
        t->mask = single_group ? (unsigned long long)process : 0;
        if (!pc_topo_from_glpi(t, single_group, group, t->mask)) {
            /* Last resort: the mask's count; cores and classes unknown, so assume no SMT, one class. */
            t->logical = single_group ? pc_topo_popcount64(t->mask)
                                      : (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            if (!t->logical) t->logical = 1;
            t->physical = t->p_logical = t->p_physical = t->logical;
            t->classes = t->system_classes = 1;
            t->system_logical = (unsigned)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            t->source = PC_CPU_SRC_MASK;
        }
    }
    if (!t->physical) t->physical = 1;
    t->tier = t->logical < 4 ? PC_CPU_TIER_FLOOR : t->physical >= 6 ? PC_CPU_TIER_FULL : PC_CPU_TIER_SMALL;
    t->power = PC_POWER_UNKNOWN;
    t->has_battery = -1;
    if (GetSystemPowerStatus(&ps)) {
        t->power = ps.ACLineStatus == 0 ? PC_POWER_BATTERY : ps.ACLineStatus == 1 ? PC_POWER_AC : PC_POWER_UNKNOWN;
        t->has_battery = ps.BatteryFlag == 255 ? -1 : (ps.BatteryFlag & 128) ? 0 : 1;
        t->saver = ps.SystemStatusFlag == 1;
    }
    pc_cpu_topo_api.role_enter = pc_thread_role_enter_impl;
    pc_cpu_topo_api.role_leave = pc_thread_role_leave_impl;
    pc_cpu_topo_api.topo = t;
    fprintf(stderr, "[CPU-TOPO] logical=%u physical=%u p=%u/%u e=%u/%u classes=%u pclass=%u/%u llc=%u system=%u hybrid=%d "
            "groups=%u mask=%llX source=%s tier=%s power=%s battery=%s saver=%d\n",
            t->logical, t->physical, t->p_logical, t->p_physical, t->e_logical, t->e_physical, t->classes,
            t->p_class, t->system_p_class, t->llc,
            t->system_logical, t->system_classes > 1, t->groups, t->mask,
            t->source == PC_CPU_SRC_CPUSETS ? "cpusets" : t->source == PC_CPU_SRC_GLPI ? "glpi" : "mask",
            pc_cpu_tier_name(t->tier),
            t->power == PC_POWER_AC ? "ac" : t->power == PC_POWER_BATTERY ? "battery" : "unknown",
            t->has_battery == 1 ? "yes" : t->has_battery == 0 ? "no" : "unknown", t->saver);
    if (t->tier == PC_CPU_TIER_FLOOR)
        fprintf(stderr, "[CPU-TOPO] below 4-thread floor (logical=%u): thread placement stays off\n", t->logical);
}

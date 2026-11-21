/* Bot flight recorder: the whole state of every bot, every recorded frame, for the whole match.
 * #included into ma2_bots_core.c after ma2_bot_ctrl.c (it reads Bot, BotCtrl, g_rt, g_drv_line).
 *
 * design note: record every bot routine, every input and every
 * position for all bots, so the full picture of any moment
 * where something goes wrong can be seen. The 11-01 VTOL crawl could not be traced: the text lines held the
 * bot's side only, never the game's own state.
 *
 * Each recorded frame (RECOMP_BOT_REC_HZ per second of game time, default 30) stores, raw:
 *   the world view the bots read (Ma2World: every player, vehicle, pickup, objective, projectile,
 *   bomb and death blast); per bot the whole Bot (order, controller status, BotCtrl) and its brain;
 *   per bot and per human the game's own actor, AI, movement / flight component (actor+0x8C) and
 *   player objects, and for humans the input behaviour (player+0x2F0: the command records the pad
 *   fills; a bot's are its own commands, kept exactly below);
 *   the brain's shared team tables (lifts, flag runs, musters ...).
 * Between recorded frames nothing the bots do is lost: every command value sent (ctrl_send_frame),
 * every drive call (g_drv_line) and every c7_add (each behaviour that acts counts there) is summed
 * per bot into the next frame; the brain's one-line status is stored when it changes.
 * Blobs are deltas against the previous frame's copy (unchanged runs skipped), keyframed every
 * 10 s. A writer thread packs ~1 s chunks with MSZIP (cabinet.dll; ~3x, ~4 MB per minute of a 5v6
 * match) and appends them, so a crash or freeze loses at most the last second. Its cost per frame is
 * [C7] rec rec_ms.
 *
 * File: the game log's name with .botrec (next to it), else %LOCALAPPDATA%\MechAssault2\Logs\
 * <time>-<pid>.botrec, or RECOMP_BOT_REC_PATH; the newest 4 dated recordings in that folder are
 * kept; RECOMP_BOT_REC_MAX_MB (default 150, about 25 minutes) caps one file; worst case 600 MB. Switch RECOMP_BOT_REC / [Bots]
 * BotRecorder (default 1). Decoder: tools/botrec.py (info / at / find / series / diff / events). */
#include <io.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);

enum { REC_INFO = 1, REC_STR, REC_MATCH, REC_FRAME, REC_BLOB, REC_EVENTS, REC_CMDS, REC_DRV, REC_TEXT, REC_END, REC_NOTE };
enum { RS_WORLD = 1, RS_BOT, RS_BRAIN, RS_ACTOR, RS_AI, RS_COMP, RS_PLAYER, RS_BEH, RS_TABLE, RS_KINDS };
#define REC_HUMANS      4
#define REC_SUBJ_GLOBAL (MAX_BOTS + REC_HUMANS)     /* bots 0..11, humans 12..15, the world 16 */
#define REC_SUBJ        (REC_SUBJ_GLOBAL + 1)
#define REC_TABLES      32
#define REC_EV_IDS      (C7_PIECES * C7_COUNTERS)
#define REC_BUF_MAX     ((size_t)48 << 20)         /* the writer has fallen this far behind: drop */
static const uint32_t k_rec_gsize[RS_KINDS] = { 0, 0, 0, 0, 0x800, 0x400, 0x400, 0x600, 0x700, 0 };   /* guest bytes kept per object */

typedef struct { uint32_t addr, size, cap; int seen; uint8_t *prev; } RecPrev;
typedef struct {
    uint16_t steps[61], nsteps; float last[61], lo[61], hi[61];     /* commands sent since the last frame */
    int drv_line[8]; char drv_kind[8]; uint16_t drv_n[8]; int ndrv;  /* drive calls since the last frame */
    float *ev_sum; uint16_t *ev_calls; uint16_t ev_touched[1024]; int nev;   /* c7_add since the last frame */
    char desc[256];
    uint32_t ai_actor, ai;                                           /* actor_ai cache */
} RecAcc;

typedef BOOL (WINAPI *RecCreateCompressorFn)(DWORD, void *, void **);
typedef BOOL (WINAPI *RecCompressFn)(void *, const void *, SIZE_T, void *, SIZE_T, SIZE_T *);

static struct {
    int state;                         /* 0 not opened yet, 1 recording, -1 off / failed / capped */
    double hz, next_t, next_key; int due, world_done, key, key_all;
    HANDLE file, thread, wake; volatile LONG stop;
    CRITICAL_SECTION cs, wcs;          /* cs: the chunk being gathered; wcs: the writer */
    uint8_t *buf, *wbuf, *out; size_t len, cap, wcap, ocap;
    uint64_t written, raw, dropped, dropped_noted, max_bytes;
    void *comp; RecCompressFn compress; int algo;
    RecPrev prev[REC_SUBJ][RS_KINDS], tprev[REC_TABLES];
    RecAcc acc[REC_SUBJ];
    uint8_t ev_named[REC_EV_IDS / 8];
    const void *tptr[REC_TABLES]; size_t tsize[REC_TABLES]; const char *tname[REC_TABLES]; int ntables;
    char path[MAX_PATH];
} g_rec;

/* ---- record building (game thread) ---- */
static uint8_t *g_rb; static size_t g_rbn, g_rbcap; static int g_rb_bad;
static void rb_need(size_t n)
{
    if (g_rbn + n <= g_rbcap) return;
    size_t c = g_rbcap ? g_rbcap : 65536;
    while (c < g_rbn + n) c *= 2;
    uint8_t *p = (uint8_t *)realloc(g_rb, c);
    if (!p) { g_rb_bad = 1; return; }
    g_rb = p; g_rbcap = c;
}
static void rb_put(const void *p, size_t n) { rb_need(n); if (g_rb_bad) return; memcpy(g_rb + g_rbn, p, n); g_rbn += n; }
static void rb_u8(uint8_t v) { rb_put(&v, 1); }
static void rb_u16(uint16_t v) { rb_put(&v, 2); }
static void rb_u32(uint32_t v) { rb_put(&v, 4); }
static void rb_f32(float v) { rb_put(&v, 4); }
static void rb_f64(double v) { rb_put(&v, 8); }
static void rb_var(uint32_t v) { while (v >= 0x80) { rb_u8((uint8_t)(v | 0x80)); v >>= 7; } rb_u8((uint8_t)v); }
static void rb_begin(int type, int sub, int id) { g_rbn = 0; g_rb_bad = 0; rb_u8((uint8_t)type); rb_u8((uint8_t)sub); rb_u16((uint16_t)id); rb_u32(0); }
static void rb_text(const char *s) { rb_put(s, strlen(s)); }
static void rb_commit(void)
{
    if (g_rb_bad || g_rbn < 8) { g_rbn = 0; return; }
    uint32_t n = (uint32_t)(g_rbn - 8); memcpy(g_rb + 4, &n, 4);
    EnterCriticalSection(&g_rec.cs);
    if (g_rec.len + g_rbn > g_rec.cap) {
        size_t c = g_rec.cap ? g_rec.cap : ((size_t)1 << 20);
        while (c < g_rec.len + g_rbn) c *= 2;
        uint8_t *q = c <= REC_BUF_MAX ? (uint8_t *)realloc(g_rec.buf, c) : NULL;
        if (!q) { g_rec.dropped += g_rbn; g_rec.key_all = 1; LeaveCriticalSection(&g_rec.cs); g_rbn = 0; return; }
        g_rec.buf = q; g_rec.cap = c;
    }
    memcpy(g_rec.buf + g_rec.len, g_rb, g_rbn); g_rec.len += g_rbn;
    LeaveCriticalSection(&g_rec.cs);
    g_rbn = 0;
}

/* ---- writer ---- */
static void rec_write_block(const uint8_t *b, size_t n)
{
    uint32_t hdr[4] = { 0x31425242u /* "BRB1" */, (uint32_t)n, (uint32_t)n, 0 };
    const uint8_t *data = b; SIZE_T got = 0;
    if (g_rec.compress) {
        size_t need = n + n / 8 + 65536;
        if (g_rec.ocap < need) { uint8_t *o = (uint8_t *)realloc(g_rec.out, need); if (o) { g_rec.out = o; g_rec.ocap = need; } }
        if (g_rec.ocap >= need && g_rec.compress(g_rec.comp, b, n, g_rec.out, g_rec.ocap, &got) && got > 0 && got < n) {
            data = g_rec.out; hdr[2] = (uint32_t)got; hdr[3] = (uint32_t)g_rec.algo;
        }
    }
    DWORD w = 0;
    WriteFile(g_rec.file, hdr, sizeof hdr, &w, NULL);
    WriteFile(g_rec.file, data, hdr[2], &w, NULL);
    g_rec.written += sizeof hdr + hdr[2]; g_rec.raw += n;
}
static void rec_drain(void)
{
    EnterCriticalSection(&g_rec.wcs);
    EnterCriticalSection(&g_rec.cs);
    uint8_t *b = g_rec.buf; size_t n = g_rec.len, cap = g_rec.cap;
    g_rec.buf = g_rec.wbuf; g_rec.cap = g_rec.wcap; g_rec.len = 0;
    g_rec.wbuf = b; g_rec.wcap = cap;
    LeaveCriticalSection(&g_rec.cs);
    if (n && g_rec.file) rec_write_block(b, n);
    if (g_rec.state == 1 && g_rec.written > g_rec.max_bytes) {
        g_rec.state = -1;
        fprintf(stderr, "[BOT-REC] size cap %.0f MB reached: recording stopped\n", g_rec.max_bytes / 1048576.0);
    }
    LeaveCriticalSection(&g_rec.wcs);
}
static DWORD WINAPI rec_thread(LPVOID unused)
{
    (void)unused;
    while (!g_rec.stop) { WaitForSingleObject(g_rec.wake, 1000); rec_drain(); }
    return 0;
}
static void rec_atexit(void)
{
    if (!g_rec.file) return;
    g_rec.stop = 1; SetEvent(g_rec.wake);
    rec_drain();
    FlushFileBuffers(g_rec.file);
    char shown[MAX_PATH * 2];
    fprintf(stderr, "[BOT-REC] closed %s: %.1f MB (%.1f MB raw), dropped %.0f KB\n", xbox_log_path(g_rec.path, shown, sizeof shown), g_rec.written / 1048576.0, g_rec.raw / 1048576.0, g_rec.dropped / 1024.0);
}

/* ---- file ---- */
static int rec_dated_name(const char *n)   /* YYYYMMDD-HHMMSS-<pid>.botrec */
{
    for (int i = 0; i < 15; i++) if (i == 8 ? n[i] != '-' : (n[i] < '0' || n[i] > '9')) return 0;
    if (n[15] != '-') return 0;
    int i = 16; while (n[i] >= '0' && n[i] <= '9') i++;
    return i > 16 && !strcmp(n + i, ".botrec");
}
static int rec_name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void rec_prune(const char *dir, int keep)
{
    char pat[MAX_PATH], p[MAX_PATH]; static char names[256][48]; int n = 0;
    WIN32_FIND_DATAA f;
    if (snprintf(pat, sizeof pat, "%s\\*.botrec", dir) >= (int)sizeof pat) return;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strlen(f.cFileName) < 48 && rec_dated_name(f.cFileName) && n < 256) strcpy(names[n++], f.cFileName); } while (FindNextFileA(h, &f));
    FindClose(h);
    qsort(names, (size_t)n, 48, rec_name_cmp);
    for (int i = 0; i < n - keep; i++) { snprintf(p, sizeof p, "%s\\%s", dir, names[i]); DeleteFileA(p); }
}
static void rec_make_path(char *out, size_t n)
{
    const char *e = getenv("RECOMP_BOT_REC_PATH");
    out[0] = 0;
    if (e && *e) { snprintf(out, n, "%s", e); return; }
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stderr));
    char p[MAX_PATH * 2];
    if (h != INVALID_HANDLE_VALUE && h && GetFileType(h) == FILE_TYPE_DISK && GetFinalPathNameByHandleA(h, p, sizeof p, 0) > 0) {
        const char *s = strncmp(p, "\\\\?\\", 4) ? p : p + 4;
        snprintf(out, n, "%s", s);
        char *dot = strrchr(out, '.'), *sl = strrchr(out, '\\');
        if (dot && (!sl || dot > sl)) *dot = 0;
        if (strlen(out) + 8 < n) strcat(out, ".botrec"); else out[0] = 0;
        return;
    }
    char d[MAX_PATH]; DWORD k = GetEnvironmentVariableA("LOCALAPPDATA", d, sizeof d);
    if (!k || k + 40 >= sizeof d) return;
    strcat(d, "\\MechAssault2"); CreateDirectoryA(d, NULL);
    strcat(d, "\\Logs"); CreateDirectoryA(d, NULL);
    SYSTEMTIME st; GetLocalTime(&st);
    snprintf(out, n, "%s\\%04u%02u%02u-%02u%02u%02u-%lu.botrec", d, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
}
static void rec_c7(const C7Counter *c, double n);
static void rec_open(void)
{
    g_rec.state = -1;
    if (!c7_on("rec", "RECOMP_BOT_REC", "BotRecorder", 1)) return;
    rec_make_path(g_rec.path, sizeof g_rec.path);
    if (!g_rec.path[0]) { fprintf(stderr, "[BOT-REC] no place to write: off\n"); return; }
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s", g_rec.path);
    char *sl = strrchr(dir, '\\'); if (sl) { *sl = 0; rec_prune(dir, 3); }   /* the newest 4 with this one (W-31) */
    g_rec.file = CreateFileA(g_rec.path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_rec.file == INVALID_HANDLE_VALUE) { g_rec.file = NULL; DWORD error = GetLastError(); char shown[MAX_PATH * 2]; fprintf(stderr, "[BOT-REC] cannot create %s (%lu): off\n", xbox_log_path(g_rec.path, shown, sizeof shown), error); return; }
    InitializeCriticalSection(&g_rec.cs); InitializeCriticalSection(&g_rec.wcs);
    const char *hz = getenv("RECOMP_BOT_REC_HZ"), *mb = getenv("RECOMP_BOT_REC_MAX_MB");
    g_rec.hz = hz && *hz ? atof(hz) : 30.0; if (g_rec.hz < 1.0) g_rec.hz = 1.0; if (g_rec.hz > 240.0) g_rec.hz = 240.0;
    g_rec.max_bytes = (uint64_t)((mb && *mb ? atof(mb) : 150.0) * 1048576.0);
    HMODULE cab = LoadLibraryA("cabinet.dll");
    RecCreateCompressorFn cc = cab ? (RecCreateCompressorFn)(void *)GetProcAddress(cab, "CreateCompressor") : NULL;
    g_rec.compress = cab ? (RecCompressFn)(void *)GetProcAddress(cab, "Compress") : NULL;
    if (cc && g_rec.compress && cc(2 /* COMPRESS_ALGORITHM_MSZIP: 3.0x at 11 ms a 320 KB chunk; XPRESS-Huffman 2.5x, LZMS 3.1x at 46 ms */, NULL, &g_rec.comp)) g_rec.algo = 2; else g_rec.compress = NULL;
    DWORD w = 0; WriteFile(g_rec.file, "MA2BREC1", 8, &w, NULL); g_rec.written = 8;
    g_rec.ntables = ma2_brain_rec_tables(g_rec.tptr, g_rec.tsize, g_rec.tname, REC_TABLES);
    g_rec.state = 1; g_rec.key_all = 1;
    /* 1.1.6: the exe and the recording's own path as the log shows them (%LOCALAPPDATA% etc. for the
     * profile folders), so a recording can go into a problem report (Report.exe scrubs it as well). */
    char mod_raw[MAX_PATH], mod[MAX_PATH * 2], path_shown[MAX_PATH * 2];
    GetModuleFileNameA(NULL, mod_raw, sizeof mod_raw);
    xbox_log_path(mod_raw, mod, sizeof mod); xbox_log_path(g_rec.path, path_shown, sizeof path_shown);
    char t[4096]; int k = snprintf(t, sizeof t,
        "version=1\nptr=%u\nhz=%.1f\nalgo=%d\nbuilt=%s %s\nexe=%s\npath=%s\nmax_bots=%d\nhumans=%d\nc7_counters=%d\n"
        "sizeof.Ma2World=%u\nsizeof.Bot=%u\nsizeof.BotCtrl=%u\nsizeof.Ma2Order=%u\nsizeof.Ma2CtrlStatus=%u\nsizeof.Ma2PlayerView=%u\nsizeof.Ma2Path=%u\nsizeof.Ma2Brain=%u\n"
        "offsetof.Bot.ctrl=%u\noffsetof.BotCtrl.val=%u\noffsetof.BotCtrl.vto_p0=%u\noffsetof.Ma2World.players=%u\noffsetof.Ma2World.blasts=%u\n"
        "guest.actor=%u\nguest.ai=%u\nguest.comp=%u\nguest.player=%u\nguest.beh=%u\n",
        (unsigned)sizeof(void *), g_rec.hz, g_rec.algo, __DATE__, __TIME__, mod, path_shown, MAX_BOTS, REC_HUMANS, C7_COUNTERS,
        (unsigned)sizeof(Ma2World), (unsigned)sizeof(Bot), (unsigned)sizeof(BotCtrl), (unsigned)sizeof(Ma2Order), (unsigned)sizeof(Ma2CtrlStatus),
        (unsigned)sizeof(Ma2PlayerView), (unsigned)sizeof(Ma2Path), (unsigned)ma2_brain_size(),
        (unsigned)offsetof(Bot, ctrl), (unsigned)offsetof(BotCtrl, val), (unsigned)offsetof(BotCtrl, vto_p0), (unsigned)offsetof(Ma2World, players), (unsigned)offsetof(Ma2World, blasts),
        k_rec_gsize[RS_ACTOR], k_rec_gsize[RS_AI], k_rec_gsize[RS_COMP], k_rec_gsize[RS_PLAYER], k_rec_gsize[RS_BEH]);
    for (int i = 0; i < g_rec.ntables && k > 0 && k < (int)sizeof t - 80; i++) k += snprintf(t + k, sizeof t - k, "table.%d=%s:%u\n", i, g_rec.tname[i], (unsigned)g_rec.tsize[i]);
    rb_begin(REC_INFO, 0, 0); rb_text(t); rb_commit();
    g_c7_rec = rec_c7;
    g_rec.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_rec.thread = CreateThread(NULL, 0, rec_thread, NULL, 0, NULL);
    atexit(rec_atexit);
    { char shown[MAX_PATH * 2]; fprintf(stderr, "[BOT-REC] recording to %s (%.0f Hz, %s)\n", xbox_log_path(g_rec.path, shown, sizeof shown), g_rec.hz, g_rec.algo ? "MSZIP" : "stored"); }
}

/* ---- blobs ---- */
/* Deltas against pv's copy: (unchanged run, changed run, bytes) pairs; key = against zeros. */
static void rec_blob(RecPrev *pv, int kind, int id, uint32_t addr, const uint8_t *data, uint32_t size)
{
    if (!data || !size) {
        if (pv->seen && !pv->size && !g_rec.key) return;   /* still gone */
        pv->seen = 1; pv->addr = addr; pv->size = 0;
        rb_begin(REC_BLOB, kind, id); rb_u32(addr); rb_u32(0); rb_u8(1); rb_commit();
        return;
    }
    int key = g_rec.key || !pv->seen || pv->addr != addr || pv->size != size;
    if (pv->cap < size) { uint8_t *q = (uint8_t *)realloc(pv->prev, size); if (!q) return; pv->prev = q; pv->cap = size; }
    if (key) memset(pv->prev, 0, size);
    pv->seen = 1; pv->addr = addr; pv->size = size;
    uint8_t *pr = pv->prev; int any = 0;
    rb_begin(REC_BLOB, kind, id); rb_u32(addr); rb_u32(size); rb_u8((uint8_t)key);
    __try {
        uint32_t i = 0;
        while (i < size) {
            uint32_t z = i;
            while (i < size && data[i] == pr[i]) i++;
            if (i >= size) break;
            uint32_t s0 = i, e = i;
            while (i < size) {
                if (data[i] != pr[i]) { e = ++i; continue; }
                if (i - e >= 4) break;       /* 4 unchanged bytes end the run */
                i++;
            }
            rb_var(s0 - z); rb_var(e - s0); rb_put(data + s0, e - s0);
            if (g_rb_bad) break;
            memcpy(pr + s0, g_rb + g_rbn - (e - s0), e - s0);   /* the bytes as stored (guest memory may move under us) */
            any = 1; i = e;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_rbn = 0; pv->seen = 0; return;
    }
    if (any || key) rb_commit(); else g_rbn = 0;
}
static void rec_guest(int kind, int s, uint32_t addr)
{
    uint32_t size = k_rec_gsize[kind];
    if (!addr || !gva_ok(addr, size)) { rec_blob(&g_rec.prev[s][kind], kind, s, 0, NULL, 0); return; }
    rec_blob(&g_rec.prev[s][kind], kind, s, addr, (const uint8_t *)((uintptr_t)addr + g_xbox_mem_offset), size);
}
static void rec_guest_set(int s, uint32_t a, uint32_t p)
{
    RecAcc *ac = &g_rec.acc[s];
    if (ac->ai_actor != a) { ac->ai_actor = a; ac->ai = a ? actor_ai(a) : 0; }
    rec_guest(RS_ACTOR, s, a);
    rec_guest(RS_AI, s, ac->ai);
    rec_guest(RS_COMP, s, a && gva_ok(a + 0x8C, 4) ? MEM32(a + 0x8C) : 0);
    rec_guest(RS_PLAYER, s, p);
    if (s >= MAX_BOTS) rec_guest(RS_BEH, s, p && gva_ok(p + 0x2F0, 4) ? MEM32(p + 0x2F0) : 0);   /* a bot's: its commands (REC_CMDS), 30% of the file */
}

/* ---- what happened between frames ---- */
static void rec_c7(const C7Counter *c, double n)
{
    if (g_rec.state != 1) return;
    ptrdiff_t pi = ((const char *)c - (const char *)g_c7.p) / (ptrdiff_t)sizeof(C7Piece);
    if (pi < 0 || pi >= C7_PIECES) return;
    ptrdiff_t ci = c - g_c7.p[pi].c;
    if (ci < 0 || ci >= C7_COUNTERS) return;
    int id = (int)(pi * C7_COUNTERS + ci);
    RecAcc *ac = &g_rec.acc[g_rec_cur >= 0 && g_rec_cur < MAX_BOTS ? g_rec_cur : REC_SUBJ_GLOBAL];
    if (!ac->ev_sum) {
        ac->ev_sum = (float *)calloc(REC_EV_IDS, sizeof(float)); ac->ev_calls = (uint16_t *)calloc(REC_EV_IDS, sizeof(uint16_t));
        if (!ac->ev_sum || !ac->ev_calls) { free(ac->ev_sum); free(ac->ev_calls); ac->ev_sum = NULL; ac->ev_calls = NULL; return; }
    }
    if (!ac->ev_calls[id]) { if (ac->nev >= 1024) return; ac->ev_touched[ac->nev++] = (uint16_t)id; }
    if (ac->ev_calls[id] < 65535) ac->ev_calls[id]++;
    ac->ev_sum[id] += (float)n;
}
static void rec_cmds(int index, const float *val)
{
    if (g_rec.state != 1 || index < 0 || index >= MAX_BOTS) return;
    RecAcc *ac = &g_rec.acc[index];
    if (ac->nsteps < 65535) ac->nsteps++;
    for (int i = 0; i < 61; i++) {
        float v = val[i];
        if (v == 0.f) continue;
        if (!ac->steps[i]) ac->lo[i] = ac->hi[i] = v;
        else { if (v < ac->lo[i]) ac->lo[i] = v; if (v > ac->hi[i]) ac->hi[i] = v; }
        if (ac->steps[i] < 65535) ac->steps[i]++;
        ac->last[i] = v;
    }
}
static void rec_flush(int s)
{
    RecAcc *ac = &g_rec.acc[s];
    if (ac->nev) {
        for (int i = 0; i < ac->nev; i++) {
            int id = ac->ev_touched[i];
            if (g_rec.ev_named[id >> 3] & (1 << (id & 7))) continue;
            g_rec.ev_named[id >> 3] |= (uint8_t)(1 << (id & 7));
            const C7Piece *p = &g_c7.p[id / C7_COUNTERS]; const C7Counter *c = &p->c[id % C7_COUNTERS];
            rb_begin(REC_STR, 0, id); rb_text(p->name ? p->name : "?"); rb_u8('\t'); rb_text(c->name ? c->name : "?"); rb_commit();
        }
        rb_begin(REC_EVENTS, 0, s); rb_u16((uint16_t)ac->nev);
        for (int i = 0; i < ac->nev; i++) {
            int id = ac->ev_touched[i];
            rb_u16((uint16_t)id); rb_u16(ac->ev_calls[id]); rb_f32(ac->ev_sum[id]);
            ac->ev_calls[id] = 0; ac->ev_sum[id] = 0.f;
        }
        rb_commit(); ac->nev = 0;
    }
    if (s < MAX_BOTS && ac->nsteps) {
        int n = 0; for (int i = 0; i < 61; i++) n += ac->steps[i] != 0;
        rb_begin(REC_CMDS, 0, s); rb_u16(ac->nsteps); rb_u8((uint8_t)n);
        for (int i = 0; i < 61; i++) if (ac->steps[i]) { rb_u8((uint8_t)i); rb_u16(ac->steps[i]); rb_f32(ac->last[i]); rb_f32(ac->lo[i]); rb_f32(ac->hi[i]); }
        rb_commit();
        memset(ac->steps, 0, sizeof ac->steps); ac->nsteps = 0;
    }
    if (s < MAX_BOTS && ac->ndrv) {
        rb_begin(REC_DRV, 0, s); rb_u8((uint8_t)ac->ndrv);
        for (int i = 0; i < ac->ndrv; i++) { rb_u8((uint8_t)ac->drv_kind[i]); rb_u32((uint32_t)ac->drv_line[i]); rb_u16(ac->drv_n[i]); }
        rb_commit(); ac->ndrv = 0;
    }
}

/* ---- hooks ---- */
static void rec_match_begin(void)
{
    if (!g_rec.state) rec_open();
    if (g_rec.state != 1) return;
    for (int s = 0; s < REC_SUBJ; s++) { for (int k = 0; k < RS_KINDS; k++) g_rec.prev[s][k].seen = 0; g_rec.acc[s].desc[0] = 0; g_rec.acc[s].ai_actor = 0; }
    for (int j = 0; j < REC_TABLES; j++) g_rec.tprev[j].seen = 0;
    memset(g_rec.ev_named, 0, sizeof g_rec.ev_named);
    g_rec.next_t = 0.0; g_rec.next_key = 0.0; g_rec.key_all = 1;
    char t[2048]; int k = snprintf(t, sizeof t, "t=%.3f\ngame_type=%d\nteam_mode=%d\nnbots=%d\nskill=%d\nnav=%s\n",
                                  MEMD(VA_GAME_TIME), g_rt.game_type, g_rt.team_mode, g_rt.nbots, g_rt.skill, g_rt.nav_source);
    for (int i = 0; i < g_rt.nbots && k > 0 && k < (int)sizeof t - 100; i++) {
        const Bot *b = &g_rt.bots[i];
        k += snprintf(t + k, sizeof t - k, "bot.%d=player:%08X port:%u team:%d slot:%d skill:%d\n", i, b->player, b->port, b->team, b->slot, b->skill);
    }
    rb_begin(REC_MATCH, 0, 0); rb_text(t); rb_commit();
}
static void rec_match_end(void)
{
    if (g_rec.state != 1) return;
    for (int s = 0; s < REC_SUBJ; s++) rec_flush(s);
    rb_begin(REC_END, 0, 0); rb_f64(MEMD(VA_GAME_TIME)); rb_commit();
    SetEvent(g_rec.wake);
}
/* First bot tick of a new game step (beside c7_frame): is a frame due? */
static void rec_step(double t, uint32_t step)
{
    g_rec.due = 0;
    if (g_rec.state != 1) return;
    if (t + 1.0 < g_rec.next_t) g_rec.next_t = 0.0;            /* the clock went back: a new match */
    if (t + 1.0 < g_rec.next_key - 10.0) g_rec.next_key = 0.0;
    if (t < g_rec.next_t) return;
    g_rec.next_t = (t - g_rec.next_t > 1.0 ? t : g_rec.next_t) + 1.0 / g_rec.hz;
    g_rec.key = g_rec.key_all || t >= g_rec.next_key;
    if (g_rec.key) { g_rec.key_all = 0; g_rec.next_key = t + 10.0; }
    g_rec.due = 1; g_rec.world_done = 0;
    static LARGE_INTEGER qf; LARGE_INTEGER q; if (!qf.QuadPart) QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q);
    rb_begin(REC_FRAME, g_rec.key, 0); rb_f64(t); rb_u32(step); rb_u32(MEM32(0x4E2430u)); rb_f64((double)q.QuadPart / (double)qf.QuadPart); rb_commit();
    if (g_rec.dropped != g_rec.dropped_noted) {
        char n[96]; snprintf(n, sizeof n, "dropped %.0f KB before this frame (the writer fell behind)", (g_rec.dropped - g_rec.dropped_noted) / 1024.0);
        g_rec.dropped_noted = g_rec.dropped;
        rb_begin(REC_NOTE, 0, 0); rb_text(n); rb_commit();
    }
}
/* After each bot's ctrl_tick: its drive call this step; on a due frame everything. */
static void rec_bot_step(int index, Bot *b, const Ma2World *w, int self)
{
    if (g_rec.state != 1 || index < 0 || index >= MAX_BOTS) return;
    RecAcc *ac = &g_rec.acc[index];
    if (g_drv_kind) {
        int i = 0; while (i < ac->ndrv && (ac->drv_line[i] != g_drv_line || ac->drv_kind[i] != g_drv_kind)) i++;
        if (i < ac->ndrv) { if (ac->drv_n[i] < 65535) ac->drv_n[i]++; }
        else if (ac->ndrv < 8) { ac->drv_line[i] = g_drv_line; ac->drv_kind[i] = g_drv_kind; ac->drv_n[i] = 1; ac->ndrv++; }
    }
    if (!g_rec.due) return;
    if (!g_rec.world_done) {
        g_rec.world_done = 1;
        rec_blob(&g_rec.prev[REC_SUBJ_GLOBAL][RS_WORLD], RS_WORLD, REC_SUBJ_GLOBAL, 0, (const uint8_t *)w, (uint32_t)sizeof *w);
        for (int j = 0; j < g_rec.ntables; j++) rec_blob(&g_rec.tprev[j], RS_TABLE, j, 0, (const uint8_t *)g_rec.tptr[j], (uint32_t)g_rec.tsize[j]);
        int h = 0;
        for (int i = 0; i < w->nplayers && h < REC_HUMANS; i++)
            if (w->players[i].is_human) { rec_guest_set(MAX_BOTS + h, w->players[i].actor, w->players[i].player); rec_flush(MAX_BOTS + h); h++; }
        rec_flush(REC_SUBJ_GLOBAL);
    }
    rec_blob(&g_rec.prev[index][RS_BOT], RS_BOT, index, b->player, (const uint8_t *)b, (uint32_t)sizeof *b);
    if (b->brain) rec_blob(&g_rec.prev[index][RS_BRAIN], RS_BRAIN, index, 0, (const uint8_t *)b->brain, (uint32_t)ma2_brain_size());
    rec_guest_set(index, self >= 0 ? w->players[self].actor : 0, b->player);
    if (b->brain) {
        char d[256]; ma2_brain_describe(b->brain, d, sizeof d);
        if (strcmp(d, ac->desc)) { snprintf(ac->desc, sizeof ac->desc, "%s", d); rb_begin(REC_TEXT, 1, index); rb_text(d); rb_commit(); }
    }
    rec_flush(index);
}

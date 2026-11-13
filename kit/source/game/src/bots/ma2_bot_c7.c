/* C7 diagnostics table (ma2_bot_c7.h). #included into ma2_bots_core.c; the functions have external
 * linkage so the brain (its own unit) logs through the same table.
 *
 * Lines: "[C7] <piece> switch=on|off" when a match starts (and when a piece first asks during one);
 * "[C7] <piece> t=<s> frames=<n> end=0|1 <counter>=<v> ..." every RECOMP_C7_EVERY seconds of match
 * time (default 60, 0 = off; running totals, end=0) and once at match end (end=1). A match that is
 * still running when the process exits is dumped from atexit. */
#include "ma2_bot_c7.h"
#include <windows.h>

int ma2_pc_ini_get_int(const char *section, const char *key, int fallback);   /* pc_settings.h */

enum { C7_PIECES = 384, C7_COUNTERS = 128 };   /* 256 ran out in the 11-08 objective-reach pass (StuckHull went unlogged); 48 ran out in batch 3, 96/24 in the push-surprise merge (127+ pieces; vtthreat 50+, hg_perch 24+, sp_mood 22 counters). ~1.7 MB BSS */
enum { C7_SUM, C7_MAX, C7_MS };

typedef struct C7Counter { const char *name; int kind; double v, frame, peak; } C7Counter;
typedef struct C7Piece {
    const char *name;
    int sw, sw_logged;            /* switch: -1 not asked yet; logged this match */
    const char *env, *ini; int def;
    int n; C7Counter c[C7_COUNTERS];
} C7Piece;

static struct {
    C7Piece p[C7_PIECES]; int n;
    int live, exit_hooked;        /* a match is being counted */
    double t, t_begin, next_dump;
    unsigned frames;
} g_c7;

static void (*g_c7_rec)(const C7Counter *c, double n);   /* the bot recorder (ma2_bot_rec.c): every add, by bot */

static int c7_streq(const char *a, const char *b) { return a == b || (a && b && strcmp(a, b) == 0); }

static C7Piece *c7_piece(const char *piece)
{
    for (int i = 0; i < g_c7.n; i++) if (g_c7.p[i].name == piece) return &g_c7.p[i];   /* same literal: no strcmp on the common path */
    for (int i = 0; i < g_c7.n; i++) if (c7_streq(g_c7.p[i].name, piece)) return &g_c7.p[i];
    if (g_c7.n >= C7_PIECES) { static int warned; if (!warned) { warned = 1; fprintf(stderr, "[C7] table full: pieces %d, '%s' dropped\n", C7_PIECES, piece ? piece : "?"); } return NULL; }
    C7Piece *p = &g_c7.p[g_c7.n++];
    memset(p, 0, sizeof *p); p->name = piece; p->sw = -1;
    return p;
}

static C7Counter *c7_counter(const char *piece, const char *counter, int kind)
{
    C7Piece *p = c7_piece(piece);
    if (!p) return NULL;
    for (int i = 0; i < p->n; i++) if (p->c[i].name == counter) return &p->c[i];
    for (int i = 0; i < p->n; i++) if (c7_streq(p->c[i].name, counter)) return &p->c[i];
    if (p->n >= C7_COUNTERS) { static int warned; if (!warned) { warned = 1; fprintf(stderr, "[C7] table full: piece '%s' counters %d, '%s' dropped\n", p->name, C7_COUNTERS, counter ? counter : "?"); } return NULL; }
    C7Counter *c = &p->c[p->n++];
    memset(c, 0, sizeof *c); c->name = counter; c->kind = kind;
    return c;
}

int c7_log_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_BOT_DIAG"); v = e && *e ? *e != '0' : ma2_pc_ini_get_int("Bots", "BotLog", 0) != 0; }
    return v;
}

int c7_on(const char *piece, const char *env, const char *ini_key, int def)
{
    C7Piece *p = c7_piece(piece);
    if (p && p->sw >= 0 && (p->sw_logged || !g_c7.live)) return p->sw;
    if (p) { p->env = env; p->ini = ini_key; p->def = def; }
    const char *e = env ? getenv(env) : NULL;
    int on = e && *e ? *e != '0' : ini_key ? ma2_pc_ini_get_int("Bots", ini_key, def) != 0 : def != 0;
    if (!p) return on;
    p->sw = on;
    if (g_c7.live && !p->sw_logged) {
        p->sw_logged = 1;
        if (c7_log_on()) fprintf(stderr, "[C7] %s switch=%s t=%.1f\n", piece, on ? "on" : "off", g_c7.t);
    }
    return on;
}

void c7_add(const char *piece, const char *counter, double n)
{
    C7Counter *c = c7_counter(piece, counter, C7_SUM);
    if (c) { c->v += n; if (g_c7_rec) g_c7_rec(c, n); }
}

void c7_max(const char *piece, const char *counter, double v)
{
    C7Counter *c = c7_counter(piece, counter, C7_MAX);
    if (c && v > c->v) c->v = v;
}

double c7_t0(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

void c7_ms(const char *piece, const char *counter, double t0)
{
    C7Counter *c = c7_counter(piece, counter, C7_MS);
    if (c) c->frame += c7_t0() - t0;
}

static void c7_dump(int end)
{
    if (!c7_log_on()) return;
    for (int i = 0; i < g_c7.n; i++) {
        const C7Piece *p = &g_c7.p[i];
        if (!p->n) continue;
        static char line[32768]; int k = snprintf(line, sizeof line, "[C7] %s t=%.0f frames=%u end=%d", p->name, g_c7.t, g_c7.frames, end);
        for (int j = 0; j < p->n && k > 0 && k < (int)sizeof line - 64; j++) {
            const C7Counter *c = &p->c[j];
            if (c->kind == C7_MS) {
                double tot = c->v + c->frame, peak = c->frame > c->peak ? c->frame : c->peak;
                k += snprintf(line + k, sizeof line - k, " %s_ms=%.4f %s_max=%.3f", c->name, g_c7.frames ? tot / g_c7.frames : tot, c->name, peak);
            } else if (c->v == (double)(long long)c->v)
                k += snprintf(line + k, sizeof line - k, " %s=%lld", c->name, (long long)c->v);
            else
                k += snprintf(line + k, sizeof line - k, " %s=%.3f", c->name, c->v);
        }
        fprintf(stderr, "%s\n", line);
    }
}

static void c7_atexit(void) { if (g_c7.live) c7_match_end(); }

void c7_match_end(void)
{
    if (!g_c7.live) return;
    c7_dump(1);
    fflush(stderr);
    for (int i = 0; i < g_c7.n; i++) { g_c7.p[i].n = 0; g_c7.p[i].sw_logged = 0; }
    g_c7.live = 0; g_c7.frames = 0;
}

void c7_match_begin(double t)
{
    c7_match_end();
    if (!g_c7.exit_hooked) { g_c7.exit_hooked = 1; atexit(c7_atexit); }
    g_c7.live = 1; g_c7.t = g_c7.t_begin = t; g_c7.frames = 0;
    int every = env_int("RECOMP_C7_EVERY", 60);
    g_c7.next_dump = every > 0 ? t + every : -1.0;
    for (int i = 0; i < g_c7.n; i++) { g_c7.p[i].n = 0; g_c7.p[i].sw_logged = 0; }
    if (c7_log_on()) fprintf(stderr, "[C7] match begin t=%.1f\n", t);
    for (int i = 0; i < g_c7.n; i++)              /* switches the pieces already asked for */
        if (g_c7.p[i].sw >= 0) c7_on(g_c7.p[i].name, g_c7.p[i].env, g_c7.p[i].ini, g_c7.p[i].def);
}

void c7_frame(double t)
{
    if (!g_c7.live) return;
    for (int i = 0; i < g_c7.n; i++)
        for (int j = 0; j < g_c7.p[i].n; j++) {
            C7Counter *c = &g_c7.p[i].c[j];
            if (c->kind != C7_MS) continue;
            c->v += c->frame; if (c->frame > c->peak) c->peak = c->frame; c->frame = 0.0;
        }
    g_c7.frames++;
    g_c7.t = t;
    if (g_c7.next_dump >= 0.0 && t >= g_c7.next_dump) {
        c7_dump(0);
        g_c7.next_dump = t + env_int("RECOMP_C7_EVERY", 60);
    }
}

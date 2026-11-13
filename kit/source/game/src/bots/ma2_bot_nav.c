/* MechAssault 2 offline bots - navigation module (path graph, A*, LOS).
 * Pure host C11. No guest memory access; heights only via Ma2GroundFn.
 * Implements the nav section of ../shared/ma2_bot_api.h. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "ma2_bot_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <windows.h>   /* W2: QueryPerformanceCounter for the sliced trav build */

#ifndef _WIN32
#include <strings.h>
#define _strnicmp strncasecmp
#endif

#define NAV_MIN_ISLAND     8      /* generated components smaller than this are dropped */
#define NAV_MAX_GEN_CELLS   (1 << 22)
#define NAV_SMOOTH_LOOKAHEAD 24

struct Ma2Nav {
    int       n;
    Ma2Vec3  *pos;
    int      *id;            /* original / saved id per node */
    int      *adj_start;     /* CSR, n+1 entries */
    int      *adj;           /* neighbour dense indices */
    float    *adj_pen;       /* per-edge extra cost, parallel to adj */
    int       n_edges;
    float     spacing;       /* typical node spacing (metres) */

    /* uniform spatial hash over x/z */
    float     hx0, hz0, hcell;
    int       hw, hh;
    int      *hstart;        /* hw*hh+1 */
    int      *hitems;        /* n */

    /* A* scratch */
    float    *g;
    int      *parent;
    unsigned *stamp;
    unsigned  cur_stamp;
    unsigned char *closed;
    struct { float f; int node; } *heap;
    int       heap_cap;

    int      *comp;          /* connected-component id per node (edges taken as undirected) */
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */
static float dist3(Ma2Vec3 a, Ma2Vec3 b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int edge_find(const Ma2Nav *nv, int a, int b)   /* index in adj or -1 */
{
    int i;
    for (i = nv->adj_start[a]; i < nv->adj_start[a + 1]; i++)
        if (nv->adj[i] == b) return i;
    return -1;
}

static void nav_free_arrays(Ma2Nav *nv)
{
    free(nv->pos); free(nv->id); free(nv->adj_start); free(nv->adj);
    free(nv->adj_pen); free(nv->hstart); free(nv->hitems);
    free(nv->comp); free(nv->g); free(nv->parent); free(nv->stamp); free(nv->closed); free(nv->heap);
}

void ma2_nav_free(Ma2Nav *nv)
{
    if (!nv) return;
    nav_free_arrays(nv);
    free(nv);
}

static Ma2Vec3 v3p(float x, float y, float z) { Ma2Vec3 r; r.x = x; r.y = y; r.z = z; return r; }


/* ================================================================== */
/* Traversability grid                                                 */
static unsigned g_tg_epoch;
unsigned ma2_trav_epoch(void) { return g_tg_epoch; }
/* ew-b8 (R1 P1-4): every epoch bump is logged with the cell rectangle that changed, so a bot (or a queued search) restarts only
 * if its route comes near the change. A bump with no recorded change (or a new map) is "everything". */
static struct { int valid, x0, z0, x1, z1; } g_dirty;
#define TG_CLOG 64
static struct { unsigned epoch; int full, x0, z0, x1, z1; } g_clog[TG_CLOG];
static unsigned g_clog_n, g_clog_floor;
static unsigned g_tg_mut;       /* ft-bots: counts every change to the blocked-cell maps (a sliced commit restarts if it moved) */
static void tg_dirty_add(int x0, int z0, int x1, int z1)
{
    g_tg_mut++;
    if (!g_dirty.valid) { g_dirty.valid = 1; g_dirty.x0 = x0; g_dirty.z0 = z0; g_dirty.x1 = x1; g_dirty.z1 = z1; return; }
    if (x0 < g_dirty.x0) g_dirty.x0 = x0;
    if (z0 < g_dirty.z0) g_dirty.z0 = z0;
    if (x1 > g_dirty.x1) g_dirty.x1 = x1;
    if (z1 > g_dirty.z1) g_dirty.z1 = z1;
}
static void tg_epoch_bump(int full)
{
    unsigned slot = g_clog_n % TG_CLOG;
    g_tg_epoch++;
    if (g_clog_n >= TG_CLOG) g_clog_floor = g_clog[slot].epoch;     /* the oldest entry is lost: queries older than it are "changed" */
    g_clog[slot].epoch = g_tg_epoch;
    g_clog[slot].full = full || !g_dirty.valid;
    g_clog[slot].x0 = g_dirty.x0; g_clog[slot].z0 = g_dirty.z0; g_clog[slot].x1 = g_dirty.x1; g_clog[slot].z1 = g_dirty.z1;
    g_clog_n++;
    g_dirty.valid = 0;
}
int ma2_trav_changed_since(unsigned since, float wx0, float wz0, float wx1, float wz1)
{
    unsigned n = g_clog_n < TG_CLOG ? g_clog_n : TG_CLOG, k;
    int x0, z0, x1, z1;
    if (since == g_tg_epoch) return 0;
    if (since < g_clog_floor || !(wx0 == wx0 && wz0 == wz0 && wx1 == wx1 && wz1 == wz1)) return 1;
    if (wx0 < -1e6f || wz0 < -1e6f || wx1 > 1e6f || wz1 > 1e6f) return 1;
    x0 = (int)floorf(wx0 / 2.0f) - 1; x1 = (int)ceilf(wx1 / 2.0f) + 1;
    z0 = (int)floorf(wz0 / 2.0f) - 1; z1 = (int)ceilf(wz1 / 2.0f) + 1;
    for (k = 0; k < n; k++) {
        if (g_clog[k].epoch <= since) continue;
        if (g_clog[k].full) return 1;
        if (g_clog[k].x1 >= x0 && g_clog[k].x0 <= x1 && g_clog[k].z1 >= z0 && g_clog[k].z0 <= z1) return 1;
    }
    return 0;
}
int ma2_trav_dirty_pending(void) { return g_dirty.valid; }
/* ================================================================== */
#define TG_CELL      2.0f       /* metres per grid cell */
#define TG_STEEP     0.80f      /* rise/run above which ground counts as a wall */
#define TG_OBST_H    2.5f       /* collidable objects taller than this (above ground) block */
#define TG_NLEARN    64
#define TG_STEP      (TG_STEEP * TG_CELL)   /* surface rise to a neighbouring cell above which it is a wall */
/* 'Mechs climb less: on New River City's red-base hill (1.1-1.3 m per 2 m cell) they walked up,
 * slid back and shuttled for a minute. Their map stops at about 29 degrees. */
#define TG_STEP_HEAVY 1.1f
#define TG_OBJ_MIN   0.4f       /* a collider top this far above the terrain is what one stands on there */

static struct TravGrid {
    int w, h, ready;
    unsigned char *blk;         /* 1 = blocked (object, cliff or off-map) */
    unsigned char *dist;        /* chamfer distance (5 per cell step) to the nearest blocked cell */
    unsigned char *slp;         /* rise/run * 100, capped at 255 */
    unsigned char *hblk;        /* blocked for a heavy body: small isolated obstacles (trees) are free */
    unsigned char *hdist;       /* chamfer distance to the nearest hard-blocked cell */
    float *th, *sf, *sh;        /* terrain, walkable surface, surface as a 'Mech sees it (-1e9 = off map) */
    unsigned char *water;       /* 1 = the probe met water first (the surface kept is the bed below) */
    int *comp, *hcomp;          /* connected walkable region of each cell (0 = blocked) */
    int n_soft;
    struct { Ma2Vec3 p; float r; double until; } learn[TG_NLEARN];
    int nlearn, learn_next;
    double learn_max_until;     /* ew-b8 B8.14: latest `until` among the marks (0 = none): after it the learn loop has nothing to do */
    int n_obst, n_steep;
} g_tg;

/* ew-b8 (R1 P1-1): a sliced reprobe job (a destroyed building = ~380 cells x 16 rays). */
static struct { int active, x0, x1, z0, z1, cx, cz, w, h; float px, pz, r; float (*probe)(void *, float, float, float, int *); void *ctx; unsigned cells; } g_rp;
int ma2_trav_ready(void) { return g_tg.ready; }
int ma2_trav_view_heavy(Ma2TravView *v)
{
    if (!g_tg.ready || !v || !g_tg.hblk) return ma2_trav_view(v);
    v->w = g_tg.w; v->h = g_tg.h; v->cell = TG_CELL; v->blk = g_tg.hblk; v->dist = g_tg.hdist; v->slp = g_tg.slp; v->comp = g_tg.hcomp;
    v->sf = g_tg.sh; v->water = g_tg.water; v->th = g_tg.th;
    return 1;
}
int ma2_trav_slope(float x, float z)   /* HPROBE: the cell's slope, rise/run x 100 (-1 off the grid) */
{
    int ix, iz;
    if (!g_tg.ready || !g_tg.slp) return -1;
    ix = (int)floorf(x / TG_CELL + 0.5f); iz = (int)floorf(z / TG_CELL + 0.5f);
    if (ix < 0 || iz < 0 || ix >= g_tg.w || iz >= g_tg.h) return -1;
    return g_tg.slp[iz * g_tg.w + ix];
}
float ma2_trav_clear_heavy(float x, float z)
{
    int ix, iz, i; float c;
    if (!g_tg.ready || !g_tg.hblk) return 99.0f;
    ix = (int)floorf(x / TG_CELL + 0.5f); iz = (int)floorf(z / TG_CELL + 0.5f);
    if (ix < 0 || iz < 0 || ix >= g_tg.w || iz >= g_tg.h) return 0.0f;
    i = iz * g_tg.w + ix;
    c = g_tg.hblk[i] ? 0.0f : (float)g_tg.hdist[i] * 0.4f - TG_CELL * 0.5f;
    return c < 0.0f ? 0.0f : c;
}
int ma2_trav_view(Ma2TravView *v)
{
    if (!g_tg.ready || !v) return 0;
    v->w = g_tg.w; v->h = g_tg.h; v->cell = TG_CELL; v->blk = g_tg.blk; v->dist = g_tg.dist; v->slp = g_tg.slp; v->comp = g_tg.comp;
    v->sf = g_tg.sf; v->water = g_tg.water; v->th = g_tg.th;
    return 1;
}

/* ft-bots: sliced commit job (see ma2_trav_commit_begin). Scratch copies of dist / hdist / comp / hcomp are computed a few ms per
 * call from the blocked-cell maps and copied over the live arrays at the end, together with the epoch bump. */
static struct {
    int active, stage, row, ci, cid, restarts, w, h; unsigned mut0, n_restart, n_fallback;
    unsigned char *dist[2]; int *comp[2]; size_t cap;
} g_cj;
static void tg_free(void)
{
    g_rp.active = 0; g_cj.active = 0;
    free(g_cj.dist[0]); free(g_cj.dist[1]); free(g_cj.comp[0]); free(g_cj.comp[1]);
    g_cj.dist[0] = g_cj.dist[1] = NULL; g_cj.comp[0] = g_cj.comp[1] = NULL; g_cj.cap = 0;
    free(g_tg.blk); free(g_tg.dist); free(g_tg.slp); free(g_tg.hblk); free(g_tg.hdist);
    free(g_tg.th); free(g_tg.sf); free(g_tg.sh); free(g_tg.comp); free(g_tg.hcomp); free(g_tg.water);
    memset(&g_tg, 0, sizeof g_tg);
}

/* Chamfer distance transform (5 per straight step, 7 diagonal) to the nearest blocked cell.
 * ft-bots: split into init / forward rows / backward rows so a sliced commit can run it a few rows at a time (same arithmetic). */
static void tg_chamfer_init(const unsigned char *blk, unsigned char *dist, int n)
{
    for (int i = 0; i < n; i++) dist[i] = blk[i] ? 0 : 255;
}
static void tg_chamfer_fwd_row(unsigned char *dist, int w, int z)
{
    for (int x = 0; x < w; x++) {
        int c = z * w + x, d = dist[c], t;
        if (!d) continue;
        if (x > 0)               { t = dist[c - 1] + 5; if (t < d) d = t; }
        if (z > 0)               { t = dist[c - w] + 5; if (t < d) d = t; }
        if (x > 0 && z > 0)      { t = dist[c - w - 1] + 7; if (t < d) d = t; }
        if (x < w - 1 && z > 0)  { t = dist[c - w + 1] + 7; if (t < d) d = t; }
        dist[c] = (unsigned char)(d > 255 ? 255 : d);
    }
}
static void tg_chamfer_bwd_row(unsigned char *dist, int w, int h, int z)
{
    for (int x = w - 1; x >= 0; x--) {
        int c = z * w + x, d = dist[c], t;
        if (!d) continue;
        if (x < w - 1)              { t = dist[c + 1] + 5; if (t < d) d = t; }
        if (z < h - 1)              { t = dist[c + w] + 5; if (t < d) d = t; }
        if (x < w - 1 && z < h - 1) { t = dist[c + w + 1] + 7; if (t < d) d = t; }
        if (x > 0 && z < h - 1)     { t = dist[c + w - 1] + 7; if (t < d) d = t; }
        dist[c] = (unsigned char)(d > 255 ? 255 : d);
    }
}
static void tg_chamfer(const unsigned char *blk, unsigned char *dist, int w, int h)
{
    tg_chamfer_init(blk, dist, w * h);
    for (int z = 0; z < h; z++) tg_chamfer_fwd_row(dist, w, z);
    for (int z = h - 1; z >= 0; z--) tg_chamfer_bwd_row(dist, w, h, z);
}

/* Blocked where the walkable surface steps more than TG_STEP to a 4-neighbour (a wall, a bridge's
 * side, a cliff, a pole); off-map cells are blocked. Writes slp for the full surface. Returns how
 * many cells changed in [x0..x1] x [z0..z1]. */
static int tg_steps(const float *s, unsigned char *out, int x0, int z0, int x1, int z1, int full, float step)
{
    static const int DX[4] = { -1, 1, 0, 0 }, DZ[4] = { 0, 0, -1, 1 };
    int w = g_tg.w, h = g_tg.h, x, z, k, changed = 0;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (z1 >= h) z1 = h - 1;
    for (z = z0; z <= z1; z++)
        for (x = x0; x <= x1; x++) {
            int c = z * w + x;
            unsigned char o;
            float m = 0.0f;
            if (s[c] < -1e8f) o = 1;
            else {
                for (k = 0; k < 4; k++) {
                    int nx = x + DX[k], nz = z + DZ[k], nc;
                    float d;
                    if (nx < 0 || nz < 0 || nx >= w || nz >= h) continue;
                    nc = nz * w + nx;
                    if (s[nc] < -1e8f) continue;
                    d = fabsf(s[nc] - s[c]);
                    if (d > m) m = d;
                }
                o = (unsigned char)(m > step);
                if (full) g_tg.slp[c] = (unsigned char)(m / TG_CELL * 100.0f > 255.0f ? 255 : (int)(m / TG_CELL * 100.0f));
            }
            if (out[c] != o) { out[c] = o; changed++; }
        }
    return changed;
}

/* Connected walkable regions (4-neighbour): a bridge deck joined by its ramps is one region with
 * the roads; a rooftop is a region of its own (only jets reach it). 0 = blocked.
 * ft-bots: resumable (tg_comp_step stops between seeds once the clock passes `stop_ms`; the ids come out in the same order). */
static double tg_now_ms(void)
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static int *tg_stack(int n)
{
    /* ew-b8 B8.13 (R1 E5): the flood-fill stack is kept between refreshes (it was a fresh 4 MB malloc, page faults and all, twice per commit) */
    static int *stk_buf; static int stk_cap;
    if (n > stk_cap) { free(stk_buf); stk_buf = (int *)malloc(sizeof(int) * (size_t)n); stk_cap = stk_buf ? n : 0; }
    return stk_buf;
}
static void tg_comp_init(const unsigned char *blk, int *comp, int n)
{
    for (int i = 0; i < n; i++) comp[i] = blk[i] ? 0 : -1;
}
/* *pi / *pid: scan position and last id (start at 0, 0). Returns 1 when every cell is labelled. stop_ms <= 0: run to the end. */
static int tg_comp_step(int *comp, int *pi, int *pid, double stop_ms)
{
    int w = g_tg.w, h = g_tg.h, n = w * h, id = *pid, i = *pi, scanned = 0;
    int *stk = tg_stack(n);
    if (!stk) { memset(comp, 0, sizeof(int) * (size_t)n); *pi = n; return 1; }
    for (; i < n; i++) {
        int sp = 0;
        if (comp[i] != -1) {
            if (stop_ms > 0.0 && ++scanned >= 16384) { scanned = 0; if (tg_now_ms() >= stop_ms) { *pi = i + 1; *pid = id; return 0; } }
            continue;
        }
        id++;
        comp[i] = id; stk[sp++] = i;
        while (sp) {
            int c = stk[--sp], cx = c % w, cz = c / w;
            if (cx > 0 && comp[c - 1] == -1) { comp[c - 1] = id; stk[sp++] = c - 1; }
            if (cx < w - 1 && comp[c + 1] == -1) { comp[c + 1] = id; stk[sp++] = c + 1; }
            if (cz > 0 && comp[c - w] == -1) { comp[c - w] = id; stk[sp++] = c - w; }
            if (cz < h - 1 && comp[c + w] == -1) { comp[c + w] = id; stk[sp++] = c + w; }
        }
        if (stop_ms > 0.0 && tg_now_ms() >= stop_ms) { *pi = i + 1; *pid = id; return 0; }
    }
    *pi = n; *pid = id;
    return 1;
}
static void tg_components(const unsigned char *blk, int *comp)
{
    int n = g_tg.w * g_tg.h, i = 0, id = 0;
    tg_comp_init(blk, comp, n);
    tg_comp_step(comp, &i, &id, 0.0);
}

/* The grid is a map of what one stands on: per cell the top of whatever is there -- a bridge deck,
 * a ramp, a roof, a rock -- or the terrain. Where that surface steps too sharply to a neighbour
 * it is a wall; so decks and ramps are walkable, their sides and the drops round them are not,
 * and a rooftop is walkable ground that only jets reach. Two surfaces: everything, and as a 'Mech
 * sees it (scenery it breaks by walking into it -- trees -- left out). */
/* W2 push (hitching): the build is resumable. ma2_trav_build_begin allocates; ma2_trav_build_step does cells
 * until budget_ms has passed (<= 0: all) and returns 1 when the grid is complete and ready; the result is
 * cell for cell the same as one ma2_trav_build call (which is begin + step(all)). */
int nv2a_hitch_enabled(void); void nv2a_hitch_note(const char *what, double ms);
static void nav_hn(const char *w) { if (nv2a_hitch_enabled()) nv2a_hitch_note(w, 0.0); }
static struct { float probe_top; Ma2GroundFn ground; void *gctx; Ma2ObstacleFn obst; void *octx; int next; int active; } g_tb;
#define TG_QUANT 0.00397f    /* not a power of two: collider tops are often dyadic (21.9316406 = 5614.5/256), which sat exactly on a rounding step */
static int g_tg_quant;                              /* NavCache on: surfaces are kept to 1/256 m (see the end of ma2_trav_build_step) */
void ma2_trav_set_quant(int on) { g_tg_quant = on; }
int ma2_trav_build_begin(float max_x, float max_z, float probe_top, Ma2GroundFn ground, void *gctx,
                         Ma2ObstacleFn obst, void *octx)
{
    int w, h;
    size_t n;
    tg_free();
    g_rp.active = 0;
    tg_epoch_bump(1);                               /* push-x3 E6: a new map's grid is a new epoch (queued searches restart) */
    g_tb.active = 0;
    if (!ground || max_x <= 0.0f || max_z <= 0.0f) return 0;
    w = (int)floorf(max_x / TG_CELL) + 1;
    h = (int)floorf(max_z / TG_CELL) + 1;
    if ((double)w * h > 6.0e6) return 0;
    n = (size_t)w * h;
    g_tg.th = (float *)malloc(n * sizeof(float)); g_tg.sf = (float *)malloc(n * sizeof(float)); g_tg.sh = (float *)malloc(n * sizeof(float));
    g_tg.blk = (unsigned char *)calloc(n, 1); g_tg.dist = (unsigned char *)malloc(n); g_tg.slp = (unsigned char *)calloc(n, 1);
    g_tg.hblk = (unsigned char *)calloc(n, 1); g_tg.hdist = (unsigned char *)malloc(n);
    g_tg.comp = (int *)malloc(n * sizeof(int)); g_tg.hcomp = (int *)malloc(n * sizeof(int));
    g_tg.water = (unsigned char *)calloc(n, 1);
    if (!g_tg.th || !g_tg.sf || !g_tg.sh || !g_tg.blk || !g_tg.dist || !g_tg.slp || !g_tg.hblk || !g_tg.hdist || !g_tg.comp || !g_tg.hcomp || !g_tg.water) { tg_free(); return 0; }
    g_tg.w = w; g_tg.h = h;
    g_tb.probe_top = probe_top; g_tb.ground = ground; g_tb.gctx = gctx; g_tb.obst = obst; g_tb.octx = octx; g_tb.next = 0; g_tb.active = 1;
    return 1;
}
int ma2_trav_build_step(double budget_ms)
{
    LARGE_INTEGER qf, q0, q1;
    int w = g_tg.w, h = g_tg.h, total = w * h, lim_check = 0;
    if (!g_tb.active) return 1;
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    for (; g_tb.next < total; g_tb.next++) {
        int c = g_tb.next, x = c % w, z = c / w, hit = 0, ok = 0;
        float t, top;
        if (budget_ms > 0.0 && (++lim_check & 63) == 0) {
            QueryPerformanceCounter(&q1);
            if ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart >= budget_ms) return 0;
        }
        t = g_tb.ground(g_tb.gctx, x * TG_CELL, g_tb.probe_top, z * TG_CELL, &hit);
        g_tg.th[c] = hit ? t : -1e9f;
        if (!hit) { g_tg.sf[c] = g_tg.sh[c] = -1e9f; continue; }
        g_tg.sf[c] = g_tg.sh[c] = t;
        if (!g_tb.obst) continue;
        top = g_tb.obst(g_tb.octx, x * TG_CELL, t + 40.0f, z * TG_CELL, &ok);
        if (ok == 3) { g_tg.water[c] = 1; continue; }        /* water: the bed stays the surface; drops never land here */
        if (!ok || top <= t + TG_OBJ_MIN) continue;
        g_tg.sf[c] = top;
        if (ok == 2) g_tg.n_soft++; else g_tg.sh[c] = top;
        if (top > t + TG_OBST_H) g_tg.n_obst++;
    }
    g_tb.active = 0;
    /* NavCache: a prop's collider top differs by a few ULP (4e-6 m on Bogged Down) from one run to the next -- moving or rotating scenery
     * sampled at another instant -- which would keep a cached grid from ever being byte-identical to a fresh one. Surfaces are kept to
     * 4 mm (TG_QUANT), far below anything a bot can tell; a value that straddles a rounding step is the only residue. */
    if (g_tg_quant) for (size_t i = 0; i < (size_t)total; i++) {
        if (g_tg.sf[i] > -1e8f) g_tg.sf[i] = floorf(g_tg.sf[i] / TG_QUANT + 0.5f) * TG_QUANT;
        if (g_tg.sh[i] > -1e8f) g_tg.sh[i] = floorf(g_tg.sh[i] / TG_QUANT + 0.5f) * TG_QUANT;
    }
    nav_hn("trav_finish_begin");
    tg_steps(g_tg.sf, g_tg.blk, 0, 0, w - 1, h - 1, 1, TG_STEP);
    tg_steps(g_tg.sh, g_tg.hblk, 0, 0, w - 1, h - 1, 0, TG_STEP_HEAVY);
    for (size_t i = 0; i < (size_t)total; i++) if (g_tg.blk[i]) g_tg.n_steep++;
    nav_hn("trav_steps_done");
    g_tg.ready = 1;
    ma2_trav_refresh();
    nav_hn("trav_refresh_done");
    return 1;
}
int ma2_trav_build(float max_x, float max_z, float probe_top, Ma2GroundFn ground, void *gctx,
                   Ma2ObstacleFn obst, void *octx)
{
    if (!ma2_trav_build_begin(max_x, max_z, probe_top, ground, gctx, obst, octx)) return 0;
    return ma2_trav_build_step(0.0);
}

void ma2_trav_block(float x, float z, int breakable)
{
    int ix, iz, i;
    if (!g_tg.ready) return;
    ix = (int)floorf(x / TG_CELL + 0.5f); iz = (int)floorf(z / TG_CELL + 0.5f);
    if (ix < 0 || iz < 0 || ix >= g_tg.w || iz >= g_tg.h) return;
    i = iz * g_tg.w + ix;
    if (!g_tg.blk[i]) { g_tg.blk[i] = 1; g_tg.n_obst++; }
    if (!breakable && g_tg.hblk) g_tg.hblk[i] = 1;
    tg_dirty_add(ix, iz, ix, iz);
}


/* Re-derive one cell of a reprobe region (shared by the synchronous and the sliced paths). */
static void tg_reprobe_cell(int x, int z, float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx)
{
    int i = z * g_tg.w + x, brk = 0;
    float dx = x * TG_CELL - cx, dz = z * TG_CELL - cz, t = g_tg.th[i], top;
    if (dx * dx + dz * dz > r * r || t < -1e8f) return;
    top = probe(ctx, x * TG_CELL, z * TG_CELL, t, &brk);
    g_tg.sf[i] = top > t + TG_OBJ_MIN ? top : t;
    g_tg.sh[i] = top > t + TG_OBJ_MIN && !brk ? top : t;
}
static int tg_reprobe_finish(int x0, int z0, int x1, int z1)
{
    int changed = tg_steps(g_tg.sf, g_tg.blk, x0 - 1, z0 - 1, x1 + 1, z1 + 1, 1, TG_STEP);
    changed += tg_steps(g_tg.sh, g_tg.hblk, x0 - 1, z0 - 1, x1 + 1, z1 + 1, 0, TG_STEP_HEAVY);
    if (changed) tg_dirty_add(x0 - 1, z0 - 1, x1 + 1, z1 + 1);      /* the surface changed here: routes through it are stale */
    return changed;
}
static int tg_reprobe_box(float cx, float cz, float r, int *x0, int *z0, int *x1, int *z1)
{
    if (!(cx == cx && cz == cz && r == r) || r > 1000.f) return 0;
    *x0 = (int)floorf((cx - r) / TG_CELL); *x1 = (int)ceilf((cx + r) / TG_CELL);
    *z0 = (int)floorf((cz - r) / TG_CELL); *z1 = (int)ceilf((cz + r) / TG_CELL);
    if (*x0 < 0) *x0 = 0;
    if (*z0 < 0) *z0 = 0;
    if (*x1 >= g_tg.w) *x1 = g_tg.w - 1;
    if (*z1 >= g_tg.h) *z1 = g_tg.h - 1;
    return *x1 >= *x0 && *z1 >= *z0;
}
/* nocommit != 0: leave the refresh to the caller (ma2_trav_commit_if_dirty), so a stall's reprobe + wall sweep + turret
 * restamp make ONE commit (ew-b8 B8.17). */
void ma2_trav_reprobe_ex(float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx, int nocommit)
{
    int x0, x1, z0, z1, x, z, changed;
    if (!g_tg.ready || !probe || !g_tg.sf) return;
    if (!tg_reprobe_box(cx, cz, r, &x0, &z0, &x1, &z1)) return;
    for (z = z0; z <= z1; z++)
        for (x = x0; x <= x1; x++) tg_reprobe_cell(x, z, cx, cz, r, probe, ctx);
    changed = tg_reprobe_finish(x0, z0, x1, z1);
    if (changed && !nocommit) ma2_trav_commit();
}
void ma2_trav_reprobe(float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx)
{
    ma2_trav_reprobe_ex(cx, cz, r, probe, ctx, 0);
}
/* Sliced version: begin once, then ma2_trav_reprobe_step(budget_ms) each frame. The cells are probed row by row (cell for
 * cell the same as the synchronous call); when the last one is done the surface steps are re-derived and the grid is left
 * dirty (the caller commits: ma2_trav_commit_if_dirty). Returns 1 when begun (0: no grid, or a job is running). */
int ma2_trav_reprobe_begin(float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx)
{
    if (!g_tg.ready || !probe || !g_tg.sf || g_rp.active) return 0;
    if (!tg_reprobe_box(cx, cz, r, &g_rp.x0, &g_rp.z0, &g_rp.x1, &g_rp.z1)) return 0;
    g_rp.px = cx; g_rp.pz = cz; g_rp.r = r; g_rp.probe = probe; g_rp.ctx = ctx;
    g_rp.cx = g_rp.x0; g_rp.cz = g_rp.z0; g_rp.w = g_tg.w; g_rp.h = g_tg.h; g_rp.cells = 0; g_rp.active = 1;
    return 1;
}
int ma2_trav_reprobe_busy(void) { return g_rp.active; }
void ma2_trav_reprobe_cancel(void) { g_rp.active = 0; }
/* 1 = the job finished this call (or was dropped because the grid changed size), 0 = more to do or no job.
 * *cells_done (optional) is added to with the cells probed in this call. */
int ma2_trav_reprobe_step(double budget_ms, unsigned *cells_done)
{
    LARGE_INTEGER qf, q0, q1;
    if (!g_rp.active) return 0;
    if (!g_tg.ready || g_tg.w != g_rp.w || g_tg.h != g_rp.h) { g_rp.active = 0; return 1; }
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    while (g_rp.cz <= g_rp.z1) {
        tg_reprobe_cell(g_rp.cx, g_rp.cz, g_rp.px, g_rp.pz, g_rp.r, g_rp.probe, g_rp.ctx);
        if (cells_done) (*cells_done)++;
        if (++g_rp.cx > g_rp.x1) { g_rp.cx = g_rp.x0; g_rp.cz++; }
        if (budget_ms > 0.0 && g_rp.cz <= g_rp.z1) {
            QueryPerformanceCounter(&q1);
            if ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart >= budget_ms) return 0;
        }
    }
    g_rp.active = 0;
    tg_reprobe_finish(g_rp.x0, g_rp.z0, g_rp.x1, g_rp.z1);
    return 1;
}

int ma2_trav_block_disc(float x, float z, float r)
{
    int x0, x1, z0, z1, ix, iz, n = 0;
    if (!g_tg.ready) return 0;
    x0 = (int)floorf((x - r) / TG_CELL); x1 = (int)ceilf((x + r) / TG_CELL);
    z0 = (int)floorf((z - r) / TG_CELL); z1 = (int)ceilf((z + r) / TG_CELL);
    for (iz = z0; iz <= z1; iz++)
        for (ix = x0; ix <= x1; ix++) {
            float dx = ix * TG_CELL - x, dz = iz * TG_CELL - z;
            int i;
            if (ix < 0 || iz < 0 || ix >= g_tg.w || iz >= g_tg.h || dx * dx + dz * dz > r * r) continue;
            i = iz * g_tg.w + ix;
            if (!g_tg.blk[i]) { g_tg.blk[i] = 1; n++; tg_dirty_add(ix, iz, ix, iz); }
            if (g_tg.hblk && !g_tg.hblk[i]) { g_tg.hblk[i] = 1; n++; tg_dirty_add(ix, iz, ix, iz); }
        }
    return n;
}
void ma2_trav_commit(void)
{
    ma2_trav_refresh();
    tg_epoch_bump(0);
}
/* ew-b8: commit only if some cell changed since the last commit (one refresh + one epoch bump however many pieces changed). */
int ma2_trav_commit_if_dirty(void)
{
    if (!g_dirty.valid) return 0;
    ma2_trav_commit();
    return 1;
}

void ma2_trav_refresh(void)
{
    if (!g_tg.ready) return;
    g_cj.active = 0;                                  /* ft-bots: a synchronous refresh supersedes a sliced commit in flight */
    tg_chamfer(g_tg.blk, g_tg.dist, g_tg.w, g_tg.h);
    if (g_tg.hblk && g_tg.hdist) tg_chamfer(g_tg.hblk, g_tg.hdist, g_tg.w, g_tg.h);
    if (g_tg.comp) tg_components(g_tg.blk, g_tg.comp);
    if (g_tg.hcomp && g_tg.hblk) tg_components(g_tg.hblk, g_tg.hcomp);
}

/* ft-bots (NRC stall ticks): the grid commit (chamfer x2 + components x2 over the whole grid: 9 ms on UC, up to 24 ms on NRC) as a
 * resumable job. ma2_trav_commit_begin starts it (only if some cell changed and none is running); ma2_trav_commit_step(budget_ms) does
 * a few ms of it and returns 1 when the grid has been committed (live arrays replaced, one epoch bump, as ma2_trav_commit_if_dirty
 * would have done), 0 while there is more. If a blocked cell changed meanwhile the job starts over (3 times), then commits at once. */
int ma2_trav_commit_busy(void) { return g_cj.active; }
void ma2_trav_commit_cancel(void) { g_cj.active = 0; }
void ma2_trav_commit_stats(unsigned *restarts, unsigned *fallbacks)
{
    if (restarts) *restarts = g_cj.n_restart;
    if (fallbacks) *fallbacks = g_cj.n_fallback;
}
int ma2_trav_commit_begin(void)
{
    size_t n;
    if (!g_tg.ready || g_cj.active || !g_dirty.valid) return 0;
    n = (size_t)g_tg.w * (size_t)g_tg.h;
    if (g_cj.cap < n) {
        free(g_cj.dist[0]); free(g_cj.dist[1]); free(g_cj.comp[0]); free(g_cj.comp[1]);
        g_cj.dist[0] = (unsigned char *)malloc(n); g_cj.dist[1] = (unsigned char *)malloc(n);
        g_cj.comp[0] = (int *)malloc(n * sizeof(int)); g_cj.comp[1] = (int *)malloc(n * sizeof(int));
        g_cj.cap = (g_cj.dist[0] && g_cj.dist[1] && g_cj.comp[0] && g_cj.comp[1]) ? n : 0;
        if (!g_cj.cap) { free(g_cj.dist[0]); free(g_cj.dist[1]); free(g_cj.comp[0]); free(g_cj.comp[1]);
                         g_cj.dist[0] = g_cj.dist[1] = NULL; g_cj.comp[0] = g_cj.comp[1] = NULL; return 0; }
    }
    g_cj.active = 1; g_cj.stage = 0; g_cj.row = 0; g_cj.restarts = 0; g_cj.mut0 = g_tg_mut; g_cj.w = g_tg.w; g_cj.h = g_tg.h;
    return 1;
}
int ma2_trav_commit_step(double budget_ms)
{
    double stop = budget_ms > 0.0 ? tg_now_ms() + budget_ms : 0.0;
    int w, h;
    if (!g_cj.active) return 0;
    if (!g_tg.ready || g_tg.w != g_cj.w || g_tg.h != g_cj.h) { g_cj.active = 0; return 1; }
    w = g_tg.w; h = g_tg.h;
    for (;;) {
        switch (g_cj.stage) {
        case 0: tg_chamfer_init(g_tg.blk, g_cj.dist[0], w * h); g_cj.stage = 1; g_cj.row = 0; break;
        case 1:
            while (g_cj.row < h) { tg_chamfer_fwd_row(g_cj.dist[0], w, g_cj.row++); if (stop > 0.0 && (g_cj.row & 15) == 0 && tg_now_ms() >= stop) return 0; }
            g_cj.stage = 2; g_cj.row = h - 1; break;
        case 2:
            while (g_cj.row >= 0) { tg_chamfer_bwd_row(g_cj.dist[0], w, h, g_cj.row--); if (stop > 0.0 && (g_cj.row & 15) == 0 && tg_now_ms() >= stop) return 0; }
            g_cj.stage = (g_tg.hblk && g_tg.hdist) ? 3 : 6; break;
        case 3: tg_chamfer_init(g_tg.hblk, g_cj.dist[1], w * h); g_cj.stage = 4; g_cj.row = 0; break;
        case 4:
            while (g_cj.row < h) { tg_chamfer_fwd_row(g_cj.dist[1], w, g_cj.row++); if (stop > 0.0 && (g_cj.row & 15) == 0 && tg_now_ms() >= stop) return 0; }
            g_cj.stage = 5; g_cj.row = h - 1; break;
        case 5:
            while (g_cj.row >= 0) { tg_chamfer_bwd_row(g_cj.dist[1], w, h, g_cj.row--); if (stop > 0.0 && (g_cj.row & 15) == 0 && tg_now_ms() >= stop) return 0; }
            g_cj.stage = 6; break;
        case 6:
            if (!g_tg.comp) { g_cj.stage = 8; break; }
            tg_comp_init(g_tg.blk, g_cj.comp[0], w * h); g_cj.ci = 0; g_cj.cid = 0; g_cj.stage = 7; break;
        case 7:
            if (!tg_comp_step(g_cj.comp[0], &g_cj.ci, &g_cj.cid, stop)) return 0;
            g_cj.stage = 8; break;
        case 8:
            if (!(g_tg.hcomp && g_tg.hblk)) { g_cj.stage = 10; break; }
            tg_comp_init(g_tg.hblk, g_cj.comp[1], w * h); g_cj.ci = 0; g_cj.cid = 0; g_cj.stage = 9; break;
        case 9:
            if (!tg_comp_step(g_cj.comp[1], &g_cj.ci, &g_cj.cid, stop)) return 0;
            g_cj.stage = 10; break;
        default: {   /* 10: done -- publish, unless a blocked cell changed while the scratch was being built */
            size_t n = (size_t)w * (size_t)h;
            if (g_tg_mut != g_cj.mut0) {
                g_cj.n_restart++;
                if (++g_cj.restarts > 3) { g_cj.n_fallback++; g_cj.active = 0; ma2_trav_commit(); return 1; }
                g_cj.mut0 = g_tg_mut; g_cj.stage = 0; g_cj.row = 0;
                return 0;
            }
            if (g_tg.dist) memcpy(g_tg.dist, g_cj.dist[0], n);
            if (g_tg.hblk && g_tg.hdist) memcpy(g_tg.hdist, g_cj.dist[1], n);
            if (g_tg.comp) memcpy(g_tg.comp, g_cj.comp[0], n * sizeof(int));
            if (g_tg.hcomp && g_tg.hblk) memcpy(g_tg.hcomp, g_cj.comp[1], n * sizeof(int));
            g_cj.active = 0;
            tg_epoch_bump(0);
            return 1;
        }
        }
        if (stop > 0.0 && tg_now_ms() >= stop) return 0;
    }
}

void ma2_trav_stats(int *w, int *h, int *nobst, int *nsteep)
{
    if (w) *w = g_tg.w;
    if (h) *h = g_tg.h;
    if (nobst) *nobst = g_tg.n_obst;
    if (nsteep) *nsteep = g_tg.n_steep;
}

void ma2_trav_dump_region(float x0, float z0, float x1, float z1)
{
    if (!g_tg.ready) return;
    int ix0 = (int)(x0 / TG_CELL), iz0 = (int)(z0 / TG_CELL), ix1 = (int)(x1 / TG_CELL), iz1 = (int)(z1 / TG_CELL);
    if (ix0 < 0) ix0 = 0; if (iz0 < 0) iz0 = 0; if (ix1 >= g_tg.w) ix1 = g_tg.w - 1; if (iz1 >= g_tg.h) iz1 = g_tg.h - 1;
    if (ix1 - ix0 > 200) ix1 = ix0 + 200;
    float lo = 1e9f;
    for (int z = iz0; z <= iz1; z++) for (int x = ix0; x <= ix1; x++) { float s = g_tg.sh[z * g_tg.w + x]; if (s > -1e8f && s < lo) lo = s; }
    fprintf(stderr, "[NAV-REGION] x %d..%d z %d..%d (cell %.0f m), heights from %.1f, 2 m per letter; rows from z1 down\n", ix0, ix1, iz0, iz1, TG_CELL, lo);
    char row[260];
    for (int pass = 0; pass < 3; pass++) {
        for (int z = iz1; z >= iz0; z--) {
            int n = 0;
            for (int x = ix0; x <= ix1 && n < 255; x++) {
                int c = z * g_tg.w + x; char ch;
                if (pass == 0) {
                    float s = g_tg.sh[c];
                    if (g_tg.hblk[c]) ch = '#';
                    else if (g_tg.blk[c]) ch = '+';
                    else if (g_tg.water && g_tg.water[c]) ch = '~';
                    else if (s < -1e8f) ch = ' ';
                    else { int k = (int)((s - lo) / 2.0f); ch = (char)('a' + (k < 0 ? 0 : k > 25 ? 25 : k)); }
                } else if (pass == 2) {   /* S: the collider top (or ground) per cell, 2 m per letter from the region's lowest ground, hard-blocked cells in capitals */
                    float s = g_tg.sf[c];
                    if (s < -1e8f) ch = ' ';
                    else { int k = (int)((s - lo) / 2.0f); ch = (char)((g_tg.hblk[c] ? 'A' : 'a') + (k < 0 ? 0 : k > 25 ? 25 : k)); }
                } else {
                    int d = g_tg.hblk[c] ? 0 : (int)((float)g_tg.hdist[c] * 0.4f - TG_CELL * 0.5f);
                    ch = d <= 0 ? '#' : d > 9 ? '+' : (char)('0' + d);
                }
                row[n++] = ch;
            }
            row[n] = 0;
            fprintf(stderr, "[NAV-REGION] %c z=%4.0f %s\n", pass == 2 ? 'S' : pass ? 'C' : 'H', z * TG_CELL, row);
        }
    }
}
int ma2_trav_dump(const char *path)
{
    FILE *f;
    int hdr[2];
    if (!g_tg.ready || !path) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    hdr[0] = g_tg.w; hdr[1] = g_tg.h;
    fwrite(hdr, 4, 2, f);
    fwrite(g_tg.blk, 1, (size_t)g_tg.w * g_tg.h, f);
    fwrite(g_tg.dist, 1, (size_t)g_tg.w * g_tg.h, f);
    fwrite(g_tg.slp, 1, (size_t)g_tg.w * g_tg.h, f);
    fclose(f);
    return 1;
}

/* ---- NavCache (1.0 release; USER-approved): the static part of the obstacle grid on disk ----
 * The grid takes 1.1-1.6 s to build at the start of every match (bots wait for it). Its result for one map and mode never changes,
 * so core (ma2_bots_core.c) keeps the base arrays in %LOCALAPPDATA%\MechAssault2\NavCache\ and reloads them. Only the arrays the
 * build writes are stored (terrain / surfaces / blocked maps / water / slope); distance maps and regions are re-derived by
 * ma2_trav_refresh. The key string (built by core: map, size, mode, grid dimensions, terrain and prop fingerprints, version)
 * must match exactly, the size must match and the payload hash must hold, else the file is rejected and the grid is built as always.
 * ma2_trav_cache_compare is the proof (RECOMP_BOT_NAV_CACHE_VERIFY): every cached array against a fresh build, byte for byte. */
#define NAVC_MAGIC 0x3143564Eu          /* "NVC1" */
typedef struct {
    uint32_t magic, keylen, extralen, w, h;
    int32_t  n_soft, n_obst, n_steep, spare[5];
    uint32_t pad[2];
    uint64_t phash;                     /* hash of everything after this header */
} NavcHdr;
typedef struct {
    const NavcHdr *h; const char *key; const unsigned char *extra, *th, *sf, *sh, *blk, *hblk, *water, *slp;
} NavcView;
const char *ma2_trav_cache_params(void)
{
    static char s[160];
    if (!s[0]) snprintf(s, sizeof s, "c%.2f,s%.2f,o%.2f,h%.2f,m%.2f,b%s %s", TG_CELL, TG_STEEP, TG_OBST_H, TG_STEP_HEAVY, TG_OBJ_MIN, __DATE__, __TIME__);
    return s;
}
static uint64_t navc_hash(const void *p, size_t n, uint64_t h)
{
    const unsigned char *b = (const unsigned char *)p; size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, b + i, 8); h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static size_t navc_total(const NavcHdr *h) { size_t n = (size_t)h->w * h->h; return sizeof(NavcHdr) + h->keylen + h->extralen + n * 16; }
static void navc_view(const unsigned char *buf, NavcView *v)
{
    size_t n = (size_t)((const NavcHdr *)buf)->w * ((const NavcHdr *)buf)->h;
    v->h = (const NavcHdr *)buf; v->key = (const char *)buf + sizeof(NavcHdr);
    v->extra = (const unsigned char *)v->key + v->h->keylen; v->th = v->extra + v->h->extralen;
    v->sf = v->th + n * 4; v->sh = v->sf + n * 4; v->blk = v->sh + n * 4; v->hblk = v->blk + n; v->water = v->hblk + n; v->slp = v->water + n;
}
static uint64_t navc_payload_hash(const char *key, uint32_t keylen, const void *extra, uint32_t extralen, size_t n,
                                  const void *th, const void *sf, const void *sh, const void *blk, const void *hblk, const void *water, const void *slp)
{
    uint64_t ph = 1469598103934665603ull;
    ph = navc_hash(key, keylen, ph); ph = navc_hash(extra, extralen, ph);
    ph = navc_hash(th, n * 4, ph); ph = navc_hash(sf, n * 4, ph); ph = navc_hash(sh, n * 4, ph);
    ph = navc_hash(blk, n, ph); ph = navc_hash(hblk, n, ph); ph = navc_hash(water, n, ph); ph = navc_hash(slp, n, ph);
    return ph;
}
static void navc_hdr_now(NavcHdr *h, uint32_t keylen, uint32_t extralen)
{
    memset(h, 0, sizeof *h);
    h->magic = NAVC_MAGIC; h->keylen = keylen; h->extralen = extralen; h->w = (uint32_t)g_tg.w; h->h = (uint32_t)g_tg.h;
    h->n_soft = g_tg.n_soft; h->n_obst = g_tg.n_obst; h->n_steep = g_tg.n_steep;
}
int ma2_trav_cache_save(const wchar_t *path, const char *key, const void *extra, uint32_t extralen)
{
    NavcHdr h; wchar_t tmp[MAX_PATH + 32]; FILE *f; size_t n; int ok;
    if (!g_tg.ready || !path || !key || extralen > 65536u) return 0;
    n = (size_t)g_tg.w * (size_t)g_tg.h;
    navc_hdr_now(&h, (uint32_t)strlen(key) + 1, extralen);
    h.phash = navc_payload_hash(key, h.keylen, extra, extralen, n, g_tg.th, g_tg.sf, g_tg.sh, g_tg.blk, g_tg.hblk, g_tg.water, g_tg.slp);
    swprintf(tmp, MAX_PATH + 32, L"%ls.%lu.tmp", path, (unsigned long)GetCurrentProcessId());
    f = _wfopen(tmp, L"wb");
    if (!f) return 0;
    ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(key, 1, h.keylen, f) == h.keylen && (!extralen || fwrite(extra, 1, extralen, f) == extralen) &&
         fwrite(g_tg.th, 4, n, f) == n && fwrite(g_tg.sf, 4, n, f) == n && fwrite(g_tg.sh, 4, n, f) == n &&
         fwrite(g_tg.blk, 1, n, f) == n && fwrite(g_tg.hblk, 1, n, f) == n && fwrite(g_tg.water, 1, n, f) == n && fwrite(g_tg.slp, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;     /* atomic: a reader sees the old file or the whole new one */
    if (!ok) DeleteFileW(tmp);
    return ok;
}
/* Reads and validates a cache file. Returns the file image (free with ma2_trav_cache_free), or NULL with *status 0 = no file,
 * 1 = rejected (any mismatch or damage). max_x / max_z are the ones the build would get (they fix the grid dimensions). */
void *ma2_trav_cache_read(const wchar_t *path, const char *key, float max_x, float max_z, int *status)
{
    FILE *f; long len; unsigned char *buf; NavcHdr h; NavcView v; size_t n; int w, hh;
    *status = 0;
    f = _wfopen(path, L"rb");
    if (!f) return NULL;
    *status = 1;
    if (fseek(f, 0, SEEK_END) || (len = ftell(f)) < (long)sizeof(NavcHdr) || len > (256L << 20) || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != NAVC_MAGIC || h.keylen != (uint32_t)strlen(key) + 1 || h.extralen > 65536u) { fclose(f); return NULL; }
    w = (int)floorf(max_x / TG_CELL) + 1; hh = (int)floorf(max_z / TG_CELL) + 1;
    if (max_x <= 0.0f || max_z <= 0.0f || (double)w * hh > 6.0e6 || h.w != (uint32_t)w || h.h != (uint32_t)hh || (size_t)len != navc_total(&h)) { fclose(f); return NULL; }
    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    memcpy(buf, &h, sizeof h);
    if (fread(buf + sizeof h, 1, (size_t)len - sizeof h, f) != (size_t)len - sizeof h) { fclose(f); free(buf); return NULL; }
    fclose(f);
    navc_view(buf, &v); n = (size_t)h.w * h.h;
    if (memcmp(v.key, key, h.keylen)) { free(buf); return NULL; }
    if (navc_payload_hash(v.key, h.keylen, v.extra, h.extralen, n, v.th, v.sf, v.sh, v.blk, v.hblk, v.water, v.slp) != h.phash) { free(buf); return NULL; }
    *status = 2;
    return buf;
}
void ma2_trav_cache_free(void *buf) { free(buf); }
/* Puts a validated cache image in place of ma2_trav_build_begin + steps + the prop pass. The caller's extra block (side-prop list)
 * goes to `extra`. Returns 1 when the grid is ready (epoch bumped once, like a new build), 0 = nothing installed. */
int ma2_trav_cache_install(const void *image, void *extra, uint32_t extra_cap, uint32_t *extralen)
{
    NavcView v; size_t n;
    if (!image) return 0;
    navc_view((const unsigned char *)image, &v); n = (size_t)v.h->w * v.h->h;
    if (v.h->extralen > extra_cap) return 0;
    tg_free();
    g_rp.active = 0;
    tg_epoch_bump(1);
    g_tb.active = 0;
    g_tg.th = (float *)malloc(n * sizeof(float)); g_tg.sf = (float *)malloc(n * sizeof(float)); g_tg.sh = (float *)malloc(n * sizeof(float));
    g_tg.blk = (unsigned char *)malloc(n); g_tg.dist = (unsigned char *)malloc(n); g_tg.slp = (unsigned char *)malloc(n);
    g_tg.hblk = (unsigned char *)malloc(n); g_tg.hdist = (unsigned char *)malloc(n);
    g_tg.comp = (int *)malloc(n * sizeof(int)); g_tg.hcomp = (int *)malloc(n * sizeof(int));
    g_tg.water = (unsigned char *)malloc(n);
    if (!g_tg.th || !g_tg.sf || !g_tg.sh || !g_tg.blk || !g_tg.dist || !g_tg.slp || !g_tg.hblk || !g_tg.hdist || !g_tg.comp || !g_tg.hcomp || !g_tg.water) { tg_free(); return 0; }
    memcpy(g_tg.th, v.th, n * 4); memcpy(g_tg.sf, v.sf, n * 4); memcpy(g_tg.sh, v.sh, n * 4);
    memcpy(g_tg.blk, v.blk, n); memcpy(g_tg.hblk, v.hblk, n); memcpy(g_tg.water, v.water, n); memcpy(g_tg.slp, v.slp, n);
    g_tg.w = (int)v.h->w; g_tg.h = (int)v.h->h;
    g_tg.n_soft = v.h->n_soft; g_tg.n_obst = v.h->n_obst; g_tg.n_steep = v.h->n_steep;
    g_tg.ready = 1;
    ma2_trav_refresh();
    /* the installed grid is fully derived (refresh above): nothing pending, so no redundant commit follows (dirty was cleared by the epoch bump) */
    if (extra && v.h->extralen) memcpy(extra, v.extra, v.h->extralen);
    if (extralen) *extralen = v.h->extralen;
    return 1;
}
/* Hash of the whole live grid (base arrays, counters and the derived distance / region maps): a miss run logs it at the save point
 * and a hit run at the install point, so the two can be compared across runs. */
uint64_t ma2_trav_state_hash(void)
{
    uint64_t h = 1469598103934665603ull; size_t n;
    if (!g_tg.ready) return 0;
    n = (size_t)g_tg.w * g_tg.h;
    h = navc_hash(&g_tg.w, sizeof g_tg.w, h); h = navc_hash(&g_tg.h, sizeof g_tg.h, h);
    h = navc_hash(&g_tg.n_soft, sizeof g_tg.n_soft, h); h = navc_hash(&g_tg.n_obst, sizeof g_tg.n_obst, h); h = navc_hash(&g_tg.n_steep, sizeof g_tg.n_steep, h);
    h = navc_hash(g_tg.th, n * 4, h); h = navc_hash(g_tg.sf, n * 4, h); h = navc_hash(g_tg.sh, n * 4, h);
    h = navc_hash(g_tg.blk, n, h); h = navc_hash(g_tg.hblk, n, h); h = navc_hash(g_tg.water, n, h); h = navc_hash(g_tg.slp, n, h);
    h = navc_hash(g_tg.dist, n, h); h = navc_hash(g_tg.hdist, n, h); h = navc_hash(g_tg.comp, n * 4, h); h = navc_hash(g_tg.hcomp, n * 4, h);
    return h;
}
void navcache_cell_probe(float x, float z);   /* ma2_bots_core.c: verify aid, which props stand near a differing cell */
/* Verify aid: how many cells of a float / byte array differ and the first few (x, z, cached, fresh). */
static void navc_report(const char *nm, const void *fresh, const void *cached, size_t n, int esz, int w)
{
    size_t i, cnt = 0; int shown = 0;
    for (i = 0; i < n; i++) {
        int d = esz == 4 ? memcmp((const char *)fresh + i * 4, (const char *)cached + i * 4, 4) : ((const unsigned char *)fresh)[i] != ((const unsigned char *)cached)[i];
        if (!d) continue;
        cnt++;
        if (shown < 8) {
            shown++;
            if (esz == 4) fprintf(stderr, "[BOT-NAV-CACHE]   %s differs at cell %d,%d (%.0f,%.0f m): cached %.7f fresh %.7f\n", nm, (int)(i % (size_t)w), (int)(i / (size_t)w), (double)(i % (size_t)w) * TG_CELL, (double)(i / (size_t)w) * TG_CELL, (double)((const float *)cached)[i], (double)((const float *)fresh)[i]);
            else fprintf(stderr, "[BOT-NAV-CACHE]   %s differs at cell %d,%d (%.0f,%.0f m): cached %d fresh %d\n", nm, (int)(i % (size_t)w), (int)(i / (size_t)w), (double)(i % (size_t)w) * TG_CELL, (double)(i / (size_t)w) * TG_CELL, ((const unsigned char *)cached)[i], ((const unsigned char *)fresh)[i]); navcache_cell_probe((float)(i % (size_t)w) * TG_CELL, (float)(i / (size_t)w) * TG_CELL);
        }
    }
    if (cnt) fprintf(stderr, "[BOT-NAV-CACHE]   %s: %zu of %zu cells differ\n", nm, cnt, n);
}
/* The proof: the live (freshly built) grid against a cache image, byte for byte -- counters, the caller's extra block, every base
 * array, and the derived distance / region maps re-derived from the cached blocked maps. Returns 1 when identical. *bytes = how
 * much was compared; *first_diff = offset of the first differing byte in that order (-1 = none), *where names the segment. */
int ma2_trav_cache_compare(const void *image, const void *extra, uint32_t extralen, long long *first_diff, long long *bytes, const char **where)
{
    NavcView v; NavcHdr now; size_t n; long long off = 0, first = -1; const char *wh = "";
    unsigned char *dd = NULL, *hd = NULL; int *cc = NULL, *hc = NULL;
    #define SEG(nm, a, b, len) do { size_t L_ = (size_t)(len); \
        if (first < 0 && L_ && memcmp((a), (b), L_)) { const unsigned char *pa_ = (const unsigned char *)(a), *pb_ = (const unsigned char *)(b); size_t k_ = 0; while (pa_[k_] == pb_[k_]) k_++; first = off + (long long)k_; wh = (nm); } \
        off += (long long)L_; } while (0)
    if (!image || !g_tg.ready) { if (first_diff) *first_diff = 0; if (bytes) *bytes = 0; if (where) *where = "none"; return 0; }
    navc_view((const unsigned char *)image, &v); n = (size_t)v.h->w * v.h->h;
    navc_hdr_now(&now, v.h->keylen, extralen);
    if (now.w != v.h->w || now.h != v.h->h) { if (first_diff) *first_diff = 0; if (bytes) *bytes = 0; if (where) *where = "dims"; return 0; }
    SEG("counters", &now.w, &v.h->w, 4 * 2);
    SEG("counters", &now.n_soft, &v.h->n_soft, 4 * 3);               /* n_soft, n_obst, n_steep */
    if (extralen != v.h->extralen) { first = off; wh = "extra_len"; }
    else SEG("extra", extra, v.extra, extralen);
    SEG("th", g_tg.th, v.th, n * 4); SEG("sf", g_tg.sf, v.sf, n * 4); SEG("sh", g_tg.sh, v.sh, n * 4);
    SEG("blk", g_tg.blk, v.blk, n); SEG("hblk", g_tg.hblk, v.hblk, n); SEG("water", g_tg.water, v.water, n); SEG("slp", g_tg.slp, v.slp, n);
    dd = (unsigned char *)malloc(n); hd = (unsigned char *)malloc(n); cc = (int *)malloc(n * sizeof(int)); hc = (int *)malloc(n * sizeof(int));
    if (dd && hd && cc && hc) {
        tg_chamfer(v.blk, dd, g_tg.w, g_tg.h); tg_chamfer(v.hblk, hd, g_tg.w, g_tg.h);
        tg_components(v.blk, cc); tg_components(v.hblk, hc);
        SEG("dist", g_tg.dist, dd, n); SEG("hdist", g_tg.hdist, hd, n); SEG("comp", g_tg.comp, cc, n * 4); SEG("hcomp", g_tg.hcomp, hc, n * 4);
    } else { if (first < 0) { first = off; wh = "no memory"; } }
    free(dd); free(hd); free(cc); free(hc);
    if (first >= 0) {
        navc_report("th", g_tg.th, v.th, n, 4, g_tg.w); navc_report("sf", g_tg.sf, v.sf, n, 4, g_tg.w); navc_report("sh", g_tg.sh, v.sh, n, 4, g_tg.w);
        navc_report("blk", g_tg.blk, v.blk, n, 1, g_tg.w); navc_report("hblk", g_tg.hblk, v.hblk, n, 1, g_tg.w);
    }
    #undef SEG
    if (first_diff) *first_diff = first;
    if (bytes) *bytes = off;
    if (where) *where = wh;
    return first < 0;
}

float ma2_trav_clear(float x, float z, double now)
{
    float c;
    int ix, iz, i;
    if (!g_tg.ready) return 99.0f;
    ix = (int)floorf(x / TG_CELL + 0.5f); iz = (int)floorf(z / TG_CELL + 0.5f);
    if (ix < 0 || iz < 0 || ix >= g_tg.w || iz >= g_tg.h) return 0.0f;
    i = iz * g_tg.w + ix;
    c = g_tg.blk[i] ? 0.0f : (float)g_tg.dist[i] * 0.4f - TG_CELL * 0.5f;
    if (c < 0.0f) c = 0.0f;
    if (now > 0.0 && now <= g_tg.learn_max_until)
    for (i = 0; i < g_tg.nlearn; i++) {
        float dx, dz, d;
        if (now <= 0.0 || now > g_tg.learn[i].until) continue;
        dx = x - g_tg.learn[i].p.x; dz = z - g_tg.learn[i].p.z;
        d = sqrtf(dx * dx + dz * dz) - g_tg.learn[i].r;
        if (d < c) c = d > 0.0f ? d : 0.0f;
    }
    return c;
}

static float tg_slope(float x, float z)
{
    int ix, iz;
    if (!g_tg.ready) return 0.0f;
    ix = (int)floorf(x / TG_CELL + 0.5f); iz = (int)floorf(z / TG_CELL + 0.5f);
    if (ix < 0 || iz < 0 || ix >= g_tg.w || iz >= g_tg.h) return 0.0f;
    return (float)g_tg.slp[iz * g_tg.w + ix] * 0.01f;
}

int ma2_trav_segment(Ma2Vec3 a, Ma2Vec3 b, float radius, double now)
{
    float dx = b.x - a.x, dz = b.z - a.z, len = sqrtf(dx * dx + dz * dz);
    int n, i;
    if (!g_tg.ready) return 1;
    n = (int)ceilf(len / 1.5f);
    for (i = 0; i <= n; i++) {
        float t = n ? (float)i / (float)n : 0.0f;
        if (t * len < 3.0f) continue;                  /* the start may already hug an obstacle */
        if (ma2_trav_clear(a.x + dx * t, a.z + dz * t, now) < radius) return 0;
    }
    return 1;
}

/* The same corridor test as a 'Mech sees it: breakable scenery does not block, stuck spots do. */
int ma2_trav_segment_heavy(Ma2Vec3 a, Ma2Vec3 b, float radius, double now)
{
    float dx = b.x - a.x, dz = b.z - a.z, len = sqrtf(dx * dx + dz * dz);
    int n, i, k;
    if (!g_tg.ready) return 1;
    if (!g_tg.hblk) return ma2_trav_segment(a, b, radius, now);
    n = (int)ceilf(len / 1.5f);
    for (i = 0; i <= n; i++) {
        float t = n ? (float)i / (float)n : 0.0f, x = a.x + dx * t, z = a.z + dz * t;
        if (t * len < 3.0f) continue;
        if (ma2_trav_clear_heavy(x, z) < radius) return 0;
        for (k = 0; k < g_tg.nlearn; k++) {
            float lx, lz;
            if (now <= 0.0 || now > g_tg.learn[k].until) continue;
            lx = x - g_tg.learn[k].p.x; lz = z - g_tg.learn[k].p.z;
            if (sqrtf(lx * lx + lz * lz) - g_tg.learn[k].r < radius) return 0;
        }
    }
    return 1;
}

/* Extra cost of walking a->b: steep ground and squeezing past obstacles. */
static float trav_extra(Ma2Vec3 a, Ma2Vec3 b)
{
    float dx = b.x - a.x, dz = b.z - a.z, len = sqrtf(dx * dx + dz * dz), sum = 0.0f;
    int n, i;
    if (!g_tg.ready || len < 0.1f) return 0.0f;
    n = (int)ceilf(len / TG_CELL);
    for (i = 0; i <= n; i++) {
        float t = (float)i / (float)n, x = a.x + dx * t, z = a.z + dz * t;
        float sl = tg_slope(x, z), cl = ma2_trav_clear(x, z, 0.0);
        float e = 0.0f;
        if (sl > 0.30f) e += 4.0f * (sl - 0.30f);
        if (cl < 10.0f) e += 0.12f * (10.0f - cl);
        sum += e * (len / (float)n);
    }
    return sum;
}

void ma2_trav_learn(Ma2Vec3 p, float radius, double now, double ttl)
{
    int i;
    if (!g_tg.ready) return;
    for (i = 0; i < g_tg.nlearn; i++) {              /* refresh a nearby mark */
        float dx = p.x - g_tg.learn[i].p.x, dz = p.z - g_tg.learn[i].p.z;
        if (dx * dx + dz * dz < 4.0f) { g_tg.learn[i].until = now + ttl; if (now + ttl > g_tg.learn_max_until) g_tg.learn_max_until = now + ttl; return; }
    }
    if (g_tg.nlearn < TG_NLEARN) i = g_tg.nlearn++;
    else { i = g_tg.learn_next; g_tg.learn_next = (g_tg.learn_next + 1) % TG_NLEARN; }
    g_tg.learn[i].p = p; g_tg.learn[i].r = radius; g_tg.learn[i].until = now + ttl;
    if (now + ttl > g_tg.learn_max_until) g_tg.learn_max_until = now + ttl;
}

static float tg_clear_len(Ma2Vec3 pos, float dx, float dz, float thresh, float look, double now)
{
    float d;
    for (d = 2.0f; d <= look; d += 2.0f)
        if (ma2_trav_clear(pos.x + dx * d, pos.z + dz * d, now) < thresh) return d - 2.0f;
    return look;
}

int ma2_trav_steer(Ma2TravSteer *st, Ma2Vec3 pos, Ma2Vec3 *dir, float radius, float look, double now)
{
    static const float DELTA[9] = { 0.0f, 0.35f, -0.35f, 0.70f, -0.70f, 1.05f, -1.05f, 1.45f, -1.45f };
    float c0, thresh, base, bestscore = -1e9f, bestL = 0.0f, bestd = 0.0f;
    int k;
    if (!g_tg.ready || !dir) return 0;
    c0 = ma2_trav_clear(pos.x, pos.z, now);
    thresh = radius;
    if (c0 < thresh) thresh = c0 - 0.3f > 0.4f ? c0 - 0.3f : 0.4f;       /* already squeezed in: do not get worse */
    base = atan2f(dir->x, dir->z);
    if (tg_clear_len(pos, dir->x, dir->z, thresh, look, now) >= look) {
        if (st && now > st->until) st->last_delta = 0.0f;
        if (st) st->steered = 0;
        return 0;
    }
    for (k = 0; k < 9; k++) {
        float a = base + DELTA[k], sx = sinf(a), cz = cosf(a);
        float L = tg_clear_len(pos, sx, cz, thresh, look, now);
        float score = L - 6.0f * fabsf(DELTA[k]);
        if (st && st->until > now && st->last_delta != 0.0f && DELTA[k] * st->last_delta > 0.0f) score += 3.0f;
        if (score > bestscore) { bestscore = score; bestL = L; bestd = DELTA[k]; }
    }
    if (bestL < 4.0f) { if (st) st->steered = 0; return 2; }
    {
        float a = base + bestd;
        dir->x = sinf(a); dir->z = cosf(a);
    }
    if (st && bestd != 0.0f) { st->last_delta = bestd; st->until = now + 1.2; st->steered = 1; }
    return bestd != 0.0f ? 1 : 0;
}

void ma2_nav_block_near(Ma2Nav *nv, Ma2Vec3 p, float radius, float extra)
{
    int i, e;
    float r2 = radius * radius;
    if (!nv || extra <= 0.0f) return;
    for (i = 0; i < nv->n; i++) {
        float dx = nv->pos[i].x - p.x, dz = nv->pos[i].z - p.z;
        if (dx * dx + dz * dz > r2) continue;
        for (e = nv->adj_start[i]; e < nv->adj_start[i + 1]; e++) {
            int j = nv->adj[e], f;
            nv->adj_pen[e] += extra;
            f = edge_find(nv, j, i);
            if (f >= 0) nv->adj_pen[f] += extra;
        }
    }
}

/* Build spatial hash, spacing, scratch buffers. pos/id/adj_start/adj must be set. */
static int nav_finalize(Ma2Nav *nv)
{
    int n = nv->n, i, e;
    float minx = FLT_MAX, minz = FLT_MAX, maxx = -FLT_MAX, maxz = -FLT_MAX;
    double sum = 0.0; int cnt = 0;

    nv->n_edges = nv->adj_start[n];
    nv->adj_pen = (float *)calloc((size_t)(nv->n_edges > 0 ? nv->n_edges : 1), sizeof(float));
    if (!nv->adj_pen) return 0;

    /* spacing = mean of each node's nearest-neighbour distance */
    for (i = 0; i < n; i++) {
        float best = FLT_MAX;
        for (e = nv->adj_start[i]; e < nv->adj_start[i + 1]; e++) {
            float d = dist3(nv->pos[i], nv->pos[nv->adj[e]]);
            if (d < best) best = d;
        }
        if (best < FLT_MAX) { sum += best; cnt++; }
        if (nv->pos[i].x < minx) minx = nv->pos[i].x;
        if (nv->pos[i].x > maxx) maxx = nv->pos[i].x;
        if (nv->pos[i].z < minz) minz = nv->pos[i].z;
        if (nv->pos[i].z > maxz) maxz = nv->pos[i].z;
    }
    nv->spacing = cnt ? (float)(sum / cnt) : 20.0f;
    if (nv->spacing < 0.5f) nv->spacing = 0.5f;

    if (n == 0) { minx = minz = maxx = maxz = 0.0f; }
    nv->hcell = nv->spacing * 2.0f;
    for (;;) {
        double w = floor((maxx - minx) / nv->hcell) + 1.0;
        double h = floor((maxz - minz) / nv->hcell) + 1.0;
        if (w * h <= (double)(1 << 20)) { nv->hw = (int)w; nv->hh = (int)h; break; }
        nv->hcell *= 1.5f;
    }
    nv->hx0 = minx; nv->hz0 = minz;
    nv->hstart = (int *)calloc((size_t)nv->hw * nv->hh + 2, sizeof(int));
    nv->hitems = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!nv->hstart || !nv->hitems) return 0;
    for (i = 0; i < n; i++) {
        int cx = (int)((nv->pos[i].x - minx) / nv->hcell);
        int cz = (int)((nv->pos[i].z - minz) / nv->hcell);
        nv->hstart[cz * nv->hw + cx + 1]++;
    }
    for (i = 0; i < nv->hw * nv->hh; i++) nv->hstart[i + 1] += nv->hstart[i];
    {
        int *fill = (int *)malloc((size_t)(nv->hw * nv->hh + 1) * sizeof(int));
        if (!fill) return 0;
        memcpy(fill, nv->hstart, (size_t)(nv->hw * nv->hh) * sizeof(int));
        for (i = 0; i < n; i++) {
            int cx = (int)((nv->pos[i].x - minx) / nv->hcell);
            int cz = (int)((nv->pos[i].z - minz) / nv->hcell);
            nv->hitems[fill[cz * nv->hw + cx]++] = i;
        }
        free(fill);
    }

    nv->g      = (float *)malloc((size_t)(n > 0 ? n : 1) * sizeof(float));
    nv->parent = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    nv->stamp  = (unsigned *)calloc((size_t)(n > 0 ? n : 1), sizeof(unsigned));
    nv->closed = (unsigned char *)calloc((size_t)(n > 0 ? n : 1), 1);
    nv->heap_cap = nv->n_edges + n + 16;
    nv->heap   = malloc((size_t)nv->heap_cap * sizeof(*nv->heap));
    nv->cur_stamp = 0;
    if (!(nv->g && nv->parent && nv->stamp && nv->closed && nv->heap)) return 0;
    /* connected components: union-find over the edges */
    nv->comp = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!nv->comp) return 0;
    for (i = 0; i < n; i++) nv->comp[i] = i;
    for (i = 0; i < n; i++)
        for (e = nv->adj_start[i]; e < nv->adj_start[i + 1]; e++) {
            int a = i, b = nv->adj[e];
            while (nv->comp[a] != a) { nv->comp[a] = nv->comp[nv->comp[a]]; a = nv->comp[a]; }
            while (nv->comp[b] != b) { nv->comp[b] = nv->comp[nv->comp[b]]; b = nv->comp[b]; }
            if (a != b) nv->comp[a < b ? b : a] = a < b ? a : b;
        }
    for (i = 0; i < n; i++) {
        int a = i;
        while (nv->comp[a] != a) a = nv->comp[a];
        nv->comp[i] = a;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* parser                                                              */
/* ------------------------------------------------------------------ */
typedef struct { int id; int idx; } IdPair;
static int idpair_cmp(const void *a, const void *b)
{
    int x = ((const IdPair *)a)->id, y = ((const IdPair *)b)->id;
    return (x > y) - (x < y);
}

Ma2Nav *ma2_nav_load_text(const char *text, size_t len)
{
    enum { SEC_NONE, SEC_PARAMS, SEC_NODES, SEC_OTHER } sec = SEC_NONE;
    size_t p = 0;
    int cap = 256, n = 0, ncap = 1024, nn = 0, i, e;
    int *ids = NULL, *nb_start = NULL, *nb_ids = NULL;
    Ma2Vec3 *pos = NULL;
    IdPair *pairs = NULL;
    Ma2Nav *nv = NULL;
    char line[1024];
    (void)cap;

    if (!text) return NULL;
    ids = (int *)malloc((size_t)cap * sizeof(int));
    pos = (Ma2Vec3 *)malloc((size_t)cap * sizeof(Ma2Vec3));
    nb_start = (int *)malloc((size_t)(cap + 1) * sizeof(int));
    nb_ids = (int *)malloc((size_t)ncap * sizeof(int));
    if (!ids || !pos || !nb_start || !nb_ids) goto fail;
    /* cap is reused as node capacity */
    cap = 256;

    while (p < len) {
        size_t s = p, l;
        char *c, *q;
        while (p < len && text[p] != '\n') p++;
        l = p - s;
        if (p < len) p++;
        if (l >= sizeof(line)) l = sizeof(line) - 1;
        memcpy(line, text + s, l);
        line[l] = 0;
        if ((c = strchr(line, ';')) != NULL) *c = 0;
        q = line;
        while (*q == ' ' || *q == '\t' || *q == '\r') q++;
        if (!*q) continue;
        if (*q == '[') {
            if (!_strnicmp(q, "[Nodes]", 7)) sec = SEC_NODES;
            else if (!_strnicmp(q, "[Params]", 8)) sec = SEC_PARAMS;
            else sec = SEC_OTHER;
            continue;
        }
        if (sec != SEC_NODES || *q != '=') continue;
        q++;
        {
            char *end;
            double v[8]; int k = 0;
            for (;;) {
                while (*q == ' ' || *q == '\t') q++;
                if (!*q || *q == '\r') break;
                if (k == 8) break;
                v[k] = strtod(q, &end);
                if (end == q) break;
                q = end; k++;
            }
            if (k >= 6) {                       /* node header */
                if (n == cap) {
                    int nc = cap * 2;
                    int *a = (int *)realloc(ids, (size_t)nc * sizeof(int));
                    Ma2Vec3 *b;
                    int *c2;
                    if (!a) goto fail; ids = a;
                    b = (Ma2Vec3 *)realloc(pos, (size_t)nc * sizeof(Ma2Vec3));
                    if (!b) goto fail; pos = b;
                    c2 = (int *)realloc(nb_start, (size_t)(nc + 1) * sizeof(int));
                    if (!c2) goto fail; nb_start = c2;
                    cap = nc;
                }
                ids[n] = (int)v[0];
                pos[n].x = (float)v[3]; pos[n].y = (float)v[4]; pos[n].z = (float)v[5];
                nb_start[n] = nn;
                n++;
            } else if (k == 1 && n > 0) {       /* neighbour of the latest node */
                if (nn == ncap) {
                    int *a;
                    ncap *= 2;
                    a = (int *)realloc(nb_ids, (size_t)ncap * sizeof(int));
                    if (!a) goto fail; nb_ids = a;
                }
                nb_ids[nn++] = (int)v[0];
            }
        }
    }
    nb_start[n] = nn;

    /* id -> dense index (first occurrence wins) */
    pairs = (IdPair *)malloc((size_t)(n > 0 ? n : 1) * sizeof(IdPair));
    if (!pairs) goto fail;
    for (i = 0; i < n; i++) { pairs[i].id = ids[i]; pairs[i].idx = i; }
    qsort(pairs, (size_t)n, sizeof(IdPair), idpair_cmp);

    nv = (Ma2Nav *)calloc(1, sizeof(*nv));
    if (!nv) goto fail;
    nv->n = n;
    nv->pos = pos; nv->id = ids;
    pos = NULL; ids = NULL;
    nv->adj_start = (int *)malloc((size_t)(n + 1) * sizeof(int));
    nv->adj = (int *)malloc((size_t)(nn > 0 ? nn : 1) * sizeof(int));
    if (!nv->adj_start || !nv->adj) goto fail;
    {
        int w = 0;
        for (i = 0; i < n; i++) {
            nv->adj_start[i] = w;
            for (e = nb_start[i]; e < nb_start[i + 1]; e++) {
                IdPair key, *f;
                int j, dup = 0;
                key.id = nb_ids[e]; key.idx = 0;
                f = (IdPair *)bsearch(&key, pairs, (size_t)n, sizeof(IdPair), idpair_cmp);
                if (!f) continue;
                /* first occurrence: bsearch may land on a later duplicate */
                { IdPair *b0 = pairs; while (f > b0 && (f - 1)->id == key.id) f--; }
                j = f->idx;
                if (j == i) continue;                       /* self link */
                { int k; for (k = nv->adj_start[i]; k < w; k++) if (nv->adj[k] == j) { dup = 1; break; } }
                if (dup) continue;
                nv->adj[w++] = j;
            }
        }
        nv->adj_start[n] = w;
    }
    free(pairs); free(nb_start); free(nb_ids);
    if (!nav_finalize(nv)) { ma2_nav_free(nv); return NULL; }
    return nv;

fail:
    free(ids); free(pos); free(nb_start); free(nb_ids); free(pairs);
    if (nv) ma2_nav_free(nv);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* save                                                                */
/* ------------------------------------------------------------------ */
int ma2_nav_save_text(const Ma2Nav *nv, const wchar_t *path)
{
    FILE *f;
    int i, e;
    if (!nv || !path) return 0;
#ifdef _WIN32
    f = _wfopen(path, L"wb");
#else
    {
        char mb[1024];
        if (wcstombs(mb, path, sizeof(mb)) == (size_t)-1) return 0;
        f = fopen(mb, "wb");
    }
#endif
    if (!f) return 0;
    fprintf(f, "[Params]\r\nNodes:\t  = %d\t\t\t\t\t\t\t\t\t\t\t\t; Number of nodes\r\n", nv->n);
    fprintf(f, "Quadrants: = 0\t\t\t\t\t\t\t\t\t\t\t\t; Number of quadrants\r\n\r\n[Nodes]\r\n");
    for (i = 0; i < nv->n; i++) {
        fprintf(f, " = %d 4 0 %.6f %.6f %.6f %d 0\r\n", nv->id[i],
                nv->pos[i].x, nv->pos[i].y, nv->pos[i].z,
                nv->adj_start[i + 1] - nv->adj_start[i]);
        for (e = nv->adj_start[i]; e < nv->adj_start[i + 1]; e++)
            fprintf(f, " = %d\r\n", nv->id[nv->adj[e]]);
    }
    fprintf(f, "\r\n[Quadrants]\r\n");
    return fclose(f) == 0;
}

/* ------------------------------------------------------------------ */
/* generator                                                           */
/* ------------------------------------------------------------------ */
Ma2Nav *ma2_nav_generate(const Ma2NavGenParams *p, Ma2GroundFn g, void *ctx)
{
    int cols, rows, ncell, i, x, z, k;
    float *h = NULL, *px = NULL, *pz = NULL; unsigned char *ok = NULL;
    int *comp = NULL, *dense = NULL, *queue = NULL, *csize = NULL;
    int ncomp = 0, n = 0, w = 0, trav = 0, n_moved = 0, n_drop = 0, n_cut = 0;
    Ma2Nav *nv = NULL;
    static const int DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int DZ[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    /* Unit the generated graph is made for: a 'Mech (radius ~4 m). Pilots and
     * tanks use the same graph; the brain's local steering handles the rest. */
    const float R_NODE = 3.5f, R_EDGE = 3.0f;

    if (!p || !g || p->cell <= 0.0f || p->max_x < p->min_x || p->max_z < p->min_z) return NULL;
    cols = (int)floorf((p->max_x - p->min_x) / p->cell) + 1;
    rows = (int)floorf((p->max_z - p->min_z) / p->cell) + 1;
    if ((double)cols * rows > (double)NAV_MAX_GEN_CELLS) return NULL;
    ncell = cols * rows;
    trav = ma2_trav_ready();

    h = (float *)malloc((size_t)ncell * sizeof(float));
    px = (float *)malloc((size_t)ncell * sizeof(float));
    pz = (float *)malloc((size_t)ncell * sizeof(float));
    ok = (unsigned char *)calloc((size_t)ncell, 1);
    comp = (int *)malloc((size_t)ncell * sizeof(int));
    dense = (int *)malloc((size_t)ncell * sizeof(int));
    queue = (int *)malloc((size_t)ncell * sizeof(int));
    if (!h || !px || !pz || !ok || !comp || !dense || !queue) goto done;

    for (z = 0; z < rows; z++)
        for (x = 0; x < cols; x++) {
            int hit = 0, c = z * cols + x;
            float wx = p->min_x + x * p->cell, wz = p->min_z + z * p->cell;
            if (trav && ma2_trav_clear(wx, wz, 0.0) < R_NODE) {
                /* the lattice point is on/against an obstacle: slide to the clearest spot nearby */
                float best = -1.0f, bx = wx, bz = wz;
                int ox, oz;
                for (oz = -4; oz <= 4; oz++)
                    for (ox = -4; ox <= 4; ox++) {
                        float cx = wx + ox * 2.0f, cz = wz + oz * 2.0f;
                        float cl = ma2_trav_clear(cx, cz, 0.0);
                        float sc = cl - 0.05f * sqrtf((float)(ox * ox + oz * oz));
                        if (cl >= R_NODE && sc > best) { best = sc; bx = cx; bz = cz; }
                    }
                if (best < 0.0f) { px[c] = wx; pz[c] = wz; h[c] = 0.0f; ok[c] = 0; n_drop++; continue; }
                wx = bx; wz = bz; n_moved++;
            }
            px[c] = wx; pz[c] = wz;
            h[c] = g(ctx, wx, p->probe_top, wz, &hit);
            ok[c] = (unsigned char)(hit ? 1 : 0);
        }

#define EDGE_OK(a, b) (fabsf(h[b] - h[a]) <= p->max_step && \
        (!trav || ma2_trav_segment(v3p(px[a], h[a], pz[a]), v3p(px[b], h[b], pz[b]), R_EDGE, 0.0)))

    /* connected components over walkable links */
    for (i = 0; i < ncell; i++) comp[i] = -1;
    csize = (int *)malloc((size_t)(ncell + 1) * sizeof(int));
    if (!csize) goto done;
    for (i = 0; i < ncell; i++) {
        int qh = 0, qt = 0;
        if (!ok[i] || comp[i] >= 0) continue;
        comp[i] = ncomp; queue[qt++] = i; csize[ncomp] = 0;
        while (qh < qt) {
            int c = queue[qh++], cx = c % cols, cz = c / cols;
            csize[ncomp]++;
            for (k = 0; k < 8; k++) {
                int nx = cx + DX[k], nz = cz + DZ[k], nc;
                if (nx < 0 || nz < 0 || nx >= cols || nz >= rows) continue;
                nc = nz * cols + nx;
                if (!ok[nc] || comp[nc] >= 0) continue;
                if (!EDGE_OK(c, nc)) continue;
                comp[nc] = ncomp; queue[qt++] = nc;
            }
        }
        ncomp++;
    }
    for (i = 0; i < ncell; i++) {
        dense[i] = -1;
        if (ok[i] && csize[comp[i]] >= NAV_MIN_ISLAND) dense[i] = n++;
    }

    nv = (Ma2Nav *)calloc(1, sizeof(*nv));
    if (!nv) goto done;
    nv->n = n;
    nv->pos = (Ma2Vec3 *)malloc((size_t)(n > 0 ? n : 1) * sizeof(Ma2Vec3));
    nv->id = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    nv->adj_start = (int *)malloc((size_t)(n + 1) * sizeof(int));
    nv->adj = (int *)malloc((size_t)(n * 8 > 0 ? n * 8 : 1) * sizeof(int));
    if (!nv->pos || !nv->id || !nv->adj_start || !nv->adj) { ma2_nav_free(nv); nv = NULL; goto done; }
    for (i = 0; i < ncell; i++) {
        int c;
        if (dense[i] < 0) continue;
        c = dense[i];
        nv->pos[c].x = px[i];
        nv->pos[c].y = h[i];
        nv->pos[c].z = pz[i];
        nv->id[c] = i;
    }
    for (i = 0; i < ncell; i++) {
        int cx, cz;
        if (dense[i] < 0) continue;
        cx = i % cols; cz = i / cols;
        nv->adj_start[dense[i]] = w;
        for (k = 0; k < 8; k++) {
            int nx = cx + DX[k], nz = cz + DZ[k], nc;
            if (nx < 0 || nz < 0 || nx >= cols || nz >= rows) continue;
            nc = nz * cols + nx;
            if (dense[nc] < 0) continue;
            if (!EDGE_OK(i, nc)) { n_cut++; continue; }
            nv->adj[w++] = dense[nc];
        }
    }
    nv->adj_start[n] = w;
    if (!nav_finalize(nv)) { ma2_nav_free(nv); nv = NULL; goto done; }
    if (trav) {
        int e, node;
        for (node = 0; node < n; node++)
            for (e = nv->adj_start[node]; e < nv->adj_start[node + 1]; e++)
                nv->adj_pen[e] = trav_extra(nv->pos[node], nv->pos[nv->adj[e]]);
        if (getenv("RECOMP_BOT_DIAG"))
            fprintf(stderr, "[BOT-NAV] generated with obstacle grid: nodes=%d (moved %d, dropped %d, components %d) edges=%d (cut %d)\n",
                    n, n_moved, n_drop, ncomp, w, n_cut);
    }
done:
    free(h); free(px); free(pz); free(ok); free(comp); free(dense); free(queue); free(csize);
    return nv;
#undef EDGE_OK
}

/* ------------------------------------------------------------------ */
/* queries                                                             */
/* ------------------------------------------------------------------ */
int ma2_nav_node_count(const Ma2Nav *nv) { return nv ? nv->n : 0; }

Ma2Vec3 ma2_nav_node_pos(const Ma2Nav *nv, int node)
{
    Ma2Vec3 z = {0, 0, 0};
    if (!nv || node < 0 || node >= nv->n) return z;
    return nv->pos[node];
}

static int nearest_in(const Ma2Nav *nv, Ma2Vec3 q, int comp)   /* comp < 0: any */
{
    int best = -1, r, cx, cz, maxr;
    float bestd = FLT_MAX, fx, fz;
    if (!nv || nv->n == 0) return -1;
    fx = floorf((q.x - nv->hx0) / nv->hcell);
    fz = floorf((q.z - nv->hz0) / nv->hcell);
    if (!(fx == fx && fz == fz)) return -1;                                  /* ew-b8 B8.9 (R5 E9): a NaN query */
    /* ew-b8 B8.9: clamp the start cell into the grid (a query at 1e7 m used to run millions of empty rings, or overflow the int cast).
     * The ring bound below still holds: the grid cell nearest q is where every ring is measured from, and a node's distance to q
     * is at least its distance to that cell's projection of q. */
    cx = fx < 0.f ? 0 : fx > (float)(nv->hw - 1) ? nv->hw - 1 : (int)fx;
    cz = fz < 0.f ? 0 : fz > (float)(nv->hh - 1) ? nv->hh - 1 : (int)fz;
    /* ring search; a ring r can only contain points >= (r-1)*hcell away */
    maxr = nv->hw > nv->hh ? nv->hw : nv->hh;
    for (r = 0; r <= maxr; r++) {
        int ix, iz;
        if (best >= 0 && (float)(r - 1) * nv->hcell > bestd) break;
        for (iz = cz - r; iz <= cz + r; iz++) {
            int edge_row = (iz == cz - r || iz == cz + r);
            if (iz < 0 || iz >= nv->hh) continue;
            for (ix = cx - r; ix <= cx + r; ix += (edge_row || r == 0) ? 1 : 2 * r) {
                int c, k;
                if (ix < 0 || ix >= nv->hw) continue;
                c = iz * nv->hw + ix;
                for (k = nv->hstart[c]; k < nv->hstart[c + 1]; k++) {
                    int nd = nv->hitems[k];
                    float d;
                    if (comp >= 0 && nv->comp[nd] != comp) continue;
                    d = dist3(q, nv->pos[nd]);
                    if (d < bestd) { bestd = d; best = nd; }
                }
            }
        }
    }
    return best;
}

int ma2_nav_nearest(const Ma2Nav *nv, Ma2Vec3 q) { return nearest_in(nv, q, -1); }

/* 1 when a and b snap to graph nodes of the same connected component. */
int ma2_nav_connected(const Ma2Nav *nv, Ma2Vec3 a, Ma2Vec3 b)
{
    int s, t;
    if (!nv || nv->n == 0) return 1;
    s = nearest_in(nv, a, -1); t = nearest_in(nv, b, -1);
    return s >= 0 && t >= 0 && nv->comp[s] == nv->comp[t];
}

/* binary min-heap */
static void heap_push(Ma2Nav *nv, int *sz, float f, int node)
{
    int i = (*sz)++;
    while (i > 0) {
        int par = (i - 1) >> 1;
        if (nv->heap[par].f <= f) break;
        nv->heap[i] = nv->heap[par];
        i = par;
    }
    nv->heap[i].f = f; nv->heap[i].node = node;
}

static int heap_pop(Ma2Nav *nv, int *sz)
{
    int top = nv->heap[0].node, i = 0, n = --(*sz);
    float f;
    int node;
    if (n == 0) return top;
    f = nv->heap[n].f; node = nv->heap[n].node;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && nv->heap[c + 1].f < nv->heap[c].f) c++;
        if (nv->heap[c].f >= f) break;
        nv->heap[i] = nv->heap[c];
        i = c;
    }
    nv->heap[i].f = f; nv->heap[i].node = node;
    return top;
}

/* edge cost: euclidean length plus a mild slope surcharge plus penalty.
 * Always >= euclidean length, so the euclidean heuristic is admissible. */
static float edge_cost(const Ma2Nav *nv, int a, int b, int e)
{
    float d = dist3(nv->pos[a], nv->pos[b]);
    float dy = fabsf(nv->pos[b].y - nv->pos[a].y);
    return d + 0.5f * dy + nv->adj_pen[e];
}

/* A* over dense indices. Returns node count in tmp (goal..start reversed to start..goal). */
static int astar(Ma2Nav *nv, int s, int t, int *tmp, int tmp_cap)
{
    int sz = 0, cnt = 0, v;
    Ma2Vec3 gp = nv->pos[t];
    if (++nv->cur_stamp == 0) {                 /* wrapped */
        memset(nv->stamp, 0, (size_t)nv->n * sizeof(unsigned));
        nv->cur_stamp = 1;
    }
    memset(nv->closed, 0, (size_t)nv->n);
    nv->stamp[s] = nv->cur_stamp; nv->g[s] = 0.0f; nv->parent[s] = -1;
    heap_push(nv, &sz, dist3(nv->pos[s], gp), s);
    while (sz > 0) {
        int u = heap_pop(nv, &sz), e;
        if (nv->closed[u]) continue;
        nv->closed[u] = 1;
        if (u == t) break;
        for (e = nv->adj_start[u]; e < nv->adj_start[u + 1]; e++) {
            int w = nv->adj[e];
            float ng;
            if (nv->closed[w]) continue;
            ng = nv->g[u] + edge_cost(nv, u, w, e);
            if (nv->stamp[w] != nv->cur_stamp || ng < nv->g[w]) {
                nv->stamp[w] = nv->cur_stamp;
                nv->g[w] = ng; nv->parent[w] = u;
                if (sz < nv->heap_cap) heap_push(nv, &sz, ng + dist3(nv->pos[w], gp), w);
            }
        }
    }
    if (nv->stamp[t] != nv->cur_stamp || !nv->closed[t]) return 0;
    for (v = t; v >= 0 && cnt < tmp_cap; v = nv->parent[v]) tmp[cnt++] = v;
    for (v = 0; v < cnt / 2; v++) { int x = tmp[v]; tmp[v] = tmp[cnt - 1 - v]; tmp[cnt - 1 - v] = x; }
    return cnt;
}

/* Is a straight segment a->b covered by existing nodes and edges? */
static int segment_walkable(const Ma2Nav *nv, Ma2Vec3 a, Ma2Vec3 b)
{
    if (ma2_trav_ready() && !ma2_trav_segment(a, b, 3.0f, 0.0)) return 0;
    float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    float stepd = nv->spacing * 0.5f;
    int ns = (int)ceilf(len / stepd), i, prev = -1;
    for (i = 0; i <= ns; i++) {
        float t = ns ? (float)i / ns : 0.0f;
        Ma2Vec3 s;
        int c;
        s.x = a.x + (b.x - a.x) * t; s.y = a.y + (b.y - a.y) * t; s.z = a.z + (b.z - a.z) * t;
        c = ma2_nav_nearest(nv, s);
        if (c < 0) return 0;
        {
            float hx = nv->pos[c].x - s.x, hz = nv->pos[c].z - s.z;
            if (sqrtf(hx * hx + hz * hz) > nv->spacing * 0.8f) return 0;
        }
        if (prev >= 0 && c != prev &&
            edge_find(nv, prev, c) < 0 && edge_find(nv, c, prev) < 0) return 0;
        prev = c;
    }
    return 1;
}

int ma2_nav_path(Ma2Nav *nv, Ma2Vec3 from, Ma2Vec3 to, Ma2Vec3 *out, int max)
{
    int s, t, cnt, i, k, n_out = 0;
    int *tmp = NULL;
    Ma2Vec3 *pts = NULL;
    if (!nv || !out || max <= 0 || nv->n == 0) return 0;
    s = ma2_nav_nearest(nv, from);
    if (s < 0) return 0;
    t = nearest_in(nv, to, nv->comp[s]);     /* an unreachable goal: walk to the closest reachable node */
    if (t < 0) return 0;
    if (s == t) { out[0] = to; return 1; }
    tmp = (int *)malloc((size_t)nv->n * sizeof(int));
    if (!tmp) return 0;
    cnt = astar(nv, s, t, tmp, nv->n);
    if (cnt == 0) { free(tmp); return 0; }

    /* string-pull: from node i jump to the furthest node whose straight
     * segment stays over existing cells/edges */
    pts = (Ma2Vec3 *)malloc((size_t)cnt * sizeof(Ma2Vec3));
    if (!pts) { free(tmp); return 0; }
    i = 0;
    while (i < cnt - 1) {
        int jmax = i + NAV_SMOOTH_LOOKAHEAD < cnt - 1 ? i + NAV_SMOOTH_LOOKAHEAD : cnt - 1;
        int j = i + 1;
        for (k = jmax; k > i + 1; k--)
            if (segment_walkable(nv, nv->pos[tmp[i]], nv->pos[tmp[k]])) { j = k; break; }
        pts[n_out++] = nv->pos[tmp[j]];
        i = j;
    }
    /* final waypoint: the exact target when it lies within reach of the goal node */
    {
        float hx = to.x - nv->pos[t].x, hz = to.z - nv->pos[t].z;
        if (sqrtf(hx * hx + hz * hz) <= nv->spacing * 1.5f) pts[n_out - 1] = to;
    }
    if (n_out > max) n_out = max;
    memcpy(out, pts, (size_t)n_out * sizeof(Ma2Vec3));
    free(tmp); free(pts);
    return n_out;
}

/* Set by the core to remember penalties across matches (per-map file). */
void (*g_ma2_nav_penalize_hook)(Ma2Vec3 a, Ma2Vec3 b, float extra);

void ma2_nav_penalize(Ma2Nav *nv, Ma2Vec3 a, Ma2Vec3 b, float extra)
{
    float len, stepd;
    int ns, i, prev = -1;
    if (!nv || nv->n == 0 || extra <= 0.0f) return;
    if (g_ma2_nav_penalize_hook) g_ma2_nav_penalize_hook(a, b, extra);
    len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    stepd = nv->spacing * 0.5f;
    ns = (int)ceilf(len / stepd);
    for (i = 0; i <= ns; i++) {
        float t = ns ? (float)i / ns : 0.0f;
        Ma2Vec3 s;
        int c;
        s.x = a.x + (b.x - a.x) * t; s.y = a.y + (b.y - a.y) * t; s.z = a.z + (b.z - a.z) * t;
        c = ma2_nav_nearest(nv, s);
        if (c < 0) continue;
        if (prev >= 0 && c != prev) {
            int e1 = edge_find(nv, prev, c), e2 = edge_find(nv, c, prev);
            if (e1 >= 0) nv->adj_pen[e1] += extra;
            if (e2 >= 0) nv->adj_pen[e2] += extra;
        }
        prev = c;
    }
}

/* ------------------------------------------------------------------ */
/* line of sight                                                       */
/* ------------------------------------------------------------------ */
int ma2_los(Ma2GroundFn g, void *ctx, Ma2Vec3 eye, Ma2Vec3 target, float step)
{
    float dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    float len = sqrtf(dx * dx + dy * dy + dz * dz);
    float top = (eye.y > target.y ? eye.y : target.y) + 200.0f;
    int n, i;
    if (!g) return 1;
    if (step <= 0.0f) step = 4.0f;
    n = (int)ceilf(len / step);
    for (i = 1; i < n; i++) {
        float t = (float)i / (float)n, sy = eye.y + dy * t;
        int hit = 0;
        float gy = g(ctx, eye.x + dx * t, top, eye.z + dz * t, &hit);
        if (hit && gy > sy + 0.5f) return 0;
    }
    return 1;
}

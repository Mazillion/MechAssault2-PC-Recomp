/* ma2_bot_path.c - bot path planning: A* on the traversability grid.
 *
 * The grid (ma2_bot_nav.c, built once per map from the game's own collision and
 * ground) marks every 2 m cell blocked or free and keeps the distance from each
 * cell to the nearest blocked one. A body of radius r may stand on a cell whose
 * clearance is at least r. Planning is plain 8-connected A* over those cells with
 * an octile heuristic, a cost for slope and for hugging walls, and circular
 * dynamic obstacles (parked vehicles, other units) added per query. The cell
 * path is string-pulled to corner points with a clearance-aware line test, and
 * a follower hands out a look-ahead point to steer for.
 *
 * No persistent state, no learned penalties: the same query on the same map
 * always gives the same path.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ma2_bot_api.h"
#include "ma2_bot_c7.h"

#define PATH_MAX_EXPAND  400000      /* node expansions per query (the grid has ~1M cells) */
#define PATH_HUG_MARGIN  2.0f        /* metres beyond the radius where wall-hugging costs extra */
#define PATH_SNAP_START  10.0f       /* search this far for a free start cell */
#define PATH_SNAP_GOAL   16.0f       /* ... and for a free cell near a blocked goal */
#define PATH_HEUR_W      1.15f       /* heuristic weight: a few % longer routes at most, far fewer expansions
                                      * (a plan cost 10-20 ms on New River City, 15+ a second in a 6v6) */
#define PATH_POCKET_DROP 40.0f       /* PocketDrop: the second-pass drop limit of a body stuck on a small raised island */
#define PATH_HOP_REACH   6           /* HopOut: cells (12 m) of blocked band a jet hop may cross */
#define PATH_HOP_LONG    14          /* HopOut: cells (28 m) a climb up a steep face may cross */
#define PATH_HOP_CLIMB   28.0f       /* HopOut: the highest climb (m) */
#define PATH_DIVE_LONG   20          /* HopDown: cells (40 m) of blocked slope a jet leap may cross on the way down */
#define PATH_HOP_CAP     50000       /* HopOut: expansions of the third pass */
#define PATH_HOP_MEMO    20.0        /* HopOut: s a goal that the hop pass could not reach is not tried again from the same ground region */
#define PATH_PULL_LOOK   96          /* string pulling: farthest cell tried from each anchor */

typedef struct { float f; int c; } HeapItem;

typedef struct SearchCtx {
    int w, h, cap;
    float *g;                   /* cost so far */
    unsigned *stamp;            /* == gen: g/from valid for this query */
    unsigned char *from;        /* direction index (0..7) we arrived from, 8 = start */
    unsigned *closed;           /* == gen: expanded in this query */
    unsigned *dynmark;          /* == gen: within reach of a moving obstacle (only there the exact test runs) */
    unsigned char *jump;        /* cells covered by the step into this cell: 1, or 2+ for a drop off a ledge */
    unsigned char *ow;          /* PitPartial: 1 = reached through a one-way drop link (a landing pocket may have no way back out) */
    HeapItem *heap; int heap_cap;
    unsigned gen;
    int hs;                     /* heap size */
} SearchCtx;
/* W7 PathSlice: ctx 0 is the synchronous planner's, ctx 1 belongs to the one sliced job in flight (a sync plan
 * called while a job is half done must not disturb its stamps). S / hsize always mean the current ctx. */
static SearchCtx g_sctx[2];
static SearchCtx *g_sp = &g_sctx[0];
#define S (*g_sp)
#define hsize (g_sp->hs)

static const int DXS[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
static const int DZS[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };

static int ensure(const Ma2TravView *v)
{
    int n = v->w * v->h;
    if (S.cap >= n && S.w == v->w && S.h == v->h) return 1;
    free(S.g); free(S.stamp); free(S.from); free(S.closed); free(S.heap); free(S.dynmark); free(S.jump); free(S.ow);
    memset(&S, 0, sizeof S);
    S.ow = (unsigned char *)malloc((size_t)n);
    S.g = (float *)malloc((size_t)n * sizeof(float));
    S.stamp = (unsigned *)calloc((size_t)n, sizeof(unsigned));
    S.from = (unsigned char *)malloc((size_t)n);
    S.closed = (unsigned *)calloc((size_t)n, sizeof(unsigned));
    S.dynmark = (unsigned *)calloc((size_t)n, sizeof(unsigned));
    S.jump = (unsigned char *)malloc((size_t)n);
    S.heap_cap = PATH_MAX_EXPAND * 8 + 16;
    S.heap = (HeapItem *)malloc((size_t)S.heap_cap * sizeof(HeapItem));
    if (!S.g || !S.stamp || !S.from || !S.closed || !S.heap || !S.dynmark || !S.jump || !S.ow) { free(S.g); free(S.stamp); free(S.from); free(S.closed); free(S.heap); free(S.dynmark); free(S.jump); free(S.ow); memset(&S, 0, sizeof S); return 0; }
    S.w = v->w; S.h = v->h; S.cap = n;
    return 1;
}

/* ---- cells ---- */
static float clearance(const Ma2TravView *v, int c)
{
    return v->blk[c] ? 0.0f : (float)v->dist[c] * (v->cell / 5.0f) - v->cell * 0.5f;
}

/* A hull outline's reach toward (dx, dz) from its centre. Each direction is the furthest reach
 * across its sector: within a sector at least that, rising toward a longer neighbour along the
 * straight line between the two (as the controller's hull_at). */
static float star_reach(const Ma2PathObstacle *o, float dx, float dz)
{
    float a = atan2f(dx, dz);
    if (a < 0.0f) a += 6.2831853f;
    float f = a / (6.2831853f / (float)MA2_HULL_DIRS), t;
    int k = (int)f;
    t = f - (float)k; k %= MA2_HULL_DIRS;
    float lin = o->star[k] * (1.0f - t) + o->star[(k + 1) % MA2_HULL_DIRS] * t;
    float own = t < 0.5f ? o->star[k] : o->star[(k + 1) % MA2_HULL_DIRS];
    return lin > own ? lin : own;
}
static int dyn_hit(float x, float z, float r, const Ma2PathObstacle *dyn, int ndyn)
{
    for (int i = 0; i < ndyn; i++) {
        if (dyn[i].has_star) {
            float dx = x - dyn[i].x, dz = z - dyn[i].z, d2 = dx * dx + dz * dz, rr = dyn[i].r + r;
            if (d2 >= rr * rr) continue;                                   /* beyond its longest arm */
            float R = star_reach(&dyn[i], dx, dz) + r;
            if (d2 < R * R) return 1;
            continue;
        }
        float dx = x - dyn[i].x, dz = z - dyn[i].z, rr = dyn[i].r + r;
        if (dx * dx + dz * dz < rr * rr) return 1;
    }
    return 0;
}

/* During a plan, cells away from every moving obstacle (not marked this query) skip the exact
 * hull test: it ran for every neighbour of every expanded cell against every obstacle. */
static int g_marks_on;
static int g_slope_max;   /* TankSlope: cells steeper than this (slp units, rise/run x 100) are not passable; 0 = no limit */
/* PilotFace: steep faces a pilot stalled on (a lip in the terrain's collision the height samples smooth into a slope), learned per map:
 * within the circle, cells of slope >= 40 are not passable when planning for a pilot. */
static double g_plan_now;
static struct { float x, z, r; double until; int mask; } g_pface[16];   /* mask: 1 pilot plans, 2 tank plans (TankFace) */
static int g_npface, g_pface_next, g_pilot_plan;
void ma2_path_set_pilot(int on) { g_pilot_plan = on ? 1 : 0; }
void ma2_path_set_tank_face(int on) { if (on) g_pilot_plan |= 2; else g_pilot_plan &= ~2; }   /* TankFace: after ma2_path_set_pilot */
void ma2_path_face_add(float x, float z, float r, double until, int mask);
void ma2_path_pilot_face_add(float x, float z, float r, double until) { ma2_path_face_add(x, z, r, until, 1); }
void ma2_path_face_add(float x, float z, float r, double until, int mask)
{
    for (int i = 0; i < g_npface; i++)
        if ((g_pface[i].x - x) * (g_pface[i].x - x) + (g_pface[i].z - z) * (g_pface[i].z - z) < 16.0f) { if (until > g_pface[i].until) g_pface[i].until = until; g_pface[i].mask |= mask; return; }
    int i = g_npface < 16 ? g_npface++ : (g_pface_next++ % 16);
    g_pface[i].x = x; g_pface[i].z = z; g_pface[i].r = r; g_pface[i].until = until; g_pface[i].mask = mask;
}
void ma2_path_pilot_face_reset(void) { g_npface = 0; g_pface_next = 0; }
static int pilot_face_blocked(const Ma2TravView *v, int ix, int iz, int c)
{
    /* TankFace: the game's tank stops at the lip itself, cells of slp 21-32 before the 43-53 face (F12_84a b5) -- a tank plan leaves slope >= 20 there */
    if (!v->slp || v->slp[c] < ((g_pilot_plan & 2) ? 20 : 40)) return 0;
    float x = ix * v->cell, z = iz * v->cell;
    for (int i = 0; i < g_npface; i++)
        if ((g_pface[i].mask & g_pilot_plan) && g_pface[i].until > g_plan_now && (g_pface[i].x - x) * (g_pface[i].x - x) + (g_pface[i].z - z) * (g_pface[i].z - z) < g_pface[i].r * g_pface[i].r) return 1;
    return 0;
}
static int passable(const Ma2TravView *v, int ix, int iz, float r, const Ma2PathObstacle *dyn, int ndyn)
{
    if (ix < 0 || iz < 0 || ix >= v->w || iz >= v->h) return 0;
    int c = iz * v->w + ix;
    if (clearance(v, c) < r) return 0;
    if (g_slope_max > 0 && v->slp && v->slp[c] > g_slope_max) return 0;   /* TankSlope: the search and the string-pull both */
    if (g_pilot_plan && g_npface && pilot_face_blocked(v, ix, iz, c)) return 0;   /* PilotFace */
    if (g_marks_on && S.dynmark[c] != S.gen) return 1;
    return !dyn_hit(ix * v->cell, iz * v->cell, r, dyn, ndyn);
}

static void to_cell(const Ma2TravView *v, float x, float z, int *ix, int *iz)
{
    /* push-x3 E4: NaN / huge values give a far-outside cell (the callers clamp), never a wild cast */
    float fx = floorf(x / v->cell + 0.5f), fz = floorf(z / v->cell + 0.5f);
    *ix = fx > -1e6f && fx < 1e6f ? (int)fx : (fx > 0.0f ? 1000000 : -1000000);
    *iz = fz > -1e6f && fz < 1e6f ? (int)fz : (fz > 0.0f ? 1000000 : -1000000);
}

/* Nearest passable cell to (ix, iz) within `range` metres (ring search). */
static int snap(const Ma2TravView *v, int *ix, int *iz, float r, const Ma2PathObstacle *dyn, int ndyn, float range)
{
    if (passable(v, *ix, *iz, r, dyn, ndyn)) return 1;
    int maxk = (int)ceilf(range / v->cell), bx = 0, bz = 0; float bd = 1e30f;
    for (int k = 1; k <= maxk; k++) {
        for (int dz = -k; dz <= k; dz++)
            for (int dx = -k; dx <= k; dx++) {
                if (abs(dx) != k && abs(dz) != k) continue;          /* ring only */
                int x = *ix + dx, z = *iz + dz;
                if (!passable(v, x, z, r, dyn, ndyn)) continue;
                float d = (float)(dx * dx + dz * dz);
                if (d < bd) { bd = d; bx = x; bz = z; }
            }
        if (bd < 1e30f) { *ix = bx; *iz = bz; return 1; }
    }
    return 0;
}

/* Nearest passable cell of region `id` to (ix, iz) within `range` metres (ring search). */
static int snap_region(const Ma2TravView *v, int *ix, int *iz, float r, const Ma2PathObstacle *dyn, int ndyn, float range, int id)
{
    int maxk = (int)ceilf(range / v->cell), bx = 0, bz = 0; float bd = 1e30f;
    for (int k = 0; k <= maxk; k++) {
        for (int dz = -k; dz <= k; dz++)
            for (int dx = -k; dx <= k; dx++) {
                if (abs(dx) != k && abs(dz) != k) continue;
                int x = *ix + dx, z = *iz + dz;
                if (x < 0 || z < 0 || x >= v->w || z >= v->h || v->comp[z * v->w + x] != id) continue;
                if (!passable(v, x, z, r, dyn, ndyn)) continue;
                float d = (float)(dx * dx + dz * dz);
                if (d < bd) { bd = d; bx = x; bz = z; }
            }
        if (bd < 1e30f) { *ix = bx; *iz = bz; return 1; }
    }
    return 0;
}

/* Every cell on the straight line a->b is passable (supercover-ish: samples every half cell). */
/* The map by cell; parked hulls and units at the point on the line itself (a cell centre is up to
 * 1.4 m off it: a pulled leg cut a VTOL's wing tip by that much). */
static int line_free(const Ma2TravView *v, float ax, float az, float bx, float bz, float r,
                     const Ma2PathObstacle *dyn, int ndyn)
{
    float dx = bx - ax, dz = bz - az, len = sqrtf(dx * dx + dz * dz);
    int n = (int)ceilf(len / (v->cell * 0.5f));
    for (int i = 0; i <= n; i++) {
        float t = n ? (float)i / (float)n : 0.0f, x = ax + dx * t, z = az + dz * t;
        int ix, iz; to_cell(v, x, z, &ix, &iz);
        if (!passable(v, ix, iz, r, NULL, 0)) return 0;
        if (ndyn && !(g_marks_on && ix >= 0 && iz >= 0 && ix < v->w && iz < v->h && S.dynmark[iz * v->w + ix] != S.gen) &&
            dyn_hit(x, z, r, dyn, ndyn)) return 0;
    }
    return 1;
}

/* ---- heap ---- */
/* ---- soft danger (known enemy guns): extra cost inside circles ---- */
static int g_crush;
void ma2_path_set_crush(int on) { g_crush = on ? 1 : 0; g_slope_max = 0; }
void ma2_path_set_slope_max(int slp) { g_slope_max = slp > 0 ? slp : 0; }
/* Drops (playtest 2026-10-14: a player with the flag walks straight home by
 * dropping off the cliff). The grid blocks every cell at a step of more than 1.6 m (1.1 m for a
 * 'Mech) -- both ways, so a cliff was a wall going down as much as going up, and a flag runner
 * walked along the cliff top to a ramp, back through the enemy base. A drop is a one-way link
 * from a free cell over the blocked band to free ground lower down: within PATH_DROP_REACH cells,
 * the ground in between never rising above where it starts (no wall, no building), the fall at
 * least PATH_DROP_MIN and at most what the body takes (set per plan). Costs a little more than
 * walking the same distance, so a ramp close by still wins. */
#define PATH_DROP_REACH  10           /* cells (20 m) */
#define PATH_DROP_MIN    1.0f
static float g_drop;
void ma2_path_set_drop(float metres) { g_drop = metres > 0.0f ? metres : 0.0f; }
/* W5 CarrierDrops: a flag carrier heading home / a chaser of the enemy carrier plans in the same search
 * with a cheaper drop link (g_dropk x the link's cost) and may land in a marsh (water bed, not a river
 * bank): the one-way drop is a shortcut it wants, not a trip it only accepts. 1 = off. */
static float g_dropk = 1.0f;
static int g_drop_water;
void ma2_path_set_drop_pref(float cost_k, int water_ok) { g_dropk = cost_k > 0.05f && cost_k <= 1.0f ? cost_k : 1.0f; g_drop_water = water_ok ? 1 : 0; }
/* F38 (batch 2): up a ledge on the jets. The planner had only the drops: a 'Mech or BattleArmor
 * walked the long way round to a ramp, and a plateau with none was out of reach (a player just
 * jets up). An up-link is the drop turned round: from a free cell over the steep band to free
 * natural ground higher up, within PATH_UP_REACH cells, at least PATH_UP_MIN higher and at most
 * what this body's jets climb (set per plan, 0 = none), nothing in between above the landing.
 * Costs well over walking (fuel, a stop), so a ramp close by still wins. jump[] marks it 64+cells. */
#define PATH_UP_REACH    5            /* cells (10 m) */
#define PATH_UP_MIN      1.2f
#define PATH_UP_FLAG     64
#define PATH_UNDER_FLAG  128          /* UnderLink: a straight leg beneath a raised deck / bridge (jump[] = 128 + cells) */
#define PATH_UNDER_REACH 16           /* cells (32 m) */
static float g_up;
/* UpStretch (default ON, RECOMP_BOT_UP_STRETCH / [Bots] UpStretch; 1.0 pass, Worker B): the measured jet climb (g_up = the best gain seen - 2 m,
 * learned from ground hops, 9-15 m in practice; BattleArmor / 'Mech full-tank burns measured 17-18.5 m median, 34 m best over the 30-cell sweep)
 * kept every 17 m canal wall on Going Commando out of reach: a BattleArmor that stepped into a canal planned no way out for 127 s (sw62 bot 4).
 * Up-links now also reach a ledge up to g_upx (at least 20 m) at a steep extra cost: a ramp or a plain jet-up link still wins, and the jet-up
 * executor gives a ledge that does not work up after 6 s (forbidden from there for 10 min). [C7] upstretch: links (stretch links relaxed). */
static float g_upx;
static int g_underlink = 1;   /* UnderLink switch, read in plan_begin */
#define PATH_UP_STRETCH_MIN  20.0f
#define PATH_UP_STRETCH_PEN  40.0f
static float upx_of(float up)
{
    static int on = -1;
    if (up <= 0.0f) return 0.0f;
    if (on < 0) on = c7_on("upstretch", "RECOMP_BOT_UP_STRETCH", "UpStretch", 1);
    if (!on) return up;
    return up + 4.0f > PATH_UP_STRETCH_MIN ? up + 4.0f : PATH_UP_STRETCH_MIN;
}
void ma2_path_set_up(float metres) { g_up = metres > 0.0f ? metres : 0.0f; g_upx = upx_of(g_up); }
/* LowJetWalk (default ON, RECOMP_BOT_LOW_JET_WALK / [Bots] LowJetWalk; parity pass 14, 11-12): a jet leg (drop kind 2: F38 up-link, BankDive / HopOut
 * leg) whose landing is within low_jet_rise() of its foot is a low lip, not a climb. Its cost (run + 12 + 1.5 x rise) is barely over walking the
 * same run, so a replan from the foot of a lip took the jet over a walk of about the same length -- and the burn carries a 'Mech 20 m+ up and well
 * past the landing, then a 90-degree turn back onto the route: Ill Wind Snatch It basin, V24_81b3 b8 t=22.9 899,-20,1487 -> 872,-17,1460 (3 m up,
 * 37 m off; the plans 1.3 s before and 0.5 s after from 917,1488 / 891,1483 walked to 742,1408), y up to +3.8 for 4 s, 2-3 s lost.
 * low_jet_rise = what the body's walkable grade climbs across its own width: 2 x radius x the grid's step limit (TG_STEP_HEAVY 1.1 m per 2 m cell
 * for a 'Mech / crush plan, TG_STEEP 0.8 for the light grid): 4.0 m for a 3.67 m 'Mech, 2.4 m for a 1.5 m BattleArmor. Such a leg costs
 * PATH_LOW_JET_PEN more: the measured loss of the jet + landing turn (2-3 s, ~40 m at the 'Mech's 18.7 m/s p50 ground speed), so a ground route
 * up to 40 m longer wins and a lip with no walk round is still jetted. [C7] lowjetwalk: links. */
#define PATH_WALK_GRADE_HEAVY 0.55f   /* ma2_bot_nav.c TG_STEP_HEAVY / TG_CELL */
#define PATH_WALK_GRADE_LIGHT 0.80f   /* ma2_bot_nav.c TG_STEEP */
#define PATH_LOW_JET_PEN      40.0f
static int low_jet_walk_on(void) { static int on = -1; if (on < 0) on = c7_on("lowjetwalk", "RECOMP_BOT_LOW_JET_WALK", "LowJetWalk", 1); return on; }
static float low_jet_rise(float radius, int heavy) { return 2.0f * radius * (heavy ? PATH_WALK_GRADE_HEAVY : PATH_WALK_GRADE_LIGHT); }
static float low_jet_pen(float rise, float radius)
{
    if (!low_jet_walk_on() || fabsf(rise) >= low_jet_rise(radius, g_crush)) return 0.0f;
    c7_add("lowjetwalk", "links", 1);
    return PATH_LOW_JET_PEN;
}
static struct { float x, z; double until; } g_nodrop[32];
static int g_nodrop_i;
static double g_nodrop_max_until;   /* ew-b8 B8.10: the latest `until` set since the last reset: after it nothing is forbidden */
void ma2_path_forbid_drop(float x, float z, double until)
{
    if (until > g_nodrop_max_until) g_nodrop_max_until = until;
    g_nodrop[g_nodrop_i].x = x; g_nodrop[g_nodrop_i].z = z; g_nodrop[g_nodrop_i].until = until;
    g_nodrop_i = (g_nodrop_i + 1) % 32;
}
void ma2_path_drops_reset(void) { memset(g_nodrop, 0, sizeof g_nodrop); g_nodrop_i = 0; g_nodrop_max_until = 0.0; }
static int drop_forbidden(float x, float z)
{
    if (g_plan_now >= g_nodrop_max_until) return 0;
    for (int i = 0; i < 32; i++)
        if (g_nodrop[i].until > g_plan_now && (g_nodrop[i].x - x) * (g_nodrop[i].x - x) + (g_nodrop[i].z - z) * (g_nodrop[i].z - z) < 225.0f) return 1;   /* 15 m: the whole edge, not the next step along it */
    return 0;
}
/* A blocked cell that is part of a slope face: its height differs from a 4-neighbour by more than
 * a metre. Flat blocked cells are something standing there (a fence, a post, a learned stuck spot)
 * -- a drop across one would walk into it. */
static int step_cell(const Ma2TravView *v, int x, int z)
{
    static const int DX4[4] = { -1, 1, 0, 0 }, DZ4[4] = { 0, 0, -1, 1 };
    float s = v->sf[z * v->w + x];
    for (int k = 0; k < 4; k++) {
        int nx = x + DX4[k], nz = z + DZ4[k];
        if (nx < 0 || nz < 0 || nx >= v->w || nz >= v->h) continue;
        float t = v->sf[nz * v->w + nx];
        if (t > -1e8f && fabsf(t - s) > 1.0f) return 1;
    }
    return 0;
}
#define PATH_MAX_DANGER 64       /* orders use up to MA2_ORDER_MAX_DANGER; lane discovery more */
static Ma2PathDanger g_danger[PATH_MAX_DANGER];
static int g_ndanger;
void ma2_path_set_danger(const Ma2PathDanger *d, int n)
{
    g_ndanger = 0;
    for (int i = 0; d && i < n && i < PATH_MAX_DANGER; i++) if (d[i].r > 1.0f && d[i].cost > 0.0f) g_danger[g_ndanger++] = d[i];
}
static float danger_cost(float x, float z)
{
    float c = 0.0f;
    for (int i = 0; i < g_ndanger; i++) {
        float dx = x - g_danger[i].x, dz = z - g_danger[i].z, d2 = dx * dx + dz * dz, r = g_danger[i].r;
        if (d2 < r * r) c += g_danger[i].cost * (1.0f - sqrtf(d2) / r);
    }
    return c;
}

static void hpush(float f, int c)
{
    if (hsize >= S.heap_cap) { c7_add("path", "heap_full", 1); return; }   /* push-x3 E13: visible when the open list drops nodes */
    int i = hsize++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (S.heap[p].f <= f) break;
        S.heap[i] = S.heap[p]; i = p;
    }
    S.heap[i].f = f; S.heap[i].c = c;
}
static HeapItem hpop(void)
{
    HeapItem top = S.heap[0], last = S.heap[--hsize];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        float fm = last.f;
        if (l < hsize && S.heap[l].f < fm) { m = l; fm = S.heap[l].f; }
        if (r < hsize && S.heap[r].f < fm) { m = r; }
        if (m == i) break;
        S.heap[i] = S.heap[m]; i = m;
    }
    if (hsize) S.heap[i] = last;
    return top;
}

static float octile(int ax, int az, int bx, int bz, float cell)
{
    int dx = abs(ax - bx), dz = abs(az - bz);
    int mn = dx < dz ? dx : dz, mx = dx < dz ? dz : dx;
    return ((float)(mx - mn) + 1.41421356f * (float)mn) * cell;
}

/* W7 PathSlice: a plan is three parts -- begin (set-up, snaps), run (the A* loop, resumable) and finish (walk back,
 * string pulling). ma2_path_plan runs all three at once; the path queue below runs the loop a few ms per game step.
 * The search code itself is unchanged, so a sliced plan gives the same path as a synchronous one on the same grid. */
typedef struct PlanSt {
    Ma2Path *p; Ma2Vec3 from, to; float radius; const Ma2PathObstacle *dyn; int ndyn; double now;
    int sx, sz, gx, gz, gok, start, goal, best, expanded, found; float best_h; unsigned cgen; int gw, gh;
    int gclamped_goal;   /* the goal was off the grid (clamped): never restart */
    int cap, capped;   /* ft-bots PathCap: this job's expansion limit (a goal on another ground region), and whether it ended on it */
    int boost;   /* PocketDrop: this job was restarted with the deeper drop limit (1); HopOut: and with wall hops for a jet body (2) */
    int best0; float best0_h; int exhausted;   /* PitPartial: the cell nearest the goal reached without a drop link, and whether the heap ran dry */
    int reach_slot;   /* ReachSet: the bot this plan is for (-1 = none) */
} PlanSt;

/* ReachSet (default ON, RECOMP_BOT_REACH_SET / [Bots] ReachSet; 1.0 pass fix round 4, Worker D): what a search that ran to the end knows and nobody used -- every cell the body can
 * reach from where it stands (walking, drops, jet legs, with this body's radius and links). A goal picked outside it can never be reached, and the brain picked such goals again
 * and again: sw46 bot 4 stood 48 s 32 m from a power-up on a hill top whose foot is ringed by a 10 m hard-blocked band (the region labels the brain asked, GroundReach, ignore the
 * body's clearance and put the hill in the bot's region); sw34 bot 9 and sw14 BattleArmor bot 8 stood 23 s / 35 s on ground that does not lead to any of the hunt / attack goals
 * they rolled, one every 3 s. The finished search of a plan that did not find its goal (heap empty, not capped, not a clamped goal) keeps its reached cells per bot as a bit
 * set; ma2_path_reach_has(slot, from, p, now) answers 1 / 0 for a point (any reached cell within 2 cells), -1 when there is no set (none yet, 12 s old, or the bot is
 * not in it any more). [C7] reachset: stored, invalidated, asked_out. */
#define REACH_SLOTS 16
static struct { unsigned char *bits; size_t cap; int w, h; double t; int valid; } g_rs[REACH_SLOTS];
static int g_reach_slot = -1;
void ma2_path_set_reach_slot(int s) { g_reach_slot = s; }
static void reach_store(int slot, const unsigned *stamp, unsigned gen, int w, int h, double now)
{
    if (slot < 0 || slot >= REACH_SLOTS) return;
    size_t nb = ((size_t)w * (size_t)h + 7) / 8;
    if (g_rs[slot].cap < nb) { free(g_rs[slot].bits); g_rs[slot].bits = (unsigned char *)malloc(nb); g_rs[slot].cap = g_rs[slot].bits ? nb : 0; }
    if (!g_rs[slot].bits) { g_rs[slot].valid = 0; return; }
    memset(g_rs[slot].bits, 0, nb);
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) if (stamp[i] == gen) g_rs[slot].bits[i >> 3] |= (unsigned char)(1u << (i & 7));
    g_rs[slot].w = w; g_rs[slot].h = h; g_rs[slot].t = now; g_rs[slot].valid = 1;
    c7_add("reachset", "stored", 1);
}
static int reach_near(int slot, float x, float z)
{
    int ix = (int)floorf(x / 2.0f + 0.5f), iz = (int)floorf(z / 2.0f + 0.5f);   /* TG_CELL = 2 m */
    for (int dz = -2; dz <= 2; dz++)
        for (int dx = -2; dx <= 2; dx++) {
            int cx = ix + dx, cz = iz + dz;
            if (cx < 0 || cz < 0 || cx >= g_rs[slot].w || cz >= g_rs[slot].h) continue;
            size_t i = (size_t)cz * (size_t)g_rs[slot].w + (size_t)cx;
            if (g_rs[slot].bits[i >> 3] & (1u << (i & 7))) return 1;
        }
    return 0;
}
int ma2_path_reach_has(int slot, Ma2Vec3 from, Ma2Vec3 p, double now)
{
    static int on = -1;
    if (on < 0) on = c7_on("reachset", "RECOMP_BOT_REACH_SET", "ReachSet", 1);
    if (!on || slot < 0 || slot >= REACH_SLOTS || !g_rs[slot].valid) return -1;
    if (now < g_rs[slot].t || now - g_rs[slot].t > 12.0) { g_rs[slot].valid = 0; return -1; }
    if (!reach_near(slot, from.x, from.z)) { g_rs[slot].valid = 0; c7_add("reachset", "invalidated", 1); return -1; }
    int r = reach_near(slot, p.x, p.z);
    if (!r) c7_add("reachset", "asked_out", 1);
    return r;
}

/* Mark each obstacle's reach (plus this unit's radius) with the current generation: only there the exact hull test runs. */
static void mark_dyn(const Ma2TravView *vp, const Ma2PathObstacle *dyn, int ndyn, float radius)
{
    const Ma2TravView v = *vp;
    for (int i = 0; i < ndyn; i++) {
        float reach = dyn[i].r + radius + v.cell * 1.5f;
        int x0 = (int)floorf((dyn[i].x - reach) / v.cell), x1 = (int)ceilf((dyn[i].x + reach) / v.cell);
        int z0 = (int)floorf((dyn[i].z - reach) / v.cell), z1 = (int)ceilf((dyn[i].z + reach) / v.cell);
        if (x0 < 0) x0 = 0; if (z0 < 0) z0 = 0; if (x1 >= v.w) x1 = v.w - 1; if (z1 >= v.h) z1 = v.h - 1;
        for (int z = z0; z <= z1; z++) for (int x = x0; x <= x1; x++) S.dynmark[z * v.w + x] = S.gen;
    }
}

/* HopOut memo: goals the hop pass failed to reach, per start ground region (an unreachable goal is replanned every 0.5 s by every bot after it). */
static struct { int gcell, comp; double until; } g_hopfail[16];
static int g_hopfail_i;
static int hop_comp(const PlanSt *st)
{
    Ma2TravView v;
    if (!ma2_trav_view_heavy(&v) || !v.comp) return -1;
    return v.comp[st->start];
}
static int hop_failed(const PlanSt *st)
{
    int comp = hop_comp(st);
    for (int i = 0; i < 16; i++)
        if (g_hopfail[i].until > st->now && g_hopfail[i].gcell == st->goal && g_hopfail[i].comp == comp) { c7_add("hopout", "memo_skips", 1); return 1; }
    return 0;
}
static void hop_failed_add(const PlanSt *st)
{
    g_hopfail[g_hopfail_i].gcell = st->goal; g_hopfail[g_hopfail_i].comp = hop_comp(st); g_hopfail[g_hopfail_i].until = st->now + PATH_HOP_MEMO;
    g_hopfail_i = (g_hopfail_i + 1) % 16;
}

/* 0: no free start cell (plan fails), 1: search ready, 2: no grid, straight line (done). */
static int plan_begin(PlanSt *st, Ma2TravView *vp)
{
    Ma2Path *p = st->p; Ma2Vec3 from = st->from, to = st->to; float radius = st->radius;
    const Ma2PathObstacle *dyn = st->dyn; int ndyn = st->ndyn; double now = st->now;
    Ma2TravView v;
    memset(p, 0, sizeof *p);
    p->goal = to; p->radius = radius; p->planned = now; p->crush = g_crush; g_plan_now = now; st->reach_slot = g_reach_slot;
    {   static int rf = -1;                                   /* push-x3 E2: ReplanFix (default ON) */
        if (rf < 0) rf = c7_on("replanfix", "RECOMP_BOT_REPLAN_FIX", "ReplanFix", 1);
        p->from = from; p->has_from = rf;
    }
    /* Keep the obstacles near the route with the path: the follower's shortcuts and the last step
     * to the goal must go round them too, not only round the map. */
    for (int i = 0; i < ndyn && p->ndyn < 96; i++) {
        float ax = dyn[i].x - from.x, az = dyn[i].z - from.z, bx = dyn[i].x - to.x, bz = dyn[i].z - to.z;
        float span = sqrtf((to.x - from.x) * (to.x - from.x) + (to.z - from.z) * (to.z - from.z)) + 30.0f;
        if (sqrtf(ax * ax + az * az) < span && sqrtf(bx * bx + bz * bz) < span) p->dyn[p->ndyn++] = dyn[i];
    }
    /* 'Mechs plan on the heavy view: trees they break by walking into them do not block them. */
    int have = p->crush ? ma2_trav_view_heavy(&v) : ma2_trav_view(&v);
    if (!have || !ensure(&v)) {                              /* no grid: straight line */
        p->pt[0] = to; p->n = 1; p->ok = 1; return 2;
    }
    if (++S.gen == 0) {
        memset(S.stamp, 0, (size_t)S.cap * sizeof(unsigned)); memset(S.closed, 0, (size_t)S.cap * sizeof(unsigned));
        memset(S.dynmark, 0, (size_t)S.cap * sizeof(unsigned)); S.gen = 1;
    }
    mark_dyn(&v, dyn, ndyn, radius);
    g_marks_on = 1;
    {   static int ul = -1;
        if (ul < 0) ul = c7_on("underlink", "RECOMP_BOT_UNDER_LINK", "UnderLink", 0);   /* OFF: shown harmful (sw10c/sw65c: 30k links, no_path 27 s, end_hold_frames x2) -- decks are not the only thing 8 m over bare terrain */
        g_underlink = ul;
    }
    int sx, sz, gx, gz;
    to_cell(&v, from.x, from.z, &sx, &sz);
    to_cell(&v, to.x, to.z, &gx, &gz);
    /* push-x3 E4: a goal off the grid (or NaN) is clamped to the nearest edge cell and can only end partial (ok = 0); the
     * lookups below index comp[] / sf[] with gx, gz. A start more than the snap range outside the grid has no plan. */
    static int gon = -1;
    if (gon < 0) gon = c7_on("navguard", "RECOMP_BOT_NAV_GUARD", "NavGuard", 1);
    int gclamped = 0;
    if (gon) {
        int sr = (int)ceilf(PATH_SNAP_START / v.cell);
        if (sx < -sr || sz < -sr || sx >= v.w + sr || sz >= v.h + sr) { c7_add("navguard", "start_off_grid", 1); g_marks_on = 0; return 0; }
        if (sx < 0) sx = 0; if (sz < 0) sz = 0; if (sx >= v.w) sx = v.w - 1; if (sz >= v.h) sz = v.h - 1;
        if (gx < 0 || gz < 0 || gx >= v.w || gz >= v.h) {
            gclamped = 1; c7_add("navguard", "goal_off_grid", 1);
            if (gx < 0) gx = 0; if (gz < 0) gz = 0; if (gx >= v.w) gx = v.w - 1; if (gz >= v.h) gz = v.h - 1;
        }
    }
    /* UnderSnap (default ON, RECOMP_BOT_UNDER_SNAP / [Bots] UnderSnap; 1.0 pass round 2, Worker B): the grid keeps one layer, the top surface. A body
     * standing under a bridge or deck (y 17 m below its cell's surface, on the bare terrain) was planned as if it stood on top of it: that deck
     * is walled in, so the route ended at the start with a gap to the goal (sw10: a BattleArmor under the Going Commando bridge, powerup 34 m
     * off in the same canal, 11 s no_path). The start now moves to the nearest free cell of its own layer (surface within 4 m of its height,
     * up to 30 m); the first leg is the walk out from under the deck. [C7] undersnap: plans. */
    {   static int us = -1;
        if (us < 0) us = c7_on("undersnap", "RECOMP_BOT_UNDER_SNAP", "UnderSnap", 1);
        if (us && v.sf && v.th && sx >= 0 && sz >= 0 && sx < v.w && sz < v.h) {
            int si = sz * v.w + sx;
            if (v.sf[si] > -1e8f && v.sf[si] - v.th[si] > 6.0f && from.y < v.sf[si] - 6.0f && fabsf(from.y - v.th[si]) < 4.0f) {
                int maxk = (int)ceilf(30.0f / v.cell), bx = -1, bz = -1; float bd = 1e30f;
                for (int k = 1; k <= maxk && bx < 0; k++)
                    for (int dz = -k; dz <= k; dz++)
                        for (int dx = -k; dx <= k; dx++) {
                            if (abs(dx) != k && abs(dz) != k) continue;
                            int x = sx + dx, z = sz + dz;
                            if (!passable(&v, x, z, radius, dyn, ndyn)) continue;
                            float sh = v.sf[z * v.w + x];
                            if (sh < -1e8f || fabsf(sh - from.y) > 4.0f) continue;
                            float d = (float)(dx * dx + dz * dz);
                            if (d < bd) { bd = d; bx = x; bz = z; }
                        }
                if (bx >= 0) { sx = bx; sz = bz; c7_add("undersnap", "plans", 1); }
            }
        }
    }
    /* A goal inside an obstacle (a vehicle's entry point beside its hull) ends at the nearest free cell to it. (push-x3 E14: snapped
     * first, so SnapWide below can use the goal's own ground.) */
    int gok = snap(&v, &gx, &gz, radius, dyn, ndyn, PATH_SNAP_GOAL);
    if (gclamped) gok = 0;
    /* A unit already wedged against something (or standing next to the vehicle it is
     * about to board) starts from the nearest free cell. */
    if (!snap(&v, &sx, &sz, radius, dyn, ndyn, PATH_SNAP_START)) {
        /* push-x SnapWide: a body jetted up a cliff face or into a pocket has no free cell within 10 m, so no plan at all
         * (path=0/0 until it ejected: NRC 384,1168 and 452,1325 are 30-40 m from the nearest free ground). A body that can drop
         * (g_drop: 'Mech, BattleArmor) plans from the nearest free cell out to 60 m: the first leg walks it off the face. */
        static int on = -1;
        if (on < 0) on = c7_on("snapwide", "RECOMP_BOT_SNAP_WIDE", "SnapWide", 1);
        int okw = 0;
        /* 1.0 pass (Worker B, SnapWide2, default on): a tactical move (Ma2Order.drop_mode) limits the drop to the jets' climb back (g_drop 8-14 m), which
         * put a perched 'Mech under the old 20 m test: no snap beyond 10 m, no plan at all, 'nostart' climbing to 12 while it stood still (sw72 bot 4 at
         * 518,1375 on a wall top; 46 s in sw72, 30 s sw92, 31 s sw93, 24 s sw81, nopath_hold on 19 cells). Any body that may step down (or jet up) snaps wide. */
        static int sw2 = -1;
        if (sw2 < 0) sw2 = c7_on("snapwide2", "RECOMP_BOT_SNAP_WIDE2", "SnapWide2", 1);
        if (on && (g_drop >= 20.0f || (sw2 && (g_drop > 0.0f || g_up > 0.0f)))) {
            if (g_drop < 20.0f) c7_add("snapwide2", "plans", 1);
            /* push-x3 E14: the goal's ground component comes from the SNAPPED goal cell (a goal against a hull / wall is a blocked
             * cell with comp 0, which dropped the "own ground first" rule and took a mesa top at 443,1330). */
            int id = 0;
            if (v.comp && gok && gx >= 0 && gz >= 0 && gx < v.w && gz < v.h) id = v.comp[gz * v.w + gx];
            int rx = sx, rz = sz;
            /* the nearest free cell of the goal's own ground first (a ledge pocket's nearest free cell can be a mesa top of another region) */
            if (id > 0 && snap_region(&v, &rx, &rz, radius, dyn, ndyn, 60.0f, id)) { sx = rx; sz = rz; okw = 1; }
            else okw = snap(&v, &sx, &sz, radius, dyn, ndyn, 60.0f);
        }
        if (!okw) { g_marks_on = 0; return 0; }
        c7_add("snapwide", "plans", 1);
    }
    /* A goal on ground that cannot be walked to from here (a rooftop, a deck with no ramp from our
     * side): end at the nearest point of our own ground instead -- a jet or the brain takes it on.
     * push-x3 E5: the drop / jet-up exceptions only hold for a goal on bare, open ground (drops and up-links land only there, and
     * never in water): a roof, a deck or a marsh goal used to run the search to exhaustion (400k cells, 100-200 ms) and end partial. */
    static int e5 = -1;
    if (e5 < 0) e5 = c7_on("unreachable_goal", "RECOMP_BOT_UNREACH_GOAL", "UnreachableGoal", 1);
    if (v.comp && v.comp[sz * v.w + sx] > 0 && v.comp[gz * v.w + gx] != v.comp[sz * v.w + sx]) {
        int gi = gz * v.w + gx;
        int bare = !e5 || !v.th || !v.sf || v.sf[gi] - v.th[gi] < 0.5f;
        int wet = e5 && v.water && v.water[gi];
        int lower = g_drop > 0.0f && bare && !(wet && !g_drop_water) && v.sf && v.sf[gi] > -1e8f && v.sf[gi] < v.sf[sz * v.w + sx] - PATH_DROP_MIN;   /* lower: maybe a drop away */
        int higher = g_up > 0.0f && bare && !wet && v.sf && v.sf[gi] > v.sf[sz * v.w + sx] + PATH_UP_MIN &&
                     ((float)((gx - sx) * (gx - sx) + (gz - sz) * (gz - sz)) * v.cell * v.cell < 150.0f * 150.0f ||
                      (g_upx > g_up && v.sf[gi] > v.sf[sz * v.w + sx] + 6.0f));   /* higher, close: maybe a jet up (UpStretch: any distance when it is 6 m+ up: the canal case) */
        if (!lower && !higher) {
            if (e5 && (g_drop > 0.0f || g_up > 0.0f) && v.sf && v.sf[gi] > -1e8f && !bare) c7_add("unreachable_goal", "resnapped", 1);
            gok = snap_region(&v, &gx, &gz, radius, dyn, ndyn, 90.0f, v.comp[sz * v.w + sx]);
        }
    }
    if (gclamped) gok = 0;
    /* ft-bots PathCap (R13 P1-2 #2): a goal that is in another ground region from the start (and not a clamped, off-grid one) exhausts the
     * search -- 100-200 ms on NRC while every other bot waits for the single job slot -- and can only end partial. Such a job is limited:
     * 60k expansions when no drop / jet-up link can reach it, 120k when one may (the real route's cost is well below either). The partial
     * result is the cell nearest the goal that was reached, as after an exhausted search. */
    st->cap = PATH_MAX_EXPAND; st->capped = 0;
    {   static int pc = -1; static int cap_plain = 60000, cap_link = 120000;
        if (pc < 0) {
            const char *e;
            pc = c7_on("pathcap", "RECOMP_BOT_PATH_CAP", "PathCap", 1);
            if ((e = getenv("RECOMP_BOT_PATH_CAP_N")) && atoi(e) > 1000) { cap_plain = atoi(e); cap_link = cap_plain * 2; }
        }
        if (pc && !gclamped && v.comp && v.comp[sz * v.w + sx] > 0 && v.comp[gz * v.w + gx] != v.comp[sz * v.w + sx]) {
            int gi2 = gz * v.w + gx, si2 = sz * v.w + sx;
            int bare2 = !v.th || !v.sf || v.sf[gi2] - v.th[gi2] < 0.5f;
            int link = bare2 && v.sf && ((g_drop > 0.0f && v.sf[gi2] > -1e8f && v.sf[gi2] < v.sf[si2] - PATH_DROP_MIN) ||
                                         (g_up > 0.0f && v.sf[gi2] > v.sf[si2] + PATH_UP_MIN));
            st->cap = link ? cap_link : cap_plain;
            c7_add("pathcap", link ? "plans_link" : "plans_plain", 1);
        }
        /* JetUpLong cap: with 28 m up-links a jet body's first pass spreads over most of a city map. To a goal on a raised built top (Rush Hour's
         * y 55 core deck: only the HopOut pass lands there) it ran the full 400k before HopOut started: p4_85b1 plan waits 87 s summed (rc14 17 s),
         * max 12.8 s, a 'Mech standing 25 s at the deck's foot. Jet bodies: pass 1 at most 160k; a goal on a built top at most 80k. [C7] jetuplong: capped. */
        if (pc && g_up > 0.0f && c7_on("jetuplong", "RECOMP_BOT_JET_UP_LONG", "JetUpLong", 1)) {
            int gi3 = gz * v.w + gx;
            int built = v.th && v.sf && v.sf[gi3] > -1e8f && v.sf[gi3] - v.th[gi3] > 3.0f;
            int jc = built ? 80000 : 160000;
            if (st->cap > jc) { st->cap = jc; c7_add("jetuplong", built ? "capped_deck" : "capped", 1); }
        }
    }

    st->cgen = S.gen; st->gw = v.w; st->gh = v.h; st->gclamped_goal = gclamped; st->boost = 0;
    const int W = v.w;
    st->start = sz * W + sx; st->goal = gz * W + gx; st->best = st->start;
    st->sx = sx; st->sz = sz; st->gx = gx; st->gz = gz; st->gok = gok;
    st->best_h = octile(sx, sz, gx, gz, v.cell);
    hsize = 0;
    S.g[st->start] = 0.0f; S.stamp[st->start] = S.gen; S.from[st->start] = 8; S.jump[st->start] = 1; S.ow[st->start] = 0;
    hpush(st->best_h, st->start);
    st->best0 = st->start; st->best0_h = st->best_h; st->exhausted = 0;
    st->expanded = 0; st->found = 0;
    if (vp) *vp = v;
    return 1;
}

static int pathtest_why(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_BOT_PATHTEST_WHY") != NULL; return on; }
#define WHY(r, val) do { if (st->boost == 1 && pathtest_why()) fprintf(stderr, "[PATHTEST-WHY]   d=%d k=%d %s %.1f\n", d, k2, (r), (double)(val)); } while (0)
static int deck_edge_on(void) { static int on = -1; if (on < 0) on = c7_on("deckedge", "RECOMP_BOT_DECK_EDGE", "DeckEdge", 1); return on; }
static int drop_clear_on(void) { static int on = -1; if (on < 0) on = c7_on("dropclear", "RECOMP_BOT_DROP_CLEAR", "DropClear", 1); return on; }
static int up_land_deep_on(void) { static int on = -1; if (on < 0) on = c7_on("uplanddeep", "RECOMP_BOT_UP_LAND_DEEP", "UpLandDeep", 1); return on; }
/* The A* loop. deadline_ms > 0: stop (return 0) once the clock passes it, resume with the next call; 1 = search over. */
static double pq_now_ms(void);
static int plan_run(PlanSt *st, const Ma2TravView *pv, double deadline_ms)
{
    const Ma2TravView v = *pv;
    const Ma2PathObstacle *dyn = st->dyn; int ndyn = st->ndyn; float radius = st->radius;
    const unsigned cgen = st->cgen;
    /* PocketDrop (default ON, RECOMP_BOT_POCKET_DROP / [Bots] PocketDrop; 1.0 pass, Worker B): a body on a small raised island (Proving Ground: pilots
     * on 2-cell ledge tops at y=174, 25-35 m over the ground, drop limit 10 m; a tank on a tactical move, limit 0) explores its few cells, finds no
     * way down and no goal: the plan ends at the start and the body stands there until the stall rules eject it (21-43 s in sw33, a tank 14 s on
     * On the Rocks). A search that ran dry on a small region, far from the goal, is run again with a 40 m drop limit: the route then walks off
     * the nearest ledge (falls do no damage). [C7] pocketdrop: restarts, found (the second pass reached the goal). */
    const float dlim = st->boost && g_drop < PATH_POCKET_DROP ? PATH_POCKET_DROP : g_drop;
    const int W = v.w, goal = st->goal, gx = st->gx, gz = st->gz;
    int best = st->best, expanded = st->expanded, found = st->found, iter = 0, best0 = st->best0;
    float best_h = st->best_h, best0_h = st->best0_h;
    int over = 1;
    while (hsize > 0 && expanded < st->cap) {
        if (deadline_ms > 0.0 && ++iter >= 24) { iter = 0; if (pq_now_ms() >= deadline_ms) { over = 0; break; } }
        HeapItem it = hpop();
        int c = it.c;
        if (S.closed[c] == cgen) continue;
        S.closed[c] = cgen;
        expanded++;
        if (c == goal) { found = 1; best = c; break; }
        int cx = c % W, cz = c / W;
        float hc = octile(cx, cz, gx, gz, v.cell);
        if (hc < best_h) { best_h = hc; best = c; }
        if (!S.ow[c] && hc < best0_h) { best0_h = hc; best0 = c; }
        int pm[8];   /* ew-b8 B8.11 (R1 E4): passable() once per neighbour (it is pure within an expansion; diagonals used to re-test both orthogonals) */
        for (int d = 0; d < 8; d++) pm[d] = passable(&v, cx + DXS[d], cz + DZS[d], radius, dyn, ndyn);
        for (int d = 0; d < 8; d++) {
            int nx = cx + DXS[d], nz = cz + DZS[d];
            if (!pm[d]) continue;
            if (d >= 4 && (!pm[DXS[d] > 0 ? 0 : 1] || !pm[DZS[d] > 0 ? 2 : 3])) continue;   /* no corner cutting */
            int n = nz * W + nx;
            if (S.closed[n] == cgen) continue;
            float step = (d < 4 ? 1.0f : 1.41421356f) * v.cell;
            float cl = clearance(&v, n), cost = step;
            cost += step * (float)v.slp[n] * 0.02f;                             /* slope: 1 m/m doubles the cost */
            if (cl < radius + PATH_HUG_MARGIN) cost += step * (radius + PATH_HUG_MARGIN - cl) * 0.5f;
            if (g_ndanger) cost += step * danger_cost(nx * v.cell, nz * v.cell);   /* keep out of known fire lanes */
            float g = S.g[c] + cost;
            if (S.stamp[n] == S.gen && g >= S.g[n]) continue;
            S.g[n] = g; S.stamp[n] = S.gen; S.from[n] = (unsigned char)d; S.jump[n] = 1; S.ow[n] = S.ow[c];
            hpush(g + PATH_HEUR_W * octile(nx, nz, gx, gz, v.cell), n);
        }
        /* Drops: where the way ahead is blocked, free ground further on and lower down. */
        /* Natural ground only: off a hillside or a cliff, never off a platform, a walkway or a roof
         * (railings, gaps and pits between buildings: a carrier fell between the blue base's
         * structures and stayed wedged, run dr7). Roofs have their own walk-off in the controller. */
        /* PocketDrop round 2: in the second pass a roof or deck edge may be stepped off too (a 'Mech perched on a roof, goal 900 m off, sat 27 s with no
         * route on On the Rocks: sw65 bot 9, y 74.7 over 63.5). The first pass keeps natural ground only. */
        if (st->boost == 1 && pathtest_why()) fprintf(stderr, "[PATHTEST-WHY] cell %d,%d sf=%.1f th=%.1f dlim=%.0f forbid=%d\n", cx, cz, v.sf ? v.sf[c] : -1.f, v.th ? v.th[c] : -1.f, dlim, drop_forbidden(cx * v.cell, cz * v.cell));
        if (dlim > 0.0f && v.sf && v.th && v.sf[c] > -1e8f && (v.sf[c] - v.th[c] < 0.5f || (st->boost && v.sf[c] - v.th[c] < 40.0f)) && !drop_forbidden(cx * v.cell, cz * v.cell)) {
            float top = v.sf[c];
            for (int d = 0; d < 8; d++) {
                if (pm[d]) continue;   /* walked normally */
                for (int k2 = 2; k2 <= PATH_DROP_REACH; k2++) {
                    int mx = cx + DXS[d] * (k2 - 1), mz = cz + DZS[d] * (k2 - 1);
                    if (mx < 0 || mz < 0 || mx >= W || mz >= v.h) break;
                    float sm = v.sf[mz * W + mx];
                    if (sm < -1e8f || sm > top + 0.5f) { WHY("rise", sm); break; }                      /* off the map, or something rises: no drop here */
                    /* DeckEdge (default ON, RECOMP_BOT_DECK_EDGE / [Bots] DeckEdge; 1.0.1): in the PocketDrop pass the edge cells of the top the body stands
                     * on are that same deck, not something built in between. The step rule (tg_steps) blocks every cell beside a 1.6 m+ step, so a narrow
                     * deck is all blocked edge but for a cell or two; the first edge cell (174 over 145 m terrain) broke every drop. Recorded failure:
                     * f10sw33 LMS Proving Ground pilot bot 3 on 460,174,700 from t=48 to the end (no route to any goal; PATHTEST exp=2 even at drop 40). */
                    if (sm - v.th[mz * W + mx] > 0.5f && !(st->boost && fabsf(sm - top) <= 0.5f && deck_edge_on())) { WHY("built", sm); break; }   /* something built in between */
                    /* DropClear (default on, RECOMP_BOT_DROP_CLEAR / [Bots] DropClear; parity pass 5, 11-11): the run of a step-off passes no closer than the
                     * body's radius to something BUILT that rises above the top (a post, a wall end). The line test above sees only the cells on the line,
                     * and the blocked cells beside a post are step cells, so a link ran 1.4 m past a 12 m post: Ill Wind team 1's base exit 374,956 ->
                     * 386,968 beside the post at 378,962 (sf 27.5 over 15.2 ground) -- a 'Mech pressed on the post 4-7 s and gave up in EVERY Ill Wind run
                     * (P8_81a1 b1, P8_81a2 b3, P8_81a3 b1, P8_81b1 b0, P8_81b2 b0, P8_81b3 b4, X_81a b2; 4-23 s lost vs the floor). [C7] dropclear: cut. */
                    if (drop_clear_on() && radius > 1.0f) {
                        float rc = radius * 0.9f / v.cell, rc2 = rc * rc + 0.5f; int rr = (int)ceilf(rc), hit = 0;
                        for (int dj = -rr; dj <= rr && !hit; dj++)
                            for (int di = -rr; di <= rr; di++) {
                                int qx = mx + di, qz = mz + dj;
                                if ((float)(di * di + dj * dj) > rc2 || qx < 0 || qz < 0 || qx >= W || qz >= v.h) continue;
                                int q = qz * W + qx;
                                if (v.sf[q] > top + 0.5f && v.sf[q] - v.th[q] > 0.5f) { hit = 1; break; }
                            }
                        if (hit) { c7_add("dropclear", "cut", 1); WHY("beside_built", sm); break; }
                    }
                    if (S.dynmark[mz * W + mx] == S.gen && dyn_hit(mx * v.cell, mz * v.cell, radius * 0.5f, dyn, ndyn)) break;   /* a parked hull, a unit */
                    if (v.blk[mz * W + mx] && !step_cell(&v, mx, mz)) { WHY("obst", sm); break; }          /* something stands there, not a slope */
                    /* TankSlope drops: a face the grid leaves open but that is too steep for this body is a slope, not a cliff -- no drop link down it
                     * (Ill Wind team-2 exit: 1190,1402 y 9.5 -> 1190,1388 y 3.7 over a 30-37 deg face, slp 64-76; the tank shuffled 30 s at the edge,
                     * p27_81a/b, while the 16-22 deg ramp at x 1200-1204 was free). */
                    if (g_slope_max > 0 && !v.blk[mz * W + mx] && v.slp && v.slp[mz * W + mx] > g_slope_max) { WHY("steep", sm); break; }
                    int nx = cx + DXS[d] * k2, nz = cz + DZS[d] * k2;
                    if (!passable(&v, nx, nz, radius, dyn, ndyn)) { WHY("nopass", v.sf[nz * W + nx]); continue; }
                    int n = nz * W + nx;
                    if (v.water && v.water[n] && !g_drop_water) break;                /* into a river or a marsh: banks are where units are stopped */
                    if (v.sf[n] - v.th[n] > 0.5f || clearance(&v, n) < radius + 2.0f) { WHY(v.sf[n] - v.th[n] > 0.5f ? "land_built" : "land_tight", v.sf[n]); /* DeckEdge: in the PocketDrop pass a cramped foot of the wall (the deck's own blocked edge beside it) is not the end: look further out */ if (st->boost && v.sf[n] - v.th[n] <= 0.5f && deck_edge_on()) continue; break; }   /* onto bare, open ground */
                    float fall = top - v.sf[n];
                    if (fall < PATH_DROP_MIN || fall > dlim || S.closed[n] == cgen) { WHY("fall", fall); break; }
                    float run = (d < 4 ? 1.0f : 1.41421356f) * v.cell * (float)k2;
                    float g = S.g[c] + (run + 3.0f + 0.3f * fall) * g_dropk;
                    if (!(S.stamp[n] == S.gen && g >= S.g[n])) {
                        S.g[n] = g; S.stamp[n] = S.gen; S.from[n] = (unsigned char)d; S.jump[n] = (unsigned char)k2; S.ow[n] = 1; if (st->boost && v.sf[c] - v.th[c] >= 0.5f) c7_add("pocketdrop", "roof_links", 1);
                        hpush(g + PATH_HEUR_W * octile(nx, nz, gx, gz, v.cell), n);
                    }
                    break;                                                          /* the first landing in this direction */
                }
            }
        }
        /* UnderLink (default ON, RECOMP_BOT_UNDER_LINK / [Bots] UnderLink; 1.0 pass round 2, Worker B): the grid keeps one layer (the top surface), so a
         * canal that runs under a bridge deck is cut in two by the deck's blocked footprint: the two halves are different ground regions, a powerup
         * 64 m along the same canal had no route (Going Commando sw10: 18.8 s no_path, a BattleArmor walked that passage in 4 s in the other run).
         * From natural ground facing a blocked band: a straight run of up to 32 m on which something stands at least 8 m over the bare terrain
         * (the deck) and which ends on natural, free ground at the same level (1.5 m) is a link, at run + 8 cost. [C7] underlink: links. */
        if (g_underlink && v.sf && v.th && v.sf[c] > -1e8f && v.sf[c] - v.th[c] < 0.5f) {
            for (int d = 0; d < 4; d++) {                                               /* cardinal runs only */
                if (pm[d]) continue;
                float deck = 0.0f;
                for (int k2 = 2; k2 <= PATH_UNDER_REACH; k2++) {
                    int mx = cx + DXS[d] * (k2 - 1), mz = cz + DZS[d] * (k2 - 1);
                    if (mx < 0 || mz < 0 || mx >= W || mz >= v.h) break;
                    float sm = v.sf[mz * W + mx];
                    if (sm < -1e8f) break;
                    if (sm - v.th[mz * W + mx] > deck) deck = sm - v.th[mz * W + mx];
                    int nx = cx + DXS[d] * k2, nz = cz + DZS[d] * k2;
                    if (nx < 0 || nz < 0 || nx >= W || nz >= v.h) break;
                    int n = nz * W + nx;
                    if (!passable(&v, nx, nz, radius, dyn, ndyn)) continue;
                    if (v.sf[n] > -1e8f && v.sf[n] - v.th[n] > 0.5f) continue;                  /* still over the deck's footprint */
                    if (deck < 8.0f || v.sf[n] < -1e8f || fabsf(v.sf[n] - v.sf[c]) > 1.5f || S.closed[n] == cgen) break;
                    if (v.water && v.water[n]) break;
                    float g = S.g[c] + v.cell * (float)k2 + 8.0f;
                    if (!(S.stamp[n] == S.gen && g >= S.g[n])) {
                        c7_add("underlink", "links", 1);
                        S.g[n] = g; S.stamp[n] = S.gen; S.from[n] = (unsigned char)d; S.jump[n] = (unsigned char)(PATH_UNDER_FLAG + k2); S.ow[n] = S.ow[c];
                        hpush(g + PATH_HEUR_W * octile(nx, nz, gx, gz, v.cell), n);
                    }
                    break;
                }
            }
        }
        /* F38: up-links, from natural ground where the way ahead is too steep to walk. */
        /* JetUpLong (default ON, RECOMP_BOT_JET_UP_LONG / [Bots] JetUpLong; parity pass 11-08): up-links reached 10 m, so a bank whose face is 12-14 cells deep was
         * never a jet-up: Ill Wind Snatch It, the 15 m bank under the core pile (716,1384 y2 -> 716,1360 y17, 24 m) -- team 2's 'Mechs walked 838 m round
         * (team 1: 501 m) where a human jets up it. They now reach PATH_HOP_LONG (28 m, the HopOut climb's reach) at the same cost. [C7] jetuplong: links. */
        static int jul_sw = -1;
        if (jul_sw < 0) jul_sw = c7_on("jetuplong", "RECOMP_BOT_JET_UP_LONG", "JetUpLong", 1);
        const int up_reach = jul_sw ? PATH_HOP_LONG : PATH_UP_REACH;
        if (g_upx > 0.0f && v.sf && v.th && v.sf[c] > -1e8f && v.sf[c] - v.th[c] < 0.5f && !drop_forbidden(cx * v.cell, cz * v.cell)) {
            float top = v.sf[c];
            for (int d = 0; d < 8; d++) {
                if (pm[d]) continue;   /* walked normally */
                float hi = -1e9f; int deep = 0;
                for (int k2 = 2; k2 <= up_reach; k2++) {
                    int mx = cx + DXS[d] * (k2 - 1), mz = cz + DZS[d] * (k2 - 1);
                    if (mx < 0 || mz < 0 || mx >= W || mz >= v.h) break;
                    float sm = v.sf[mz * W + mx];
                    if (sm < -1e8f || sm > top + g_upx + 0.5f) break;              /* off the map, or higher than the jets go (even stretched) */
                    if (sm - v.th[mz * W + mx] > 0.5f) break;                       /* something built: roofs are the brain's */
                    if (S.dynmark[mz * W + mx] == S.gen && dyn_hit(mx * v.cell, mz * v.cell, radius * 0.5f, dyn, ndyn)) break;
                    if (v.blk[mz * W + mx] && !step_cell(&v, mx, mz)) break;          /* something stands there, not a slope */
                    if (sm > hi) hi = sm;
                    int nx = cx + DXS[d] * k2, nz = cz + DZS[d] * k2;
                    if (!passable(&v, nx, nz, radius, dyn, ndyn)) continue;
                    int n = nz * W + nx;
                    if (v.water && v.water[n]) break;
                    /* UpLandDeep (default on, RECOMP_BOT_UP_LAND_DEEP / [Bots] UpLandDeep; parity pass 5, 11-11): the first passable cell over the band
                     * sits at its foot, under radius + 2 m of clearance, and broke the search in that direction -- so a ledge was only reached by an
                     * up-link where the band ends in a wide-open top. The jet lands further on: on higher natural ground, look on out (within the
                     * reach) for the first open landing. Ill Wind's basin -> core-pile terrace (2.5 m -> 16.7 m, the 6 m band at z 1364-1376): no
                     * link from the ramp top 700-740,1376-1396 (team 2 walked 60-80 m west round the rocks and jetted at 690,1346; P8_81b2 b6
                     * 43-58 s, 9-14 s lost vs the floor in every team-2 run); with it 712,1382 -> 712,1358 (24 m, 14 m up). [C7] uplanddeep: links. */
                    if (v.sf[n] - v.th[n] > 0.5f) break;                                 /* onto bare ground */
                    if (clearance(&v, n) < radius + 2.0f) {                              /* and open */
                        if (up_land_deep_on() && v.sf[n] - top >= PATH_UP_MIN) { deep = 1; continue; }
                        break;
                    }
                    float rise = v.sf[n] - top;
                    if (rise < PATH_UP_MIN || rise > g_upx || hi > v.sf[n] + 0.5f || S.closed[n] == cgen) break;
                    float run = (d < 4 ? 1.0f : 1.41421356f) * v.cell * (float)k2;
                    float g = S.g[c] + run + 12.0f + 1.5f * rise + (rise > g_up ? PATH_UP_STRETCH_PEN : 0.0f) + low_jet_pen(rise, radius);   /* LowJetWalk */
                    if (!(S.stamp[n] == S.gen && g >= S.g[n])) {
                        if (rise > g_up) c7_add("upstretch", "links", 1);
                        if (k2 > PATH_UP_REACH) c7_add("jetuplong", "links", 1);
                        if (deep) c7_add("uplanddeep", "links", 1);
                        S.g[n] = g; S.stamp[n] = S.gen; S.from[n] = (unsigned char)d; S.jump[n] = (unsigned char)(PATH_UP_FLAG + k2); S.ow[n] = S.ow[c];
                        hpush(g + PATH_HEUR_W * octile(nx, nz, gx, gz, v.cell), n);
                    }
                    break;                                                          /* the first landing in this direction */
                }
            }
        }
        /* HopOut (default ON, RECOMP_BOT_HOP_OUT / [Bots] HopOut; 1.0 pass fix round 3, Worker B): a jet body whose search ran dry 10 m+ short of the goal gets a
         * third pass with wall hops. Cases: sw81 Ill Wind -- a flag carrier hopped over a ramp wall into a 14 x 32 m yard and stood 48.6 s (133 cells, goal 112 m
         * off; no walk out, no drop with a wall in between, no up-link to a landing that is not higher); sw54 Proving Ground -- a BA target on a mesa behind a 10 m
         * band of low walls (no_path 26 s); sw85 Rush Hour -- five flags on a 25 m plateau over a 45-degree embankment (no_path 20 s, one grab per match). Two link
         * kinds, both a jet-up leg (drop kind 2, the controller's executor), one-way (S.ow), from any surface: a hop over a band of up to 12 m whose tops stay
         * clear of the jets (start + climb - 1 m) onto ground from 60 m down to the stretched climb up; a climb up a face of up to 28 m that never rises above
         * the landing (up to 28 m). A goal this pass could not reach is not retried from the same ground region for 20 s. [C7] hopout: restarts, links, climbs,
         * found, failed, memo_skips. */
        /* BankDive (default ON, RECOMP_BOT_BANK_DIVE / [Bots] BankDive; parity pass 11-10): HopDown's leap down a slope band of up to 40 m ran only in the HopOut
         * pass (a dry search), so a 'Mech never planned down a bank the walk cannot take when a long way round existed: Snatch It Ill Wind, team 2's base rim
         * (1160,1450 y 15) over a 35 m, 45-76 deg bank into the y -20 basin straight under the core plateau. Team 2 walked 930-1440 m round the north rim or
         * the south loop (B_81a1: first ground arrivals 60 / 83 / 95 s, team 1 31 / 43 / 50 s) where a human jets down the bank and crosses the open floor.
         * Now a jet body's first pass also gets the dive legs (natural ground only, same cost: run + 16 + 0.15 x fall). A 'Mech without jets cannot
         * walk off that bank (tried as a plain step-off: P6_81b1 b10 'did not come down' at 6 spots of the rim, 10-60 s), so it keeps the walk round.
         * [C7] bankdive: links. */
        static int bdv_sw = -1;
        if (bdv_sw < 0) bdv_sw = c7_on("bankdive", "RECOMP_BOT_BANK_DIVE", "BankDive", 1);
        const int bdv = !st->boost && bdv_sw && dlim >= 30.0f && v.th && v.sf && v.sf[c] > -1e8f && v.sf[c] - v.th[c] < 0.5f;
        if ((st->boost == 2 || bdv) && g_up > 0.0f && v.sf && v.sf[c] > -1e8f && !drop_forbidden(cx * v.cell, cz * v.cell)) {
            float top = v.sf[c];
            const float climb = g_upx > PATH_HOP_CLIMB ? g_upx : PATH_HOP_CLIMB;   /* a climb up a steep face: the 'Mech jets and walks up it, 25 m on Rush Hour */
            static int hdn_sw = -1;
            if (hdn_sw < 0) hdn_sw = c7_on("hopdown", "RECOMP_BOT_HOP_DOWN", "HopDown", 1);
            const int hdn = hdn_sw;
            for (int d = 0; d < 8; d++) {
                if (pm[d]) continue;   /* walked normally */
                float hi = -1e9f;
                for (int k2 = 2; k2 <= (hdn ? PATH_DIVE_LONG : PATH_HOP_LONG); k2++) {
                    int mx = cx + DXS[d] * (k2 - 1), mz = cz + DZS[d] * (k2 - 1);
                    if (mx < 0 || mz < 0 || mx >= W || mz >= v.h) break;
                    float sm = v.sf[mz * W + mx];
                    if (sm < -1e8f || sm > top + climb + 0.5f) break;                /* off the map, or higher than any climb */
                    if (k2 > PATH_HOP_LONG && sm > top + 0.5f) break;                 /* HopDown: past the climb reach only a slope that never rises above the top */
                    if (S.dynmark[mz * W + mx] == S.gen && dyn_hit(mx * v.cell, mz * v.cell, radius * 0.5f, dyn, ndyn)) break;
                    if (sm > hi) hi = sm;
                    int nx = cx + DXS[d] * k2, nz = cz + DZS[d] * k2;
                    if (!passable(&v, nx, nz, radius, dyn, ndyn)) continue;
                    int n = nz * W + nx;
                    if ((v.water && v.water[n]) || v.sf[n] < -1e8f || S.closed[n] == cgen) break;
                    float rise = v.sf[n] - top;
                    int hop = k2 <= PATH_HOP_REACH && hi <= top + g_up - 1.0f && rise >= -dlim && rise <= g_upx;   /* over a low band */
                    int clm = k2 <= PATH_HOP_LONG && rise >= PATH_UP_MIN && rise <= climb && hi <= v.sf[n] + 0.5f;   /* up a face that never rises above the top */
                    /* HopDown (default ON, RECOMP_BOT_HOP_DOWN / [Bots] HopDown; 1.0 pass fix round 4, Worker D): the way back DOWN a slope the walk cannot take. HopOut climbed onto the Rush Hour deck
                     * (a 25 m natural plateau in a hard-blocked 26 m embankment) but a hop reaches 12 m, so a carrier on top had no route off: PATHTEST 1175,1207 -> 798,1017 ok=0 gap 417,
                     * and the same 'Mech stood 91 s (sw85 r5 bot 5) / 107 s (WP3-C). A leap down over a band of up to 40 m whose tops never rise above the start, onto bare free
                     * ground at least 1 m lower (and no more than the drop limit), is one more one-way jet leg (cost: run + 16 + 0.15 x fall). [C7] hopout: dives. */
                    int dive = hdn && rise <= -PATH_DROP_MIN && rise >= -dlim && hi <= top + 0.5f && v.sf[n] - v.th[n] < 0.5f;
                    if (bdv && st->boost != 2) hop = clm = 0;   /* BankDive: the first pass takes the dives only */
                    if (!hop && !clm && !dive) continue;
                    float run = (d < 4 ? 1.0f : 1.41421356f) * v.cell * (float)k2;
                    float g = S.g[c] + run + 16.0f + 1.5f * (rise > 0.0f ? rise : 0.0f) + 0.15f * (rise < 0.0f ? -rise : 0.0f) + (rise > g_up ? PATH_UP_STRETCH_PEN : 0.0f) + low_jet_pen(rise, radius);   /* LowJetWalk */
                    if (!(S.stamp[n] == S.gen && g >= S.g[n])) {
                        if (st->boost != 2) c7_add("bankdive", "links", 1); else
                        c7_add("hopout", dive && !hop && !clm ? "dives" : clm && !hop ? "climbs" : "links", 1);
                        S.g[n] = g; S.stamp[n] = S.gen; S.from[n] = (unsigned char)d; S.jump[n] = (unsigned char)(PATH_UP_FLAG + k2); S.ow[n] = 1;
                        hpush(g + PATH_HEUR_W * octile(nx, nz, gx, gz, v.cell), n);
                    }
                    break;                                                          /* the first landing in this direction */
                }
            }
        }
    }
    st->best = best; st->expanded = expanded; st->found = found; st->best_h = best_h; st->best0 = best0; st->best0_h = best0_h;
    if (over && !found && hsize == 0) st->exhausted = 1;
    {   static int pd = -1;
        if (pd < 0) pd = c7_on("pocketdrop", "RECOMP_BOT_POCKET_DROP", "PocketDrop", 1);
        static int ho = -1;
        if (ho < 0) ho = c7_on("hopout", "RECOMP_BOT_HOP_OUT", "HopOut", 1);
        int nb = 0;   /* the next pass of a dry search that ended short of the goal: 1 PocketDrop (small region, 30 m+ short), 2 HopOut (jets only, 10 m+ short) */
        if (over && !found && hsize == 0 && st->boost < 2 && !st->gclamped_goal) {
            if (pd && !st->boost && g_drop < PATH_POCKET_DROP && expanded < 40000 && best0_h > 30.0f) nb = 1;
            else if (ho && g_up > 0.0f && best0_h > 10.0f && !hop_failed(st)) nb = 2;
        }
        /* JetUpLong: with 28 m up-links a jet body's first pass reaches most of a city map and runs into its cap before it runs dry, so the HopOut
         * pass (the only one that climbs onto a deck) never started: PATHTEST Rush Hour team 2 -> the y 55 core deck, ok=1 (19.5k) -> ok=0 gap 55 (400k).
         * A capped jet search that ended 10 m+ short gets the HopOut pass too. [C7] jetuplong: hop_after_cap. */
        else if (over && !found && hsize > 0 && expanded >= st->cap && st->boost == 0 && !st->gclamped_goal && ho && g_up > 0.0f && best0_h > 10.0f &&
                 !hop_failed(st) && c7_on("jetuplong", "RECOMP_BOT_JET_UP_LONG", "JetUpLong", 1)) { nb = 2; c7_add("jetuplong", "hop_after_cap", 1); }
        if (nb) {
            st->boost = nb; c7_add(nb == 2 ? "hopout" : "pocketdrop", "restarts", 1);
            if (nb == 2 && st->cap > PATH_HOP_CAP) st->cap = PATH_HOP_CAP;
            if (++S.gen == 0) {
                memset(S.stamp, 0, (size_t)S.cap * sizeof(unsigned)); memset(S.closed, 0, (size_t)S.cap * sizeof(unsigned));
                memset(S.dynmark, 0, (size_t)S.cap * sizeof(unsigned)); S.gen = 1;
            }
            mark_dyn(&v, dyn, ndyn, radius);
            st->cgen = S.gen; hsize = 0;
            S.g[st->start] = 0.0f; S.stamp[st->start] = S.gen; S.from[st->start] = 8; S.jump[st->start] = 1; S.ow[st->start] = 0;
            st->best = st->best0 = st->start; st->best_h = st->best0_h = octile(st->sx, st->sz, gx, gz, v.cell);
            hpush(st->best_h, st->start);
            st->expanded = 0; st->found = 0; st->exhausted = 0;
            return plan_run(st, pv, deadline_ms);
        }
    }
    if (over && !found && hsize > 0 && expanded >= st->cap && st->cap < PATH_MAX_EXPAND) st->capped = 1;
    if (over && !found && st->boost == 2 && (hsize == 0 || expanded >= st->cap)) { hop_failed_add(st); c7_add("hopout", "failed", 1); }
    if (over && found && st->boost == 2) c7_add("hopout", "found", 1);
    return over;
}

/* Walk back, string-pull, finish the Ma2Path. Returns what ma2_path_plan returns. */
static int g_last_exp;
int ma2_path_last_expanded(void) { return g_last_exp; }
static int plan_finish(PlanSt *st, const Ma2TravView *pv)
{
    g_last_exp = st->expanded;
    if (st->capped) { c7_add("pathcap", "hits", 1); c7_max("pathcap", "hit_exp_max", st->expanded); }
    const Ma2TravView v = *pv;
    Ma2Path *p = st->p; Ma2Vec3 from = st->from, to = st->to; float radius = st->radius;
    const Ma2PathObstacle *dyn = st->dyn; int ndyn = st->ndyn;
    const int W = v.w, found = st->found, gok = st->gok;
    int best = st->best, pit_replaced = 0;
    /* PitPartial (default ON, RECOMP_BOT_PIT_PARTIAL / [Bots] PitPartial; 1.0 pass, Worker B): a search that ran dry without the goal ends at the cell
     * nearest it -- and that cell could be the landing of a one-way drop: the route walked the bot off a ledge into a pocket it cannot leave (sw14: a
     * BattleArmor stepped 35 m into a gully 22 m short of a roof powerup and stood there 17 s, hp 350, until the match ended). When the search is
     * complete (the heap ran dry, not capped) the partial route ends at the nearest cell reached WITHOUT a drop link instead. A capped search keeps
     * the old end (its far side is unexplored, not known to be a pocket). [C7] pitpartial: replaced (ends moved off a drop landing). */
    {   static int pp = -1;
        if (pp < 0) pp = c7_on("pitpartial", "RECOMP_BOT_PIT_PARTIAL", "PitPartial", 1);
        if (pp && !found && st->exhausted && S.ow[best] && st->best0 != best) { best = st->best0; pit_replaced = 1; c7_add("pitpartial", "replaced", 1); }
    }

    /* Walk back best -> start into a temporary cell list (reversed). */
    static int cells[1 << 16];
    int nc = 0, c = best;
    while (nc < (int)(sizeof cells / sizeof cells[0])) {
        cells[nc++] = c;
        int d = S.from[c];
        if (d == 8) break;
        int k2 = S.jump[c] ? (S.jump[c] & (PATH_UP_FLAG - 1)) : 1;
        int cx = c % W - DXS[d] * k2, cz = c / W - DZS[d] * k2;
        c = cz * W + cx;
    }
    /* String pulling: from each anchor, jump to the farthest cell still in clear sight. */
    int k = nc - 1;                                             /* cells[nc-1] = start */
    float ax = from.x, az = from.z;
    while (k > 0 && p->n < MA2_PATH_MAX - 1) {
        /* Extend along the cell path while the straight line from the anchor stays clear. */
        int j = k - 1;
        for (int t = k - 1; t >= 0 && t >= k - PATH_PULL_LOOK; t--) {
            int cc = cells[t];
            if (line_free(&v, ax, az, (cc % W) * v.cell, (cc / W) * v.cell, radius, dyn, ndyn)) j = t;
            else break;
        }
        int cc = cells[j];
        p->pt[p->n].x = (cc % W) * v.cell; p->pt[p->n].y = from.y; p->pt[p->n].z = (cc / W) * v.cell;
        p->drop[p->n] = (unsigned char)(j == k - 1 && S.jump[cc] > 1 ? (S.jump[cc] & PATH_UNDER_FLAG ? 3 : S.jump[cc] & PATH_UP_FLAG ? 2 : 1) : 0);   /* the leg into it steps off a ledge (2: jets up one) */
        p->n++;
        ax = p->pt[p->n - 1].x; az = p->pt[p->n - 1].z;
        k = j;
    }
    /* LinkFoot (default ON, RECOMP_BOT_LINK_FOOT / [Bots] LinkFoot; 1.0 pass fix round 3, Worker B): a jet-up leg that starts at the start cell itself came out as
     * pt[0] = the landing, so the follower's next index stayed 0 and the controller's jet-up executor (it needs next > 0 and the foot pt[next-1] within 8 m)
     * never ran: the body walked at the landing through the wall. The start cell now leads the route as the foot of the leg. [C7] linkfoot: legs. */
    {   static int lf = -1;
        if (lf < 0) lf = c7_on("linkfoot", "RECOMP_BOT_LINK_FOOT", "LinkFoot", 1);
        if (lf && p->n > 0 && p->n < MA2_PATH_MAX && p->drop[0] == 2 && nc > 0) {
            int sc = cells[nc - 1];
            for (int i = p->n; i > 0; i--) { p->pt[i] = p->pt[i - 1]; p->drop[i] = p->drop[i - 1]; }
            p->pt[0].x = (sc % W) * v.cell; p->pt[0].y = from.y; p->pt[0].z = (sc / W) * v.cell; p->drop[0] = 0;
            p->n++; c7_add("linkfoot", "legs", 1);
        }
    }
    if (p->n == 0) { p->pt[0] = pit_replaced ? from : to; p->n = 1; }   /* PitPartial: nothing reachable is nearer than the start: stand (the controller holds and reports no path), never walk the raw line at the pocket */
    /* Reaching the goal cell: end on the requested point itself when it is in clear sight. */
    if (found && gok) {
        Ma2Vec3 e = p->pt[p->n - 1];
        float gx2 = e.x - to.x, gz2 = e.z - to.z;
        if (gx2 * gx2 + gz2 * gz2 < 1.0f) p->pt[p->n - 1] = to;
        else if (line_free(&v, e.x, e.z, to.x, to.z, radius * 0.5f, p->dyn, p->ndyn) && p->n < MA2_PATH_MAX) p->pt[p->n++] = to;
    }
    Ma2Vec3 e = p->pt[p->n - 1];
    p->end_gap = sqrtf((e.x - to.x) * (e.x - to.x) + (e.z - to.z) * (e.z - to.z));
    p->ok = found && gok;
    if (st->reach_slot >= 0 && st->reach_slot < REACH_SLOTS) {   /* ReachSet: a finished search that missed its goal knows the whole reachable set */
        if (!found && st->exhausted && !st->gclamped_goal && !st->capped) reach_store(st->reach_slot, S.stamp, st->cgen, v.w, v.h, st->now);
        else g_rs[st->reach_slot].valid = 0;
    }
    g_marks_on = 0;
    return p->n > 0;
}


int ma2_path_plan(Ma2Path *p, Ma2Vec3 from, Ma2Vec3 to, float radius,
                  const Ma2PathObstacle *dyn, int ndyn, double now)
{
    PlanSt st; Ma2TravView v;
    memset(&st, 0, sizeof st);
    st.p = p; st.from = from; st.to = to; st.radius = radius; st.dyn = dyn; st.ndyn = ndyn; st.now = now;
    int r = plan_begin(&st, &v);
    if (r == 0) return 0;
    if (r == 2) return 1;
    (void)plan_run(&st, &v, 0.0);
    return plan_finish(&st, &v);
}

/* Reach (1.0 pass, Worker B): is b on the walkable region (the grid's connected component, no drop or jet link) that a stands on? 1 yes, 0 no,
 * -1 unknown (no grid / off the grid / no walkable cell within 8 m of either). A pickup that fails it is on a roof, a deck or in another pocket: the
 * brain may skip it (a mech stood 27 s under a roof powerup on In Your Face; a BattleArmor 17 s in a gully beside one on Over the Falls). The
 * result only says "not by walking": a body with jets may still reach a ledge, so callers keep their own jet / roof logic for those. */
static int comp_near(const Ma2TravView *v, Ma2Vec3 p, int range_cells)
{
    int ix, iz; to_cell(v, p.x, p.z, &ix, &iz);
    if (ix < 0 || iz < 0 || ix >= v->w || iz >= v->h) return -1;
    if (v->comp[iz * v->w + ix] > 0) return v->comp[iz * v->w + ix];
    int best = -1, bd = 1 << 30;
    for (int dz = -range_cells; dz <= range_cells; dz++)
        for (int dx = -range_cells; dx <= range_cells; dx++) {
            int x = ix + dx, z = iz + dz;
            if (x < 0 || z < 0 || x >= v->w || z >= v->h || v->comp[z * v->w + x] <= 0) continue;
            int d = dx * dx + dz * dz;
            if (d < bd) { bd = d; best = v->comp[z * v->w + x]; }
        }
    return best;
}
int ma2_path_same_ground(Ma2Vec3 a, Ma2Vec3 b, int heavy)
{
    Ma2TravView v;
    int have = heavy ? ma2_trav_view_heavy(&v) : ma2_trav_view(&v);
    if (!have || !v.comp) return -1;
    int rc = (int)ceilf(8.0f / v.cell);
    int ca = comp_near(&v, a, rc), cb = comp_near(&v, b, rc);
    if (ca <= 0 || cb <= 0) return -1;
    return ca == cb;
}

/* ---- W7 PathSlice: the plan queue ----
 * A plan costs 40-230 ms on a big map. A request is queued with a copy of everything the search reads (the
 * obstacles, the set_* parameters at submit time, the time), the A* loop runs a few ms per game step
 * (ma2_pathq_pump), and the finished path is taken by its owner on its next tick. One job runs at a time, in
 * its own search ctx, so a synchronous ma2_path_plan in between cannot disturb it. A grid change
 * (ma2_trav_epoch) during the search restarts it (twice at most), so the path is the one a synchronous plan
 * would give on the grid as it is at the end. */
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);
static double pq_now_ms(void)
{
    static long long f; long long t;
    if (!f) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t * 1000.0 / (double)f;
}

void ma2_path_get_params(Ma2PathParams *o)
{
    o->crush = g_crush; o->drop = g_drop; o->dropk = g_dropk; o->drop_water = g_drop_water; o->up = g_up;
    o->slope_max = g_slope_max; o->pilot = g_pilot_plan;   /* TankSlope / PilotFace: a sliced plan keeps its own (bal40 review) */
    o->ndanger = g_ndanger; memcpy(o->danger, g_danger, sizeof o->danger);
}
void ma2_path_set_params(const Ma2PathParams *i)
{
    g_crush = i->crush; g_drop = i->drop; g_dropk = i->dropk; g_drop_water = i->drop_water; g_up = i->up; g_upx = upx_of(g_up);
    g_slope_max = i->slope_max; g_pilot_plan = i->pilot;
    g_ndanger = i->ndanger; memcpy(g_danger, i->danger, sizeof g_danger);
}

#define PQ_SLOTS 48
enum { PQ_FREE = 0, PQ_QUEUED, PQ_ACTIVE, PQ_DONE };
typedef struct PqSlot {
    int state, owner, ticket; unsigned seq;
    Ma2Vec3 from, to; float radius; double now;
    Ma2PathObstacle dyn[96]; int ndyn;
    Ma2PathParams pp;
    int ph, restarts; unsigned epoch;           /* 0 begin, 1 search, 2 finish */
    unsigned chk;                               /* ew-b8: the grid epoch up to which changes were judged harmless to this search */
    double ms;                                  /* time spent searching */
    double t_submit_ms;                         /* wall clock at submit (queue latency counter) */
    PlanSt st;
    Ma2Path path; int ret;
} PqSlot;
static PqSlot g_pq[PQ_SLOTS];
static PqSlot *g_pq_active;
static int g_pq_ticket; static unsigned g_pq_seq;
static unsigned g_pq_jobs, g_pq_restarts, g_pq_steps; static double g_pq_job_ms, g_pq_job_max;

static PqSlot *pq_find(int owner)
{
    for (int i = 0; i < PQ_SLOTS; i++) if (g_pq[i].state != PQ_FREE && g_pq[i].owner == owner) return &g_pq[i];
    return NULL;
}
int ma2_pathq_state(int owner) { PqSlot *s = pq_find(owner); return s ? s->state : 0; }
void ma2_pathq_cancel(int owner)
{
    PqSlot *s = pq_find(owner);
    if (!s) return;
    if (s == g_pq_active) g_pq_active = NULL;
    s->state = PQ_FREE;
}
int ma2_pathq_submit(int owner, Ma2Vec3 from, Ma2Vec3 to, float radius, const Ma2PathObstacle *dyn, int ndyn, double now, const Ma2PathParams *pp)
{
    PqSlot *s = pq_find(owner);
    if (s && (s->state == PQ_ACTIVE || s->state == PQ_DONE)) return -2;      /* running or waiting to be taken */
    int was_queued = s && s->state == PQ_QUEUED;
    if (!s) {
        for (int i = 0; i < PQ_SLOTS && !s; i++) if (g_pq[i].state == PQ_FREE) s = &g_pq[i];
        if (!s) return 0;
        s->seq = ++g_pq_seq;
    }
    s->state = PQ_QUEUED; s->owner = owner; s->ticket = ++g_pq_ticket; if (g_pq_ticket > 0x3FFFFFFF) g_pq_ticket = 1;
    s->from = from; s->to = to; s->radius = radius; s->now = now;
    s->ndyn = ndyn > 96 ? 96 : ndyn;
    if (s->ndyn > 0) memcpy(s->dyn, dyn, (size_t)s->ndyn * sizeof(Ma2PathObstacle));
    if (pp) s->pp = *pp; else ma2_path_get_params(&s->pp);
    s->ph = 0; s->restarts = 0; s->ret = 0; if (!was_queued) s->t_submit_ms = pq_now_ms();
    return s->ticket;
}
/* 1: done (copied out, slot freed), 0: still queued / running, -1: no such request (cancelled, replaced). */
int ma2_pathq_take(int owner, int ticket, Ma2Path *out, int *ret)
{
    PqSlot *s = pq_find(owner);
    if (!s || s->ticket != ticket) return -1;
    if (s->state != PQ_DONE) return 0;
    *out = s->path; if (ret) *ret = s->ret;
    s->state = PQ_FREE;
    return 1;
}
/* ew-b8 B8.7 (R5 E7): a new level -- every queued / running / finished job belongs to the old grid. */
void ma2_pathq_reset(void)
{
    for (int i = 0; i < PQ_SLOTS; i++) g_pq[i].state = PQ_FREE;
    g_pq_active = NULL;
}
/* ms the oldest unfinished plan (queued or running) has waited since submit (0: none). */
double ma2_pathq_oldest_ms(void)
{
    double oldest = 0.0, now = pq_now_ms();
    for (int i = 0; i < PQ_SLOTS; i++)
        if ((g_pq[i].state == PQ_QUEUED || g_pq[i].state == PQ_ACTIVE) && now - g_pq[i].t_submit_ms > oldest) oldest = now - g_pq[i].t_submit_ms;
    return oldest;
}
int ma2_pathq_pending(void)
{
    int n = 0;
    for (int i = 0; i < PQ_SLOTS; i++) if (g_pq[i].state == PQ_QUEUED || g_pq[i].state == PQ_ACTIVE) n++;
    return n;
}
void ma2_pathq_stats(unsigned *jobs, unsigned *restarts, unsigned *steps, double *job_ms, double *job_max)
{
    *jobs = g_pq_jobs; *restarts = g_pq_restarts; *steps = g_pq_steps; *job_ms = g_pq_job_ms; *job_max = g_pq_job_max;
}

/* One slice of a job; 1 = finished (s->ret, s->path valid). */
static int pq_step(PqSlot *s, double deadline)
{
    Ma2PathParams save; double save_now = g_plan_now; int done = 0;
    Ma2TravView v;
    ma2_path_get_params(&save);
    ma2_path_set_params(&s->pp);
    g_plan_now = s->now;                                       /* push-x3 E3: drop_forbidden compares against this job's clock */
    g_sp = &g_sctx[1];
    int save_slot = g_reach_slot; g_reach_slot = s->owner;      /* ReachSet: this job's bot */
    for (;;) {
        if (s->ph == 0) {
            s->epoch = ma2_trav_epoch(); s->chk = s->epoch;
            memset(&s->st, 0, sizeof s->st);
            s->st.p = &s->path; s->st.from = s->from; s->st.to = s->to; s->st.radius = s->radius;
            s->st.dyn = s->dyn; s->st.ndyn = s->ndyn; s->st.now = s->now;
            int r = plan_begin(&s->st, &v);
            if (r == 0) { s->ret = 0; done = 1; break; }
            if (r == 2) { s->ret = 1; done = 1; break; }
            s->ph = 1;
        } else {
            int have = s->path.crush ? ma2_trav_view_heavy(&v) : ma2_trav_view(&v);
            if (!have) { memset(&s->path, 0, sizeof s->path); s->path.goal = s->to; s->path.radius = s->radius; s->path.pt[0] = s->to; s->path.n = 1; s->path.ok = 1; s->ret = 1; done = 1; break; }
            if (s->ph == 1 && (v.w != s->st.gw || v.h != s->st.gh)) {      /* push-x3: a different map's grid (size changed): never continue on it */
                s->restarts = 0; g_pq_restarts++; s->ph = 0; s->epoch = ma2_trav_epoch(); s->chk = s->epoch; continue;
            }
            if (s->chk != ma2_trav_epoch() && s->ph == 1) {                 /* the grid changed under the search */
                /* ew-b8 (R1 P1-4): start over only if the change is near this search (its endpoints' box, 50 m round); otherwise keep
                 * going -- the plan keeps the epoch it started on, so its bot still checks the finished route against the change. */
                static int pr = -1;
                if (pr < 0) pr = c7_on("replan_rect", "RECOMP_BOT_REPLAN_RECT", "ReplanRect", 1);
                float pad = 50.f + s->radius;
                float bx0 = (s->from.x < s->to.x ? s->from.x : s->to.x) - pad, bx1 = (s->from.x > s->to.x ? s->from.x : s->to.x) + pad;
                float bz0 = (s->from.z < s->to.z ? s->from.z : s->to.z) - pad, bz1 = (s->from.z > s->to.z ? s->from.z : s->to.z) + pad;
                if (pr && !ma2_trav_changed_since(s->chk, bx0, bz0, bx1, bz1)) { s->chk = ma2_trav_epoch(); c7_add("replan_rect", "job_kept", 1); }
                else {
                    if (s->restarts < 2) { s->restarts++; g_pq_restarts++; s->ph = 0; c7_add("replan_rect", "job_restart", 1); continue; }
                    s->epoch = ma2_trav_epoch(); s->chk = s->epoch;
                }
            }
        }
        if (s->ph == 1) {
            g_marks_on = 1;
            int over = plan_run(&s->st, &v, deadline);
            g_marks_on = 0;
            if (!over) break;
            s->ph = 2;
            if (deadline > 0.0 && pq_now_ms() >= deadline) break;          /* the walk back next step */
        }
        if (s->ph == 2) {
            g_marks_on = 1;
            s->ret = plan_finish(&s->st, &v);
            s->path.epoch = s->epoch;                                       /* push-x3 E1: the grid epoch this plan was searched on */
            g_marks_on = 0;
            done = 1; break;
        }
    }
    g_sp = &g_sctx[0];
    g_reach_slot = save_slot;
    ma2_path_set_params(&save); g_plan_now = save_now;
    return done;
}
/* Runs queued plans for up to budget_ms (at least one slice). Returns the ms used. */
double ma2_pathq_pump(double budget_ms)
{
    double t0 = pq_now_ms(), dl = t0 + budget_ms;
    for (int guard = 0; guard < 4 * PQ_SLOTS; guard++) {
        PqSlot *s = g_pq_active;
        if (!s) {
            for (int i = 0; i < PQ_SLOTS; i++)
                if (g_pq[i].state == PQ_QUEUED && (!s || (int)(g_pq[i].seq - s->seq) < 0)) s = &g_pq[i];
            if (!s) break;
            s->state = PQ_ACTIVE; s->ph = 0; g_pq_active = s; s->restarts = 0; s->ms = 0.0;
            c7_max("pathslice", "queue_ms_max", (pq_now_ms() - s->t_submit_ms));   /* push-x3: real queue latency (submit to activation) */
            c7_add("pathslice", "queue_ms_sum", (pq_now_ms() - s->t_submit_ms));
        }
        double a0 = pq_now_ms();
        int d = pq_step(s, dl);
        double a1 = pq_now_ms();
        g_pq_steps++; g_pq_job_ms += a1 - a0; s->ms += a1 - a0;
        if (!d) break;
        g_pq_jobs++; if (s->ms > g_pq_job_max) g_pq_job_max = s->ms;
        s->state = PQ_DONE; g_pq_active = NULL;
        if (a1 >= dl) break;
    }
    return pq_now_ms() - t0;
}

static float hd(Ma2Vec3 a, Ma2Vec3 b) { float dx = a.x - b.x, dz = a.z - b.z; return sqrtf(dx * dx + dz * dz); }
/* SkipHump (default ON, RECOMP_BOT_SKIP_HUMP / [Bots] SkipHump; parity pass 14, 11-12): the follower's corner skip ("the corner after the next
 * is in clear sight") tested clearance only, so it cut over a mound the planner had routed round on the flat: Ill Wind Snatch It basin,
 * V24_81a2 T2 first arriver passed pt[0] 864,1488 (flat -20 round the mound's north foot) at 1054,1503 and steered at pt[1] 742,1408 for 5.1 s,
 * over the 12 m mound (-20 -> -8 -> -20, 926,1464). Now a skip whose line rises more than low_jet_rise (the body's walkable grade across its
 * own width, LowJetWalk; at least 2 m) over the straight grade between its two ends keeps the corner. [C7] skiphump: kept. */
static int skip_hump(const Ma2TravView *v, Ma2Vec3 a, Ma2Vec3 b, float radius, int heavy)
{
    static int on = -1;
    if (on < 0) on = c7_on("skiphump", "RECOMP_BOT_SKIP_HUMP", "SkipHump", 1);
    if (!on || !v->sf) return 0;
    float lim = low_jet_rise(radius, heavy); if (lim < 2.0f) lim = 2.0f;
    float dx = b.x - a.x, dz = b.z - a.z, len = sqrtf(dx * dx + dz * dz);
    int n = (int)ceilf(len / v->cell), ia, ja, ib, jb;
    to_cell(v, a.x, a.z, &ia, &ja); to_cell(v, b.x, b.z, &ib, &jb);
    if (n < 2 || ia < 0 || ja < 0 || ib < 0 || jb < 0 || ia >= v->w || ib >= v->w || ja >= v->h || jb >= v->h) return 0;
    float ya = v->sf[ja * v->w + ia], yb = v->sf[jb * v->w + ib];
    if (ya < -1e8f || yb < -1e8f) return 0;
    for (int i = 1; i < n; i++) {
        float t = (float)i / (float)n; int ix, iz;
        to_cell(v, a.x + dx * t, a.z + dz * t, &ix, &iz);
        if (ix < 0 || iz < 0 || ix >= v->w || iz >= v->h) continue;
        float s = v->sf[iz * v->w + ix];
        if (s > -1e8f && s > ya + (yb - ya) * t + lim) { c7_add("skiphump", "kept", 1); return 1; }
    }
    return 0;
}

Ma2Vec3 ma2_path_follow(Ma2Path *p, Ma2Vec3 pos, float reach, int *arrived)
{
    if (arrived) *arrived = 0;
    if (p->n <= 0) { if (arrived) *arrived = 1; return pos; }
    while (p->next < p->n - 1 && hd(pos, p->pt[p->next]) < reach) p->next++;
    /* Skip a corner once the one after it is in clear sight (we were pushed past it). */
    Ma2TravView v;
    if (p->next < p->n - 1 && !(p->drop[p->next + 1] && c7_on("leaptop", "RECOMP_BOT_LEAP_TOP", "LeapTop", 1)) &&   /* LeapTop: never skip the top of a leap */
        (p->crush ? ma2_trav_view_heavy(&v) : ma2_trav_view(&v)) &&
        line_free(&v, pos.x, pos.z, p->pt[p->next + 1].x, p->pt[p->next + 1].z, p->radius, p->dyn, p->ndyn) &&
        !skip_hump(&v, pos, p->pt[p->next + 1], p->radius, p->crush))   /* SkipHump */
        p->next++;
    if (arrived && p->next == p->n - 1 && hd(pos, p->pt[p->next]) < reach) *arrived = 1;
    return p->pt[p->next];
}

int ma2_path_off(const Ma2Path *p, Ma2Vec3 pos, float tol)
{
    if (p->n <= 0) return 1;
    Ma2Vec3 b = p->pt[p->next];
    /* push-x3 E2: before the first corner the leg is start -> pt[0] (it was pt[0] -> pt[0], so every fresh path read as off by
     * more than the tolerance and was replanned at the 0.5 s floor until the bot came within 8 m of its first corner) */
    Ma2Vec3 a = p->next > 0 ? p->pt[p->next - 1] : (p->has_from ? p->from : b);
    float vx = b.x - a.x, vz = b.z - a.z, l2 = vx * vx + vz * vz, t = 0.0f;
    if (l2 > 1e-4f) { t = ((pos.x - a.x) * vx + (pos.z - a.z) * vz) / l2; t = t < 0 ? 0 : t > 1 ? 1 : t; }
    float qx = a.x + vx * t - pos.x, qz = a.z + vz * t - pos.z;
    return qx * qx + qz * qz > tol * tol;
}

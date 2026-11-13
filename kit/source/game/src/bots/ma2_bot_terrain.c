/* See ma2_bot_terrain.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ma2_bot_terrain.h"

#define PI_F 3.14159265f
#define BIN_W (2.0f * PI_F / MA2T_NBIN)
#define PROBE_BUDGET 16        /* guest probes per tick, per bot */
#define CACHE_AGE 0.40         /* s */
#define CACHE_MOVE 2.5f        /* m */

typedef struct KindTerrain {
    float d[MA2T_NDIST];   /* probe distances (m)                              */
    float slope;           /* max climbable rise/run                           */
    float slack;           /* extra step height tolerated per segment (m)      */
    float drop;            /* max drop per segment (m)                         */
    float jump_h;          /* max obstacle rise a jump can clear (m), 0 = none */
    float width_pen;       /* cost added per blocked neighbouring bin          */
    int wide;              /* flanking headings must be clear too              */
} KindTerrain;

static int kind_params(int kind, KindTerrain *k)
{
    switch (kind) {
    case MA2_KIND_PILOT:
        *k = (KindTerrain){ { 1.5f, 4.0f, 8.0f }, 0.70f, 0.40f, 4.0f, 0.0f, 0.10f }; return 1;
    case MA2_KIND_BATTLEARMOR:
        *k = (KindTerrain){ { 3.0f, 7.0f, 14.0f }, 0.70f, 0.80f, 10.0f, 6.0f, 0.15f }; return 1;
    case MA2_KIND_MECH:
        *k = (KindTerrain){ { 7.0f, 16.0f, 28.0f }, 0.40f, 0.50f, 14.0f, 6.0f, 0.40f, 1 }; return 1;
    case MA2_KIND_TANK:
    case MA2_KIND_MOVER:
        *k = (KindTerrain){ { 7.0f, 16.0f, 28.0f }, 0.35f, 0.50f, 10.0f, 0.0f, 0.40f, 1 }; return 1;
    default: return 0;
    }
}

void ma2_terrain_init(Ma2TerrainState *st, int bot_index)
{
    memset(st, 0, sizeof *st);
    st->id = bot_index;
    st->last_bin = -1;
}

static int wrap_bin(int k) { k %= MA2T_NBIN; return k < 0 ? k + MA2T_NBIN : k; }
static float bin_angle(int k) { return (float)k * BIN_W; }
static float wrap_pi(float a)
{
    while (a > PI_F) a -= 2.0f * PI_F;
    while (a < -PI_F) a += 2.0f * PI_F;
    return a;
}
static float hd(Ma2Vec3 a, Ma2Vec3 b) { float x = a.x - b.x, z = a.z - b.z; return sqrtf(x * x + z * z); }

static int sample_bin(Ma2TerrainState *st, int bi, const KindTerrain *kt, int ndist,
                      Ma2Vec3 p, double now, Ma2GroundFn g, void *gctx)
{
    Ma2TerrainBin *b = &st->bin[bi];
    float a = bin_angle(bi), sx = sinf(a), cz = cosf(a);
    int i;
    for (i = 0; i < MA2T_NDIST; i++) {
        int ok = 0; float h = p.y;
        if (i < ndist) h = g(gctx, p.x + sx * kt->d[i], p.y + 40.0f, p.z + cz * kt->d[i], &ok);
        b->h[i] = ok ? h : st->h0; b->ok[i] = ok;
    }
    b->t = now; b->origin = p;
    st->n_probes += ndist;
    return ndist;
}

static int bin_valid(const Ma2TerrainBin *b, Ma2Vec3 p, double now)
{
    return b->t > 0.0 && now - b->t < CACHE_AGE && hd(b->origin, p) < CACHE_MOVE;
}

void ma2_terrain_note_stuck(Ma2TerrainState *st, Ma2Vec3 pos, Ma2Vec3 move, double now)
{
    if (!st || (move.x * move.x + move.z * move.z) < 1e-6f) return;
    st->sm[st->sm_next].pos = pos;
    st->sm[st->sm_next].ang = atan2f(move.x, move.z);
    st->sm[st->sm_next].until = now + 10.0;
    st->sm_next = (st->sm_next + 1) % 4;
    if (getenv("RECOMP_TERRAIN_LOG")) {
        int i;
        fprintf(stderr, "[TERR3] t=%.2f bot=%d pos=%.1f,%.1f,%.1f ang=%.2f h0=%.1f", now, st->id, pos.x, pos.y, pos.z,
                atan2f(move.x, move.z), st->h0);
        for (i = 0; i < MA2T_NBIN; i++)
            fprintf(stderr, " |%d:%.1f,%.1f,%.1f", i, st->bin[i].h[0], st->bin[i].h[1], st->bin[i].h[2]);
        fprintf(stderr, "\n");
    }
}

static int stuck_blocked(const Ma2TerrainState *st, int bi, Ma2Vec3 p, double now)
{
    int i;
    for (i = 0; i < 4; i++)
        if (st->sm[i].until > now && hd(st->sm[i].pos, p) < 12.0f &&
            fabsf(wrap_pi(bin_angle(bi) - st->sm[i].ang)) < 0.6f)
            return 1;
    return 0;
}

typedef struct BinEval {
    int known, blocked, jumpable, near_blocked;
    float rise;          /* worst uphill steepness relative to the limit (1 = at limit) */
} BinEval;

static BinEval eval_bin(const Ma2TerrainBin *b, const KindTerrain *kt, float h0, int ndist)
{
    BinEval e; int i; float prev_h = h0, prev_d = 0.0f;
    memset(&e, 0, sizeof e);
    e.known = 1; e.jumpable = 1;
    for (i = 0; i < ndist; i++) {
        float gap = kt->d[i] - prev_d, rise = b->h[i] - prev_h, cum = b->h[i] - h0;
        float allow = kt->slack + kt->slope * gap, allow_c = kt->slack + 0.9f * kt->slope * kt->d[i];
        float r = rise / allow;
        if (r > e.rise) e.rise = r;
        if (rise > allow || cum > allow_c) {
            float over = cum > rise ? cum : rise;
            e.blocked = 1;
            if (i == 0) e.near_blocked = 1;
            if (kt->jump_h <= 0.0f || over > kt->jump_h) e.jumpable = 0;
        }
        if (-rise > kt->drop) { e.blocked = 1; e.jumpable = 0; }
        prev_h = b->h[i]; prev_d = kt->d[i];
    }
    if (!e.blocked) e.jumpable = 0;
    return e;
}

int ma2_terrain_steer(Ma2TerrainState *st, const Ma2PlayerView *me, int kind, int can_jump,
                      float goal_dist, Ma2GroundFn g, void *gctx, double now,
                      Ma2Vec3 *move, float *speed, int *jump)
{
    KindTerrain kt;
    int ndist = MA2T_NDIST, used = 0, changed = 0;
    float mx, mz, ml, want, fidx;
    int c0, k, off, lo, hi, best = -1, ahead_free;
    BinEval ev[MA2T_NBIN];
    float best_cost = 1e9f;
    static int s_log = -1, s_off = -1;
    Ma2Vec3 move0; float speed0;

    if (jump) *jump = 0;
    if (!st || !me || !move || !speed || !g || !kind_params(kind, &kt)) return 0;
    if (s_log < 0) { const char *e = getenv("RECOMP_TERRAIN_LOG"); s_log = e && *e && *e != '0'; }
    if (s_off < 0) { const char *e = getenv("RECOMP_TERRAIN_OFF"); s_off = e && *e && *e != '0'; }
    move0 = *move; speed0 = *speed;
    if (kind != st->kind) { memset(st->bin, 0, sizeof st->bin); st->h0_t = 0; st->last_bin = -1; st->kind = kind; }
    mx = move->x; mz = move->z; ml = sqrtf(mx * mx + mz * mz);
    if (*speed < 0.2f || ml < 1e-3f) return 0;
    mx /= ml; mz /= ml;

    if (goal_dist > 0.0f) {
        if (goal_dist < kt.d[0] + 3.0f) return 0;
        while (ndist > 1 && kt.d[ndist - 1] > goal_dist) ndist--;
    }

    if (!(st->h0_t > 0.0 && now - st->h0_t < CACHE_AGE && hd(st->h0_origin, me->pos) < CACHE_MOVE)) {
        int ok = 0;
        st->h0 = g(gctx, me->pos.x, me->pos.y + 40.0f, me->pos.z, &ok);
        if (!ok || st->h0 > me->pos.y + 1.0f) st->h0 = me->pos.y;   /* own body / overhead object */
        st->h0_t = now; st->h0_origin = me->pos; used++; st->n_probes++;
    }

    if (now < st->jump_until) { *speed = 1.0f; return 0; }     /* mid-jump: hold heading */

    want = atan2f(mx, mz);                       /* heading: (sin, cos) = (x, z) */
    fidx = want / BIN_W;
    c0 = wrap_bin((int)floorf(fidx + 0.5f));
    lo = wrap_bin((int)floorf(fidx)); hi = wrap_bin((int)floorf(fidx) + 1);

    for (k = 0; k < MA2T_NBIN; k++) ev[k].known = 0;
    /* evaluate in order of angular distance; stop widening once a free heading
     * within +-45 degrees exists */
    for (off = 0; off <= 7; off++) {
        int s;
        for (s = 0; s < (off == 0 ? 1 : 2); s++) {
            int bi = wrap_bin(c0 + (s == 0 ? off : -off));
            if (!bin_valid(&st->bin[bi], me->pos, now)) {
                if (used + ndist > PROBE_BUDGET) continue;
                used += sample_bin(st, bi, &kt, ndist, me->pos, now, g, gctx);
            }
            ev[bi] = eval_bin(&st->bin[bi], &kt, st->h0, ndist);
            if (stuck_blocked(st, bi, me->pos, now)) { ev[bi].blocked = 1; ev[bi].jumpable = 0; }
        }
        if (off == 2) {
            int q;
            for (q = -2; q <= 2; q++) { int bi = wrap_bin(c0 + q); if (ev[bi].known && !ev[bi].blocked) best = bi; }
            if (best >= 0) break;
        }
    }

    ahead_free = ev[lo].known && !ev[lo].blocked && ev[hi].known && !ev[hi].blocked;
    if (ahead_free && kt.wide) {            /* big units: also keep the flanking headings clear near by */
        int l2 = wrap_bin(lo - 1), h2 = wrap_bin(hi + 1);
        if ((ev[l2].known && ev[l2].near_blocked) || (ev[h2].known && ev[h2].near_blocked)) ahead_free = 0;
    }
    if (ahead_free) {
        float cr = ev[lo].rise > ev[hi].rise ? ev[lo].rise : ev[hi].rise;
        if (cr > 0.6f) { *speed *= 1.0f - 0.35f * fminf((cr - 0.6f) / 0.4f, 1.0f); changed = 1; }
        st->last_bin = -1;
    } else {
        float dev_best = 9.0f;
        int cb = c0;
        for (k = 0; k < MA2T_NBIN; k++) {
            float dev, c; int nb;
            if (!ev[k].known || ev[k].blocked) continue;
            dev = fabsf(wrap_pi(bin_angle(k) - want));
            c = dev + 0.35f * ev[k].rise;
            for (nb = -1; nb <= 1; nb += 2) {
                int j = wrap_bin(k + nb);
                if (ev[j].known && ev[j].blocked) c += kt.width_pen;
            }
            if (k == st->last_bin) c -= 0.30f;
            if (c < best_cost) { best_cost = c; best = k; dev_best = dev; }
        }
        if (best_cost >= 1e8f) best = -1;
        st->n_blocked++;
        if (can_jump && kt.jump_h > 0.0f && now >= st->next_jump &&
            ev[cb].known && ev[cb].jumpable && (best < 0 || dev_best > 0.8f)) {
            if (jump) *jump = 1;
            st->next_jump = now + 3.0; st->jump_until = now + 1.1;
            st->n_jumps++; *speed = 1.0f; changed = 1;
        } else if (best >= 0) {
            float a = bin_angle(best);
            move->x = sinf(a); move->z = cosf(a);
            if (dev_best > 0.8f) *speed *= 0.7f;
            if (ev[best].rise > 0.6f) *speed *= 1.0f - 0.3f * fminf((ev[best].rise - 0.6f) / 0.4f, 1.0f);
            st->last_bin = best; st->n_steered++; changed = 1;
        } else {
            /* boxed in: with jump jets hop over the least-bad heading, else let stuck recovery act */
            if (can_jump && kt.jump_h > 0.0f && now >= st->next_jump) {
                int lb = -1; float lr = 1e9f;
                for (k = 0; k < MA2T_NBIN; k++)
                    if (ev[k].known && ev[k].rise < lr && fabsf(wrap_pi(bin_angle(k) - want)) < 1.6f) { lr = ev[k].rise; lb = k; }
                if (lb >= 0) { move->x = sinf(bin_angle(lb)); move->z = cosf(bin_angle(lb)); }
                if (jump) *jump = 1;
                st->next_jump = now + 3.0; st->jump_until = now + 1.1;
                st->n_jumps++; *speed = 1.0f;
            } else *speed *= 0.6f;
            changed = 1;
        }
    }

    if (s_log && now >= st->log_next) {
        float hs = sqrtf(me->vel.x * me->vel.x + me->vel.z * me->vel.z);
        int vb = wrap_bin((int)floorf(atan2f(me->vel.x, me->vel.z) / BIN_W + 0.5f));
        float slope = ev[vb].known ? (st->bin[vb].h[0] - st->h0) / kt.d[0] : 99.0f;
        fprintf(stderr, "[TERR] t=%.2f bot=%d kind=%d hspeed=%.2f cmd=%.2f slope1=%.2f blk=%d steer=%d jump=%d\n",
                now, st->id, kind, hs, *speed, slope, !ahead_free, st->last_bin >= 0, jump ? *jump : 0);
        if (ev[c0].known)
            fprintf(stderr, "[TERR2] t=%.2f bot=%d kind=%d pos=%.0f,%.0f dir=%d h0=%.1f h=%.1f,%.1f,%.1f rise=%.2f blk=%d\n",
                    now, st->id, kind, me->pos.x, me->pos.z, c0, st->h0, st->bin[c0].h[0], st->bin[c0].h[1],
                    st->bin[c0].h[2], ev[c0].rise, ev[c0].blocked);
        st->log_next = now + 0.5;
    }
    if (s_off) { *move = move0; *speed = speed0; if (jump) *jump = 0; return 0; }   /* measure-only mode */
    return changed;
}

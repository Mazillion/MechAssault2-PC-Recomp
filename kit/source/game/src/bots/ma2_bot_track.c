/* MechAssault 2 offline bots - observed-motion tracks (C1) and intent (C2); see ma2_bot_track.h.
 *
 * NOT a standalone translation unit: #include it from ma2_bots_core.c (like ma2_bot_entry.c).
 * Plain C, no guest memory: positions come from the caller. */
#include <math.h>
#include <string.h>
#include "ma2_bot_track.h"

#define TRK_MAX   48          /* (team, unit) pairs */
#define TRK_RING  24          /* samples kept per pair */
#define TRK_SPAN  1.0         /* seconds of history used */
#define TRK_STEP  0.04        /* minimum spacing between kept samples */

typedef struct Trk {
    int      team;
    uint32_t unit;
    int      n, head;         /* samples in the ring, index of the newest */
    double   t[TRK_RING];
    Ma2Vec3  p[TRK_RING];
} Trk;
static Trk g_trk[TRK_MAX];

typedef struct Intent {
    int      team;
    uint32_t unit, target;
    double   since;           /* start of the current 'heading toward' spell, 0 = not in one */
    double   last_true;       /* last tick it was heading toward */
    double   active_until;    /* held long enough: counts until here (linger) */
    double   touched;         /* last update, for reuse of the slot */
} Intent;
#define INT_MAX_N 64
static Intent g_int[INT_MAX_N];

void ma2_track_reset(void)
{
    memset(g_trk, 0, sizeof g_trk);
    memset(g_int, 0, sizeof g_int);
}

static Trk *trk_find(int team, uint32_t unit, int create, double now)
{
    int i, free_i = -1, old_i = 0;
    for (i = 0; i < TRK_MAX; i++) {
        Trk *k = &g_trk[i];
        if (k->unit == unit && k->team == team && k->n) return k;
        if (!k->n && free_i < 0) free_i = i;
        if (k->n && g_trk[old_i].n && k->t[k->head] < g_trk[old_i].t[g_trk[old_i].head]) old_i = i;
    }
    if (!create) return NULL;
    i = free_i >= 0 ? free_i : old_i;          /* full: reuse the stalest */
    memset(&g_trk[i], 0, sizeof g_trk[i]);
    g_trk[i].team = team; g_trk[i].unit = unit;
    (void)now;
    return &g_trk[i];
}

void ma2_track_observe(int team, uint32_t unit, Ma2Vec3 pos, double now)
{
    Trk *k;
    if (!unit || pos.x != pos.x || pos.z != pos.z) return;
    k = trk_find(team, unit, 1, now);
    if (k->n && now < k->t[k->head] - 1.0) k->n = 0;                  /* the clock went back: a new match */
    if (k->n && now - k->t[k->head] < TRK_STEP) { k->p[k->head] = pos; return; }   /* too soon: refresh the newest */
    k->head = k->n ? (k->head + 1) % TRK_RING : 0;
    k->t[k->head] = now; k->p[k->head] = pos;
    if (k->n < TRK_RING) k->n++;
}

static float wrap_pi(float a)
{
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

/* The oldest kept sample no older than `span` before the newest (index, or -1). */
static int trk_back(const Trk *k, double span)
{
    int i, best = -1;
    for (i = 1; i < k->n; i++) {
        int j = (k->head - i + TRK_RING) % TRK_RING;
        if (k->t[k->head] - k->t[j] > span) break;
        best = j;
    }
    return best;
}

int ma2_track_get(int team, uint32_t unit, double now, Ma2TrackState *out)
{
    Trk *k = trk_find(team, unit, 0, now);
    int j, m;
    double dt;
    memset(out, 0, sizeof *out);
    if (!k || now - k->t[k->head] > 1.0) return 0;
    out->pos = k->p[k->head]; out->seen = k->t[k->head];
    j = trk_back(k, TRK_SPAN);
    if (j < 0) return 1;                                               /* seen, no motion yet */
    dt = k->t[k->head] - k->t[j];
    if (dt < 0.2) return 1;
    out->vel.x = (float)((k->p[k->head].x - k->p[j].x) / dt);
    out->vel.y = (float)((k->p[k->head].y - k->p[j].y) / dt);
    out->vel.z = (float)((k->p[k->head].z - k->p[j].z) / dt);
    out->speed = sqrtf(out->vel.x * out->vel.x + out->vel.z * out->vel.z);
    out->heading = atan2f(out->vel.x, out->vel.z);
    out->valid = 1;
    /* turn rate: heading of the older half against the newer half */
    m = trk_back(k, dt * 0.5);
    if (m >= 0 && m != j) {
        double t1 = k->t[m] - k->t[j], t2 = k->t[k->head] - k->t[m];
        if (t1 > 0.05 && t2 > 0.05) {
            float h1 = atan2f(k->p[m].x - k->p[j].x, k->p[m].z - k->p[j].z);
            float h2 = atan2f(k->p[k->head].x - k->p[m].x, k->p[k->head].z - k->p[m].z);
            out->turn_rate = wrap_pi(h2 - h1) / (float)(0.5 * (t1 + t2) + 1e-3);
        }
    }
    return 1;
}

Ma2Vec3 ma2_track_predict(const Ma2TrackState *s, float dt, float *uncert)
{
    Ma2Vec3 p = s->pos;
    if (s->valid) {
        /* straight on along the observed velocity, bending with the observed turn rate */
        float h = s->heading + s->turn_rate * dt * 0.5f;
        p.x += sinf(h) * s->speed * dt;
        p.z += cosf(h) * s->speed * dt;
        p.y += s->vel.y * dt;
    }
    if (uncert) *uncert = 1.0f + (0.5f + 0.15f * s->speed) * dt;
    return p;
}

int ma2_track_heading_toward(const Ma2TrackState *s, Ma2Vec3 target, float cone_deg)
{
    float dx, dz, d, closing, ang;
    if (!s->valid || s->speed < 1.5f) return 0;                        /* standing or shuffling */
    dx = target.x - s->pos.x; dz = target.z - s->pos.z;
    d = sqrtf(dx * dx + dz * dz);
    if (d < 1.0f) return 1;                                            /* there */
    closing = (s->vel.x * dx + s->vel.z * dz) / d;
    if (closing < 1.0f) return 0;
    ang = fabsf(wrap_pi(atan2f(dx, dz) - s->heading));
    return ang <= cone_deg * 3.14159265f / 180.0f;
}

int ma2_intent_update(int team, uint32_t unit, uint32_t target, Ma2Vec3 target_pos, double now)
{
    Ma2TrackState s;
    Intent *e = NULL;
    int i, free_i = -1, old_i = 0, toward;
    for (i = 0; i < INT_MAX_N; i++) {
        Intent *q = &g_int[i];
        if (q->unit == unit && q->target == target && q->team == team && q->touched > 0.0) { e = q; break; }
        if (q->touched <= 0.0 && free_i < 0) free_i = i;
        if (q->touched < g_int[old_i].touched) old_i = i;
    }
    if (!e) {
        e = &g_int[free_i >= 0 ? free_i : old_i];
        memset(e, 0, sizeof *e);
        e->team = team; e->unit = unit; e->target = target;
    }
    if (now < e->touched - 1.0) { e->since = 0.0; e->active_until = 0.0; e->last_true = 0.0; }   /* new match */
    e->touched = now;
    toward = ma2_track_get(team, unit, now, &s) && ma2_track_heading_toward(&s, target_pos, MA2_INTENT_CONE_DEG);
    if (toward) {
        if (e->since <= 0.0 || now - e->last_true > 0.25) e->since = now;   /* a short gap breaks the spell */
        e->last_true = now;
        if (now - e->since >= MA2_INTENT_HOLD) e->active_until = now + MA2_INTENT_LINGER;
    }
    return e->active_until > 0.0 && now <= e->active_until;   /* (at match start both are 0) */
}

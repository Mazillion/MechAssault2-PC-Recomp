/* MechAssault 2 offline bots - terrain/obstacle awareness for ground units.
 *
 * Pure host C. The brain calls ma2_terrain_steer() once per tick with the
 * horizontal move direction/speed it wants; the module probes the surface
 * height (terrain + objects) on a fan of headings around it and returns an
 * adjusted direction/speed and an optional jump-jet request.
 */
#pragma once
#include "ma2_bot_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MA2T_NBIN  16     /* absolute heading bins, 22.5 degrees each */
#define MA2T_NDIST 3      /* probe distances per bin */

typedef struct Ma2TerrainBin {
    double t;                     /* sample time; <= 0 = empty */
    Ma2Vec3 origin;               /* bot position when sampled */
    float h[MA2T_NDIST];
    int ok[MA2T_NDIST];
} Ma2TerrainBin;

typedef struct Ma2TerrainState {
    int id;                       /* bot index (logging only) */
    int kind;                     /* kind the cache was built for */
    Ma2TerrainBin bin[MA2T_NBIN];
    double h0_t; Ma2Vec3 h0_origin; float h0;
    int last_bin;                 /* previously chosen bin, -1 none */
    double next_jump, jump_until; /* jump cooldown / keep-heading window */
    double log_next;
    struct { Ma2Vec3 pos; float ang; double until; } sm[4];   /* recent stuck spots: headings to avoid */
    int sm_next;
    int n_probes, n_steered, n_jumps, n_blocked;   /* diagnostics */
} Ma2TerrainState;

void ma2_terrain_init(Ma2TerrainState *st, int bot_index);

/* Adjust the brain's intended horizontal motion.
 *   me        : controlled unit (pos, vel)
 *   kind      : enum Ma2ActorKind (non-ground kinds are left untouched)
 *   can_jump  : unit has jump jets / jumps
 *   goal_dist : horizontal distance to the final goal in metres (<=0: unknown).
 *               Probes beyond it are dropped and very near goals (a vehicle to
 *               board) disable steering so the goal is not seen as an obstacle.
 *   move/speed: in/out, world horizontal direction (x,z used) and 0..1 speed
 *   jump      : out, 1 when a jump-jet tap should be issued now (may be NULL)
 * Returns 1 if move/speed/jump were altered. */
int ma2_terrain_steer(Ma2TerrainState *st, const Ma2PlayerView *me, int kind, int can_jump,
                      float goal_dist, Ma2GroundFn g, void *gctx, double now,
                      Ma2Vec3 *move, float *speed, int *jump);

/* Call when the brain declares the unit stuck: the heading it was pushing on is
 * avoided for ~10 s around that spot, so the next attempt goes another way. */
void ma2_terrain_note_stuck(Ma2TerrainState *st, Ma2Vec3 pos, Ma2Vec3 move, double now);

#ifdef __cplusplus
}
#endif

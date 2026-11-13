/* MechAssault 2 offline bots - observed-motion tracks (C1) and intent (C2).
 *
 * C1: per observing team, a short history (~1 s) of the positions it has seen of a unit, from
 * which velocity, heading and a short predicted track follow. Feed it only units the team can
 * actually sense; nothing here reads anyone's plan or controller.
 * C2: heading_toward(unit, target): the unit's observed motion points at the target (within a
 * cone), it is closing, and that has held for a moment; it lingers a little after it stops.
 *
 * Bots use it to see what a teammate is going for (BOTS-AI-PLAN.md C1/C2; first user: the
 * 'Mech yield to a human teammate). */
#ifndef MA2_BOT_TRACK_H
#define MA2_BOT_TRACK_H

#include "ma2_bot_api.h"

typedef struct Ma2TrackState {
    int     valid;          /* enough history for a velocity */
    Ma2Vec3 pos, vel;       /* last seen position, observed velocity (m/s, horizontal + vertical) */
    float   speed;          /* horizontal speed */
    float   heading;        /* horizontal direction of travel, radians from +z toward +x */
    float   turn_rate;      /* rad/s, + = turning toward +x */
    double  seen;           /* time of the last observation */
} Ma2TrackState;

/* Observe `unit` (an actor address: a new body is a new track) at `pos` for team `team`. */
void ma2_track_observe(int team, uint32_t unit, Ma2Vec3 pos, double now);
/* The team's track of `unit`; 0 = none (or not seen for over a second). */
int  ma2_track_get(int team, uint32_t unit, double now, Ma2TrackState *out);
/* Where the unit will be in `dt` seconds on its observed motion; *uncert = radius of doubt. */
Ma2Vec3 ma2_track_predict(const Ma2TrackState *s, float dt, float *uncert);

/* Right now: moving at walking pace or faster, closing on `target` and pointed at it within
 * `cone_deg` (horizontal). Distance does not matter. */
int  ma2_track_heading_toward(const Ma2TrackState *s, Ma2Vec3 target, float cone_deg);

/* C2 with memory: 1 while `unit` has been heading toward `target` (an entity id, at `target_pos`)
 * for at least MA2_INTENT_HOLD seconds, and for MA2_INTENT_LINGER seconds after it stops.
 * Call it every tick for the pairs you care about. */
#define MA2_INTENT_CONE_DEG 20.0f
#define MA2_INTENT_HOLD     0.5
#define MA2_INTENT_LINGER   2.0
int  ma2_intent_update(int team, uint32_t unit, uint32_t target, Ma2Vec3 target_pos, double now);

/* A new map / match: forget every track and intent. */
void ma2_track_reset(void);

#endif

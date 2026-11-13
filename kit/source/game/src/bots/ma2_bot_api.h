/* MechAssault 2 offline bots — shared interface between the staged modules.
 *
 * Owners (each module implements only its section; nobody edits this file
 * except the integrator — propose changes in your HANDOFF.md instead):
 *   core    : guest hooks, world snapshot, BotPlayer + BotController (ma2_bots_core.c,
 *             ma2_bot_player.c, ma2_bot_ctrl.c)
 *   nav     : path graph, A*, line of sight                   (ma2_bot_nav.c)
 *   brain   : strategy: orders for the controller             (ma2_bot_brain.c)
 *   ui      : New Game barrels, settings                      (ma2_bots_ui.c)
 *   content : callsigns + localized strings                   (ma2_bot_content.c)
 *
 * Design reference: MechAssault 2/BOTS_IMPLEMENTATION_GUIDE.md (revision 2).
 * Guest addresses are uint32_t Xbox virtual addresses. Brain and nav are pure
 * host code: they never touch guest memory directly; core fills Ma2World and
 * provides the ground-probe callback.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MA2_BOT_MAX_PLAYERS   12
#define MA2_WORLD_MAX_PLAYERS 24     /* players + AI combatants in co-op modes */
#define MA2_BOT_MAX_VEHICLES  64
#define MA2_BOT_MAX_PICKUPS   96    /* F67: was 32 (switch PickupCap off keeps 32 in use) */
#define MA2_BOT_PORT_BASE     4      /* bot controller ids are 4..15 */

typedef struct Ma2Vec3 { float x, y, z; } Ma2Vec3;   /* game axes: y is up */

/* ---- World snapshot (core fills, read-only for brain) ---- */
enum Ma2ActorKind {
    MA2_KIND_NONE = 0,
    MA2_KIND_PILOT,        /* MACorporeal on foot */
    MA2_KIND_MECH,
    MA2_KIND_BATTLEARMOR,  /* MAMech with IsBattleArmor */
    MA2_KIND_TANK,
    MA2_KIND_VTOL,         /* MAFlyer with IsVTOL */
    MA2_KIND_FLYER,
    MA2_KIND_MOVER,        /* other ground vehicles */
    MA2_KIND_TURRET
};

/* Defensive systems (DEFENSE_SPEC.md): shield bubble, camouflage (Null Signature), jamming,
 * absorber (Reflective / Reactive armour shielding), Advanced AMS. */
enum { MA2_DEFT_NONE = 0, MA2_DEFT_SHIELD, MA2_DEFT_CAMO, MA2_DEFT_JAM, MA2_DEFT_ABSORB, MA2_DEFT_AMS };

typedef struct Ma2PlayerView {
    uint32_t player, actor;      /* guest pointers (identity only) */
    int      kind;               /* enum Ma2ActorKind of controlled actor */
    int      nation;             /* team / FFA nation */
    int      is_bot, is_human, alive;
    Ma2Vec3  pos, vel;
    float    yaw;                /* radians, heading of controlled actor */
    float    hp, hp_max;
    int      defense_active;     /* RS-click defensive system running */
    int      defense_type;       /* MA2_DEFT_*: which defence this body carries */
    int      defense_ready;      /* carried, not running, not recharging: a click starts it */
    uint32_t clamped_to;         /* actor this BA is clamped onto, else 0 */
    uint32_t riding;             /* friendly VTOL this BA hangs from (a lift), else 0 */
    int      cargo;              /* VTOL: items on its tether (actor+0x57C; capacity +0x578), else 0 */
    uint32_t enemy_rider;        /* enemy BA clamped onto this actor, else 0 */
    /* Own-actor NeuroHack minigame (actor+0x274), guide §2.5 */
    int      qte_active;
    uint8_t  qte_required;       /* +00 bitmask */
    uint8_t  qte_pressed;        /* +20 bitmask */
    float    qte_window;         /* +04 seconds left to answer */
    float    qte_progress;       /* +18/+A8, 0..1 */
    float    qte_opp_progress;   /* the opponent's QTE bar (clamped target / enemy rider), 0..1, -1 unknown */
    int      hack_eject;         /* HackEject: a NeuroHack on this bot completed < 8 s ago (sub_C6A62(1) eject request; core stamps it) */
    /* On foot inside a vehicle's entry zone: the vehicle Y would board
     * (player +0x438, the target behind the game's "enter" prompt), else 0. */
    uint32_t enter_target;
    /* Combat state as another player could notice it (core fills):
     * attacking = actor this player is shooting at / attacking (bots: their controller's
     *             target; the game's AI: its target slot; the human: the enemy in his sights),
     * firing    = a weapon fired within the last ~1 s (audible). */
    int      kills, deaths;      /* the scoreboard (player stats 2 and 5) */
    uint32_t attacking;
    int      firing;
    /* Weapons of the controlled body (core reads the arsenal): longest / shortest fire-group range (m). */
    float    reach, reach_short;
    /* Active defensive system (RS-click): what it stops right now (MA2_DEF_* bits), 0 = none up. */
    int      defense_blocks;
    /* Alpha Strike ('Mechs with all three weapon classes at Level 3: Atlas, Prometheus, Star Adder,
     * Blood Asp): ready to charge; the charge running (1..100 %, 0 = none -- the glow everyone sees);
     * heat in percent (-1 = not a 'Mech; at 100 the 'Mech is overheated and cannot fire). */
    int      alpha_ready, alpha_charge;
    int      alpha_cap, alpha_short;  /* F42: this 'Mech's arsenal has an Alpha group; upgrade levels still missing in classes 0-2 */
    int      wpn_room;       /* bit c (0-2): weapon class c of this body is below its top level, so an upgrade of class c helps (0 = none) */
    float    wpn_lvl;        /* F43: upgrade levels held above the arsenal's lowest, classes 0-2 summed, 0..1 (-1 = no arsenal) */
    float    heat;
    /* Camo (DefenseType 5) as the game rates it (def_kind: visibility +0x24 under the threshold
     * [0x50C968] after the fade): 0 = seen normally (no camo, or revealed: it fired, was hit, runs
     * hot), 1 = cloaked (the game's AI sees it within ~50 m), 2 = strictly cloaked (the game's AI
     * ignores it). cloak_shimmer = the render alpha +0x28 (speed / heat shimmer, 0..1).
     * Bots batch 1 F5: switch RECOMP_BOT_CLOAK / ini [Bots] BotCloak (default 1). */
    int      cloak;
    float    cloak_shimmer;
    /* The slowest fire group of the controlled body that is slow to refire (barrel +0x180 FireRate
     * ms >= 900: mortar, gauss, crossbow, warhammer...): its refire time in seconds, 0 = none.
     * Peek-and-hide times the hide to it (F79). */
    float    slow_reload;
} Ma2PlayerView;
#define MA2_DEF_DIRECT  1        /* direct hits (beams, bullets, slugs) absorbed  */
#define MA2_DEF_SPLASH  2        /* explosions / splash absorbed                  */

#define MA2_HULL_DIRS 16                /* directions a parked hull is measured in */
typedef struct Ma2VehicleView {
    uint32_t actor;
    int      kind;               /* enum Ma2ActorKind */
    Ma2Vec3  pos;
    float    yaw, hp, hp_max;
    int      occupied;           /* parent is an MAPlayer */
    int      has_entry;          /* an active entry zone ("Press Y to enter") exists */
    Ma2Vec3  entry;              /* its centre (world); the prompt shows inside entry_r, height ignored */
    float    entry_r;
    int      nation;
    uint32_t claimed_by;         /* player of the bot that is walking to board it (core fills), else 0 */
    int      claim_nation;       /* that bot's nation */
    /* Parked: how far its hull reaches from pos, measured with the game's collision rays from its
     * base to a pilot's head in MA2_HULL_DIRS world directions (k * 22.5 degrees from +z toward +x),
     * each the furthest across its sector; 0 = unknown. ext_hi: the same up to a 'Mech's hips. */
    float    ext[MA2_HULL_DIRS], ext_hi[MA2_HULL_DIRS], ext_max;
    /* A human on foot is heading for it (C2 intent, ma2_bot_track.h; core fills, only with the
     * yield switch on): that human's player and nation, else 0 / -1. Teammate bots leave it. */
    uint32_t human_claim;
    int      human_claim_nation;
} Ma2VehicleView;

/* The 'Mech yield to human teammates (BOTS-AI-PLAN.md APPROVED): env RECOMP_BOT_YIELD_HUMAN=0/1
 * overrides pc_settings.ini [Bots] YieldToHuman (default 1 in bots batch 1; logs [C7] yield ...). */
int ma2_bot_yield_human_on(void);
/* Bots batch 1 switches: env RECOMP_BOT_<NAME>=0/1 overrides pc_settings.ini [Bots] <Key>, default on.
 * C3 team vehicle assignment (VehAssign), F1 swap cool-down + Elite keeps its 'Mech (SwapCool),
 * F24 commit to one vehicle (VehCommit), F12 Grinder swap (GrinderSwap), F14 on-foot pilot fetches a
 * ride (FootRide). Counters: [C7] c3 ... every 30 s. */
enum { MA2_SW_VEH_ASSIGN, MA2_SW_SWAP_COOL, MA2_SW_VEH_COMMIT, MA2_SW_GRINDER_SWAP, MA2_SW_FOOT_RIDE, MA2_SW_COUNT };
int ma2_bot_switch_on(int sw);
enum { MA2_C3_ASSIGNED, MA2_C3_REASSIGNED, MA2_C3_HUMAN_WINS, MA2_C3_SWAPS_CLAIMED, MA2_C3_SWAP_COOL_BLOCKS,
       MA2_C3_ELITE_KEEPS, MA2_C3_SWITCHES, MA2_C3_COMMIT_HOLDS, MA2_C3_GRINDER_SWAPS, MA2_C3_FOOT_GETVEH, MA2_C3_COUNT };
extern int g_c7_c3[MA2_C3_COUNT];
void ma2_c3_count(int c);   /* g_c7_c3[c]++ and the C7 table (piece "c3") */

/* C4 hybrid crowd avoidance (BOTS-AI-PLAN.md USER DECISIONS 3): env RECOMP_BOT_AVOID_HYBRID=0/1
 * overrides pc_settings.ini [Bots] AvoidHybrid (default 1 in bots batch 1; logs [C7] avoid ...).
 * Off = the old crowd steering exactly. */
int ma2_bot_avoid_hybrid_on(void);
#define MA2_TRACK_AVOID 99   /* ma2_bot_track team id of the shared 'units near some bot' tracks */

/* Bots batch 1 F16 (custom maps): a pilot stalled within 15 m of the 'Mech it is boarding, outside its
 * entry zone, walks round the hull to the zone; every dropped ride logs [BOT-NOBOARD] with its reason.
 * Env RECOMP_BOT_ENTRY_FIX=0/1 overrides pc_settings.ini [Bots] EntryFix (default 1 in batch 1;
 * logs [C7] entry drops= orbits=). */
int ma2_bot_entry_fix_on(void);

enum Ma2PickupKind { MA2_PICKUP_POWERUP = 1, MA2_PICKUP_FLAG, MA2_PICKUP_CHECKPOINT,
                     MA2_PICKUP_DATACORE, MA2_PICKUP_COLLECTION_AREA };
typedef struct Ma2PickupView { uint32_t entity; int kind; Ma2Vec3 pos; int nation;
                               int armor;          /* power-up: armour it repairs (PowerupArmor), 0 = a weapon upgrade */
                               int wclass;         /* PuClass: weapon upgrade's fire class 0-2 (entity +0x1B4/+0x1B8/+0x1BC flag), -1 unknown / not a weapon upgrade */
} Ma2PickupView;

/* ---- Game-mode objectives (core fills them: ma2_bot_objective.c) ----
 * Game type ids (sub_1661C1 name table): 1 TeamLMS, 2 Soccer, 3 TeamDestruction,
 * 4 NotIt, 5 GiantKillers, 6 Grinder, 7 ScoutMatch, 8 SkirmishMatch, 9 LastManStanding,
 * 10 Destruction, 11 TeamScoutMatch, 12 TeamSkirmishMatch, 13 TakeIt, 14 CTF,
 * 15 CheckIt, 16 BaseWar, 17 SnatchIt. Team modes: 1..3, 11..17 (sub_16616C). */
enum Ma2GameType { MA2_GT_TEAM_LMS = 1, MA2_GT_TEAM_DESTRUCTION = 3, MA2_GT_NOTIT = 4, MA2_GT_GRINDER = 6,
                   MA2_GT_LMS = 9, MA2_GT_DESTRUCTION = 10, MA2_GT_CTF = 14, MA2_GT_CHECKIT = 15,
                   MA2_GT_BASEWAR = 16, MA2_GT_SNATCHIT = 17 };
enum Ma2ObjKind  { MA2_OBJ_FLAG = 1,        /* CTF flag (MAFlagCTF), nation = owner team      */
                   MA2_OBJ_CORE,            /* Snatch-It data core (MASnatchItDataCore)       */
                   MA2_OBJ_CORE_BASE,       /* Snatch-It base of a team (MASnatchItBase)      */
                   MA2_OBJ_CHECKPOINT,      /* Check-It checkpoint (MACheckpoint), nation=owner */
                   MA2_OBJ_TARGET };        /* destructible team objective (Base War)         */
enum Ma2ObjState { MA2_OBJS_HOME = 0,       /* flag at its stand / core free on the ground    */
                   MA2_OBJS_CARRIED,        /* carrier != 0                                    */
                   MA2_OBJS_DROPPED,        /* flag lying away from home                       */
                   MA2_OBJS_IN_BASE };      /* core stored in a team base (nation = that team) */
typedef struct Ma2ObjectiveView {
    uint32_t entity;
    int      kind, nation, state;
    Ma2Vec3  pos, home;          /* current / spawn (flag stand) position */
    uint32_t carrier;            /* Ma2PlayerView.player carrying it, else 0 */
    int      carrier_nation;
    float    radius;             /* capture / claim / drop radius (m) */
    float    hp, hp_max;         /* MA2_OBJ_TARGET */
    int      flags;              /* MA2_OBJF_* */
} Ma2ObjectiveView;
#define MA2_OBJF_FOOT 1          /* checkpoint: claimed on foot at its terminal (pos/radius = terminal zone) */
#define MA2_BOT_MAX_OBJECTIVES 24

/* A route across the map between the two teams' spawn areas (lanes_build, core). side: -1 / +1 =
 * a flank on that side of the line from lane_a to lane_b, 0 = the main route. apex = its point
 * farthest from the main route (where a flank lane is most itself). */
#define MA2_LANE_PTS 32
typedef struct Ma2Lane { int side, n; Ma2Vec3 pt[MA2_LANE_PTS]; Ma2Vec3 apex; float apex_off;
                         int drop_lo, drop_hi; } Ma2Lane;   /* lowest / highest point reached by stepping off a ledge (-1: none): one way, from pt[0]'s end */
/* A choke (C5): a narrow, long stretch of a lane (wall to wall under ~46 m for 30 m+, e.g. the New
 * River City valley exit), from end a to end b. Found once a match with the lanes. */
#define MA2_MAX_CHOKES 8
typedef struct Ma2Choke { Ma2Vec3 a, b; float width; int lane; } Ma2Choke;

typedef struct Ma2World {
    double   time;               /* MEM64(0x4E2440) */
    float    dt;                 /* MEM64(0x4E2428) */
    uint32_t frame;              /* MEM32(0x4E2430) */
    int      game_type;          /* session+0x44 */
    int      team_mode;          /* sub_16616C(game_type) */
    float    radar_range;        /* RadarDist, metres */
    float    view_range;         /* F28: how far the level draws (authored camera far plane, not DrawDistance-scaled); 0 = no cap */
    int      nplayers;   Ma2PlayerView  players[MA2_WORLD_MAX_PLAYERS];
    int      nvehicles;  Ma2VehicleView vehicles[MA2_BOT_MAX_VEHICLES];
    int      npickups;   Ma2PickupView  pickups[MA2_BOT_MAX_PICKUPS];
    /* Objectives (appended; zero outside objective modes). */
    uint32_t it_player;          /* Not-It: the player who is "It" (game+0x48 == player+0x88), else 0 */
    int      nobjectives; Ma2ObjectiveView objectives[MA2_BOT_MAX_OBJECTIVES];
    /* Lanes (team modes; appended): main route + up to two flanks, found once a match. */
    int      nlanes; Ma2Lane lanes[3]; Ma2Vec3 lane_a, lane_b;
    /* Projectiles in flight (appended): position and velocity from frame to frame. */
    int      nproj; struct { uint32_t ent; Ma2Vec3 pos, vel; } proj[32];
    /* Chokes on the lanes (appended; team modes, switch RECOMP_BOT_CHOKES / [Bots] Chokes). */
    int      nchokes; Ma2Choke chokes[MA2_MAX_CHOKES];
    /* F45 (appended): the nation whose spawn area is lane_a (lane_b is the other team's), -1 = no lanes. */
    int      lane_nation_a;
    /* Batch 3 (appended): the flanks as the lane_b team walks them, planned from its spawn (pt[0] still
     * at lane_a's end); side 9 = no flank of its own that way. lanes_back = 0: use lanes for both. */
    int      lanes_back; Ma2Lane lanes_b[3];
    /* Pilot bombs on the ground (appended; batch 3, switch RECOMP_BOT_BOMB_AVOID / [Bots] BombAvoid): every
     * player's placed pack (DropPackManager avatar+0x254: +0x7C packs out, +0x84 the pack), kept after its pilot boards. It goes off
     * when a unit of another nation stays inside 15 m for 5 s: 1000 damage over 25 m, teammates included.
     * bomb_epoch counts new sightings (routes planned before one was seen are checked again). fuse = when a
     * unit that sets it off first came inside 15 m (0 = not yet): it goes off 5 s later (or sooner, by splash). */
    int      nbombs; struct { uint32_t ent, owner; Ma2Vec3 pos; int nation; double since, fuse; } bombs[16];
    unsigned bomb_epoch;
    /* Death blasts (appended; switch RECOMP_BOT_DEATH_BLAST / [Bots] DeathBlastFlee): a 'Mech whose health has just
     * reached 0 lies where it fell and blows up a few seconds later -- a huge blast that kills 'Mechs around it and
     * chains through a bunched group. Each is kept for 6 s from t0 with radius r (core blasts_update). */
    int      nblasts; struct { uint32_t actor; Ma2Vec3 pos; double t0; float r; } blasts[8];
} Ma2World;

/* Ground probe supplied by core (wraps original sub_1760E1). Returns the
 * height of the highest collidable surface below (x, y_from, z); *ok=0 if
 * nothing was hit. Brain/nav may call it at most ~200 times per frame. */
typedef float (*Ma2GroundFn)(void *ctx, float x, float y_from, float z, int *ok);

/* ---- Navigation (nav module) ---- */
typedef struct Ma2Nav Ma2Nav;
typedef struct Ma2NavGenParams {
    float min_x, min_z, max_x, max_z;   /* terrain bounds */
    float cell;                          /* 20.0 to match shipped maps */
    float max_step;                      /* max walkable height delta per cell */
    float probe_top;                     /* y to start ground probes from */
} Ma2NavGenParams;
Ma2Nav *ma2_nav_load_text(const char *text, size_t len);          /* shipped *_Mech.map format */
Ma2Nav *ma2_nav_generate(const Ma2NavGenParams *p, Ma2GroundFn g, void *ctx);
int     ma2_nav_save_text(const Ma2Nav *nav, const wchar_t *path);  /* same text format */
void    ma2_nav_free(Ma2Nav *nav);
int     ma2_nav_node_count(const Ma2Nav *nav);
int     ma2_nav_connected(const Ma2Nav *nav, Ma2Vec3 a, Ma2Vec3 b);        /* same connected component? */
int     ma2_nav_nearest(const Ma2Nav *nav, Ma2Vec3 p);              /* node index or -1 */
Ma2Vec3 ma2_nav_node_pos(const Ma2Nav *nav, int node);
/* Writes up to max waypoints (start excluded, goal included); returns count, 0 = no path. */
int     ma2_nav_path(Ma2Nav *nav, Ma2Vec3 from, Ma2Vec3 to, Ma2Vec3 *out, int max);
void    ma2_nav_penalize(Ma2Nav *nav, Ma2Vec3 a, Ma2Vec3 b, float extra_cost); /* failed edge */
/* 2.5-D line of sight (guide §3.6): 1 = visible. */
int     ma2_los(Ma2GroundFn g, void *ctx, Ma2Vec3 eye, Ma2Vec3 target, float step);

/* ---- Traversability grid (nav module): obstacles found with ray casts + steep ground ----
 * Built once per map by core (ma2_trav_build), then used by the graph generator, the
 * path smoother and the brain's local steering. Ray fn returns the top height of the
 * highest solid (non-water, non-vehicle) collider under (x, z), *ok = 0 if none. */
typedef float (*Ma2ObstacleFn)(void *ctx, float x, float y_from, float z, int *ok);
typedef struct Ma2TravSteer { float last_delta; double until; int steered; } Ma2TravSteer;
int   ma2_trav_build(float max_x, float max_z, float probe_top, Ma2GroundFn ground, void *gctx,
                     Ma2ObstacleFn obst, void *octx);
int   ma2_trav_build_begin(float max_x, float max_z, float probe_top, Ma2GroundFn ground, void *gctx,
                           Ma2ObstacleFn obst, void *octx);   /* W2: resumable build; ..._step(ms) = 1 when the grid is ready */
int   ma2_trav_build_step(double budget_ms);
int   ma2_trav_ready(void);
/* After the build: block the cell under (x, z) for an object found by a finer probe (thin poles
 * slip between the 2 m rays); breakable = a 'Mech walks through it. Then refresh once. */
void  ma2_trav_block(float x, float z, int breakable);
void  ma2_trav_refresh(void);
/* Block every cell within r of (x, z) (a solid thing placed after the build: a turret);
 * ma2_trav_commit() then refreshes and marks plans stale. */
int   ma2_trav_block_disc(float x, float z, float r);
void  ma2_trav_commit(void);
/* Something was destroyed: re-derive the cells within r of (cx, cz) from a fresh probe --
 * probe() returns the top of what stands at the cell (the ground if nothing), *brk = 1 when only
 * breakable scenery -- then refresh.
 * ma2_trav_epoch() counts such changes (plans made before one are stale). */
void  ma2_trav_reprobe(float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx);
unsigned ma2_trav_epoch(void);
/* ew-b8 (R1 P1-4): 1 if any grid commit after epoch `since` changed cells inside the world rectangle (or the log lost that
 * far back / a new map): a route that does not come near a change stays valid. */
int   ma2_trav_changed_since(unsigned since, float wx0, float wz0, float wx1, float wz1);
/* ew-b8: cells changed since the last commit / commit only then (one refresh however many pieces changed). */
int   ma2_trav_dirty_pending(void);
int   ma2_trav_commit_if_dirty(void);
/* ew-b8 B8.17: like ma2_trav_reprobe, but nocommit != 0 leaves the refresh to ma2_trav_commit_if_dirty(). */
void  ma2_trav_reprobe_ex(float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx, int nocommit);
/* ew-b8 P1-1: the same re-derivation sliced over frames. begin -> step(budget_ms) each frame until it returns 1 (then the
 * grid is dirty: ma2_trav_commit_if_dirty). *cells_done is added to with the cells probed. */
int   ma2_trav_reprobe_begin(float cx, float cz, float r, float (*probe)(void *ctx, float x, float z, float ground, int *brk), void *ctx);
int   ma2_trav_reprobe_step(double budget_ms, unsigned *cells_done);
int   ma2_trav_reprobe_busy(void);
/* ft-bots: the grid commit as a resumable job (begin: 1 = started; step(budget_ms): 1 = committed, 0 = more to do). */
int   ma2_trav_commit_begin(void);
int   ma2_trav_commit_step(double budget_ms);
int   ma2_trav_commit_busy(void);
void  ma2_trav_commit_cancel(void);
void  ma2_trav_commit_stats(unsigned *restarts, unsigned *fallbacks);
void  ma2_trav_reprobe_cancel(void);
void  ma2_trav_stats(int *w, int *h, int *nobst, int *nsteep);
int   ma2_trav_dump(const char *path);
/* Diagnostic: the grid over [x0..x1] x [z0..z1] as text to stderr ([NAV-REGION] lines): per cell
 * '#' blocked for a 'Mech, '+' blocked for light bodies only, '~' water, else the surface height
 * as a letter (a = lowest in the region, 2 m a step); a second map gives clearance in metres. */
void  ma2_trav_dump_region(float x0, float z0, float x1, float z1);
float ma2_trav_clear(float x, float z, double now);                    /* metres to the nearest blocked cell */
int   ma2_trav_segment(Ma2Vec3 a, Ma2Vec3 b, float radius, double now); /* 1 = free corridor */
void  ma2_trav_learn(Ma2Vec3 p, float radius, double now, double ttl);  /* a place a unit got stuck on */
/* Bends *dir (unit, horizontal) around blocked ground within `look` metres.
 * Returns 0 clear, 1 steered, 2 boxed in (nothing free ahead). */
int   ma2_trav_steer(Ma2TravSteer *st, Ma2Vec3 pos, Ma2Vec3 *dir, float radius, float look, double now);
/* Raise the cost of every edge within `radius` of p (hard stuck spot). */
void  ma2_nav_block_near(Ma2Nav *nav, Ma2Vec3 p, float radius, float extra);

/* Read-only view of the traversability grid for the path planner. */
typedef struct Ma2TravView {
    int w, h; float cell;
    const unsigned char *blk;   /* 1 = blocked */
    const unsigned char *dist;  /* chamfer distance to nearest blocked cell: 5 per cell step */
    const unsigned char *slp;   /* rise/run * 100 */
    const int *comp;            /* connected walkable region per cell (0 = blocked); NULL = unknown */
    const float *sf;            /* walkable surface height per cell (-1e9 = off map); NULL = unknown */
    const unsigned char *water; /* 1 = under water (sf is the bed); NULL = unknown */
    const float *th;            /* bare terrain height per cell (sf above it = something built there); NULL = unknown */
} Ma2TravView;
int ma2_trav_view(Ma2TravView *v);                                     /* 0 = no grid yet */
/* The same grid as a 'Mech sees it: scenery the game flags as broken by a 'Mech walking into it
 * (trees) is free; `dist` is measured to hard obstacles only. */
int ma2_trav_view_heavy(Ma2TravView *v);
float ma2_trav_clear_heavy(float x, float z);                           /* metres to the nearest hard obstacle */
int   ma2_trav_segment_heavy(Ma2Vec3 a, Ma2Vec3 b, float radius, double now); /* ma2_trav_segment for a 'Mech */

/* ---- Path planner (path module): grid A* + string pulling + path following ----
 * Plans on the traversability grid for a body of a given radius, around dynamic
 * circular obstacles (parked vehicles, other units). Self-contained: no node graph,
 * no penalties, no persistent state. */
#define MA2_PATH_MAX 96
/* A disc (x, z, r), or with has_star a parked hull's outline: its reach from (x, z) along
 * MA2_HULL_DIRS directions (k * 360/N degrees, 0 = +z, clockwise toward +x), straight between them. */
typedef struct Ma2PathObstacle { float x, z, r; int has_star; float star[MA2_HULL_DIRS]; } Ma2PathObstacle;
/* Soft cost circles (known enemy fire zones): cells inside cost `cost` x step x (1 - d/r) more.
 * Set before ma2_path_plan; they apply to the next plans until set again (n = 0 clears). */
typedef struct Ma2PathDanger { float x, z, r, cost; } Ma2PathDanger;
void ma2_path_set_danger(const Ma2PathDanger *d, int n);
/* 1: the next plans are for a 'Mech, which walks through breakable scenery (trees). */
void ma2_path_set_crush(int on);
/* The next plans may step down off a ledge or cliff of up to `metres` (0 = never): one way only,
 * down, where nothing rises in between. */
void ma2_path_set_drop(float metres);
void ma2_path_set_drop_pref(float cost_k, int water_ok);   /* W5: drop-link cost factor (1 = normal) + landing in marsh water allowed */
void ma2_path_set_up(float metres);      /* F38: jet-up links over ledges up to this height (0 = none) */
int  ma2_path_same_ground(Ma2Vec3 a, Ma2Vec3 b, int heavy);   /* 1.0 pass (Worker B): 1 = b is on a's walkable region (no drop / jet link), 0 = not by walking, -1 = unknown */
void ma2_path_set_reach_slot(int s);   /* ReachSet (1.0 pass round 4): the bot the next plan is for (-1 = none) */
int  ma2_path_reach_has(int slot, Ma2Vec3 from, Ma2Vec3 p, double now);   /* 1 = p is in the set the last finished search of that bot reached, 0 = not, -1 = no valid set */
/* A drop from near (x, z) did not work (an invisible wall, a fence the grid missed): no plan
 * steps off within 15 m of it until `until`. ma2_path_drops_reset() forgets them (new map). */
void ma2_path_forbid_drop(float x, float z, double until);
void ma2_path_drops_reset(void);
/* W7 PathSlice: the set_* parameters as one block (a queued plan snapshots them at submit time). */
typedef struct Ma2PathParams { int crush; float drop, dropk; int drop_water; float up; int ndanger; Ma2PathDanger danger[64]; int slope_max, pilot; } Ma2PathParams;
void ma2_path_get_params(Ma2PathParams *o);
void ma2_path_set_slope_max(int slp);   /* TankSlope: 0 = off; reset by ma2_path_set_crush */
void ma2_path_set_pilot(int on);                                   /* PilotFace: this plan is for a pilot */
void ma2_path_set_tank_face(int on);   /* TankFace: this plan is for a tank (after ma2_path_set_pilot) */
void ma2_path_face_add(float x, float z, float r, double until, int mask);   /* PilotFace / TankFace: mask 1 pilot, 2 tank plans */
void ma2_path_pilot_face_add(float x, float z, float r, double until);   /* PilotFace: a steep face a pilot stalled on */
int ma2_trav_slope(float x, float z);
void ma2_path_set_params(const Ma2PathParams *i);
typedef struct Ma2Path {
    Ma2Vec3 pt[MA2_PATH_MAX]; int n;   /* corner points, pt[n-1] = reachable end */
    int next;                           /* index of the corner being steered for */
    Ma2Vec3 goal; float radius;         /* what was asked for */
    int crush;                          /* planned through breakable scenery */
    unsigned char drop[MA2_PATH_MAX];   /* 1: pt[i] is reached by stepping down off a ledge from pt[i-1] */
    int ok;                             /* 1 = reaches the goal (or the nearest free cell to it) */
    float end_gap;                      /* metres from pt[n-1] to the requested goal */
    double planned;                     /* time of planning */
    Ma2PathObstacle dyn[96]; int ndyn;  /* the parked hulls and units planned round (near the route) */
    Ma2Vec3 from; int has_from;         /* push-x3 E2: where the plan started (the first leg of ma2_path_off) */
    unsigned epoch;                     /* push-x3 E1: grid epoch the finished plan was searched on (sliced plans) */
} Ma2Path;
/* Plan from -> to. Returns 1 if a path was found (p->ok), else 0 (p->n = 0). */
int  ma2_path_last_expanded(void);       /* push-x3: cells expanded by the last finished plan (PATHTEST cost) */
int  ma2_path_plan(Ma2Path *p, Ma2Vec3 from, Ma2Vec3 to, float radius,
                   const Ma2PathObstacle *dyn, int ndyn, double now);

/* W7 PathSlice (plan queue, ma2_bot_path.c): a plan is searched a few ms per game step instead of all at once.
 * submit: pp = the parameters to plan with (NULL = the set_* ones now); returns a ticket (> 0), 0 = queue full (plan
 * synchronously), -2 = this owner's plan is running or finished and not yet taken. A request still queued is
 * replaced in place (keeps its place in line). take: 1 = done (path + the ma2_path_plan result copied out, request
 * freed), 0 = not yet, -1 = no such request. pump: run queued plans for up to budget_ms, returns the ms used. */
int    ma2_pathq_submit(int owner, Ma2Vec3 from, Ma2Vec3 to, float radius, const Ma2PathObstacle *dyn, int ndyn, double now, const Ma2PathParams *pp);
int    ma2_pathq_take(int owner, int ticket, Ma2Path *out, int *ret);
int    ma2_pathq_state(int owner);       /* 0 none, 1 queued, 2 running, 3 done */
void   ma2_pathq_cancel(int owner);
int    ma2_pathq_pending(void);
double ma2_pathq_oldest_ms(void);   /* how long the oldest unfinished plan has waited, ms (0: none) */
double ma2_pathq_pump(double budget_ms);
void   ma2_pathq_reset(void);   /* ew-b8 B8.7: free every job (a new level) */
void   ma2_pathq_stats(unsigned *jobs, unsigned *restarts, unsigned *steps, double *job_ms, double *job_max);
/* Point to steer for this tick (look-ahead along the path); *arrived = 1 at the end. */
Ma2Vec3 ma2_path_follow(Ma2Path *p, Ma2Vec3 pos, float reach, int *arrived);
/* 1 if pos has drifted off the path far enough to replan. */
int  ma2_path_off(const Ma2Path *p, Ma2Vec3 pos, float tol);

/* ---- Orders (brain -> BotController, ma2_bot_ctrl.c) ----
 * The brain is strategy only: every step it states what the bot should be doing.
 * The controller executes it through the game's own interfaces (campaign-AI
 * MoveTo / attack mode, command frames to the player's control behaviour). */
enum Ma2OrderType {
    MA2_ORD_IDLE = 0,
    MA2_ORD_GET_VEHICLE,     /* walk to `vehicle`'s entry zone and board it (command 39)               */
    MA2_ORD_TRAVEL,          /* go to `point` (A* waypoints + MoveTo), stop within `stop`; with `fire`,
                                shoot `target` natively whenever the shot is clear (on the move)      */
    MA2_ORD_EJECT,           /* leave the current vehicle (command 39)                                   */
    MA2_ORD_ENGAGE,          /* duel `target` with the body's campaign-AI attack state (it aims, strafes,
                                fires); `point` = where to close in from while it is out of reach      */
    MA2_ORD_FIGHT_MOVE,      /* go to `point` (cover, firing or retreat position) while firing natively at
                                `target`; at the point hold and keep firing                             */
    MA2_ORD_HACK             /* BattleArmor: close on the `target` 'Mech (jet hops) and clamp onto it    */
};
#define MA2_ORDER_MAX_DANGER 6
typedef struct Ma2Order {
    int      type;
    uint32_t vehicle;        /* GET_VEHICLE: parked vehicle actor                                       */
    Ma2Vec3  point;          /* TRAVEL / FIGHT_MOVE destination, ENGAGE approach point                  */
    float    stop;           /* arrival radius (m)                                                      */
    uint32_t target;         /* enemy actor to fight / shoot at / hack                                  */
    int      fire;           /* TRAVEL / FIGHT_MOVE: fire at `target` when the shot is clear              */
    int      engage;         /* objectives: fight what is met on the way (brain turns it into fire)      */
    int      hold_fire;      /* objective duty: never stop to fight                                      */
    int      jump;           /* jumpjet hop toward the next waypoint this step (terrain / dodge)         */
    int      dodge;          /* jumpjet sidestep (+1 right, -1 left of the line to `target`), 0 = none   */
    int      dodge_now;      /* the dodge answers a projectile on its way in (do it now, along dodge_dx/dz) */
    float    dodge_dx, dodge_dz; /* world direction out of the projectile's line (unit, xz)                 */
    int      vt_low;         /* VTOL: fly low (nap of the earth) on this leg                              */
    int      vt_sk, vt_thr;  /* VtolThreatAlt counters: this VTOL pilot's skill, and whether it is under threat now (ctrl measures the height over the surface) */
    float    vt_cover;       /* VTOL in a fight: chance per firing pass to duck behind cover afterwards   */
    int      defend;         /* tap defensive system (command 52) this step                              */
    int      bomb_here;      /* pilot on foot guarding its own objective: plant a bomb at `bomb_pt` (ctrl, friendly-fire checked) */
    Ma2Vec3  bomb_pt;
    uint8_t  qte_press;      /* NeuroHack answer bits to press this step (command 49 + bit)             */
    int      ndanger;        /* soft path costs around known enemy guns (on foot, retreating)           */
    Ma2PathDanger danger[MA2_ORDER_MAX_DANGER];
    /* ENGAGE: fight from a distance (tanks always, long-gunned or cautious 'Mechs at range):
     * keep `band_min`..`band_max` from the target, fire natively, strafe ('Mechs) or hold (tanks)
     * instead of handing the body to the campaign AI's close-range attack state. */
    int      standoff;
    float    band_min, band_max;
    int      cargo;          /* VTOL tether: 1 = hover over `point` and hook what lies there (A), 2 = drop it over `point` (B),
                              * 3 = come down and hover low by `point` (a BattleArmor climbs on / off)                           */
    int      detach;         /* BattleArmor riding a VTOL: let go now                                                             */
    uint32_t structure;      /* objective building to shoot at (Destruction modes), with its position `structure_pos`             */
    Ma2Vec3  structure_pos;
    int      alpha;          /* 'Mech: hold the Alpha Strike charge at `target` (0 again = let go: it fires when charged) */
    int      vt_heal;        /* VTOL repair run to `point`: 1 = its base helipad (set down, A), 2 = a health pack (skim through it) */
    int      drop_mode;      /* stepping off ledges: 0 = a trip, any drop the body takes; 1 = a tactical move, only a drop it can jet back up */
    int      drop_pref;      /* W5 CarrierDrops: 1 = a flag carrier heading home / a chaser of the enemy carrier: any drop, cheaper link, marsh landing ok (drop_mode is 0) */
    uint32_t want_vehicle;   /* the free vehicle this bot is going for (getting a ride or a swap): core's C3 team assignment */
    int      seen;           /* the brain perceives `target` (sight, a cloak spot, or the engage grace): the controller fires
                              * at a cloaked target only with this set (F5) */
    int      blind;          /* F83: `target` cloaked and only remembered (the grace): aim at `blind_pt`, where the brain
                              * thinks it is, not at its body */
    Ma2Vec3  blind_pt;
    int      climb;          /* (a) BattleArmor: climb the building face toward `climb_pt` (a roof top), claw latch + stick up */
    Ma2Vec3  climb_pt;
    uint32_t ff_skip, ff_rider; /* surprise set C peel-off: the ridden teammate F4 skips when firing at its rider `ff_rider` (direct-fire groups only) */
    int      exact;          /* F79b: TRAVEL / FIGHT_MOVE: the last few metres straight at `point` (a hide corner by a wall), not to
                              * the planner's nearest free cell; `stop` is then the real arrival radius                         */
    int      sp_straight;    /* surprise (rookie mistake C): fight moves go straight at the target, no strafing (ctrl_fight) */
    int      sp_far;         /* surprise (rookie mistake D): the guns fire at targets up to 1.4x their range (ctrl_fire) */
    int      beeline;        /* FlagBeeline: VTOL, a flag run leg -- the straight line first, the lane route only as the fallback */
    int      touch;          /* FlagTouch: a flag to pick up / return -- never "arrived": walk onto `point` and on over it until it is ours */
    float    exact_r;        /* `exact` reach (m): 0 = the hide corner's 8 m; a flag grab sets 15-25 m by skill (TRAVEL too) */
    float    keep_r;         /* BlastBrake: > 0 = a 'Mech about to blow up at `keep_pt`; ctrl stops momentum (a jet hop, a run) carrying the body inside keep_r */
    Ma2Vec3  keep_pt;
    int      yield_wait;     /* ChokeWaitYield: standing aside at a choke mouth for oncoming teammates -- ctrl's right-of-way never waits for this body */
} Ma2Order;

/* Controller feedback for the brain (filled by core after each step). */
typedef struct Ma2CtrlStatus {
    uint32_t enter_failed;   /* vehicle the last boarding attempts failed on (once), else 0 */
    int      no_path;        /* the planner found no route to the order's goal              */
    int      stalled;        /* no progress toward the goal for a while                     */
    int      engaged;        /* the attack state has the body                               */
    uint32_t engaged_tgt;
    int      engage_wedged;  /* the attack state holds the body without hurting the target  */
    int      arrived;        /* within `stop` of the order's point                          */
    int      fired;          /* a native shot left this step                                */
    int      trapped;        /* a vehicle that has not got out of the same spot for ~25 s   */
    int      roof_failed;    /* jetted at a roof / ledge and did not end up standing on it  */
    int      climb_state;    /* (a) wall climb: 0 none, 1 under way, 2 over the top, 3 failed (no face / no latch / fell) */
    int      bomb_out;       /* our pilot bomb is planted (at bomb_at) and not gone yet */
    Ma2Vec3  bomb_at;
    Ma2Vec3  roof_pt;
    int      route_fails;    /* ObjRouteFails: plans in a row that ended short of the goal (ctrl path_fails), every step; no_path is set only on a plan step */
} Ma2CtrlStatus;

/* ---- Senses (core -> brain): what a bot can find out about the world, like a player ----
 * The brain is pure host code; every query that needs the game goes through these. */
typedef struct Ma2DmgEvent { uint32_t actor; float amount; } Ma2DmgEvent;
/* Push-s3 (Level trick): a destructible building (class MABuilding) as the game reports it. */
typedef struct Ma2BldView { uint32_t entity, cls; Ma2Vec3 pos; float hp, hp_max; int nation; /* team 1/2, or -1 unknown (merge safety: Level must not shoot its own side's building) */ } Ma2BldView;
#define MA2_SENSE_MAX_EVENTS 12
typedef struct Ma2Senses {
    void *ctx;
    /* The game AI's own collision ray (terrain, buildings, props, vehicles), ignoring `self`
     * and `target`: 1 clear, 0 blocked, -1 unavailable. Costly: the brain budgets it. */
    int   (*los)(void *ctx, uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to);
    /* Metres from (x, z) to the nearest blocked cell of the traversability grid (99 = no grid). */
    float (*clearance)(void *ctx, float x, float z);
    /* Ground height at (x, z); *ok = 0 if unknown. */
    float (*ground)(void *ctx, float x, float z, int *ok);
    /* Height of what a body would stand on at (x, z): a rooftop, ledge or rock top, else the
     * ground; *ok = 0 if unknown. Lets jump-jet units pick high spots the walk planner cannot reach. */
    float (*surface)(void *ctx, float x, float z, int *ok);
    /* push-x2: roofs (stand points, y = roof height) of known buildings whose roof is within maxd of `from`, nearest first;
     * returns the count (<= max). NULL when the core does not provide it. */
    int   (*roofs)(void *ctx, Ma2Vec3 from, float maxd, Ma2Vec3 *out, int max);
    /* Damage since the previous brain tick: received (actor = attributed source) and dealt
     * (actor = victim). Attribution follows what a player can tell: who is shooting at me. */
    /* Push-s3: the AI ray from..to ignoring self/target; 1 = the thing that blocks it is a MABuilding (filled in *out),
     * 0 = clear or blocked by something else (out->entity / cls say what), -1 unavailable. bld_get: the building's
     * live state, 0 = gone / no longer a building. */
    int (*bld_ray)(void *ctx, uint32_t self, uint32_t target, Ma2Vec3 from, Ma2Vec3 to, Ma2BldView *out);
    int (*bld_get)(void *ctx, uint32_t ent, Ma2BldView *out);
    int nin;  Ma2DmgEvent in[MA2_SENSE_MAX_EVENTS];
    int nout; Ma2DmgEvent out[MA2_SENSE_MAX_EVENTS];
} Ma2Senses;

/* ---- Brain (brain module) ---- */
typedef struct Ma2Brain Ma2Brain;
enum Ma2Skill { MA2_SKILL_RECRUIT = 0, MA2_SKILL_REGULAR, MA2_SKILL_VETERAN, MA2_SKILL_ELITE };
/* Bot Skill setting only (never a bot's own skill): each side gets a spread of skills, see mixed_skills. */
#define MA2_SKILL_MIXED 4
/* Bot Skill setting only: Dragoon (experimental, playtest 11-03 every difficulty setting at its maximum). Every bot is an Elite
 * (its own skill 3, every Elite table) with the knobs past Elite (k_dragoon in the brain, the aim / cadence / reaction
 * tables in ctrl). Damage, armour, health and weapons stay the same (fair-play rule). */
#define MA2_SKILL_DRAGOON 5
int ma2_bots_dragoon(void);   /* 1 = this match is on Dragoon */
Ma2Brain *ma2_brain_create(int bot_index, int skill, uint32_t seed);
void      ma2_brain_free(Ma2Brain *b);
/* Called once per simulation step. self = index into w->players. */
void      ma2_brain_tick(Ma2Brain *b, const Ma2World *w, int self, Ma2Nav *nav,
                         const Ma2CtrlStatus *st, const Ma2Senses *sn, Ma2Order *out);
/* 1 while the bot must keep its own body for an objective (carrying a flag or
 * core, grabbing one, standing on a checkpoint). */
int       ma2_brain_objective_lock(const Ma2Brain *b);
/* 1 if this vehicle kind may be chosen in the current game type (phase 1: 'Mech, tank;
 * BattleArmor only where everyone starts in it). */
int       ma2_brain_kind_allowed(int kind, int game_type);
int       ma2_brain_hg_place(int self);   /* [C7] high ground: 0 ground, 1 roof, 2 hill (this frame) */
/* One-line status for RECOMP_BOT_DIAG. */
void      ma2_brain_describe(const Ma2Brain *b, char *buf, size_t len);
/* Bot recorder (ma2_bot_rec.c): the brain's size, and its shared team tables (lifts, flag runs ...) to keep raw. */
size_t    ma2_brain_size(void);
int       ma2_brain_rec_tables(const void **ptr, size_t *size, const char **name, int max);

/* ---- Configuration (ui module owns persistence) ---- */
typedef struct Ma2BotConfig {
    int red, blue;       /* team modes: bots per team */
    int ffa;             /* FFA: opponents */
    int skill;           /* enum Ma2Skill, or MA2_SKILL_MIXED */
    int autobalance;     /* 1 = fill the short team */
} Ma2BotConfig;
const Ma2BotConfig *ma2_bots_config(void);
void ma2_bots_config_set(const Ma2BotConfig *c);   /* persists to pc_settings */
int  ma2_bots_newgame_update(uint32_t menu);       /* hook at sub_226587 entry; 0 = continue original */

/* ---- Content (content module) ---- */
enum Ma2Language { MA2_LANG_ENGLISH = 0, MA2_LANG_FRENCH, MA2_LANG_GERMAN,
                   MA2_LANG_ITALIAN, MA2_LANG_JAPANESE, MA2_LANG_SPANISH, MA2_LANG_COUNT };
enum Ma2BotText {
    MA2_TEXT_BOTS = 0, MA2_TEXT_RED_BOTS, MA2_TEXT_BLUE_BOTS, MA2_TEXT_BOT_SKILL,
    MA2_TEXT_OFF, MA2_TEXT_SKILL_RECRUIT, MA2_TEXT_SKILL_REGULAR,
    MA2_TEXT_SKILL_VETERAN, MA2_TEXT_SKILL_ELITE, MA2_TEXT_SKILL_MIXED,
    /* one-line descriptions of each skill, shown on the bots screen when the skill changes */
    MA2_TEXT_DESC_RECRUIT, MA2_TEXT_DESC_REGULAR, MA2_TEXT_DESC_VETERAN, MA2_TEXT_DESC_ELITE, MA2_TEXT_DESC_MIXED,
    MA2_TEXT_SKILL_DRAGOON, MA2_TEXT_DESC_DRAGOON,
    /* W-32: descriptions of the count rows, shown while the row has focus */
    MA2_TEXT_DESC_TEAM_ROWS, MA2_TEXT_DESC_FFA_ROW, MA2_TEXT_DESC_GRINDER_ROW,
    MA2_TEXT_COUNT
};
const wchar_t *ma2_bot_text(int text, int language);
/* Airspace a VTOL could not fly into (an invisible barrier the sight rays do not see), learned
 * by the controller for every bot on the map: 1 if (x, z) lies inside one. */
int            ma2_bot_air_blocked(float x, float z);
int            ma2_bot_turret_near(float x, float z, float r);
int            ma2_bot_has_jets(uint32_t actor);   /* a 'Mech / BattleArmor with jump-jet fuel (actor+0x49C > 0.05) */
float          ma2_bot_ground_y(float x, float z, int *ok);   /* terrain height under x,z (*ok = 0: unknown) */
/* VTOL helipads (where the map's VTOLs stand at the start): the one nearest `from`; 0 = none known. */
int            ma2_bot_pad_near(Ma2Vec3 from, Ma2Vec3 *out);
/* F8/F9 (bots batch 1, switch env RECOMP_BOT_VTOL_PADS / ini [Bots] VtolPads, default 1): the
 * nearest pad of me's own team (team modes: nearer its bots' starts than any other team's; FFA: no
 * one within 150 m of it), pads in skip_mask left out; logs [C7] vtolpads every 60 s. */
int            ma2_bot_vtol_pads_on(void);
int            ma2_bot_pad_pick(const Ma2World *w, const Ma2PlayerView *me, Ma2Vec3 home, unsigned skip_mask, Ma2Vec3 *out, int *idx);
void           ma2_bot_pad_home(int nation, Ma2Vec3 p);   /* a bot's first spawn, for pad ownership */
void           ma2_bot_pad_c7(double now);
void           ma2_bot_air_reset(void);
void           ma2_bot_brain_reset(void);   /* new map: drop the roster's shared memories */
void           ma2_brain_test_refuse(uint32_t actor, double now, double secs);   /* test hooks */
void           ma2_brain_test_bail(Ma2Brain *b, double secs);
void           ma2_brain_test_lift(uint32_t ba, int nation, Ma2Vec3 pos, Ma2Vec3 dest, double now, int clear);   /* RECOMP_TEST_HUMAN_FERRY */
int            ma2_bot_callsign_count(void);
const wchar_t *ma2_bot_callsign(int index);       /* wraps modulo count */

#ifdef __cplusplus
}
#endif

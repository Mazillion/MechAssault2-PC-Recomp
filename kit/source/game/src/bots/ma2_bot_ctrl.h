/* MechAssault 2 offline bots - BotController state (ma2_bot_ctrl.c).
 * Included by ma2_bots_core.c only (after ma2_bot_api.h). */
#pragma once

enum { CM_IDLE = 0, CM_MOVE, CM_STOP, CM_ATTACK, CM_BOARD };

typedef struct BotCtrl {
    uint32_t actor; int kind; int mode;
    uint32_t seen_tgt;          /* the order's target while the brain perceives it (F5: a cloaked target is fired at only then) */
    int sp_far;                 /* surprise (rookie D): fire up to 1.4x weapon range */
    int blind; Ma2Vec3 blind_pt; /* F83: seen_tgt is only remembered: shoot where the brain thinks it is */
    /* locomotion (A* waypoints -> campaign-AI MoveTo) */
    Ma2Path path; int have_path, path_fails; Ma2Vec3 path_goal; double path_t, next_plan; unsigned path_epoch;
    Ma2Vec3 dest; int arrived;
    Ma2Vec3 prog_pos; double prog_t; int stalls; Ma2Vec3 stall_pos;
    Ma2Vec3 stuck_pt[8]; double stuck_t[8]; int nstuck;      /* this match's stuck spots: planned around */
    Ma2Vec3 crumb[8]; int ncrumb, crumb_i; double crumb_t;    /* recent places we got to: a way back out */
    Ma2Vec3 area[12]; int narea, area_i; double area_t;         /* where we were each second while travelling */
    double bdiag_t, cargo_next, obj_nolos_t;
    Ma2Vec3 obj_side_pt, obj_side_from; double obj_side_t; int obj_side;   /* objective building, no clear shot: firing spot round it */
    int vt_phase; double vt_until; Ma2Vec3 vt_spot; float vt_side;
    float vt_alt, vt_boost; double vt_alt_t, vt_prog_t; Ma2Vec3 vt_prog_pos; double vt_moveto_until;      /* VTOL cruise height over what lies ahead */   /* VTOL fight: 1 = firing hover, 2 = sliding to a new spot */
    Ma2Vec3 back_pt; double back_until; unsigned backtracks; int nostart;   /* nostart (push-x3 E11): consecutive plans that found no start cell */
    Ma2Vec3 esc_pt; double esc_until; unsigned escapes;
    uint32_t xu_hull, xu_actor; int xu_try; double xu_until; Ma2Vec3 xu_pt; double tc_log; Ma2Vec3 pf_anchor; double pf_t; Ma2Vec3 ue_pos; double ue_t, ue_last; uint32_t ue_actor; int ue_n, ue_frames, ue_vy;   /* ExitUnwedge: the hull this pilot just left, escape tries */
    /* Pilot bomb (on foot, fire = command 21 -> DropPackManager avatar+0x254): next try, hold frames left, run-off */
    double bomb_next, bomb_flee_until, bomb_t, bomb_press_t, bomb_det_next; Ma2Vec3 bomb_pos, bomb_flee_pt; uint32_t bomb_pack; int bomb_hold, bomb_why; unsigned bombs_placed; double bomb_life;          /* a pilot backing off a hull it is pressed against */
    /* Bomb avoid (known pilot bombs on the ground): inside 16 m since, leaving it (point, until), bomb sightings planned for */
    double bav_in_since, bav_until; Ma2Vec3 bav_pt; unsigned bav_epoch; uint32_t bav_ent; int bav_blast; double bav_nofire_until;   /* bav_blast: the escape is from a death blast; no shots until bav_nofire_until (BlastHoldFire) */
    double upjet_until; unsigned upjets; Ma2Vec3 upjet_goal;    /* jetting up onto a roof / ledge: steer at it */
    double hotup_t, hotup_seen;   /* W-33 HotUpJet: start of this hot up-jet wish, and the last time it was asked */
    Ma2Vec3 upfail_pt; double upfail_t;                          /* where an up-jump last failed: not again for a while */
    /* boarding (command 39 inside the vehicle's entry zone) */
    uint32_t board_veh; double board_tap_t, zone_since, near_since; int board_taps;
    uint32_t wr_veh; int wr_n; Ma2Vec3 wr_pt[3];   /* WedgeRetry: the vehicle we wedged on, how often, the spots that failed */
    uint32_t rsl_veh; int rsl_n, rsl_side; double rsl_t; Ma2Vec3 rsl_pt, rsl_pos;   /* RimSlide: vehicle, steps taken, way round, this step's start / target / start spot */
    Ma2Vec3 orb_pos; double orb_t; int orb_side, orb_n;                  /* F16: stalled by the hull outside its zone: walk round it */
    /* fighting: the AI attack state (duel) or native aim + fire (on the move) */
    uint32_t tgt; double eng_since; int engaged;
    Ma2Vec3 eng_pos; double eng_wt; float eng_thp; unsigned eng_shots;
    Ma2Vec3 box_pt; double box_until, box_next;     /* boxed in at a fight spot: the way out found */
    double box_last, box_since;                     /* BoxWedge: last frame stood boxed in, since when */
    double vt_trace_t, blind_t;
    int vt_climb;                    /* VTOL climbing to its cruise height under MoveTo */
    double vt_slow_t;                /* VTOL awake drive parked since */
    double release_next;             /* BA letting go of a VTOL: next try */
    int trig_act;                    /* the player's trigger step runs each frame on this action (Alpha Strike) */
    int skill;                       /* this bot's skill (enum Ma2Skill) -- aim spread and fire delay */
    Ma2Vec3 aim_off; double aim_roll_t, aim_since; uint32_t aim_tgt, aim_rng;   /* F7/F17: skill aim error, re-rolled */
    double grp_next[3], lapse_until, lapse_roll_t;   /* F7/F17: fire cadence per group, attention lapses */
    double f74_burst_end[3], f74_next[3], f74_shot[3], f74_fav_t; int f74_fav;   /* F74: trigger bursts per group, Recruit favourite */
    int heat_vent;                   /* BurstFire: hot, the heat weapons hold until the 'Mech has cooled */
    float vt_need, vt_rise;                   /* NeedClimb: the height the way ahead calls for */
    float vt_alt_force;              /* hold this altitude (0 = off): setting down to get out; tests */
    double land_log_t, land_t; float land_y;
    double heal_tap_t, heal_log_t;   /* VTOL repair run: A taps on the pad, log */
    double exit_tap_t;               /* VTOL getting out: exit presses */
    int settle;                      /* VTOL repair run: settling onto the spot (latched) */
    double pad_land_t;               /* VTOL repair run: when it last asked to land on the pad (Y) */
    double exit_off_until, eject_t0; /* VTOL getting out: exit let go until (stuck, lifting off); when it began */
    int btn_slot; float btn_y; Ma2Vec3 btn_pos;   /* test: VTOL command sweep */
    int ft_side; uint32_t ft_pu; double ft_t;   /* test: VTOL fly-through passes over a power-up */
    int alpha_hold; double alpha_rel_t; unsigned alpha_charges;   /* Alpha Strike: trigger held / let go at */
    int vt_hides, vt_hide_tries, vt_passes;     /* VTOL fight: ducked out of sight / looked for a place to */
    double vt_stat_t, vt_agl_sum, vt_agl_n, vt_low_n, vt_alt_err;   /* VTOL altitude over the ground, for [BOT-VTOLSTAT] */
    double drop_log_t;           /* last [BOT-DROP] line */
    int cd_plan;                                              /* W5: the last plan was a carrier/chaser drop-preferring one */
    int pq_ticket, pq_stale; uint32_t pq_actor; Ma2Vec3 pq_goal, pq_from; int pq_nd;    /* W7 PathSlice: plan queued/running (ticket), for whom, and what ctrl_plan_done needs */
    double drop_t0; Ma2Vec3 drop_top; float drop_y0;         /* the drop attempt under way: since, at which edge, from what height */
    Ma2Vec3 drop_p0;                                          /* ... and where the body stood when it began (F11) */
    double up_t0; Ma2Vec3 up_p0, up_land; float up_y0;                 /* F38: the jet-up attempt under way: since, from where, what height */
    double hop_check_t; int hop_ok;                           /* travel hops: no structure along the way (rechecked each second) */
    uint32_t jet_actor; float jet_gain;                       /* how high this body's jets have climbed (learned, 0 = not yet) */
    int jl_on; float jl_y0, jl_max, jl_fuel0;                 /* the burn under way */
    double vdodge_until; float vdodge_dx, vdodge_dz; int vt_low_pref;   /* VTOL missile dodge, low flight */
    int vt_rider;                 /* VTOL: a BattleArmor hangs under us (cruise 12 m higher) */
    /* W6 push (VtolTerrainClimb / VtolFastPad): flight telemetry and the heal-run clock */
    double vtk_t; Ma2Vec3 vtk_goal, vtk_pos; float vtk_d; int vtk_direct;   /* last tick of a VTOL leg: goal, distance to it, straight-line plan */
    double hz_tr_t, vtk_diag_t; double hz_t0, hz_a_t, hz_last, hz_full_t; float hz_hp0; int hz_in, hz_hp_full;   /* heal-run episode: zone entry, first A, last tick, hp full */
    Ma2Vec3 vdodge_pos; int vdodge_logged;
    Ma2Vec3 vt_hide;                 /* VTOL fight: where it ducks out of sight */
    int vt_prev; double vt_pass_t, vt_hide_t0, vt_ready_at;   /* F81: phase last step; this firing pass / duck began; slow gun ready again */
    Ma2Vec3 ex_pos; double ex_t, ex_last, ex_off_until;   /* F79b: straight walk into a hide corner: progress, last step, given up until */
    Ma2Vec3 fs_pos; double fs_t;      /* fight strafe: where we were and since when (no headway check) */
    float vt_ceiling, vt_climb_y; double vt_climb_t, vt_ceil_t; uint32_t vt_ceil_actor;   /* the height it would not climb past */     /* in the band with no line to the target since */
    uint32_t vt_sceil_actor; float vt_sceil, vt_sc_y, vt_ceil_agl; double vt_sc_t;   /* StickCeil: the craft, the ceiling over the terrain its stick climb found, and where / since when a held climb gained nothing */
    uint32_t fl_tgt; double fl_t, fl_seen; int fl_clear;      /* shot line of sight cache */
    double last_fire_t;
    double strafe_flip; int strafe_side, strafe_hold; unsigned strafes;   /* fight movement */
    Ma2Vec3 tr_pos; double tr_t;                                         /* RECOMP_BOT_TRACE */
    uint32_t vtol_unhold, hacked_actor; double hacked_t;
    double yield_until; float min_sep; int in_contact; unsigned contacts, avoids, replans_blocked;
    int av_side; double av_side_t;                                       /* C4: side of the last swerve (+1 right) and when */
    unsigned av_trav_mask, av_trav_frame; Ma2Vec3 av_trav_pos; float av_trav_base;   /* C4: swerve map checks, kept one frame */
    double bm_t0, bm_tzone; float bm_len; int bm_kind; unsigned bm_retargets;
    double clamp_next, jump_next, dodge_t; unsigned jumps, dodges, shots, clamp_tries, clamps;
    double rs_t0, vt_probe_t, vt_mneed_t, vt_manual_t, vt_mclimb_log; float rs_y0, vt_mneed, vt_mneed_at, vt_c90_seen; int vt_mclimb; uint32_t vt_lift_actor; float vt_lift_demand;       /* StickClimb: holding RS since, at height */
    Ma2Vec3 bau_pos; double bau_t;   /* BaUnstick: where a walking BattleArmor last made headway, when */
    Ma2Vec3 mcu_pos; double mcu_t;   /* MechClimbJet: where a jet 'Mech walking uphill last made headway, when */
    Ma2Vec3 mcu_np; float mcu_d;     /* ClimbProgress: the route point it was closing on, its distance then */
    double bal_t; float bal_y; int bal_n;          /* BaLand: since when a landed BattleArmor sits in the land state (actor+0x8B0), releases */
    double ft_over;                  /* FlagTouch: last went over the flag point */
    double up_fire;                  /* JetTryFuel: when this jet-up try first fired its jets (0: not yet) */
    double actor_since; uint32_t last_hit_cls;
    /* command frames: value per command id, remaining press steps */
    float val[61]; uint8_t press[61];
    float axis[61];                  /* analog value for one frame (stick), set each step; overrides press */
    int cl_phase; double cl_t, cl_tap_t, cl_log_t; Ma2Vec3 cl_wall, cl_dir; float cl_top;   /* wall climb */
    int co_phase, co_latched, co_done, co_res; double co_t, co_tap_t; Ma2Vec3 co_wall, co_dir; float co_top, co_ymax;   /* (a) wall climb on order */
    /* statistics */
    int stat_init; double first_t; Ma2Vec3 first_pos; float max_dist63, max_dist;
    double board_t, eng_time; unsigned los_clear, los_block, frames, engagements, ff_holds;
    uint32_t ff_skip, ff_rider;   /* surprise set C peel-off (from the order) */
    unsigned taps[61];
    float dmg, dmg_human;
    unsigned n_stalls, n_trapped, n_dropfail;   /* F50: this match's stuck count for [BOT-SUM] (stalls, trapped spells, drops given up) */
    /* VtolLaneRoute / VtolRouteVary: this flight's roll (vr_roll): its goal, seed, K, cost-field amplitude, corner slack, cruise band, lane factors */
    Ma2Vec3 vr_goal; uint32_t vr_actor, vr_seed; float vr_k, vr_amp, vr_slack, vr_band, vr_lane_f[3]; int vr_lane, vr_n, vr_logged; double vr_t, vr_log_t;
    float ve_min, ve_ymax; Ma2Vec3 ve_at; int ve_n, ve_type; double ve_t;   /* VtolEdgeKeep: this flight's closest approach to the type-map border, where, the type under it, peak height */
    double bah_t0;                   /* BaHackFlee: an enemy BattleArmor first came inside 18 m (0 = none near) */
    int hh_on; uint32_t hh_tgt; double hh_t0, hh_next; float hh_d0, hh_dmin, hh_dy;   /* BaHackHop: the aimed hop at a 'Mech under way: since, at whom, from / closest / height gap there */
    Ma2Vec3 dip_pos, dip_pt, dip_from; double dip_t, dip_t0, dip_until, dip_chk_t; int dip_n, dip_spell;   /* BaDipHop: no headway since (where), the hop out (toward, from, until), its check */
    double ve2_t;                    /* VtolEdgeAll: last [BOT-VEDGE2] trace line */
    int drop_air;                    /* BaDipHop: the drop attempt was seen in the air (begun mid-hop): it starts again on landing */
    char va_branch; double va_crawl_t, va_crawl_log;   /* which branch flew the VTOL this step (S stick, M MoveTo, W MoveTo window, A awake drive); crawl diag */
    float va_r0; Ma2Vec3 va_bgoal;   /* VtolArrive: where the braking began, for which goal */
    int va_brake, va_done, va_set, va_os, va_climb, va_far; Ma2Vec3 va_goal; double va_t60, va_tr_t; float va_rmin;   /* VtolArrive: braking for this goal; its first 60 m / rider-down clock, closest, overshoot; RiderClimbUp */
    char va_bprev; float va_sp_s, va_ragl, va_rr; int va_rhum, vs_flags, vs_fprev, vs_sfl, vs_sbr, vs_sord, vs_scargo, vs_srider; float vs_dt, vs_sd, vs_sstop; double vs_t0, vs_last, vs_fast_t;   /* VTOL: last step's branch and what acted on it, the speed smoothed (crawl diag); the open slow spell */
    int vto_on, vto_done, vto_clear; double vto_t0, vto_tr_t, vt_slow_seen; float vto_br[6], vto_t20, vto_hold; Ma2Vec3 vto_p0;   /* VTOL takeoff after the claw: a rider aboard since, 20 m/s reached, rider 12 m clear, time per branch; VtolLiftOff: the parked clock last run */
    uint32_t yw_actor, yw_skip; double yw_t0, yw_last, yw_skip_until;   /* YieldTimeout: whom it has waited for since when, whom it stopped waiting for until */
    Ma2Vec3 fq_pos; double fq_t;     /* FastStall: where and since when a ground body has not moved */
    double bd_t;                     /* BoardDeep: since when the pilot walks on to the zone centre (-1 = done for this entry)  */
    double pk_t, pk_until;           /* VtolPadKick: since when the pad run hangs, the MoveTo window it earned */
    Ma2Vec3 rz_pos, rz_pos0; double rz_t, rz_off_until; int rz_n;   /* RaisedGuard: where and since when the straight walk on a roof / ledge made no headway, the planner's turn, spells this spell */
    Ma2Vec3 kk_pos, kk_pos0; double kk_t, kk_last, kk_chk; int kk_n;   /* AiKick: where and since when a walking 'Mech / BattleArmor has not moved, the last kick, its check, kicks this spell */
    Ma2Vec3 lw_pos, lw_pt, lw_p0; double lw_t, lw_until, lw_moved; float lw_y0; int lw_chain;   /* LedgeWalk: where / since when still, the low point walked to directly, from where, until, start height */
    Ma2Vec3 tp_pos, tp_pt; double tp_t, tp_until;                    /* TankPivot: where / since when a stopped tank sat, the carrot it turns to, until */
    double ed_t; int ed_on;                                             /* EscapeDirect: since when a blast escape heads inward, direct walk latched */
    uint32_t ejs_actor;                                                 /* EjectStep: the pilot body already stepped out of its hull */
    Ma2Vec3 as_pt, as_goal; double as_until; int as_n;   /* ArriveSpread: the stand-off spot a late arrival walked to, for which goal, until when, tries */
    Ma2Vec3 fz_pos, fz_pos0; double fz_t, fz_wig_t, fz_wig_until, fz_dir_until; int fz_n, fz_k0;   /* FrozenPilot: where and since when a pilot has not moved at all, the wiggle under way (since, until, first heading), spells this stand */
    Ma2Vec3 dt_top, dt_land; double dt_t; int dt_n;                     /* DropTrap: the last drop made (its top, where the body came down, when), stalls near it since */
    Ma2Vec3 ls_top; double ls_t; float ls_best;                         /* LeapStall: the leap top being closed on, since when no 1 m closer, closest */
    Ma2Vec3 gw_hp[12], gw_spos; double gw_ht[12], gw_st; int gw_hi;   /* GotoWindow: ground positions every 0.25 s (ring), where / since when standing */
    double cruise_next;                                                 /* CruisePace: the next cruise hop along a long leg (7 s pacing; other jets 3 s) */
} BotCtrl;

static void ctrl_reset(BotCtrl *c);

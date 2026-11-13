/* MechAssault 2 offline bots - core glue (guide section 3.1 "src/ma2_bots.h").
 *
 * Every function here is called from generated code (hooks applied by the
 * patch_*.py scripts, patch_native.py for the native-bots sites)
 * on the guest thread. All of them preserve every guest register.
 *
 * Zero footprint: with 0 bots configured every predicate returns 0 and every
 * hook returns without touching guest memory.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MA2_BOT_PORT_BASE
#define MA2_BOT_PORT_BASE 4            /* bot controller ids 4..15 */
#endif

/* Session setter / getter field ids (session arrays, guide 2.2). */
enum Ma2BotSessionField {
    MA2_BOT_FIELD_VEHICLE = 0,         /* session+0x388  sub_1D49F0 / sub_1D47C0 */
    MA2_BOT_FIELD_OPTION  = 1,         /* session+0x398  sub_1D4CA0 / sub_1D4760 */
    MA2_BOT_FIELD_TEAM    = 2,         /* session+0x3A8  sub_1D4DA0 / sub_1D4790 */
    MA2_BOT_FIELD_READY   = 3          /* session+0x3B8  sub_1D4B60 */
};

/* ---- guide 3.1 interface ---- */
int  ma2_bot_is_port(uint32_t port);
int  ma2_bot_is_local_ai(uint32_t player);          /* a BotPlayer this machine simulates (registry) */
int  ma2_bot_is_player(uint32_t player);            /* = ma2_bot_is_local_ai */
int  ma2_player_is_local(uint32_t player);          /* human || bot */
int  ma2_bots_lobby_active(uint32_t session);       /* session+0x4C==2 && bots */
int  ma2_bot_count(void);                           /* for the current session MEM32(0x4EBBFC) */
void ma2_bot_lobby_record(int i, uint32_t session, uint32_t rec);
int  ma2_bot_session_set(int field, uint32_t ctrl, uint32_t value); /* 1 = handled (ctrl >= 4) */
void ma2_bots_level_begin(uint32_t session);        /* sub_1DB150 entry: roster */
void ma2_bots_level_spawn(uint32_t session);        /* loc_001DB1E8: create the BotPlayers */
void ma2_bot_player_tick(uint32_t player);          /* loc_000C8130: per-step BotController tick */
void ma2_bot_view_drawn(uint32_t view);             /* loc_0008C091: diagnostic view counter */
void ma2_bot_player_removed(uint32_t player);
/* ma2_bots_newgame_update(menu) is implemented by the UI module (ma2_bot_api.h). */

/* ---- additions used by patch_bots.py (see HANDOFF.md) ---- */
int      ma2_bot_count_for(uint32_t session);       /* bot count for an explicit session */
uint32_t ma2_bot_session_get(int field, uint32_t ctrl); /* getters sub_1D4760/1D4790/1D47C0, ctrl >= 4 */
/* player+0x244 with bit 2 forced on for local AI players: the authority gates
 * (spec section 3 "needed") read the flags through this instead of MEM32(p+0x244). */
uint32_t ma2_bot_local_flags(uint32_t player);


/* Map identification (ma2_bot_mapid.c): the loading multiplayer map, detected
 * from its level cache file. NULL when the level is not a multiplayer map. */
const char *ma2_bot_map_archive(void);
const char *ma2_bot_map_nav(void);

#ifdef __cplusplus
}
#endif

/* Split screen and the bot rows: how many bots a match has room for next to the humans.
 * Pure functions, used by ma2_bots_ui.c (the New Game rows), ma2_bots_core.c (the roster the
 * match spawns) and the unit test tests/game/split_slots_test.c.
 *
 * Owner rules (1.2.0):
 *   - the bot-count limit = total slots minus humans (12 players, 6 a side);
 *   - a guest who joins when the slots are full takes a bot's place: in free-for-all any bot, in
 *     a team mode a bot of the team the guest is on (the game's own team choice; the guest may
 *     switch sides in the Briefing Room and the balance follows);
 *   - the saved bot counts never change because of a guest: the bot comes back when the guest
 *     leaves. Only a count the player picks is saved.
 *   - Grinder stays two-player co-op as the game has it: at most one bot, guest or not. */
#ifndef MA2_SPLIT_SLOTS_H
#define MA2_SPLIT_SLOTS_H

static inline int split_max0(int v) { return v < 0 ? 0 : v; }

/* Free-for-all: bots that fit next to `humans` in `total_cap` players. */
static inline int split_ffa_room(int total_cap, int humans) { return split_max0(total_cap - humans); }

/* Team mode: bots that fit on a side of `side_cap` players that already holds `humans_side`. */
static inline int split_side_room(int side_cap, int humans_side) { return split_max0(side_cap - humans_side); }

/* The count the match uses (and the row shows): the saved count, cut to the room there is. */
static inline int split_bots_shown(int saved, int room)
{
    room = split_max0(room);
    return saved < 0 ? 0 : saved < room ? saved : room;
}

/* The player moved a row from `shown_before` to `picked`: the count to save. No move = the saved
 * count stays (a guest's arrival or a side switch is not a pick). A pick past the room is cut to
 * the room, but a higher saved count is kept (it is still wanted once the room comes back). */
static inline int split_bots_pick(int saved, int shown_before, int picked, int room)
{
    if (picked < 0 || picked == shown_before) return saved;
    room = split_max0(room);
    if (picked > room) return saved > room ? saved : room;
    return picked;
}

/* The roster for one match. team_mode: per side; else free-for-all (one pool, in *red).
 * grinder: the game's two-player co-op (one bot at most). Returns the number of bots. */
static inline int split_roster(int team_mode, int grinder, int total_cap, int cfg_red, int cfg_blue, int cfg_ffa,
                               int humans_red, int humans_blue, int *red, int *blue)
{
    int humans = humans_red + humans_blue, room = split_ffa_room(total_cap, humans);
    *red = *blue = 0;
    if (grinder && room > 1) room = 1;
    if (room <= 0) return 0;
    if (!team_mode) {
        *red = split_bots_shown(cfg_ffa, room);
    } else {
        int cap = total_cap / 2;
        *red = split_bots_shown(cfg_red, split_side_room(cap, humans_red));
        *blue = split_bots_shown(cfg_blue, split_side_room(cap, humans_blue));
        if (*red + *blue > room) *blue = split_max0(room - *red);
    }
    return *red + *blue;
}

#endif

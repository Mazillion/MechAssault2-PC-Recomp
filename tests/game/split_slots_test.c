/* 1.2.0 unit test: split screen.
 *   - kit/source/game/src/bots/ma2_split_slots.h: bot room next to the humans, rows shown/saved, the roster;
 *   - kit/source/runtime/src/input/split_route.h: which pad joins, which pad drives which guest port.
 * Build from the repo root: cl /nologo /W3 /WX tests\game\split_slots_test.c /Fe:split_slots_test.exe
 * (tests/installer/test_split_120.py builds and runs it when the MSVC tools are found). */
#include <stdio.h>
#include "../../kit/source/game/src/bots/ma2_split_slots.h"
#include "../../kit/source/runtime/src/input/split_route.h"

static int checks, failures;
static void check(int ok, const char *what) { checks++; if (!ok) { failures++; printf("FAIL %s\n", what); } }

static void roster(int team, int grinder, int red_cfg, int blue_cfg, int ffa_cfg, int hr, int hb, int want_r, int want_b, const char *what)
{
    int r = -1, b = -1, n = split_roster(team, grinder, 12, red_cfg, blue_cfg, ffa_cfg, hr, hb, &r, &b);
    char line[160];
    snprintf(line, sizeof line, "%s: got %d/%d want %d/%d", what, r, b, want_r, want_b);
    check(r == want_r && b == want_b && n == r + b, line);
}

int main(void)
{
    /* ---- the room ---- */
    check(split_ffa_room(12, 1) == 11, "ffa room solo = 11");
    check(split_ffa_room(12, 2) == 10, "ffa room with a guest = 10 (total slots minus humans)");
    check(split_side_room(6, 2) == 4, "side room, two humans = 4");
    check(split_side_room(6, 7) == 0, "side room never negative");

    /* ---- free-for-all: full slots, a guest joins, a bot makes room, comes back ---- */
    roster(0, 0, 0, 0, 11, 1, 0, 11, 0, "ffa 11 bots solo");
    roster(0, 0, 0, 0, 11, 2, 0, 10, 0, "ffa 11 bots + guest: one bot out");
    roster(0, 0, 0, 0, 11, 1, 0, 11, 0, "ffa guest left: the bot is back");
    roster(0, 0, 0, 0, 5, 2, 0, 5, 0, "ffa not full: nobody removed");

    /* ---- team: 6 a side, the bot leaves the guest's team, follows a side switch ---- */
    /* saved Red 5 + Blue 6, player 1 on Red (the short side): 6 v 6 */
    roster(1, 0, 5, 6, 0, 1, 0, 5, 6, "team 5/6 + P1 red = 6v6");
    roster(1, 0, 5, 6, 0, 2, 0, 4, 6, "guest joins red: a red bot makes room, 6v6");
    roster(1, 0, 5, 6, 0, 1, 1, 5, 5, "guest switches to blue: red bot back, a blue bot out, 6v6");
    roster(1, 0, 5, 6, 0, 1, 0, 5, 6, "guest leaves: every bot back");
    roster(1, 0, 3, 4, 0, 2, 0, 3, 4, "team not full (3/4): nobody removed");
    roster(1, 0, 6, 6, 0, 1, 1, 5, 5, "6/6 saved, one human each side: 6v6");

    /* ---- Grinder: two-player co-op as the game has it, one bot at most ---- */
    roster(0, 1, 0, 0, 5, 1, 0, 1, 0, "grinder solo: one bot");
    roster(0, 1, 0, 0, 5, 2, 0, 1, 0, "grinder with a guest: unchanged (game's co-op)");
    roster(0, 1, 0, 0, 0, 1, 0, 0, 0, "grinder Off");

    /* ---- the rows: shown = saved cut to the room; a cut is never saved ---- */
    check(split_bots_shown(11, 10) == 10, "row shows 10 with a guest");
    check(split_bots_shown(11, 11) == 11, "row shows 11 again when the guest left");
    check(split_bots_pick(11, 11, 11, 10) == 11, "a guest's arrival is not a pick: 11 stays saved");
    check(split_bots_pick(11, 10, 10, 10) == 11, "no move: saved stays");
    check(split_bots_pick(11, 10, 9, 10) == 9, "the player picks 9: saved 9");
    check(split_bots_pick(9, 9, 10, 10) == 10, "the player picks 10: saved 10");
    check(split_bots_pick(11, 10, 11, 10) == 11, "a pick past the room keeps the higher saved count");
    check(split_bots_pick(8, 10, 11, 10) == 10, "a pick past the room from below saves the room");
    check(split_bots_pick(4, 4, -1, 10) == 4, "no value: saved stays");

    /* ---- 3 and 4 players: the room ---- */
    roster(0, 0, 0, 0, 11, 3, 0, 9, 0, "ffa 11 bots + 2 guests: 9");
    roster(0, 0, 0, 0, 11, 4, 0, 8, 0, "ffa 11 bots + 3 guests: 8");
    roster(1, 0, 5, 6, 0, 2, 2, 4, 4, "team 5/6, two humans a side: 6v6");
    roster(1, 0, 5, 6, 0, 1, 3, 5, 3, "team 5/6, P1 red + 3 guests blue: 6v6");
    roster(1, 0, 6, 6, 0, 3, 1, 3, 5, "team 6/6, 3 red humans + 1 blue: 6v6");

    /* ---- input: which press joins ---- */
    {
        const int none_in[SPLIT_PORTS] = { SPLIT_NO_SLOT, SPLIT_NO_SLOT, SPLIT_NO_SLOT, SPLIT_NO_SLOT };
        const int one_in[SPLIT_PORTS] = { SPLIT_NO_SLOT, 1, SPLIT_NO_SLOT, SPLIT_NO_SLOT };
        const int full[SPLIT_PORTS] = { SPLIT_NO_SLOT, 1, 2, 3 };
        unsigned short none[4] = { 0, 0, 0, 0 }, start1[4] = { 0, SPLIT_PAD_START, 0, 0 }, start0[4] = { SPLIT_PAD_START, 0, 0, 0 };
        unsigned short a1[4] = { 0, SPLIT_PAD_A, 0, 0 }, start2[4] = { 0, 0, SPLIT_PAD_START, 0 };
        int p1pad = split_p1_pad(SPLIT_P1_NONE, 0, 1), p1kbm = split_p1_pad(SPLIT_P1_NONE, 0, 0);
        check(p1pad == 0 && p1kbm == SPLIT_NO_SLOT, "player 1's pad: the active pad only while player 1 uses a pad");
        check(split_p1_pad(SPLIT_P1_KBM, 0, 1) == SPLIT_NO_SLOT, "locked to the keyboard: no pad is player 1's");
        check(split_p1_pad(2, 0, 0) == 2, "locked to pad 2: pad 2 is player 1's whatever was used last");
        check(split_join_slot(0x3, none, p1pad, none_in, 0) == SPLIT_NO_SLOT, "no press, no join");
        check(split_join_slot(0x3, start1, p1pad, none_in, 0) == 1, "START on the second pad joins (P1 on pad 0)");
        check(split_join_slot(0x3, start0, p1pad, none_in, 0) == SPLIT_NO_SLOT, "START on player 1's own pad never joins");
        check(split_join_slot(0x1, start0, p1kbm, none_in, 0) == 0, "one pad, P1 on the keyboard on purpose: START on it joins");
        check(split_join_slot(0x3, a1, p1pad, none_in, 0) == SPLIT_NO_SLOT, "A does not quick-join");
        check(split_join_slot(0x3, a1, p1pad, none_in, 1) == 1, "A joins in the Add Guest dialog");
        check(split_join_slot(0x7, start1, p1pad, one_in, 0) == SPLIT_NO_SLOT, "a guest's own START is not a new join");
        check(split_join_slot(0x7, start2, p1pad, one_in, 0) == SPLIT_NO_SLOT, "1.2.0: a third pad never joins (two players)");
        check(split_join_slot(0xF, start0, SPLIT_P1_KBM == -1 ? SPLIT_NO_SLOT : 0, full, 0) == SPLIT_NO_SLOT, "4 players: no free port, no join");
        check(split_join_slot(0x1, start1, SPLIT_NO_SLOT, none_in, 0) == SPLIT_NO_SLOT, "a slot with no pad never joins");
        check(split_next_port(none_in, 0x1) == 1 && split_next_port(one_in, 0x3) == SPLIT_NO_SLOT && split_next_port(full, 0xF) == SPLIT_NO_SLOT,
              "1.2.0: the guest takes port 1, no further port");
        /* ---- input: who drives which port ---- */
        check(split_port_slot(0, none_in, SPLIT_P1_NONE, 0, SPLIT_NO_SLOT) == 0, "solo: player 1 keeps the active pad (the hop)");
        check(split_port_slot(0, none_in, SPLIT_P1_NONE, 0, 0) == SPLIT_NO_SLOT, "a joining pad's press never reaches player 1");
        check(split_port_slot(0, one_in, SPLIT_P1_KBM, 0, SPLIT_NO_SLOT) == SPLIT_NO_SLOT, "guest in, locked to keyboard: no pad for player 1");
        check(split_port_slot(0, one_in, 0, 2, SPLIT_NO_SLOT) == 0, "guest in, locked to pad 0: pad 0 even when another pad hopped");
        check(split_port_slot(1, one_in, 0, 0, SPLIT_NO_SLOT) == 1 && split_port_slot(3, full, 0, 0, SPLIT_NO_SLOT) == 3, "guest ports: their own pads");
        check(split_port_slot(2, one_in, 0, 0, SPLIT_NO_SLOT) == SPLIT_NO_SLOT, "a free guest port has no pad");
        check(split_p1_keyboard(SPLIT_P1_NONE) && split_p1_keyboard(SPLIT_P1_KBM) && !split_p1_keyboard(0),
              "the keyboard drives player 1 unless player 1 is locked to a pad");
        check(split_p1_lock_for(0, 1, 1) == 0, "first guest joins while P1 uses pad 0: P1 locked to pad 0");
        check(split_p1_lock_for(0, 0, 1) == SPLIT_P1_KBM, "first guest joins while P1 uses the keyboard: locked to keyboard");
        check(split_p1_lock_for(0, 1, 0) == SPLIT_P1_KBM, "the joining pad itself never becomes player 1's lock");
        check(!split_hop_allowed(1, one_in, SPLIT_NO_SLOT) && !split_hop_allowed(2, none_in, 2) && split_hop_allowed(2, one_in, SPLIT_NO_SLOT),
              "the hop never takes a guest's or a joining pad");
    }

    printf("split_slots_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

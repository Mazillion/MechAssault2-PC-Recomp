/* Split screen: which host pad drives which guest port, and which press joins a guest.
 * Pure functions (no Windows, no globals), used by xinput_device.c and by the unit test
 * tests/game/split_slots_test.c.
 *
 * The rules (owner scope, 1.2.0):
 *   - Player 1 is guest port 0. While nobody has joined, port 0 is the keyboard and mouse
 *     plus the "active" pad (whichever host pad last pressed a button: the hop), so a solo
 *     player keeps playing with a pad as before.
 *   - The guest is guest port 1, driven by exactly one host pad (guest_slot). While a guest
 *     is in, port 0 is the keyboard and mouse only: no pad ever reaches player 1, so a
 *     second or third pad cannot take player 1 over.
 *   - A join press: in the Multiplayer menus, START on a connected pad that is not player
 *     1's pad. Player 1's pad is the active pad only while player 1 is actually using a pad
 *     (the last input on port 0 came from a pad); when player 1 is on the keyboard and
 *     mouse, every pad is free. In the game's own Add Guest dialog A also joins, as the
 *     dialog accepts it on a console. */
#ifndef MA2_SPLIT_ROUTE_H
#define MA2_SPLIT_ROUTE_H

#define SPLIT_PAD_START 0x0010u   /* XINPUT_GAMEPAD_START (Menu) */
#define SPLIT_PAD_A     0x1000u   /* XINPUT_GAMEPAD_A */
#define SPLIT_NO_SLOT   (-1)

/* The host slot (0-3) whose new press joins a guest, or SPLIT_NO_SLOT.
 *   connected   bit i set: host slot i holds a pad
 *   pressed[i]  buttons that went down on slot i since the last poll
 *   p1_slot     player 1's pad, or SPLIT_NO_SLOT when player 1 is on the keyboard and mouse
 *   guest_slot  the guest's pad, or SPLIT_NO_SLOT (a second guest never joins: 2 local slots)
 *   dialog      the Add Guest dialog is open (A joins too) */
static inline int split_join_slot(unsigned connected, const unsigned short pressed[4], int p1_slot,
                                  int guest_slot, int dialog)
{
    unsigned want = SPLIT_PAD_START | (dialog ? SPLIT_PAD_A : 0u);
    int i;
    if (guest_slot != SPLIT_NO_SLOT) return SPLIT_NO_SLOT;
    for (i = 0; i < 4; i++) {
        if (!(connected & (1u << i)) || i == p1_slot) continue;
        if (pressed[i] & want) return i;
    }
    return SPLIT_NO_SLOT;
}

/* The host slot that drives guest port `port`, or SPLIT_NO_SLOT (port 0: keyboard and mouse only).
 *   active_slot   the hop's pad for player 1 (SPLIT_NO_SLOT when no pad is connected)
 *   joining_slot  a pad whose join press the game side has not taken yet: not player 1's either */
static inline int split_port_slot(int port, int guest_slot, int active_slot, int joining_slot)
{
    if (port == 0) return guest_slot != SPLIT_NO_SLOT || active_slot == joining_slot ? SPLIT_NO_SLOT : active_slot;
    if (port == 1) return guest_slot;
    return SPLIT_NO_SLOT;
}

/* May the hop give player 1 slot `slot`? Never the guest's pad, never a pad that is joining. */
static inline int split_hop_allowed(int slot, int guest_slot, int joining_slot)
{
    return slot != guest_slot && slot != joining_slot;
}

#endif

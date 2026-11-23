/* Split screen: which host pad drives which guest port, and which press joins a guest.
 * Pure functions (no Windows, no globals), used by xinput_device.c and by the unit test
 * tests/game/split_slots_test.c.
 *
 * The rules (owner, 1.2.0: two players, as the original game shipped split screen):
 *   - Player 1 is guest port 0; the guest is port 1 (the game's controller 2), driven by
 *     exactly one host pad (port_slot[port]).
 *   - While nobody has joined, player 1 is the keyboard and mouse AND the active pad (whichever
 *     host pad last pressed a button: the hop), so a solo player plays as before.
 *   - The moment the first guest joins, player 1 is LOCKED to ONE device: the one player 1 was
 *     using on purpose (the last key, click or pad press; mouse motion alone does not count).
 *       p1_lock = SPLIT_P1_KBM: keyboard and mouse only; every pad can be a guest.
 *       p1_lock = slot 0-3:     that pad only; the keyboard and mouse stop driving player 1.
 *     Two devices never drive player 1 while guests are in. The lock goes when the last guest
 *     leaves.
 *   - A join press: in the Multiplayer menus, START on a connected pad that is neither player 1's
 *     pad nor a guest's, while a guest port is free. In the game's own Add Guest dialog A joins too. */
#ifndef MA2_SPLIT_ROUTE_H
#define MA2_SPLIT_ROUTE_H

#define SPLIT_PAD_START 0x0010u   /* XINPUT_GAMEPAD_START (Menu) */
#define SPLIT_PAD_A     0x1000u   /* XINPUT_GAMEPAD_A */
#define SPLIT_NO_SLOT   (-1)
#define SPLIT_P1_NONE   (-2)      /* no guest: player 1 = keyboard, mouse and the active pad */
#define SPLIT_P1_KBM    (-1)      /* guests in, player 1 locked to the keyboard and mouse */
#define SPLIT_PORTS     4         /* the game's 4 controller ports (port_slot arrays) */
/* 1.2.0 ships two-player split screen (owner 2026-11-23): one guest. Pads 3 and 4 reach nobody.
 * 3-4 players were built and shelved: branch splitscreen-4p-wip, wiki pack t76_splitscreen_4p. */
#define SPLIT_MAX_GUESTS 1

/* Player 1's pad for the join rules: the locked pad, or (no guest yet) the active pad while player 1
 * uses a pad on purpose. SPLIT_NO_SLOT when player 1 is on the keyboard and mouse. */
static inline int split_p1_pad(int p1_lock, int active_slot, int p1_on_pad)
{
    if (p1_lock >= 0) return p1_lock;
    if (p1_lock == SPLIT_P1_KBM) return SPLIT_NO_SLOT;
    return p1_on_pad ? active_slot : SPLIT_NO_SLOT;
}

/* The host slot (0-3) whose new press joins a guest, or SPLIT_NO_SLOT.
 *   connected  bit i: host slot i holds a pad;   pressed[i]: buttons that went down on slot i
 *   p1_pad     split_p1_pad();                   port_slot[1..3]: the guests' pads (SPLIT_NO_SLOT = free)
 *   dialog     the Add Guest dialog is open (A joins too) */
static inline int split_join_slot(unsigned connected, const unsigned short pressed[4], int p1_pad,
                                  const int port_slot[SPLIT_PORTS], int dialog)
{
    unsigned want = SPLIT_PAD_START | (dialog ? SPLIT_PAD_A : 0u);
    int i, p, free_port = 0;
    for (p = 1; p <= SPLIT_MAX_GUESTS; p++) if (port_slot[p] == SPLIT_NO_SLOT) free_port = 1;
    if (!free_port) return SPLIT_NO_SLOT;
    for (i = 0; i < 4; i++) {
        int taken = i == p1_pad;
        for (p = 1; p < SPLIT_PORTS; p++) if (port_slot[p] == i) taken = 1;
        if (!(connected & (1u << i)) || taken) continue;
        if (pressed[i] & want) return i;
    }
    return SPLIT_NO_SLOT;
}

/* The lowest free guest port (1-3) given the game's local-player mask, or SPLIT_NO_SLOT. */
static inline int split_next_port(const int port_slot[SPLIT_PORTS], unsigned local_mask)
{
    int p;
    for (p = 1; p <= SPLIT_MAX_GUESTS; p++)
        if (port_slot[p] == SPLIT_NO_SLOT && !(local_mask & (1u << p))) return p;
    return SPLIT_NO_SLOT;
}

/* The host slot that drives guest port `port`, or SPLIT_NO_SLOT (port 0: no pad).
 *   active_slot   the hop's pad (SPLIT_NO_SLOT when none is connected)
 *   joining_slot  a pad whose quick-join press the game side has not taken yet */
static inline int split_port_slot(int port, const int port_slot[SPLIT_PORTS], int p1_lock, int active_slot, int joining_slot)
{
    if (port == 0) {
        if (p1_lock >= 0) return p1_lock;
        if (p1_lock == SPLIT_P1_KBM) return SPLIT_NO_SLOT;
        return active_slot == joining_slot ? SPLIT_NO_SLOT : active_slot;
    }
    return port > 0 && port < SPLIT_PORTS ? port_slot[port] : SPLIT_NO_SLOT;
}

/* Does the keyboard (and mouse aim) drive player 1? Not while player 1 is locked to a pad. */
static inline int split_p1_keyboard(int p1_lock) { return p1_lock < 0; }

/* May the hop give player 1 slot `slot`? Never a guest's or a joining pad. */
static inline int split_hop_allowed(int slot, const int port_slot[SPLIT_PORTS], int joining_slot)
{
    int p;
    if (slot == joining_slot) return 0;
    for (p = 1; p < SPLIT_PORTS; p++) if (port_slot[p] == slot) return 0;
    return 1;
}

/* The lock taken when the first guest joins: the pad player 1 is using on purpose, or the keyboard. */
static inline int split_p1_lock_for(int active_slot, int p1_on_pad, int joining_slot)
{
    return p1_on_pad && active_slot >= 0 && active_slot != joining_slot ? active_slot : SPLIT_P1_KBM;
}

#endif

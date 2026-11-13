"""Offline-bots rows for interface/menus/multi/multi_newgame.ggf.

Wraps bringup/build_pc_assets.py without modifying it: the base module's
patch_legends() is wrapped so that, when it sees the New Game form, the three
Bots rows are cloned from the original Time Limit barrel group (barrel id 88
and children 89..93) and appended to the root sibling chain after Kill/Scores.

  Red Bots   action 910  (label child action 911)  ids 101..106
  Blue Bots  action 920  (label child action 921)  ids 107..112
  Bot Skill  action 930  (label child action 931)  ids 113..118
  chain: 94 -> 101 -> 107 -> 113 -> 100 (description)

Only existing retail widgets are cloned: same classes, fonts, focus bar,
arrows and 29 px row pitch. No new art. Label text ids are in the new
0x7002xxxx range (resolved by ma2_pc_localized, see ma2_bots_ui.c):
  0x70020001 Bots (FFA label, set at runtime)   0x70020002 Red Bots
  0x70020003 Blue Bots                          0x70020004 Bot Skill

The initial rows sit in the Public / Private / Time Limit slots. In modes where
the hook (ma2_bots_newgame_update) does not run, or Local Xbox is not active,
the hook hides them; in Local Xbox it re-lays them under the lowest visible
original row every frame.

Usage:
  python build_pc_assets_bots.py --project <game root>        (same as base)
  python build_pc_assets_bots.py --transform in.ggf out.ggf   (no archive I/O)
  python build_pc_assets_bots.py --dump file.ggf              (print widget tree)
"""
from pathlib import Path
import copy, struct, sys

BRINGUP = Path(__file__).resolve().parent   # the installer keeps build_pc_assets.py beside this file
sys.path.insert(0, str(BRINGUP))
import build_pc_assets as base   # noqa: E402  (read-only import, never edited)

ID, PARENT, CHILD, NEXT = base.ID, base.PARENT, base.CHILD, base.NEXT
ACTION, TEXT, NAME = base.ACTION, base.TEXT, base.NAME
X, Y, W, H = base.X, base.Y, base.W, base.H
WS_X, WS_Y = 0x193444DF, 0x24546D6F          # widescreen rectangle origin
TEXT_ID_BASE = 0x70020000

TIME_LIMIT_BARREL = 88                        # barrel + children 89..93
GROUP_IDS = list(range(88, 94))
KILL_SCORES = 94
DESCRIPTION = 100
FIRST_NEW_ID = 101

# (row name, barrel action, label action, label text id, first y, arrow x pair)
ROWS = [
    ('Red Bots Barrel',  910, 911, TEXT_ID_BASE + 2, 212, None),
    ('Blue Bots Barrel', 920, 921, TEXT_ID_BASE + 3, 241, None),
    ('Bot Skill Barrel', 930, 931, TEXT_ID_BASE + 4, 270, (68, 69)),  # arrow columns of the Game Type barrel
]


def _i(o, k):
    return struct.unpack('<i', o[1][k][1])[0]


def _set_i(o, k, v):
    typ = o[1][k][0]
    o[1][k] = (typ, struct.pack('<i', v))


def is_new_game_form(objects):
    actions = {base.get(o, ACTION) for o in objects}
    return {800, 100, 200, 600, 500, 700} <= actions


LIFT = -58          # Time Limit and Kill/Scores move up two rows, into the Public/Private gap
LIFT_FROM = 270     # retail Time Limit barrel y; the lift applies only once


def lift_time_kill(objects):
    """Re-stack the retail Team rows so the Bots rows fit above the footer.

    Game Type 154, Mission 183, Time Limit 212, Kill/Scores 241, then Red 270,
    Blue 299, Skill 328 (29 px pitch). The hook stacks the bot rows under the
    lowest visible row, so this is all the layout change needed.
    """
    by_id = {base.get(o, ID): o for o in objects}
    if _i(by_id[TIME_LIMIT_BARREL], Y) != LIFT_FROM:
        return objects                                    # already lifted
    for i in range(88, 100):
        o = by_id[i]
        if o[0] == 0x83E58A2E:                             # item-list helper keeps its origin
            continue
        for k in (Y, WS_Y):
            _set_i(o, k, _i(o, k) + LIFT)
    return objects


def add_bots_rows(objects):
    """Mutates and returns objects with the three cloned barrel groups."""
    by_id = {base.get(o, ID): o for o in objects}
    if any(base.get(o, ACTION) in (910, 920, 930) for o in objects):
        return lift_time_kill(objects)                    # already applied (lift is idempotent)
    assert by_id[TIME_LIMIT_BARREL][1][ACTION][0] == 6 and base.get(by_id[88], ACTION) == 600
    assert base.get(by_id[KILL_SCORES], NEXT) == DESCRIPTION
    proto = [by_id[i] for i in GROUP_IDS]
    proto_y = _i(proto[0], Y)                              # 270
    new_id = FIRST_NEW_ID
    first_barrels = []
    for name, action, label_action, text_id, y, arrows in ROWS:
        ids = list(range(new_id, new_id + 6))
        new_id += 6
        group = [copy.deepcopy(p) for p in proto]
        dy = y - proto_y
        for o, i in zip(group, ids):
            base.put(o, ID, i)
            if o[0] == 0x83E58A2E:                         # item-list helper keeps its retail origin
                continue
            for k in (Y, WS_Y):
                _set_i(o, k, _i(o, k) + dy)
        barrel, lst, label, focus, left, right = group
        base.put(barrel, PARENT, 51)
        base.put(barrel, CHILD, ids[1])
        base.put(barrel, ACTION, action)
        base.string(barrel, NAME, name)
        for o, nxt in zip(group[1:], ids[2:] + [0]):
            base.put(o, PARENT, ids[0])
            base.put(o, CHILD, 0)
            base.put(o, NEXT, nxt)
        base.put(label, ACTION, label_action)             # lets the hook find the label
        base.put(label, TEXT, text_id)
        base.string(label, NAME, name.replace('Barrel', 'normal'))
        if arrows:                                         # Game Type barrel arrow columns (ids 68, 69)
            for o, src in ((left, by_id[arrows[0]]), (right, by_id[arrows[1]])):
                for k in (X, WS_X):
                    _set_i(o, k, _i(src, k))
        first_barrels.append(barrel)
        objects.extend(group)
    # sibling chain of the form root: ... 94 -> new barrels -> 100
    chain = [base.get(b, ID) for b in first_barrels]
    base.put(by_id[KILL_SCORES], NEXT, chain[0])
    for b, nxt in zip(first_barrels, chain[1:] + [DESCRIPTION]):
        base.put(b, NEXT, nxt)
    return lift_time_kill(objects)


def dump(objects):
    lines = []
    for o in objects:
        nm = o[1].get(NAME, (9, b''))[1].decode('ascii', 'replace')
        def v(k):
            return struct.unpack('<i', o[1][k][1])[0] if k in o[1] and len(o[1][k][1]) == 4 else None
        lines.append(f"id={base.get(o, ID):3} kind={o[0]:08X} par={base.get(o, PARENT):3} "
                     f"child={base.get(o, CHILD):3} next={base.get(o, NEXT):3} act={base.get(o, ACTION):4} "
                     f"text={base.get(o, TEXT):#x} xywh={v(X)},{v(Y)},{v(W)},{v(H)} ws={v(WS_X)},{v(WS_Y)} name={nm!r}")
    return '\n'.join(lines)


def verify(objects):
    """Re-parse check: unique ids, valid sibling/child chains, 94 -> ... -> 100."""
    by_id = {base.get(o, ID): o for o in objects}
    assert len(by_id) == len(objects), 'duplicate widget ids'
    def siblings(first):
        out, seen = [], set()
        while first:
            assert first in by_id and first not in seen, f'bad chain at {first}'
            seen.add(first); out.append(first); first = base.get(by_id[first], NEXT)
        return out
    root_chain = siblings(base.get(by_id[51], CHILD))
    k = root_chain.index(KILL_SCORES)
    assert root_chain[k + 1:] == [101, 107, 113, 100], root_chain[k:]
    for b in (101, 107, 113):
        kids = siblings(base.get(by_id[b], CHILD))
        assert len(kids) == 5 and all(base.get(by_id[c], PARENT) == b for c in kids)
        assert base.get(by_id[b], PARENT) == 51
    for o in objects:
        for key in (PARENT, CHILD, NEXT):
            ref = base.get(o, key)
            assert ref == 0 or ref in by_id, f'dangling reference {ref}'
    return True


_original_patch_legends = base.patch_legends


def _patch_legends_and_bots(objects, template):
    changed = _original_patch_legends(objects, template)
    if is_new_game_form(objects):
        add_bots_rows(objects)
        changed = True
    return changed


base.patch_legends = _patch_legends_and_bots


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--project', type=Path, default=base.PROJECT,
                   help='Game root containing game_files; writes only its pc_assets directory.')
    p.add_argument('--transform', nargs=2, metavar=('IN', 'OUT'),
                   help='Apply only the bots-row transform to an extracted .ggf; no archive I/O.')
    p.add_argument('--dump', type=Path, help='Print the widget tree of a .ggf and exit.')
    a = p.parse_args()
    if a.dump:
        print(dump(base.parse(a.dump.read_bytes())))
    elif a.transform:
        objs = add_bots_rows(base.parse(Path(a.transform[0]).read_bytes()))
        verify(objs)
        Path(a.transform[1]).write_bytes(bytes(base.encode(objs)))
        verify(base.parse(Path(a.transform[1]).read_bytes()))
        print('wrote', a.transform[1], 'objects:', len(objs))
    else:
        base.PROJECT = a.project.resolve()
        base.build()

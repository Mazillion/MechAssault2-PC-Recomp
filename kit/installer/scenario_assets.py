r"""Custom Scenarios on the hub: interface/menus/hub/hub_main_demos.ggf (pc_assets step 4).

The PC build shows the hub variant with a Game Demos button (the disc has Demo\cdx.xbe). That button
(ref_id 8) becomes Custom Scenarios:
  - activatecommand MAAutoMenuConquestWarStatus (0xB824796E, the demo slot's placeholder) ->
    MAAutoMenuCampaignPick (0xF83FF842), the Campaign button's target. The game's
    scenario/ma2_scenario.c enters Custom Scenarios mode for action 8 instead of launching
    d:\demo\cdx.xbe, so the picker finds only the generated custom campaign;
  - its two labels (FontTextID 39 "Game Demos") -> 0x00FFFF00 ("Custom Scenarios", ma2_scn_text).
Same transform as patch_scenario_assets.py.
"""
import struct, zlib

NAME = 'interface/menus/hub/hub_main_demos.ggf'
_key = lambda s: zlib.crc32(s.encode('ascii')) ^ 0xFFFFFFFF
K_CMD, K_TEXT, K_REF, K_ID, K_PARENT = (_key('activatecommand'), _key('FontTextID'), _key('ref_id'),
                                        _key('SerializeObj::ID'), _key('CompParent'))
CMD_CAMPAIGN_PICK, CMD_DEMO_SLOT = 0xF83FF842, 0xB824796E
TEXT_GAME_DEMOS, TEXT_CUSTOM = 39, 0x00FFFF00


def transform(data, ggf):
    """ggf = the build_pc_assets module (parse/encode/put). Returns the new form bytes."""
    objs = ggf.parse(data)
    u32 = lambda o, k: struct.unpack('<I', o[1][k][1])[0] if k in o[1] else None
    button = [o for o in objs if u32(o, K_REF) == 8 and u32(o, K_CMD) in (CMD_DEMO_SLOT, CMD_CAMPAIGN_PICK)]
    if len(button) != 1:
        raise ValueError('hub Game Demos button not found (%d)' % len(button))
    ids = {u32(button[0], K_ID)}
    ggf.put(button[0], K_CMD, CMD_CAMPAIGN_PICK)
    labels = [o for o in objs if u32(o, K_TEXT) in (TEXT_GAME_DEMOS, TEXT_CUSTOM) and u32(o, K_PARENT) in ids]
    if len(labels) != 2:
        raise ValueError('expected 2 Game Demos labels, found %d' % len(labels))
    for o in labels:
        ggf.put(o, K_TEXT, TEXT_CUSTOM)
    return bytes(ggf.encode(objs))

# Mission picker barrel: Custom Scenarios shift and Campaign centring (2026-11-05)

## Overview

Campaign and Custom Scenarios use the same retail mission picker (menu vtable 0x48F3E0). On Custom Scenarios the PC port draws a Scenario Editor button to the left of the level selector (the "barrel"), so the barrel is moved 50 640x480 units right to make room. Campaign has no Editor button, so its barrel must stay in the retail, centred position.

Before 2026-11-05 the shift was only ever applied, never taken back. Because the Campaign picker reuses the barrel widget, it stayed shifted right after a visit to Custom Scenarios. The fix makes the shift reversible.

## Key code

| Function | What it does | Source |
|---|---|---|
| ma2_pc_menu_pulse | Sets the offset each pulse: 50 on 0x48F3E0 in Custom Scenarios mode, else 0 | pc_native_settings.cpp:518 |
| ma2_barrel_offset / g_barrel_dx | Holds the current offset | ma2_bots_ui.c:518 |
| shift_widget | Moves one widget's +0x4C and +0x5C x to base + offset; remembers base and applied offset (g_shifted[16]) and restores when the offset drops to 0 | ma2_bots_ui.c:523 |
| shift_barrel | Shifts the barrel and its non-arrow text children; skipped when the offset is 0 and nothing is still shifted (g_shifted_live) | ma2_bots_ui.c:542 |
| ma2_scn_hub_action | Ends Custom Scenarios mode on any other hub item, so Campaign gets offset 0 | ma2_scenario.c:2226 |

## Bugs and fixes

- **Campaign level select off centre (fixed 2026-11-05, the game source tree on top of e4a03a9).** shift_widget only added the offset, and shift_barrel returned early when it was 0, so the reused widget kept +50. Each tracked widget now stores its applied offset and is moved back to its base x when the offset changes.
- **Builds.** v13au28 (A046C9FC.62E4) does not have the fix. The PC Release exe 1446948D.8800 (11-05) is v13au28 plus the fix.

## Test

Open Custom Scenarios (selector shifted right, Editor button on the left), back out to the hub, then open Campaign. The selector must be centred.

## Open questions

- Not yet confirmed in-game after the fix.


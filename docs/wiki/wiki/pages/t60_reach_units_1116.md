# "Can it get here?" reach overlay: per-unit movement facts (2026-11-16)

## Overview
Source for the numbers in the editor's reach overlay (`kit/source/game/src/core/reach_rules.h`, branch `reach`): what each unit type can climb, jump, drop off, fly or land on, each with its source. Rows are `measured` (in-game run M1-M6, notes `reach\MEASURED.md`), `code` (bot planner source v35-edgame, `reach\`), `catalog` (GlobalEntities templates) or `estimated`. Estimated values are marked "estimated" and the overlay shows "(est.)" when one decided a verdict. Status of this pack: final (2026-11-17; M5 claw and a few runs not done, marked).

The overlay's two views: **Player** = real ability; **Bots** = min(planner rule, real ability). The differences are on the bot-gaps page (`t61_reach_bot_gaps_1116`).

## Unit table
| Unit group | Climb / step | Jets | Drop (edge) | Notes | Evidence |
|---|---|---|---|---|---|
| 'Mech with jets (14 chassis) | Atlas 148% measured (all chassis est.) | apex 24.7-27.3 m (M1); overlay = apex - 1; Uziel/Summoner est. (same jet stats as Belial / Hellbringer) | >= 30 m (Atlas, M3) | Jump key is Shift | M1, M3 measured |
| 'Mech without jets (16 chassis) | Atlas 148%, blocked at 171% | none | >= 30 m (Atlas, M3) | planner walls at 55% | V4 + M3 measured |
| Battle Armor | slope no walker limit found in the final M2 run: the Atlas, Hackman, Mad Dog, pilot and BA climbed a 66 m single-cell face (825%) and the Atlas every lane up to 375%; the Rommel tank climbed up to a +50 m cell (625%) and was blocked at 825% (measured, MEASURED.md M2) | apex 19.71 m (M1); median 16.9, overlay keeps 16 | >= 30 m (M3) | claw (Space) latches objects only; not measured (M5 was tried twice on the Bogged Down power plant, the claw did not latch; no MACorporeal wall in the base archive) | M1, M3; UpStretch |
| Pilot on foot | bracket: 39-50 deg blocks, 66 grid passes (est. 1.32 m/2 m) | none | >= 30 m (M3) | speed p90 12.1 m/s | wiki FootSlope; M3 |
| Tank (Rommel) | planner 55 (real: no walker limit found in the final M2 run: the Atlas, Hackman, Mad Dog, pilot and BA climbed a 66 m single-cell face (825%) and the Atlas every lane up to 375%; the Rommel tank climbed up to a +50 m cell (625%) and was blocked at 825% (measured, MEASURED.md M2)) | none | >= 30 m (M3) | AI-only tracked variants not player-usable | code + M3 |
| VTOL | flies | n/a | n/a | ceiling terrain + 48.4 m; landing a VTOL settles on a 30% slope and slides off a 50% slope (bracket 30-50%, measured M4); the overlay uses 0.30, still estimated | measured ceiling |
| Turrets | immobile | n/a | n/a |  | catalog |

Also: no hover vehicles exist in MA2; mission AI (20 m nav graph) is not covered by overlay v1; `--start-in` is an editor flag. Water flags: the Atlas, Rommel and pilot cross a 4.7 m deep and a 1.2 m shallow editor pond at full speed (Bogged Down flags all 1); BA, type-16 cells and flags 0 not tested (M6).

## Corrections
The older BankDive fact ("a jetless 'Mech did not come down a long bank") concerned a slope band, not a vertical edge: see `t60.bankdive-correction-slope-band-not-edge`.


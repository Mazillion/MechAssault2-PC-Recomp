# VTOL ferry crawl: stale AI forward speed (2026-11-10)

## Overview
A flying VTOL's body velocity is written each step by the generic integrator sub_002AD940 as
`flight-model velocity (comp+0x10C) + heading x ground speed` (sub_002AD800). The ground speed ramps toward the
forward-speed target comp+0x98 that the AI MoveTo sets (sub_002AD5C0). The stick flight model never clears it, so a bot
craft handed from AI MoveTo to the stick kept whatever MoveTo last asked: a rider pick-up hover left -53.4 m/s (red ferry
crawled at 6 m/s) or +43 m/s (blue ferry overspeed 100+ m/s). The 11-04 "height ceiling" explanation was wrong.

## Key code
| address / function | what it does |
|---|---|
| sub_002AD940 | body integrator; SetVel at 0x2ADAB9 |
| sub_002AD800 | ground term: heading (sub_002AD120) x speed ramped by sub_002ACCA0 |
| sub_002AD5C0 | SetForwardSpeed: pct x comp+0xBC x 0.01 -> comp+0x98 |
| vtol_stick_take (ma2_bot_ctrl.c) | stick takes the craft, zeroes comp+0x98 (StickSpeed0) |
| vt_edge_recover (ma2_bot_ctrl.c) | EdgeStickOut: no MoveTo while the stick already flies out of the edge band |

## Env vars / switches
RECOMP_BOT_VTOL_STICK_SPEED0, RECOMP_BOT_VTOL_EDGE_STICK_OUT (both on), RECOMP_TEST_VTOL_VELWATCH=<n> (write watch).

## History
Fixed in v26-vtol b91f768 (2026-11-10), proven on botrec 2026-11-10 and in NRC/Bogged Down CTF runs.


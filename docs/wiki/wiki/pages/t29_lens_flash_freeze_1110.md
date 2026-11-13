# Explosion lens flash and the screen haze fix (R26 tint, 2026-11-10)

## Overview
Explosion and hit effects carry a `MAModSubObjectLensFilter` sub-object. When the effect comes into a view, the
game shows a full-screen colour quad (a filter object, class vtable 0x463D74) on that view's overlay list and fades
it: white, StartAlpha 0.2 at 0.6/s for a mech death (Expl_Mech_01, starting 2.2 s after the blast timer), 0.2 at
3/s for a BA mortar hit, 0.5 at 2.0/s for some other hits. The quad is drawn after the 3D scene and glow, before
the HUD.

The quad's visible flag and alpha are written only by the sub-object's per-frame update (sub_0013FC88). If that
update is skipped while the effect still exists, the flash stays on screen at constant strength: the uniform

## Key code
| Address | What it does | Source |
|---|---|---|
| sub_0013F4BD | LensFilter ctor (vtable 0x471E18) | recomp_0032.c |
| sub_0013F5D3 | creates the filter per view; colour = Color + StartAlpha; hidden | recomp_0032.c |
| sub_0013FC88 / sub_0013FD69 | per-frame update: in view, fade and show; else hide | recomp_0032.c |
| sub_0013F832 | release: removes the filters | recomp_0032.c |
| sub_000AFCB0 | filter draw (untextured or textured quad, colour bytes clamped to 0x80) | recomp_0018.c |
| sub_002B1270 then sub_00107DA3 | modifier update gate (slot 25, then slot 24) | recomp_0070.c, recomp_0026.c |
| sub_00107F63 | range test: view distance against max(600, camera vt+0x38) | recomp_0026.c |

## Added code and hooks
- recomp_manual.c: ma2_lens_shown, ma2_lens_draw_check, ma2_lens_forget (the fix); ma2_lens_test_skip (test
hook); ma2_lens_trace_update, ma2_lens_trace_draw (diagnostics).
- recomp_0032.c: calls in sub_0013FC88 (three show sites, including sub_0013FD69) and at sub_0013F832 entry.
- recomp_0018.c: call at sub_000AFCB0 entry. Reapply these hooks if the recomp sources are regenerated.
- nv2a_pb_exec.c: cover_trace (RECOMP_COVER_TRACE).

## Env vars and test hooks
RECOMP_LENS_TRACE=1, RECOMP_TEST_LENS_FREEZE=<s>, RECOMP_LENS_FREEZE_FIX=0, RECOMP_COVER_TRACE=1.

## Bugs and fixes
- Frozen lens flash = screen haze. Fixed in v26-tint 9896cc8 (exe sha256 d64c14b6.): a lens quad its update did
not show in this frame or the one before is hidden. Proven with RECOMP_TEST_LENS_FREEZE=6 (before: constant haze,
frame 1st-percentile luma 125 for about 3 s; after: hidden two frames later, scene normal) and a regression run
(natural flash 0.486 to 0.014 in 0.24 s, no hide events; black fades unchanged).

## Open questions
- Which gate skips the update in natural play: the range test with the stale x87 far value, the view mapping, or the
timing window. RECOMP_LENS_TRACE logs '[LENS] STALE' with the fields when it happens.


# Movies and front-end flow

Generated C: `generated (made at install time, not in the repository)`. Facts: `facts/t13_movies_frontend.json`.

## Overview

Two front-end screen classes play movies through a movie object stored at `this+0x64` (opened with `sub_000B0CF0(path, &this+0x64)`). The Title/Legal Screen (vtable 00481D00) plays the startup logos (MAIntro.mgv). The Attract Screen (vtable 004820C8) plays AttractMode, ConquestIntro and SPIntro. It takes its path from a table at 0x00459FE8 (stride 0x104) indexed by the global `[0x0050C9EC]` (0 attract, 1 Conquest, 2 SPIntro).

SPIntro starts from the mission-start state `sub_002439B7`. For a mission with no `loadmovie` entry, outside the demo, and only the first time (`[0x0050CA4A]` is 0), it sets `[0x50C9EC]=2`, switches to the Attract Screen state `[0x004EC04C]` and sets `[0x50CA4A]=1`. The second pass sees the flag and loads the mission. A mission with `loadmovie` plays a loading-screen movie and skips this path.

The Attract Screen update (slot 5, `sub_001F84DE`) ends the screen with result 2 when the movie object is gone, a pad 0 button is down (`sub_0001EE70(0)`), a key word is set (`sub_0001EDD0`), or the movie reports finished (vtable +0x28). So SPIntro is skippable in retail. The PC skip flag (`pc_input_movie_skip`) is read only in the Title/Legal update `sub_001F40B3`, so SPIntro ignores it.

## Key code

| Address | What it does | Source |
|---|---|---|
| 001F84DE | Attract Screen update; `loc_001F8521` returns 2 (done) | recomp_0052.c:8623 |
| 001F8439 | Attract Screen init; opens the movie from table index `[0x50C9EC]` | recomp_0052.c |
| 001F84B3 | Attract Screen exit; releases the movie | recomp_0052.c |
| 002439B7 | mission-start state; selects SPIntro once | ma2.xbe |
| 001F40B3 | Title/Legal update; PC hook at line 33742 | recomp_0051.c |
| 0001EE70 / 0001EE10 / 0001EDD0 | any pad button / Start-range button / any key words | ma2.xbe |

## Added code and hooks

`pc_input_movie_skip_pending(1)` (platform/pc_frontend.c:92) is consumed only in `sub_001F40B3`. `recomp_manual.c:565` sets the flag on any pad 0 button; `scenario/ma2_scenario.c` `quick_movies` sets it and also posts Enter after 1.5 s because of this gap.

Proposed hook (not applied): in `sub_001F84DE` after the movie-null check, `if(pc_input_movie_skip_pending(1))goto loc_001F8521;`. No seek or stop is needed; slot 15 releases the movie.

## Open questions

- No caller of `pc_display_movie(+1/-1)` was found in the source tree searched; the movie counter is driven elsewhere.
- `[0x4E4BB8]` and `[0x4E4BBC]` as any-key words is inferred.
- The 002439B7 flow is read from disassembly only (not generated C, not run).


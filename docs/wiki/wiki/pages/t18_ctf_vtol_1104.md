# Bot CTF VTOL flag runs: route, boarding, opening pair (2026-11-04)

After d23e814 (v13l), runs started but none reached the flag. 2cff8b2 fixes the causes below. Facts are in pack `t18_ctf_vtol_1104`.

## Why the VTOL "could not climb" and shuttled by its base

- **Implicit `clampf2`.** BeelineVary's bow called `clampf2` before its definition. MSVC compiled it as an implicit `int` function, so every bow point landed about 35 km off the map. The only hint was warning C4142. It is now defined before `vtol_direct_path`.
- **VtolEdgeAll bend pile-up.** A bend pushed out of the 25 m edge band could land on the same spot every time. It was then inserted 12 times (path n = 15), and after the craft passed it, each 0.5 s replan put it back behind the craft. EdgeBendAhead refuses bends more than 60° off the leg, or within 10 m of either end.
- **The climb was a symptom.** ModClimb's demand flickered because the route ahead flickered. Note that `comp+0x90` in a .botrec always reads 0, because it is sampled after the input pass, so it says nothing about the hook.

## Why the enemy pilot never boarded its VTOL

- **Entry zone geometry.** The NRC VTOL's entry zone (r 4.1) is centred 3.0 m ahead of the hull and 1.9 m up. The game offers the craft (player+0x428) only from the sides, 4.7–5.8 m from the hull centre at ±27–45° off the nose. A pilot in front of the nose, or pinned under the wing, gets no offer.
- **The measured hull rejects every spot.** The hull is a sector maximum and reads 6.6 m (the wing) toward the zone. So every candidate spot was "inside the hull", and the aim stayed on one blocked line.
- **WedgeRetry never stepped back.** `pilot_escape` had a 5 m reach, but the hull centre was 7 m away.
- **The fix is RimSlide.** It steps the pilot round the zone toward offer spots learned that match (mirrored to both sides, transferred between copies of the same model). It does this both at the rim and inside the zone after 1 s without an offer.

## The opening push (CtfPair)

- **One pair per team.** In the first seconds each team names one VTOL pilot and one BattleArmor pilot, the pair that reaches them soonest. Everyone else is steered to the 'Mechs.
- **The run starts at once.** It begins when the partner is in its suit, with no random roll. The BattleArmor holds until its VTOL is near; the hold must stand down, or the 0.05 ride margin never beats it.
- **The player can take a role.** If the player takes the VTOL, the bot BattleArmor waits for him but lets go if he flies off: 250 m away, 80 m beyond his nearest approach, or 30 s. If the player takes the BattleArmor, the bot VTOL holds 25 s for him.
- **The pair ends early when needed.** It ends when the run is on, when a paired bot takes another ride, or at 60 s. After that the normal emergent picking runs.
- **Start-up timing.** At t = 0.0 the world view can have no vehicles yet, so the pairing retries for 8 s.

## Results and what is left

A/B tests on NRC CTF Elite, no hooks:
- **v13l:** 0 of 5 runs reached the flag, 0 captures.
- **v13n–p:** 9 runs reached the flag, 3 captures, 0 wedges.

Still open:
- The NRC red BattleArmor pilot's ground route circles its suit.
- The VtolTwoRiders hold at pickup costs 5–8 s.
- The cruise drops to 15–30 m/s under fire.


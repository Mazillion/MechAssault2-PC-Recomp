# Public-release polish (2026-11): what changed, which fact says so, which build has it

## Overview
This topic collects the changes made while preparing the public release (version 1.0.2). Two groups: changes of
2026-10-21.11-09 that were already in the published PC Release (par24, 48831dc, exe 28c3eb44) but had no
fact, and the release-polish work of 2026-11-10 in the r26 branch (game), the editor repository and the installer kit.
The r26 items are code-complete and mostly proven in a hidden test run; they are NOT yet published. Each row below
names the fact (merged id = `t28.<id>`), the commit and where it shipped. Every fact also has a how_to_use line with
the switch and the proof. Corrections to older facts are in (BattleArmor walk, frame limit,
tank step, FPU shipped note, status clean-up).

Reading the status words: "par24" = in the published build; "r26" = in the r26 source branch, not yet published;
"editor" = editor repository after dcdcad4; "kit" = installer kit history after 7e85a1e.

## Fixes already in par24 (facts written late)
| Fix | Fact | Commit | Shipped in |
|---|---|---|---|
| Bot BattleArmor ran on the spot after a jet (walker bit comp+0x145 bit 0) | `ba-walker-bit-cleared-by-jet` | a8eab5c | par24 |
| Missing voice lines (one-packet XACT streams, XwbDefer) | `xact-one-packet-stream-xwbdefer` | c036eb4 | par24 |
| VTOL climb demand (comp+0x90) | `vtol-climb-demand-comp-90` | 37bfc0e | par24 |
| VTOL stick snap at high frame rates | `vtol-stick-snap-high-fps` | 7db3064 | par24 |
| Frame limit 144, no Unlimited | `frame-limit-no-unlimited-default-144` | c3bee9c | par24 |
| NavCache on by default | `navcache-default-on` | 46cfe22 | par24 |
| [CS-WAIT] hang sampler, DSound guards | `cs-wait-hang-sampler`, `audio-defer-guard-env`, `audio-active-guard-env` | 0163bba, 5a2b21c | par24 |
| quick-test, one archive at a time | `quick-test-mode`, `scenario-assets-one-archive-at-a-time` | (2026-10-21) | par24 |
| Bots 1.0.2 | `bots-1-0-2-shipped-par24` | 34d026e | par24 |
| Installer kit 11-09 (post-FastCode patch) | `installer-kit-1109-post-fastcode` | kit 94801368 | par24 installer |

## Fixes of 2026-11-10 (r26 and the other trees)
| Fix | Fact | Commit | Shipped in |
|---|---|---|---|
| Esc on a hub dialog | `esc-exit-needs-no-open-dialog` | da9d7fa | r26 |
| Sticky Keys restored on close | `sticky-keys-restore-on-close` | b175e9f | r26 |
| First-run display defaults | `first-run-display-defaults` | 1bdff90 | r26 |
| BOT MATCH label | `mp-menu-bot-match-label` | 842d862 | r26 |
| Binding rows named by action | `bindings-page-action-names` | 14b3ef6, f853f7a | r26 |
| Build header in the log, crash box | `log-build-header` | b0e15fd | r26 |
| One version | `one-version-file` | b0e15fd, f4bd842, kit 4204fc0 | r26 / editor / kit |
| Hang monitor default on | `hang-monitor-default-on` | df32c4e | r26 |
| Crash dump retention | `crash-dump-retention` | 7eba2c6 | r26 |
| Log hygiene | `runtime-log-policy` | 8a8cd00 | r26 |
| Bot diagnostics behind a switch | `bot-diag-switch` | 3285ed5, 2c51429 | r26 |
| Pause and mute on focus loss | `focus-loss-pause-mute` | 4aa50eb | r26 |
| Start-up failure boxes | `startup-failure-messages` | ca26290 | r26 |
| Scenario names in the picker | `custom-scenarios-picker-lists-scenario-name` | 44886f4, editor 5ae9bd7 | r26 / editor |
| Scenario save revision | `scenario-savedwith-revision` | 1d83c36, editor 7d40d9f | r26 / editor |
| Skipped scenario notes | `scenario-skip-reasons-note` | 9490421, editor 7b5e45b | r26 / editor |
| Unfinished scenarios (Ready = 0) | `scenario-ready-flag` | ed50ee7, editor b61993f | r26 / editor |
| Shader cache folder | `shader-cache-folder` | b9a367d | r26 |
| Footer key hints | `footer-key-hints-brackets` | 968a4d5 | r26 |
| Bot defaults, recorder limits, row texts | `bots-fresh-install-defaults`, `botrec-limits`, `bot-row-descriptions` | 1fa6613, 2fa5170, d938c8e | r26 |
| HotUpJet, BoxNear | `hot-upjet-hold`, `box-near-wedge` | 5201300, e9c9038 | r26 |
| Guard hit counts | `runtime-guards-sweep-hits` | 967efdc | r26 |
| Editor: release build, version, names, dialogs | `editor-release-build-flags`, `editor-version-about-userguide`, `scenario-name-unique-on-save`, `editor-new-dialog-scaling`, `scenario-name-description-limits`, `editor-damaged-file-warning`, `editor-labels-recent-files` | editor 5dc3f9a . f1f27ee | editor |
| Installer and kit | `installer-*`, `kit-contents-and-patch-format`, `public-settings-ini-keys`, `build-fails-without-fpu-edits`, `support-url-placeholder` | kit 87daf91 . 6bd63f3 | kit |
| Shipped documents | `public-documentation-set` | kit e27901a, 72264ea, 6d4e29f, ba01db0 | kit |

## Runtime guards (what each one is, its switch and log tag)
| Guard | Switch | Log tag | Fact |
|---|---|---|---|
| XACT list-walk spin guard | (always on) | `[XACT-LIST]`, `[XACT-UAF]`, `[XACT-HEAP]` | pack t02 |
| Voice allocation wait | (always on) | `[VOICE-NEW-WAIT]`, `[XACT-WAIT]` | pack t02 |
| Wave bank read deferral | `RECOMP_XWB_DEFER_MS`, `RECOMP_XWB_PAD=0` | `[XWB-DEFER]` | `t28.xact-one-packet-stream-xwbdefer` |
| DSound double insert | `RECOMP_AUDIO_DEFER_GUARD` | `[AUDIO-DEFER]` | `t28.audio-defer-guard-env` |
| DSound active-list re-insert | `RECOMP_AUDIO_ACTIVE_GUARD` | `[AUDIO-ACTIVE]` | `t28.audio-active-guard-env` |
| Critical-section wait sampler | `RECOMP_BOT_STALL=1` | `[BOT-STALL]`, `[CS-WAIT]` | `t28.cs-wait-hang-sampler` |
| Hang monitor | `RECOMP_HANG_MONITOR=0` turns it off | `[HANG]` | `t28.hang-monitor-default-on` |

A guard line in a player's log means the guard saved a freeze or crash; ask for the log. In 77 par24 sweep logs
only one [AUDIO-ACTIVE] line appeared (`t28.runtime-guards-sweep-hits`).

## Not done in r26 (still open)
Intel tint (W-24, needs the Intel laptop), game action names for four binding rows (W-05 step 2), score key in a bot
match (W-18), checkpoint save pause (W-20), shoreline tanks (W-21), frame-rate matrix (W-22), mission-2 close (W-23),
controller invert default (W-19), VTOL + BattleArmor rider crawl (W-35). Pointers were added to the matching open
facts (`t05.save-stall-open`, `t05.tank-shoreline-symptom`, `t10.bs-desktop-close`).

## Open questions
Does a Ready = 0 file written by the new editor hide correctly in a real game run? Does a clean bot match with no
RECOMP_BOT_DIAG print zero [C7] lines? Do a real elevated Build Tools install and an RTX 50 enhanced-content run
work (kit W-61, W-62)?


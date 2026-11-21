# Releasing

How a release of the MechAssault 2 PC Recomp is made: the branch rules, the two-step freeze, and the one tool that
builds, checks and gates a release, `tools/release.py`. Building the installer by hand is in [building.md](building.md).

## Branches

| Branch | What it holds | Rule |
|:--|:--|:--|
| `main` | the last public release | moves only when a release is published, after the maintainer's own test of the kept install |
| `release/1.x` | the 1.x line | **bug fixes only** (below); every 1.x release is built from here |
| `v2` | 2.0 development | always passes `gate task` |
| task branches | one change each | merged after a `gate task` pass, deleted on merge |

**1.x is bug-fix only.** A 1.x release needs a crash, a hang, lost saves or a failed install seen in two or more reports;
any privacy problem; or a fix for something the previous release broke. Features, tuning, new settings and text polish
go to 2.0. At most one 1.x release every two weeks unless a blocker appears.

**Forward merge within two days.** Every fix that lands on `release/1.x` is merged into `v2` within two days. Nothing is
merged backward from `v2` into `release/1.x`. The installer is shared: installer fixes land on `release/1.x` and flow
forward; 2.0 has no second installer line.

## One version number

The version is decided in `VERSION` (line 1 the number, line 2 the release date). Seven places carry it and must agree:

| Place | What |
|:--|:--|
| `VERSION` | the number and the date |
| `kit/installer/data/release.json` | `version` and `date`: the installer, its window and the install stamp |
| `kit/source/game/src/ma2_release.h` | `MA2_PUBLIC_VERSION` and `MA2_VERSION_MAJOR/MINOR/PATCH`: title screen and exe resource |
| `editor/src/app/version.h` | `MA2SE_VERSION`: the editor's About box |
| `editor/res/editor.rc` | `FileVersion`/`ProductVersion` strings and `FILEVERSION`/`PRODUCTVERSION` numbers |
| `kit/defaults/Docs/Changelog.txt` | the heading of the newest entry |
| `.github/ISSUE_TEMPLATE/bug_report.yml` | the Version field's placeholder |

The stub and the report helper read `VERSION` when they are built; the installed README and Docs get the number from
`release.json` at install time. `py -3.13 tools/release.py versions` checks every place;
`py -3.13 tools/release.py versions --set 1.2.3 --date 2026.12.01` writes all of them except the Changelog entry, which
needs real text. `tools/build_release.py` refuses to build when they disagree.

## The two-step freeze

Late wishes are expensive when they first show up in a finished build. So every release goes through two steps.

**Step 1: preview and decision sheet (the cheap phase).**

```bat
py -3.13 tools\release.py sheet <last-release-tag> --notes <release-notes-draft.md>
py -3.13 tools\release.py preview --config <your-config.json> --keep-install "<new folder>"
```

The **decision sheet** lists, from the changes since the last release: every text a player can see that was added or
changed (installer window, installer messages, game menus and pages, report helper, editor) with the text it replaces;
the code that orders menus (to check in the preview); defaults and switches (`pc_settings.ini`, settings and environment
switches the game reads, installer options); addresses (the report relay, the support page, any added URL); numbers with
a unit in the docs and texts; the full docs diff; the version stamps and whether they agree; and the release notes. The
appendix (commits, log lines) is not part of what is signed.

The **preview** is an installer that calls itself `X.Y.Z-preview.N` in its window, its stamp and the game's version
strings; everything else is the same code. The tool also writes a pre-release notes draft and the exact
`gh release create ... --prerelease` command into a text file. It never runs that command: publishing a preview, like
any release, needs the maintainer's yes.

The maintainer tries the preview, reads the sheet and changes what they like. When they are happy:

```bat
py -3.13 tools\release.py sign dist\release\sheet-<tag>.md
```

This records the sheet's hash and the date in `dist/release/signoff.json` (never committed). The hash covers the signed
part with line numbers taken out, so a fix that only moves code keeps the sign-off; any change to a text, default,
address, number, doc or the notes breaks it.

**Step 2: release candidate.**

```bat
py -3.13 tools\release.py rc --config <your-config.json> --keep-install "<new folder>"
```

`rc` refuses to start when the checkout has uncommitted changes or the signed sheet no longer matches the changes since
its base. Then it runs `gate rc` (which builds the release candidate in a fresh clone), copies the installer, its
SHA-256 and the gate result to `<work_dir>\rc-<version>-<commit>\`, and, if the gate passed, makes one kept install for
the maintainer's own test. One RC, one `gate rc`, one kept install, one yes.

After the RC is cut, only fixes for gate failures go in. **A later wish** goes into the next release. To put it into this
one is an **RC reset**, and the cost is shown before it is decided:

- a **light reset** (texts or data only): change, new sheet, sign again, `rc` again. The gate is the whole cost
  (see the timings below), plus the maintainer's second test of the kept install;
- a **full reset** (code): the same, plus whatever new tests the code needs; any earlier test of the kept install no
  longer counts.

## The gate

```bat
py -3.13 tools\release.py gate task --config <your-config.json>
py -3.13 tools\release.py gate rc   --config <your-config.json>
py -3.13 tools\release.py steps
```

One command; every program runs without a window; one `result.json` and `result.md` in
`<work_dir>\gate-<mode>-<time>\`. `--only a,b` runs a subset (with what those steps need), `--skip a,b` leaves steps out,
`--with a,b` adds `rc` steps to a `task` run, `--installer EXE` tests an installer you already built,
`--ref REF` tests another commit, `--keep-installs` keeps the test installs (they are deleted otherwise; logs and
manifests stay). The exit code is 0 when nothing failed.

`gate task` (a merge into `v2`, every RC):

| Step | Checks |
|:--|:--|
| clone | a fresh clone of the commit under test; uncommitted changes are never tested (and say so) |
| versions | the seven version places agree |
| deps | Capstone (and FFmpeg for the tests) from local copies, every file hash-checked against the lock |
| build | `tools/build_release.py`: editor, stub, report helper, pack, privacy check, SHA-256 |
| privacy_tree | `tools/privacy_scan.py` over the tree |
| privacy_bin | `privacy_scan.py --binaries`: the installer and every packed file, the stub, the editor, the helper |
| grepbin | the configured words, the user-profile path and this PC's user and machine names in every binary and packed file (ASCII and UTF-16) |
| relay_packed | the address in `kit/installer/data/report.json` is in the packed `report.json` and `MA2 Report.exe`; no staging address, no empty `relay_url` |
| helper_build | the tests' own report helper, built without any relay address (checked: none compiled in) |
| suites | every `tests/installer/test_*.py` |
| ui_flow | every `tests/installer/ui_flow*.py` (the installer windows, transparent and off screen) |
| relay_tests | the relay's own tests and its cross-checks against this kit (when `relay_dir` is configured) |
| kit_diff | packed files against the previous release's installer (`tools/kit_diff.py`): a review list |
| sheet | the decision sheet from `base` to the commit |

`gate rc` adds:

| Step | Checks |
|:--|:--|
| signoff | the signed sheet still matches |
| ab_fresh | a fresh install with the previous release's installer and with the new one, same disc and options: no file removed, only files that carry the version or build identity differ |
| relay_installed | the relay address in the installed tree, no staging or empty address |
| diagnostics | `--diagnostics` on the new install: the zip, its members, a privacy scan of every member |
| update | an update of a **copy** of the kept previous install: exit 0, the stamp says the new version, nothing a fresh install has is missing, nothing beyond identity and player files differs |
| fastpath | a second update takes the incremental fast path (only when the kit keeps an update cache; otherwise "not available in this kit") |
| fallback | a forced fingerprint change (`fallback_env`) falls back to a clean build and still installs (same condition) |
| fault | fault injection: every entry of `tests/installer/gate_faults.json` raises inside a real install and a real `--diagnostics`; both must finish and every reachable fault must fire. Add each new fail-safe method there |

Isolation: every test and install gets its own `LOCALAPPDATA`, `TEMP`, Documents and shortcut folders, and
`MA2_REPORT_RELAY` points at a loopback port nothing listens on, so a report can never be sent from a test. The
installers' own "one installer at a time" lock is respected (the gate waits). At the end the gate checks that no
installer log appeared in your own `%LOCALAPPDATA%\MechAssault2\Logs` during the run.

### The configuration file

JSON, kept outside the repository (it names folders on your PC). Only `work_dir` is required; a step whose inputs are
missing is skipped with the reason.

| Key | What |
|:--|:--|
| `work_dir` | where runs, previews and RCs go (big: installs are a few GB each while they run) |
| `build_dir` | the neutral build folder for `build_release.py` (ASCII, outside your user profile); default `<work_dir>\build` |
| `base` | the last release's tag or branch, for the sheet |
| `notes` | the release notes draft, shown in the sheet |
| `xiso`, `xbe`, `game_files` | your disc image, its `ma2.xbe` and an extracted copy (tests and installs) |
| `capstone`, `ffmpeg` | extracted copies of the two dependencies (no download) |
| `previous_installer`, `previous_install` | the last release's installer and a kept install made with it (only ever copied) |
| `relay_dir`, `node` | the relay's source folder and Node.js, for `relay_tests` |
| `grep_words` | extra words that must never appear in a binary (names, e-mail domains, time-zone offsets) |
| `relay_forbidden` | address fragments that must not be in a build (default `-staging`) |
| `known_failures` | `{"suite": ["test_name", ...]}`: failures that count as a warning, with the reason recorded elsewhere |
| `identity_files`, `ab_allowed_removed` | extra patterns for the A/B comparison |
| `fastpath_marker`, `fallback_env` | how the fast path shows in the install log, and the environment that forces a fallback |
| `install_timeout_minutes`, `suite_timeout_minutes`, `wait_for_installer_minutes` | time limits |

### Timings (one 24-thread PC, 32 GB)

Measured on the 1.1.6 code while other builds ran on the same PC, so treat them as upper bounds:

| Part | Time |
|:--|:--|
| `gate task` in all | 18 to 30 minutes |
| of which build (editor, stub, helper, pack, privacy check) | 5 to 11 minutes |
| of which the installer suites | 1.5 to 2 minutes |
| of which the three window flows | 11 to 14 minutes |
| clone, scans, relay tests, kit diff, sheet | under 2 minutes together |
| each real install in `gate rc` (two fresh, one update, one fault run) | 17 to 32 minutes each, one at a time |
| `--diagnostics` | under a minute |

`gate rc` therefore takes about two and a half hours on a quiet PC (four real installs on top of `gate task`), more when
another installer runs on the PC (the gate waits for it). A light RC reset costs one `gate rc`; a full reset costs that
plus the new tests.

### What the gate does not do

- It does not play the game. The game is checked by its own harness and by the maintainer's test of the kept install.
- Menu order is decided at run time: the sheet lists the code that changed it, the preview shows it.
- Low-memory builds, real slow PCs and a mid-build kill are covered by the installer's tests, not by a real install here.
- It never pushes, tags, publishes, uploads or deploys anything, and never sends a report.

# Contributing

Bug reports with logs are the most useful thing you can send. Code is welcome too.

## Rules

1. No game data. Don't commit, attach, paste or link disc images, files from the disc, extracted assets, the translated C code, upscaled texture packs, shader caches or save files.
2. No piracy. Don't ask for, offer or link to copies of the game.
3. If you used an AI assistant for a substantial part of a change, say so in the pull request. One line is enough.

## Reporting a bug

Use the [bug report form](https://github.com/Mazillion/MechAssault2-PC-Recomp/issues/new?template=bug_report.yml). Run the installer again and press Collect diagnostics, then attach the zip it puts on your Desktop. It has the game and installer logs and your settings, with your Windows user name removed. Say what you did, what you expected and what happened.

Crashes also leave a `crash-<number>.dmp` in `%LOCALAPPDATA%\MechAssault2\Logs`. A dump can contain your user name, so only share one if you're asked for it.

## Working on the code

Start with [docs/building.md](docs/building.md). You need your own disc to build or test anything.

- Changes to translated game code go in as patch entries, made with `tools/make_patches.py`. See [docs/patches.md](docs/patches.md).
- Keep the game logic original. A fix should be the smallest change that makes the PC build behave like the Xbox, or a clearly optional PC feature with a setting.
- Bot skill may change reactions, aim and decisions only, never damage, armour, health or weapons.
- Anything the installer runs has to stick to the Python standard library.
- Run `py -3.13 tools/privacy_scan.py` and the installer tests in `tests/installer` before opening a pull request.

Contributions are licensed under the project's MIT licence.

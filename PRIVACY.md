# Privacy: what "Report a problem" sends

Nothing is ever sent unless you press **Send report**. There are no accounts and no tracking. The only thing the game sends on its own is its version number, once per start, to check for updates (see [Checking for updates](#checking-for-updates)); you can turn that off.

## Where the button is

- In the game: the pause menu, Audio settings, and the window that opens if the game crashes or stops responding.
- In the installer: the failure screen and Help, then Report a problem.
- In the Start menu: Report a problem (MechAssault 2).

Before you send, **What's included?** lists the exact files. **Save a copy instead** keeps the report on your PC without sending anything.

## What a report contains

- The game's logs from your last few sessions, and the installer's logs.
- Your game settings file and the install record (version, build, install date).
- PC details: Windows version, CPU model, amount of memory, graphics card, and whether a laptop is on battery.
- After a crash or freeze: a crash dump of the game.
- From a bot match: the last few minutes of the match recording (unit positions and actions).
- Whatever you type in the "What happened?" box.

## What is removed or never collected

- Your Windows user name, PC name and e-mail addresses are replaced before the report leaves your PC. This includes the logs, the settings, the crash dump and the recording, and any of them you type in the text box.
- No personal files are included, only the files listed above.
- **Your IP address is not stored.** The report service briefly turns it into a scrambled code, only to stop one connection from sending too many reports in an hour. The key for that code is thrown away every day, and the code is never written down. Like any website, Cloudflare sees the address while delivering the report. The service's logging is switched off.

## Where it goes and how long it is kept

1. A small service on Cloudflare receives the report. Its code is in [`relay/`](relay/).
2. The report file is stored in Cloudflare storage and **deleted automatically after 60 days**.
3. A short entry is opened in a **private** GitHub repository that only the developer can see. It holds the report number, what you typed, and a short summary: version, Windows, CPU, memory, graphics card, and the error. This entry is kept until the developer deletes it.

A copy of every report also stays on your PC, in `%LOCALAPPDATA%\MechAssault2\Diagnostics`. You can delete it there at any time.

## Deleting a report

Ask for a report to be deleted by giving its report number (shown after sending) in an [issue](https://github.com/Mazillion/MechAssault2-PC-Recomp/issues), or in a message to the developer. The report file and its private entry will then be deleted.

## Checking for updates

Each time the game starts, it asks the report service once which version is the newest. The only thing it sends is the game's version number, for example 1.1.7. It sends nothing about you or your PC, and the service does not use or store your IP address for this.

If a newer version is out, the main menu shows one line, for example "Version 1.1.8 is available. Run the installer to update." Nothing is downloaded or installed, and nothing changes in the game.

To turn the check off, close the game, open `%LOCALAPPDATA%\MechAssault2\pc_settings.ini` in Notepad and set `CheckForUpdates=0` under `[Update]`.

## Checking it yourself

All of the code is in this repository:

- what goes into a report and how it is sent: [`kit/installer/report.py`](kit/installer/report.py), [`kit/installer/precheck.py`](kit/installer/precheck.py) and [`report-helper/`](report-helper/);
- the in-game page: [`kit/source/game/src/pc_native_settings.cpp`](kit/source/game/src/pc_native_settings.cpp);
- the update check: [`kit/source/game/src/pc_update_check.h`](kit/source/game/src/pc_update_check.h);
- the service that receives reports: [`relay/`](relay/).

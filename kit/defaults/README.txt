MechAssault 2 - PC recompilation, version {{VERSION}} (build {{BUILD}})
======================================================================

Run "MechAssault 2.exe".

This folder was built on this PC from your own MechAssault 2 disc by the
MechAssault 2 Recomp Installer: the xboxrecomp toolkit translated the game's
Xbox program to C, the PC version's fixes and features were applied, and the
result was compiled here. game_files holds the contents of your disc.

  MechAssault 2.exe        the game
  MA2 Scenario Editor.exe  make your own maps and missions
  game_files\              your disc's files (plus any enhanced textures/videos)
  pc_assets\               PC menus built from your disc, and ready-made
                           graphics shaders
  pc_settings.ini          first-launch settings (copied to %LOCALAPPDATA%)
  recomp-install.json      which installer version made this folder (used by
                           Update)
  Controls.txt             controls, menus, settings and bot matches
  Docs\                    guides: install, troubleshooting, known issues,
                           changes, first run and settings, bots, editor,
                           credits and licences
  Licenses\                licences of the project and the included components
  *.dll                    FFmpeg (cutscene playback) and the Microsoft C++
                           runtime


WHAT YOU NEED
-------------
64-bit Windows 10 (version 1903 or later) or Windows 11; a graphics card with
DirectX 11 (feature level 11.0); 8 GB of RAM recommended; about 3 GB installed
(5 GB free needed while installing, plus about 8 GB on C: if the installer has
to add Microsoft's C++ Build Tools). Only the US (NTSC) retail disc is
supported. Details: Docs\Install guide.txt.


SETTINGS, SAVES AND LOGS
------------------------
Your settings, saves and logs are kept per Windows user in
%LOCALAPPDATA%\MechAssault2, not here. The pc_settings.ini in this folder only
holds the first-launch defaults. To reset every setting, close the game and
delete %LOCALAPPDATA%\MechAssault2\pc_settings.ini; it is made again from the
defaults on the next launch. Updating keeps your settings and only adds new
ones (a backup is made first). Enhanced textures and videos are switched on in
Video Settings.

The shader cache is in %LOCALAPPDATA%\MechAssault2\ShaderCache and the bots'
route cache in %LOCALAPPDATA%\MechAssault2\NavCache (up to about 300 MB); both
and Logs can be deleted at any time, the game rebuilds them.

Saves: to back up your saves copy only UserData and TitleData from
%LOCALAPPDATA%\MechAssault2\Saved Games. The Partition*.img files there are
empty placeholders that take no real disk space.

Bot match recordings: off by default. Turn them on with BotRecorder=1 under
[Bots] in your pc_settings.ini and each offline bot match then writes a flight
record of every player's position and input to the Logs folder (about 6 MB a
minute; the newest 4 are kept, at most 150 MB each). It is only used to find
and fix bot problems, is never sent anywhere, and holds the game folder path
(which can include your Windows user name).


UPDATING AND UNINSTALLING
-------------------------
Updating: run the new "MechAssault 2 Recomp Installer.exe" and choose this
folder; it offers Update (no disc needed, your saves and settings are kept).

Uninstalling: run the installer; its first page finds this install and offers
Uninstall. It removes this folder and the shortcuts; tick "Also delete my
settings and saves" to remove %LOCALAPPDATA%\MechAssault2 as well. Your own
scenarios stay in Documents\MechAssault 2\Scenarios. If the installer added
Microsoft's C++ Build Tools, remove them in Windows Settings > Apps.


WINDOWS WARNINGS
----------------
"Windows protected your PC" when you start the installer is SmartScreen, which
warns about any new program that is not code-signed. This installer is not
signed. If you got it from where you expected, choose More info, then Run
anyway. Windows Smart App Control, when it is on, can block unsigned programs
without a way around; the installer never turns it off. The installer needs
no administrator rights, except for Microsoft's C++ Build Tools if you let it
install them (Microsoft's own installer asks).

If your antivirus quarantines a file during the install, the install stops
with a message; restore the file (or allow the folder) and run the installer
again, it resumes. Windows "Controlled Folder Access", if you turned it on,
can also block the game or the Scenario Editor from writing to Documents or
Desktop: allow them there.


GETTING HELP AND REPORTING A PROBLEM
------------------------------------
First look in Docs\Troubleshooting.txt and Docs\Known issues.txt.

To report a problem: run the installer and press Collect diagnostics on its
first page (it works even if nothing is installed). It puts one zip file on
your Desktop with the logs and settings (your Windows user name is removed
from the text files). Attach that zip at {{MA2_SUPPORT_URL}} and say which
version you have: the first line of this file. Crash or hang dumps and bot
recordings are added only if you tick them; do that only when a maintainer
asks for one.

Crash and hang dumps hold the call stack and part of the game's memory at
that moment (they can include folder paths and your Windows user name), so
share them only with someone you trust to look into the problem. Nothing is
ever sent anywhere by the game; delete them whenever you like.


CUSTOM SCENARIOS
----------------
"MA2 Scenario Editor.exe" makes multiplayer maps and single-player missions
from your own disc's models and textures. It saves .ma2map files to
Documents\MechAssault 2\Scenarios (change the folder under File > Settings >
General in the editor). Saved multiplayer maps appear in Multiplayer > Bot
Match > New Game, where the bot rows fill them with players; saved missions
appear under Custom Scenarios on the main menu. A scenario file refers to the
disc's models and textures instead of copying them; the receiving game builds
the level from its own disc. Help > About in the editor shows its version;
quote it in bug reports. Guide: Docs\Scenario editor.txt.


ENHANCED CONTENT (OPTIONAL)
---------------------------
2x/4x enhanced textures and 2x enhanced cutscene videos can be added at any
time: run "MechAssault 2 Recomp Installer.exe" again, choose this folder and
tick the parts you want. Video Settings offers each one once it is installed.
This needs an NVIDIA graphics card with 8 GB or more, an internet connection
(several GB of AI software are downloaded) and hours of processing. The
textures and the videos also work on AMD, Intel and other graphics cards,
through Vulkan (new; not yet tested on AMD or Intel cards; the videos are
made frame by frame there, so very slowly). 2x
textures add 0.6 GB, 4x add 2.2 GB, videos add 3.5 GB (25 GB free while they
are made).


LEGAL
-----
This is an unofficial fan project. It is not affiliated with or endorsed by
Microsoft, Xbox, Day 1 Studios or the BattleTech/MechWarrior rights holders.
MechAssault and Xbox are trademarks of their respective owners.

No game data files are included: the game's program, art, sound and levels
come from your own disc. Graphics shaders are compiled on your PC as the game
meets them, so a level's first visit can hitch briefly; nothing compiled from
the game is included.

The project's own code (the PC layer, bots, scenario support, installer and
scenario editor) is released under the MIT licence: Licenses\LICENSE.txt.
Third-party components and their licences are listed in
Licenses\THIRD-PARTY.txt. Video playback uses FFmpeg (GPL version 3, full
build by gyan.dev); where to get its source: Licenses\FFmpeg-README.txt and
Docs\Credits and licences.txt.

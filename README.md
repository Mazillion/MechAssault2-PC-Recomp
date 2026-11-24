<p align="center"><img src="docs/img/banner.jpg" alt="MechAssault 2: Lone Wolf Expanded" width="100%"></p>

# MechAssault 2: Lone Wolf Expanded

An unofficial PC port of MechAssault 2: Lone Wolf (Xbox, 2004). You need your own copy of the game to use it. There are no game files in this repo or in the releases.

**This is an offline-focused version of the game.** The goal is to bring new and interesting ways to play MechAssault 2 on your own or with friends on the same PC. So far that means offline bot matches in every multiplayer mode, split screen, and a scenario editor for building your own missions and maps.

<!-- trailer goes here -->

MechAssault 2 never came out on PC. This project takes the game's code off your own disc, recompiles it into a native Windows program using [xboxrecomp](https://github.com/sp00nznet/xboxrecomp), and puts a PC layer on top for graphics, input and settings. It's the original game code running natively, not a full emulator, although the Xbox audio chip and GPU are still emulated.

This is a personal project of mine, so it hasn't undergone extensive testing beyond my two systems, but I thought I would share it with the world in case others want to give feedback or build on it. There is no online multiplayer support, since that isn't my interest, would be a lot of work, and is a niche already filled by XLink Kai.

Not affiliated with Microsoft, Day 1 Studios or anyone who owns BattleTech / MechWarrior.

## The campaign, with PC features

The whole single-player campaign is here with the things you'd want from a PC version:

- Any resolution, including widescreen and ultrawide, with the HUD fixed to fit
- Frame rates up to 360 FPS
- Keyboard and mouse or a controller, fully rebindable. You can switch between them mid-game and the button prompts change to match.
- The menus work with the mouse, so you can point and click your way through them
- Extended draw distance
- Cutscenes that fill a widescreen display and run at 60 FPS
- Optional 2x/4x upscaled textures, generated on your own PC (takes hours; works on NVIDIA cards, and since 1.1.2 on AMD and Intel cards through Vulkan, which hasn't been tested on those cards yet)

| | |
|:-:|:-:|
| <img src="docs/img/campaign.jpg" alt="The campaign in ultrawide"> | <img src="docs/img/input.jpg" alt="Keyboard and mouse prompts"> |
| The campaign in ultrawide | Button prompts follow the keyboard and mouse |

<img src="docs/img/draw-distance.jpg" alt="Draw distance comparison on two maps" width="100%">

Draw distance on two maps: the original setting on the left, extended on the right.

## Bot matches

Every multiplayer mode can be played offline against bots, on every map.

The bots play to win the mode rather than just shooting at you:

- They go for the objective, whether that's a flag, a core or a terminal
- They use everything in the game: 'Mechs, tanks, VTOLs and Battle Armor. VTOL pilots pick up Battle Armor teammates and fly them across the map.
- They NeuroHack enemy 'Mechs, raise their own defences and pick weapons to suit the target
- Five difficulty levels from Green to Dragoon, plus Mixed. Harder bots aim, react and decide better. They never get extra damage or armour.
- Up to six a side

| | |
|:-:|:-:|
| <img src="docs/img/bots-vtol-flag.jpg" alt="A bot VTOL carrying a Battle Armor with the flag"> | <img src="docs/img/bots-tank-flag.jpg" alt="A bot tank carrying the flag under fire"> |
| A bot VTOL carrying a Battle Armor teammate who has the enemy flag | A bot tank makes off with the flag while a defender opens fire |

## Scenario editor

The MA2 Scenario Editor installs alongside the game. You can build your own campaign missions, with objectives, triggers and cutscenes, or your own multiplayer maps, out of the game's terrain, buildings and units. Press Test and you're playing it a few seconds later. Custom multiplayer maps work with bots too.

<img src="docs/img/editor-build-test-play.webp" alt="Building a mission in the editor, pressing Test, and playing it" width="100%">

| | |
|:-:|:-:|
| <img src="docs/img/editor-terrain.jpg" alt="Sculpting terrain"> | <img src="docs/img/editor-units.jpg" alt="Placing units"> |
| Sculpting terrain | Placing 'Mechs from the palette |
| <img src="docs/img/editor-objectives.jpg" alt="Mission objectives"> | <img src="docs/img/editor-environment.jpg" alt="Sky, weather and lighting"> |
| Writing a mission's briefing and objectives | Time of day, weather and fog |
| <img src="docs/img/editor-buildings.jpg" alt="Placing buildings"> | <img src="docs/img/custom-map.jpg" alt="A custom map"> |
| Editing a custom scenario in the scenario editor | A custom multiplayer map being played |

Your scenarios are saved as single files in `Documents\MechAssault 2\Scenarios` and show up in the game's Custom Scenarios menu.

## Installing

What you need:

- A dump of your own US (NTSC) MechAssault 2 disc, as an ISO/XISO or an extracted folder. Regular PC DVD drives can't read Xbox discs, so you have to dump it yourself.
- Windows 10 or 11, 64-bit, and a GPU that does Direct3D 11
- At least 8 GB of memory. With less than 16 GB the build takes longer, but the installer adjusts for it.
- About 5 GB of free space. If you don't have Visual Studio or the C++ Build Tools, the installer can install them for you, which needs another 8 GB or so.
- An internet connection during the install. It downloads FFmpeg, which the cutscenes need.

It has been tested mainly on a desktop with an RTX 3090 and an i9-12900K, and to a lesser degree on a laptop with an i7-1260P and Iris Xe graphics.

Steps:

1. Download `MechAssault 2 Expanded Installer.exe` from [Releases](https://github.com/Mazillion/MechAssault2-PC-Recomp/releases/latest).
2. Run it and point it at your disc dump.
3. Wait while it builds the game. This takes around 15 minutes on a decent 8-core CPU, and an hour or more with 4 cores or 8 GB of memory.
4. Launch MechAssault 2 from the shortcut.

The installer isn't signed, so Windows SmartScreen will warn you about it. Click "More info", then "Run anyway". If you have Smart App Control turned on, it will block the installer completely. The [install guide](kit/defaults/Docs/Install%20guide.txt) covers that, and installing without internet.

You can run the installer again later to update, repair or uninstall.

## Known issues

- AMD graphics cards haven't been tested. Intel integrated graphics showed a tinted picture and a low frame rate on one laptop, and that isn't explained yet.
- The first time you load each level it can run badly for a few seconds while shaders compile. It's fine after that.
- Only the US disc works. PAL and Japanese discs aren't supported.
- Every mission and cutscene loads and runs, but I haven't played all of them through to the end on this build.
- No online multiplayer.

The full list is in [Known issues.txt](kit/defaults/Docs/Known%20issues.txt). If the game crashes or something goes wrong, use Report a problem (in the window that opens after a crash, the pause menu, the Start menu or the installer's Help menu) and press Send report: it sends the logs, settings and PC details (plus a crash dump after a crash, or the last few minutes of a bot match), with your user name and PC name removed, and shows a report number. Nothing is sent until you press it, and sent reports are kept for 60 days. [PRIVACY.md](PRIVACY.md) lists exactly what a report contains and what it never collects. You can quote the number in an [issue](https://github.com/Mazillion/MechAssault2-PC-Recomp/issues). Don't attach game files.

## Building and modding

Everything in the release is built from this repo. [docs/building.md](docs/building.md) explains how to build it yourself. The rest of [docs/](docs/README.md) has notes on how the port works, the bots, the editor, and a big pile of research notes on the game's code.

## Credits

- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nz, the recompiler toolkit and runtime this is built on
- [xemu](https://github.com/xemu-project/xemu), for the audio and GPU code the runtime adapts
- [MGF-Explorer](https://github.com/LarvalExtract/MGF-Explorer) by LarvalExtract, for the MGF archive format
- Dear ImGui, FFmpeg, Capstone and the rest listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
- Day 1 Studios and Microsoft Game Studios, for making the game

AI coding tools were used in making this project.

## Legal

This is a free fan project and it will never be sold. MechAssault, Xbox and Microsoft are trademarks of Microsoft. BattleTech, MechWarrior and BattleMech are trademarks of their owners. MechAssault 2: Lone Wolf is © Microsoft Corporation. This project isn't affiliated with or endorsed by any of them.

The screenshots and trailer on this page were created under Microsoft's "Game Content Usage Rules" using assets from MechAssault 2: Lone Wolf, and are not endorsed by or affiliated with Microsoft.

The project's own code is MIT licensed, see [LICENSE](LICENSE). Rights holders, see [LEGAL.md](LEGAL.md) for how to reach me.

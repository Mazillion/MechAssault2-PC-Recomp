# Enhancer (optional enhanced textures and cutscenes)

The Enhancer makes the optional **2x / 4x enhanced texture packs** and the **2x enhanced cutscene videos** on the
player's own PC, from the player's own disc. Nothing it makes is shipped. It needs an NVIDIA GPU with CUDA
(a CPU run is possible but extremely slow) and several GB of downloads (PyTorch, the AI models, DirectXTex).

## How the installer runs it

When textures or videos are ticked on the Loadout page (or `--textures 2x,4x` / `--videos` on the command line),
`kit/installer/install.py` (`Installer.enhancer`, `textures`, `videos`) does this:

1. copies this folder into the install's work folder and runs `setup.py` there with a Python 3.11-3.13 (the
   installer's own 3.13 by default): it creates `.venv` with PyTorch (CUDA) and downloads the models and DirectXTex
   tools listed, with their SHA-256 values, in `tools.lock.json`;
2. textures: `extract_textures.py --all --game-dir <install>\game_files` (decodes every texture in the disc's
   `.mgf` archives into `originals/`), then `enhance.py --scale 2|4 --no-preview` (Real-ESRGAN, then DDS packing
   into `packs/<scale>x/`). The pack is moved to `<install>\game_files\texture_packs\<scale>x\`;
3. videos: writes the source movie and the FFmpeg paths into `FMV/config.json`, runs `FMV/setup_fmv.py`
   (RealBasicVSR model) and `FMV/enhance_all_fmvs.py --movie-root <install>\game_files\Movie --output-root
   <install>\game_files\Movie\Enhanced\2x --no-zip` (see [FMV/README.md](FMV/README.md)).

All steps resume where they stopped when the installer is run again. The licences of the AI tools travel with
their output (`<install>\Licenses\Enhancer\`).

## Running it by hand

From a copy of this folder, with Python 3.13 (64-bit):

```bat
py -3.13 setup.py
.venv\Scripts\python.exe extract_textures.py --all --game-dir "C:\Games\MechAssault 2\game_files"
.venv\Scripts\python.exe enhance.py --scale 2 --limit 3 --no-pack      & rem a short first try
.venv\Scripts\python.exe enhance.py --scale 2                          & rem the whole 2x pack
```

`setup.py --check` checks the imports and CUDA only. `enhance.py --pack-only` rebuilds the DDS pack from the
enhanced PNGs without AI, and `--scale 4` makes the 4x pack (it never overwrites the 2x outputs). Then copy
`packs\2x\textures` and `packs\2x\index.json` to `<install>\game_files\texture_packs\2x\` and choose
**Video Settings > Textures** in the game.

## Processing policies

`config.json` holds the defaults. Ordinary textures go through the AI model; packed light maps, cube and volume
textures, and effect sprites (smoke, sparks, glows) are resized per channel instead (`resample_data`), because
the AI model hardens soft effects and changes lighting data. `processing_overrides` in `config.json` changes the
policy of one source path. `tile_size` lowers GPU memory use; `device` can be `cpu`.

## How the game uses a pack

`index.json` records, for each texture, its original size, type and a fingerprint of its original pixels
(`original_rgba_match_key`: SHA-256 over `uint32(version=1, width, height, depth, frames)` followed by the top-level
RGBA8 bytes). The renderer computes the same fingerprint for each texture the game uploads and swaps in the DDS
when it matches; anything missing or invalid falls back to the original. Render targets and movie surfaces are
never replaced.

## Credits and licences

- [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN): BSD-3-Clause (models downloaded by `setup.py`).
- BasicSR's RRDBNet, adapted in `rrdbnet.py`: Apache-2.0 ([BasicSR](https://github.com/XPixelGroup/BasicSR)).
- [DirectXTex](https://github.com/microsoft/DirectXTex) `texconv` / `texassemble`: MIT (downloaded by `setup.py`).
- The MGF archive and picture format follows [MGF-Explorer](https://github.com/LarvalExtract/MGF-Explorer) by LarvalExtract.

Licence texts are in `LICENSES/`; the video tools' notice is `FMV/NOTICE.txt`.

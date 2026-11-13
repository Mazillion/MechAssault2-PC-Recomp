# Enhanced cutscenes (RealBasicVSR)

This workflow makes the optional **2x enhanced cutscene videos**. It runs the official **RealBasicVSR** model on
every Bink movie of the player's disc and writes MKVs that the game plays instead of the originals when the
enhanced videos are switched on (`FMVQuality=1` in `pc_settings.ini`; the installer sets it after making them). It reuses the CUDA PyTorch environment of the
texture Enhancer (`..\.venv`) and the FFmpeg build the installer downloaded for the game.

## How the installer runs it

`kit/installer/install.py` (`Installer.videos`) writes the source movie and the FFmpeg paths into `config.json`,
then runs:

```bat
python -B setup_fmv.py                  & rem downloads the RealBasicVSR checkpoint (hash in NOTICE.txt), checks FFmpeg
python -B enhance_all_fmvs.py --movie-root "<install>\game_files\Movie" ^
    --output-root "<install>\game_files\Movie\Enhanced\2x" --metadata-root "<work>\fmv-metadata" --no-zip
```

The disc has 28 movie files (25 fps under `50/`, 29.97 fps under `60/`, six languages). Language copies share
their video packets, so only **8 distinct video streams** (about 14,700 frames) go through the AI; each output
keeps its own file's audio. Every finished batch of frames is checked and kept, so a stopped run resumes.

## Running it by hand

Set `source`, `ffmpeg` and `ffprobe` in `config.json` (the defaults are placeholders), then from this folder:

```bat
..\.venv\Scripts\python.exe -B setup_fmv.py --check-only
..\.venv\Scripts\python.exe -B enhance_all_fmvs.py --movie-root "C:\Games\MechAssault 2\game_files\Movie" --scan-only
..\.venv\Scripts\python.exe -B enhance_intro.py --frames 300 --comparison      & rem 10 s of the campaign intro, side by side
```

`enhance_intro.py` works on the one movie named in `config.json` and writes to `output/`; `--scale` (2, 2.5, 3, 4),
`--strength`, `--seconds` / `--frames` and `--compare-scale` make comparison runs.

## Defaults

- **Size:** 1440 x 960, twice the original 720 x 480; the 4:3 shape is kept with an 8:9 sample aspect ratio.
- **Timing:** each source frame gives one output frame at the source rate; no motion interpolation.
- **Strength:** 50 % RealBasicVSR, 50 % Lanczos enlargement of the original, to keep the source's look.
- **Model:** the released checkpoint is native 4x; each batch runs at 4x and is resized to the final size.
- **Temporal batches:** 8 frames with up to 4 frames of context each side; strong scene cuts reset the context.
- **Precision:** FP32 (TF32 off). No extra sharpening, colour grading or frame-rate change.

## Licences

RealBasicVSR and the MMEditing reference modules in `vendor/reference/` are Apache-2.0 (see `NOTICE.txt` and
`vendor/reference/`); `realbasicvsr_model.py` adapts them for inference only.

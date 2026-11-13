"""Install/check the intro workflow. Never calls the model's forward method."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import urllib.request

from fmv_pipeline import ROOT, CHECKPOINT_SHA256, atomic_json, digest, source_info, subprocess_options


WEIGHTS_URL = "https://dl.dropboxusercontent.com/s/eufigxmmkv5woop/RealBasicVSR.pth"
AUTHOR_COMMIT = "b908e08323c84e96b08f0d58621026ab27f56bcb"
MMEDIT_COMMIT = "8b819f1d28d6eed6244721278a099f5dc0848a20"
REFERENCES = {
    "realbasicvsr_LICENSE.txt": ("ckkelvinchan/RealBasicVSR", AUTHOR_COMMIT, "LICENSE"),
    "official_realbasicvsr_x4.py": ("ckkelvinchan/RealBasicVSR", AUTHOR_COMMIT, "configs/realbasicvsr_x4.py"),
    "official_inference_realbasicvsr.py": ("ckkelvinchan/RealBasicVSR", AUTHOR_COMMIT, "inference_realbasicvsr.py"),
    "mmediting_LICENSE.txt": ("open-mmlab/mmagic", MMEDIT_COMMIT, "LICENSE"),
    "basicvsr_net.py": ("open-mmlab/mmagic", MMEDIT_COMMIT, "mmedit/models/backbones/sr_backbones/basicvsr_net.py"),
    "real_basicvsr_net.py": ("open-mmlab/mmagic", MMEDIT_COMMIT, "mmedit/models/backbones/sr_backbones/real_basicvsr_net.py"),
    "flow_warp.py": ("open-mmlab/mmagic", MMEDIT_COMMIT, "mmedit/models/common/flow_warp.py"),
    "sr_backbone_utils.py": ("open-mmlab/mmagic", MMEDIT_COMMIT, "mmedit/models/common/sr_backbone_utils.py"),
    "upsample.py": ("open-mmlab/mmagic", MMEDIT_COMMIT, "mmedit/models/common/upsample.py"),
}


def resolve(value):
    return (ROOT / value).resolve()


def download_file(url: str, target: Path, sha256=None):
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": "MechAssault2-FMV-Enhancer"})
    print(f"Downloading {target.name}...", flush=True)
    try:
        with urllib.request.urlopen(request, timeout=60) as remote, temp.open("wb") as local:
            while block := remote.read(1024 * 1024):
                local.write(block)
    except OSError as error:   # URLError, HTTPError, timeouts, connection resets, a full disk
        temp.unlink(missing_ok=True)
        raise RuntimeError(f"Could not download {target.name} from {url} ({error}). Check your internet connection and press Resume; "
                           "if the address no longer works, the model's host has moved or removed the file.") from error
    if sha256 and digest(temp) != sha256:
        raise RuntimeError(f"Downloaded {target.name} has an unexpected checksum; existing file was kept.")
    temp.replace(target)


def validate_setup(config, download=True):
    import numpy
    import PIL
    import torch
    from realbasicvsr_model import load_model

    if tuple(int(p) for p in torch.__version__.split("+")[0].split(".")[:2]) < (2, 6):
        raise RuntimeError("Use PyTorch >=2.6 for safe weights-only checkpoint loading. Run Enhancer/Setup.cmd.")
    source, ffmpeg, ffprobe, checkpoint = (resolve(config[k]) for k in
                                          ("source", "ffmpeg", "ffprobe", "checkpoint"))
    for path in (source, ffmpeg, ffprobe):
        if not path.is_file():
            raise FileNotFoundError(f"Missing file: {path}. Update FMV/config.json if this installation moved.")
    if download:
        if not checkpoint.is_file() or digest(checkpoint) != CHECKPOINT_SHA256:
            download_file(WEIGHTS_URL, checkpoint, CHECKPOINT_SHA256)
        for name, (repo, commit, path) in REFERENCES.items():
            target = ROOT / "vendor" / "reference" / name
            if not target.is_file():
                download_file(f"https://raw.githubusercontent.com/{repo}/{commit}/{path}", target)
    if not checkpoint.is_file() or digest(checkpoint) != CHECKPOINT_SHA256:
        raise RuntimeError("Model weights are missing or have the wrong checksum. Run FMV/Setup.cmd.")
    if config["device"] == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA is unavailable. Run Enhancer/Setup.cmd to install CUDA PyTorch.")
    encoders = subprocess.run([str(ffmpeg), "-hide_banner", "-encoders"], capture_output=True,
                              text=True, check=True, **subprocess_options()).stdout
    if "libx264 " not in encoders or "pcm_f32le " not in encoders:
        raise RuntimeError("FFmpeg needs libx264 and pcm_f32le encoders")
    info = source_info(ffprobe, source)
    if info["codec"] != "binkvideo":
        raise RuntimeError("The configured source is not the original Bink video")
    model, model_info = load_model(checkpoint, "cpu")
    del model  # Setup never runs inference or transfers the model to the GPU.
    gpu = torch.cuda.get_device_name(0) if torch.cuda.is_available() else None
    validation = {"status": "ready", "inference_run": False, "python": sys.version.split()[0],
                  "torch": torch.__version__, "numpy": numpy.__version__, "Pillow": PIL.__version__,
                  "gpu": gpu, "checkpoint_sha256": CHECKPOINT_SHA256, "model": model_info,
                  "source": info, "source_repo_commit": AUTHOR_COMMIT, "architecture_commit": MMEDIT_COMMIT,
                  "ffmpeg": str(ffmpeg), "ffprobe": str(ffprobe), "final_scale": config["scale"],
                  "default_strength": config["strength"]}
    atomic_json(ROOT / "setup_validation.json", validation)
    for folder in ("originals", f"enhanced/{config['scale']}x", "output", "logs"):
        (ROOT / folder).mkdir(parents=True, exist_ok=True)
    print(f"Ready: {gpu or 'CPU'}; PyTorch {torch.__version__}; official EMA checkpoint.")
    print(f"Intro: {info['width']}x{info['height']}, {info['frames']:,} frames, {info['fps']} fps, "
          f"{len(info['audio'])} audio tracks.")
    print(f"Configured output: {info['width'] * config['scale']}x{info['height'] * config['scale']}, "
          f"AI strength {config['strength']:.0%}.")
    print("Setup validated. This check did not extract or enhance frames or run AI inference.")
    return validation


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--check-only", action="store_true")
    options = args.parse_args()
    config = json.loads((ROOT / "config.json").read_text(encoding="utf-8"))
    validate_setup(config, download=not options.check_only)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"Setup failed: {exc}", file=sys.stderr)
        sys.exit(1)

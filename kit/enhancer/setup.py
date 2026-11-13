"""Install the local Enhancer stack. Never performs image inference."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request
import venv

ROOT = Path(__file__).resolve().parent
DOWNLOADS = [
    ("models/RealESRGAN_x2plus.pth", "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth", None),
    ("models/RealESRGAN_x4plus.pth", "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth", None),
    ("tools/texconv.exe", "https://github.com/microsoft/DirectXTex/releases/download/may2026/texconv.exe", "dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06"),
    ("tools/texassemble.exe", "https://github.com/microsoft/DirectXTex/releases/download/may2026/texassemble.exe", "324721a80cf954eccfd888a6c587f6602146f043a01d0181af25884c549e8f46"),
    ("LICENSES/BasicSR.txt", "https://raw.githubusercontent.com/XPixelGroup/BasicSR/master/LICENSE.txt", None),
    ("LICENSES/Real-ESRGAN.txt", "https://raw.githubusercontent.com/xinntao/Real-ESRGAN/master/LICENSE", None),
    ("LICENSES/DirectXTex.txt", "https://raw.githubusercontent.com/microsoft/DirectXTex/main/LICENSE", None),
]


def nvidia_gpu_names():
    """The NVIDIA graphics cards nvidia-smi reports (empty list if there is none or the tool is missing)."""
    try:
        result = subprocess.run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"], capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return []
    return [line.strip() for line in result.stdout.splitlines() if line.strip()] if result.returncode == 0 else []


def torch_plan(names):
    """Which PyTorch build suits these cards: RTX 50 (Blackwell) needs the current CUDA 12.8 build; older cards keep the tested 2.6.0 / CUDA 12.4.
    Returns (label, pip arguments, check expression run inside the venv)."""
    if any(re.search(r"RTX\s*(PRO\s*)?50\d\d|Blackwell", name, re.I) for name in names):
        return ("current PyTorch with CUDA 12.8 (RTX 50 series)", ["torch", "--index-url", "https://download.pytorch.org/whl/cu128"],
                "import torch; v = tuple(int(x) for x in torch.__version__.split('+')[0].split('.')[:2]); assert v >= (2, 7) and torch.version.cuda.startswith('12.8')")
    return ("PyTorch 2.6.0 with CUDA 12.4", ["torch==2.6.0", "--index-url", "https://download.pytorch.org/whl/cu124"],
            "import torch; assert torch.__version__.split('+')[0] == '2.6.0' and torch.version.cuda == '12.4'")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(relative, url, expected, recorded):
    path = ROOT / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        actual = digest(path)
        previous = recorded.get(relative, {}).get("sha256")
        if (expected and actual != expected) or (previous and actual != previous):
            raise RuntimeError(f"Checksum changed for {relative}; inspect the file before replacing it")
        print(f"Present: {relative}", flush=True)
    else:
        print(f"Downloading: {relative}", flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "MA2-Enhancer-Setup/1.0"})
        temp = path.with_suffix(path.suffix + ".part")
        with urllib.request.urlopen(request, timeout=90) as response, temp.open("wb") as stream:
            while chunk := response.read(1024 * 1024):
                stream.write(chunk)
        actual = digest(temp)
        if expected and actual != expected:
            raise RuntimeError(f"Official release checksum mismatch for {relative}")
        temp.replace(path)
    return {"url": url, "sha256": actual, "bytes": path.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Check installed imports and CUDA without installing or upscaling")
    args = parser.parse_args()
    python = ROOT / ".venv/Scripts/python.exe"
    if args.check:
        if not python.exists():
            raise RuntimeError("Run Setup.cmd first")
        subprocess.run([str(python), "-c", "import torch,numpy,PIL; print('PyTorch',torch.__version__,'NumPy',numpy.__version__,'Pillow',PIL.__version__); print('CUDA available:',torch.cuda.is_available()); print('GPU:',torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU')"], check=True)
        return
    if not python.exists():
        print("Creating local .venv (reusing installed CUDA PyTorch where available)...", flush=True)
        venv.EnvBuilder(with_pip=True, system_site_packages=True).create(ROOT / ".venv")
    packages = (("numpy", "2.2.3", "numpy==2.2.3"), ("PIL", "11.1.0", "Pillow==11.1.0"))
    for module, version, requirement in packages:
        probe = subprocess.run([str(python), "-c", f"import {module}; assert {module}.__version__ == '{version}'"], capture_output=True)
        if probe.returncode:
            subprocess.run([str(python), "-m", "pip", "install", requirement], check=True)
    names = nvidia_gpu_names()
    label, pip_args, expression = torch_plan(names)
    if subprocess.run([str(python), "-c", expression], capture_output=True).returncode:
        print(f"Installing {label}...", flush=True)
        subprocess.run([str(python), "-m", "pip", "install", *pip_args], check=True)
    if names and subprocess.run([str(python), "-c", "import torch; torch.zeros(1).cuda()"], capture_output=True).returncode:
        raise RuntimeError(f"PyTorch does not support this graphics card yet ({names[0]}). The enhanced content cannot be made on this PC; "
                           "the game itself is not affected.")
    lock_path = ROOT / "tools.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8")) if lock_path.exists() else {"downloads": {}}
    for relative, url, expected in DOWNLOADS:
        lock["downloads"][relative] = download(relative, url, expected, lock["downloads"])
        temp = lock_path.with_suffix(".json.part")
        temp.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
        temp.replace(lock_path)
    for folder in ("originals", "raw", "enhanced/2x", "enhanced/4x", "packs/2x", "packs/4x", "previews", "logs"):
        (ROOT / folder).mkdir(parents=True, exist_ok=True)
    subprocess.run([str(python), "-c", "import torch,numpy,PIL; print('PyTorch',torch.__version__,'CUDA:',torch.cuda.is_available()); print('GPU:',torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU')"], check=True)
    print("Setup complete. No textures were upscaled. Run Enhance.cmd when ready.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        sys.exit(1)

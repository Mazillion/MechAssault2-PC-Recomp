"""Install the local Enhancer stack. Never performs image inference."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import venv

ROOT = Path(__file__).resolve().parent
# Every tool runs without a console window of its own (nothing a user can close by accident) and in its own process group (no Ctrl+C
# from a console it would otherwise share); its output still goes to this script's output, which the installer writes to its log.
NO_CONSOLE = (getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)) if sys.platform == "win32" else 0
NTSTATUS = {
    0xC000013A: "stopped from outside: its window was closed or security software ended it",
    0x40010004: "ended from outside (another program or security software stopped it)",
    0xC0000005: "crashed (access violation)",
    0xC0000135: "could not start: a DLL it needs is missing",
    0xC0000142: "could not start: a DLL failed to initialise (security software can cause this)",
    0xC0000022: "was refused access (security software can cause this)",
}
ANTIVIRUS = ("Antivirus software may be blocking Python: allow the install folder in it, then try again. "
             "The game itself is installed and not affected.")


def explain(code):
    """A readable meaning for a Windows exit code that is an NTSTATUS (3221225786 = 0xC000013A), else ''."""
    code = int(code) & 0xFFFFFFFF
    if code in NTSTATUS:
        return NTSTATUS[code]
    return "ended with Windows error status 0x%08X" % code if code >= 0xC0000000 else ""


# pip: no user/global config, no cache, patient with slow connections. Inherited PIP_*, CONDA_* and PYENV_* settings never reach the tools.
PIP = ["-m", "pip", "--isolated", "--no-cache-dir", "--disable-pip-version-check"]
PIP_INSTALL = PIP + ["install", "--timeout", "120", "--retries", "10"]


def child_env():
    """The tools' environment: without PIP_*, CONDA_* and PYENV_* variables; TMP, TEMP and PIP_CACHE_DIR inside the work folder."""
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(("PIP_", "CONDA_", "PYENV_"))}
    tmp = ROOT / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env.update(TMP=str(tmp), TEMP=str(tmp), PIP_CACHE_DIR=str(tmp / "pip-cache"))
    return env


def run(args, what, capture=False):
    """Run a tool with no console of its own. capture=True: a quiet probe, returns the exit code. Otherwise a failure raises a readable error."""
    args = [str(a) for a in args]
    if capture:
        return subprocess.run(args, capture_output=True, stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE, env=child_env()).returncode
    code = subprocess.run(args, stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE, env=child_env()).returncode
    if code:
        meaning = explain(code)
        if meaning:
            raise RuntimeError(f"{what} {meaning} (exit code {code & 0xFFFFFFFF} = 0x{code & 0xFFFFFFFF:08X}). {ANTIVIRUS}")
        raise RuntimeError(f"{what} failed (exit code {code}); its messages are above")
    return 0


TLS_FAILURE = re.compile(r"CERTIFICATE_VERIFY_FAILED|SSLError|certificate verify failed|self[- ]signed certificate", re.I)
INTERCEPTED = ("This network intercepts secure (HTTPS) connections (a company proxy or security software that inspects them), so pip "
               "could not verify the download servers. Try another network, or ask the network's administrator.")


def pip_version(python):
    """(major, minor) of the venv's pip, or (0, 0)."""
    try:
        out = subprocess.run([str(python), *PIP, "--version"], capture_output=True, text=True, stdin=subprocess.DEVNULL,
                             creationflags=NO_CONSOLE, env=child_env(), timeout=60).stdout
        m = re.search(r"pip (\d+)\.(\d+)", out)
        return (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    except Exception:
        return (0, 0)


def pip_install(python, args, what):
    """pip install exactly as before; only if it FAILED on certificate verification: once more with the system certificate store
    (--use-feature=truststore, for pip 23.3-24.1; newer pip uses it already), then a plain "your network intercepts HTTPS" message."""
    cmd = [str(a) for a in [python, *PIP_INSTALL, *args]]
    tail = []
    no_console = NO_CONSOLE                          # (no console window of its own, as every tool here)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=no_console, env=child_env())
    for raw in proc.stdout:
        line = raw.decode("utf-8", errors="replace").rstrip()
        print(line, flush=True); tail = (tail + [line])[-60:]
    code = proc.wait()
    if not code:
        return 0
    if TLS_FAILURE.search("\n".join(tail)):
        if (23, 3) <= pip_version(python) < (24, 2):
            print("pip could not verify the servers' certificates; trying again with the Windows certificate store...", flush=True)
            if run([python, *PIP_INSTALL, "--use-feature=truststore", *args], "", capture=True) == 0:
                return 0
        raise RuntimeError(f"{what} failed: {INTERCEPTED}")
    meaning = explain(code)
    if meaning:
        raise RuntimeError(f"{what} {meaning} (exit code {code & 0xFFFFFFFF} = 0x{code & 0xFFFFFFFF:08X}). {ANTIVIRUS}")
    raise RuntimeError(f"{what} failed (exit code {code}); its messages are above")


class QuietEnvBuilder(venv.EnvBuilder):
    """venv's own child processes (ensurepip, upgrades) with no console of their own either; output still to this script's output."""
    def _call_new_python(self, context, *py_args, **kwargs):
        kwargs["creationflags"] = kwargs.get("creationflags", 0) | NO_CONSOLE
        kwargs.setdefault("stdin", subprocess.DEVNULL)
        return super()._call_new_python(context, *py_args, **kwargs)


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
        result = subprocess.run(["nvidia-smi", "--query-gpu=name", "--format=csv,noheader"], capture_output=True, text=True, timeout=30,
                                stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE)
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


def download_helper():
    """The installer's download.py (MA2_INSTALLER_DIR, set by the installer), or None when this script runs on its own."""
    folder = os.environ.get("MA2_INSTALLER_DIR")
    if not folder or not (Path(folder) / "download.py").is_file():
        return None
    if folder not in sys.path:
        sys.path.insert(0, folder)
    try:
        import download as helper
        return helper
    except ImportError:
        return None


def download(relative, url, expected, recorded):
    if relative.endswith(".pth"):                    # the models: the digest pinned in the shipped tools.lock.json, on every download
        expected = expected or recorded.get(relative, {}).get("sha256")
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
        temp = path.with_suffix(path.suffix + ".part")
        helper = download_helper()
        if helper:                                   # the installer's helper: resume, retries, the cause named, a per-user cache
            try:
                helper.fetch(url, temp, lambda line: print(line, flush=True), digest=expected, timeout=90)
            except helper.DownloadError as error:
                raise RuntimeError(f"Could not download {relative}: {error}") from error
        else:
            request = urllib.request.Request(url, headers={"User-Agent": "MA2-Enhancer-Setup/1.0"})
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
    parser.add_argument("--engine", choices=("pytorch", "vulkan"), default="pytorch",
                        help="vulkan: textures through realesrgan-ncnn-vulkan (fetched by the installer), so no PyTorch and no .pth models")
    args = parser.parse_args()
    vulkan = args.engine == "vulkan"
    python = ROOT / ".venv/Scripts/python.exe"
    if args.check:
        if not python.exists():
            raise RuntimeError("Run Setup.cmd first")
        run([python, "-c", "import torch,numpy,PIL; print('PyTorch',torch.__version__,'NumPy',numpy.__version__,'Pillow',PIL.__version__); print('CUDA available:',torch.cuda.is_available()); print('GPU:',torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU')"], "The check")
        return
    if python.exists() and run([python, *PIP, "--version"], "", capture=True):
        print("The .venv from an earlier run does not start pip; making it again...", flush=True)
        shutil.rmtree(ROOT / ".venv", ignore_errors=True)
    if not python.exists():
        print("Creating local .venv...", flush=True)
        QuietEnvBuilder(with_pip=False, system_site_packages=False).create(ROOT / ".venv")     # pip below, with no console of its own
    if not (ROOT / ".venv/Lib/site-packages/pip").is_dir():          # venv's own pip step, run here with no console
        print("Adding pip to the .venv...", flush=True)
        run([python, "-m", "ensurepip", "--upgrade", "--default-pip"], "Adding pip to the .venv")
    packages = (("numpy", "2.2.3", "numpy==2.2.3"), ("PIL", "11.1.0", "Pillow==11.1.0"))
    for module, version, requirement in packages:
        if run([python, "-c", f"import {module}; assert {module}.__version__ == '{version}'"], "", capture=True):
            pip_install(python, [requirement], f"pip install {requirement}")
    names = [] if vulkan else nvidia_gpu_names()
    label, pip_args, expression = torch_plan(names)
    if not vulkan and run([python, "-c", expression], "", capture=True):
        print(f"Installing {label}...", flush=True)
        pip_install(python, pip_args, f"pip install {pip_args[0]}")
    if names and run([python, "-c", "import torch; torch.zeros(1).cuda()"], "", capture=True):
        raise RuntimeError(f"PyTorch does not support this graphics card yet ({names[0]}). The enhanced content cannot be made on this PC; "
                           "the game itself is not affected.")
    lock_path = ROOT / "tools.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8")) if lock_path.exists() else {"downloads": {}}
    for relative, url, expected in DOWNLOADS:
        if vulkan and relative.endswith(".pth"):
            continue                                 # the Vulkan tool has its own model
        lock["downloads"][relative] = download(relative, url, expected, lock["downloads"])
        temp = lock_path.with_suffix(".json.part")
        temp.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
        temp.replace(lock_path)
    for folder in ("originals", "raw", "enhanced/2x", "enhanced/4x", "packs/2x", "packs/4x", "previews", "logs"):
        (ROOT / folder).mkdir(parents=True, exist_ok=True)
    if vulkan:
        run([python, "-c", "import numpy,PIL; print('NumPy',numpy.__version__,'Pillow',PIL.__version__,'(textures through Vulkan, no PyTorch)')"], "The final check")
    else:
        run([python, "-c", "import torch,numpy,PIL; print('PyTorch',torch.__version__,'CUDA:',torch.cuda.is_available()); print('GPU:',torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU')"], "The final check")
    print("Setup complete. No textures were upscaled. Run Enhance.cmd when ready.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        sys.exit(1)

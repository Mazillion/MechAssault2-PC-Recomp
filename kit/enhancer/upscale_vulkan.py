"""Real-ESRGAN through realesrgan-ncnn-vulkan: any graphics card with Vulkan (AMD, Intel or NVIDIA), no PyTorch or CUDA.

The tool is Real-ESRGAN's own Windows build (MIT; the installer downloads it and checks every file it uses). It has the
realesrgan-x4plus model, the same network as RealESRGAN_x4plus.pth stored at half precision. There is no x2plus model
for it, so 2x is the 4x result reduced with Lanczos, the way Real-ESRGAN's own --outscale 2 works with the x4 model.
Edge padding, alpha handling and the processing policies are those of upscale_engine.py, so the sizes and format of
every image, and so the texture pack, are unchanged.

Every run of the tool has no console window of its own and its own process group. Images are sent in batches
(prefetch), because each start of the tool loads the model again. The tool crashes (0xC0000409) when its working folder
or image paths are not plain ASCII, so it always runs in a plain folder: when the tool or work folder has any other
character (an accented user name, Japanese folder names, ...) the exe, its models and every image it reads or writes are
staged in a private <drive>:/MA2RecompVulkan-xxxxxxxx folder (removed again by close()).
"""
from __future__ import annotations
import atexit
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

MODEL = "realesrgan-x4plus"
NET_SCALE = 4
NO_CONSOLE = (getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)) if sys.platform == "win32" else 0
NTSTATUS = {
    0xC000013A: "stopped from outside: its window was closed or security software ended it",
    0x40010004: "ended from outside (another program or security software stopped it)",
    0xC0000005: "crashed (access violation)",
    0xC0000135: "could not start: a DLL it needs is missing",
    0xC0000142: "could not start: a DLL failed to initialise (security software can cause this)",
    0xC0000022: "was refused access (security software can cause this)",
    0xC0000409: "crashed (stack buffer overrun)",
}
TOOL_CRASHES = (0xC0000409, 0xC0000005)     # the tool itself fell over (a path it cannot take, a damaged file): not a driver problem
PLAIN = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _.-\\:")


def plain(path):
    """Only characters the tool takes: letters, digits, space, _ . - and the drive / folder separators."""
    return all(c in PLAIN for c in str(Path(path).resolve()))


def stage_bases(work):
    """Where a plain staging folder may go: the work folder's drive, ProgramData, Public, the system drive."""
    out = [Path(Path(work).resolve().anchor)]
    for v in ("ProgramData", "PUBLIC"):
        if os.environ.get(v): out.append(Path(os.environ[v]))
    out.append(Path(os.environ.get("SystemDrive", "C:") + "\\"))
    return out


def stage_folder(work):
    """A new, empty, writable folder with a plain ASCII path for the tool (named after the work folder, so a rerun reuses it)."""
    name = "MA2RecompVulkan-" + hashlib.sha256(str(Path(work).resolve()).encode("utf-8")).hexdigest()[:8]
    for base in stage_bases(work):
        folder = base / name
        if not plain(folder): continue
        try:
            shutil.rmtree(folder, ignore_errors=True)
            folder.mkdir(parents=True)
            (folder / "probe").write_bytes(b"")
            (folder / "probe").unlink()
            return folder
        except OSError:
            continue
    raise RuntimeError("no folder with a plain name could be made for the Vulkan upscaler")
PROGRESS = re.compile(r"^\s*\d+(\.\d+)?%\s*$")


class VulkanError(RuntimeError):
    fatal = False                     # True: the tool keeps failing; enhance.py stops the run instead of trying every image


def exit_text(code):
    """'exit code 3221225786 = 0xC000013A: stopped from outside: ...' or 'exit code 1'."""
    value = int(code) & 0xFFFFFFFF
    meaning = NTSTATUS.get(value) or ("ended with Windows error status 0x%08X" % value if value >= 0xC0000000 else "")
    return f"exit code {value} = 0x{value:08X}: {meaning}" if meaning else f"exit code {int(code)}"


class VulkanUpscaler:
    engine = "vulkan"

    def __init__(self, tool_dir, scale, config, work):
        tool_dir, work = Path(tool_dir), Path(work)
        if not (tool_dir / "realesrgan-ncnn-vulkan.exe").is_file() or not all((tool_dir / "models" / f"{MODEL}.{x}").is_file() for x in ("param", "bin")):
            raise VulkanError(f"The Vulkan upscaler is missing from {tool_dir}")
        if scale not in (2, 4):
            raise ValueError("scale must be 2 or 4")
        self.stage = None
        if not (plain(tool_dir) and plain(work)):         # the tool cannot take this path: a plain private copy of tool and work folder
            try:
                self.stage = stage_folder(work)
                (self.stage / "tool" / "models").mkdir(parents=True)
                for name in ("realesrgan-ncnn-vulkan.exe", "vcomp140.dll", f"models/{MODEL}.param", f"models/{MODEL}.bin"):
                    if (tool_dir / name).is_file(): shutil.copyfile(tool_dir / name, self.stage / "tool" / name)
            except (OSError, RuntimeError) as error:
                raise VulkanError(f"The Vulkan upscaler needs a folder with a plain name and none could be made ({error})") from error
            print(f"The Vulkan upscaler runs in {self.stage} (the install path has characters it cannot take).", flush=True)
            tool_dir, work = self.stage / "tool", self.stage / "work"
            atexit.register(self.close)                   # also when the run stops early
        self.exe = tool_dir / "realesrgan-ncnn-vulkan.exe"
        self.models = tool_dir / "models"
        self.scale, self.config, self.work = scale, config, work
        self.cache = {}
        self.gpu_name = ""
        self.failed_runs = 0          # tool runs in a row that failed
        shutil.rmtree(self.work, ignore_errors=True)
        self.work.mkdir(parents=True, exist_ok=True)
        try:
            self._selftest()
        except BaseException:
            self.close()                                  # no staging folder left behind
            raise
        print(f"{scale}x Real-ESRGAN ({MODEL}{', reduced to 2x' if scale == 2 else ''}) ready through Vulkan on "
              f"{self.gpu_name or 'the default graphics card'}.", flush=True)

    # -- the tool
    def _run(self, source, target):
        args = [str(self.exe), "-i", str(source), "-o", str(target), "-n", MODEL, "-s", str(NET_SCALE), "-m", str(self.models), "-f", "png"]
        gpu = str(self.config.get("vulkan_gpu", "")).strip()
        if gpu: args += ["-g", gpu]
        tile = int(self.config.get("vulkan_tile", 0) or 0)
        if tile: args += ["-t", str(tile)]
        try:
            result = subprocess.run(args, capture_output=True, stdin=subprocess.DEVNULL, creationflags=NO_CONSOLE,
                                    cwd=str(self.exe.parent))        # a plain working folder too (a non-ASCII one crashes the tool)
        except OSError as error:
            raise VulkanError(f"realesrgan-ncnn-vulkan could not be started ({error}). Antivirus software may be blocking it: "
                              "allow the install folder in it, then try again.") from error
        lines = [line.strip() for line in (result.stderr + result.stdout).decode("utf-8", "replace").splitlines()
                 if line.strip() and not PROGRESS.match(line)]
        for line in lines:
            match = re.match(r"^\[\d+ (.+?)\]\s", line)
            if match and not self.gpu_name: self.gpu_name = match.group(1)
        if result.returncode:
            tail = " | ".join(lines[-4:])
            self.failed_runs += 1
            error = VulkanError(f"realesrgan-ncnn-vulkan failed ({exit_text(result.returncode)}). {tail}".strip())
            error.fatal = self.failed_runs >= 3
            error.code = result.returncode & 0xFFFFFFFF
            raise error
        self.failed_runs = 0
        return lines

    def _selftest(self):
        """A tiny image first: a graphics card or driver without working Vulkan stops here with a clear message."""
        source, target = self.work / "selftest-in.png", self.work / "selftest-out.png"
        Image.new("RGB", (16, 16), (128, 96, 64)).save(source)
        try:
            lines = self._run(source, target)
        except VulkanError as error:
            if getattr(error, "code", None) in TOOL_CRASHES:
                raise VulkanError(f"The Vulkan upscaler crashed while starting: {error} This is a problem with the tool or the folder "
                                  "it runs in, not with the graphics driver.") from error
            raise VulkanError(f"The Vulkan upscaler could not run on this PC's graphics card: {error}. Updating the graphics driver "
                              "may help.") from error
        try:
            with Image.open(target) as image:
                ok = image.size == (16 * NET_SCALE, 16 * NET_SCALE)
        except OSError:
            ok = False
        if not ok:
            raise VulkanError("The Vulkan upscaler could not run on this PC's graphics card (no picture came back: "
                              f"{' | '.join(lines[-4:]) or 'no message'}). Updating the graphics driver may help.")
        source.unlink(missing_ok=True); target.unlink(missing_ok=True)

    # -- images
    def _padded(self, image, edge_mode):
        pad = int(self.config.get("boundary_padding", 16))
        if pad < 0 or edge_mode not in ("reflect", "wrap", "edge"):
            raise ValueError("Invalid boundary padding/edge mode")
        array = np.asarray(image.convert("RGB"), dtype=np.uint8)
        array = np.pad(array, ((pad, pad), (pad, pad), (0, 0)), mode=edge_mode)
        key = hashlib.sha256(b"%dx%d|" % array.shape[:2] + array.tobytes()).hexdigest()[:32]
        return array, pad, key

    def prefetch(self, requests):
        """Upscale a batch in one start of the tool. requests: [(image, edge_mode)], each the picture rgb() will be asked for."""
        inputs, outputs = self.work / "in", self.work / "out"
        shutil.rmtree(inputs, ignore_errors=True); inputs.mkdir(parents=True)
        outputs.mkdir(parents=True, exist_ok=True)
        wanted = []
        for image, edge_mode in requests:
            array, _, key = self._padded(image, edge_mode)
            if key in self.cache or key in wanted: continue
            Image.fromarray(array, "RGB").save(inputs / f"{key}.png")
            wanted.append(key)
        if not wanted:
            return 0
        self._run(inputs, outputs)
        for key in wanted:
            if (outputs / f"{key}.png").is_file(): self.cache[key] = outputs / f"{key}.png"
        shutil.rmtree(inputs, ignore_errors=True)
        return len(wanted)

    def rgb(self, image: Image.Image, edge_mode="reflect") -> Image.Image:
        width, height = image.size
        array, pad, key = self._padded(image, edge_mode)
        path = self.cache.pop(key, None)
        if path is None or not path.is_file():
            single = self.work / "single"
            shutil.rmtree(single, ignore_errors=True); single.mkdir(parents=True)
            Image.fromarray(array, "RGB").save(single / "in.png")
            self._run(single / "in.png", single / "out.png")
            path = single / "out.png"
        with Image.open(path) as opened:
            result = opened.convert("RGB")
        path.unlink(missing_ok=True)
        padded_h, padded_w = array.shape[:2]
        if result.size != (padded_w * NET_SCALE, padded_h * NET_SCALE):
            raise VulkanError(f"realesrgan-ncnn-vulkan returned {result.size[0]}x{result.size[1]}, expected "
                              f"{padded_w * NET_SCALE}x{padded_h * NET_SCALE}")
        if self.scale != NET_SCALE:
            result = result.resize((padded_w * self.scale, padded_h * self.scale), Image.Resampling.LANCZOS)
        start = pad * self.scale
        return result.crop((start, start, start + width * self.scale, start + height * self.scale))

    def requests(self, image, processing="ai_rgb", edge_mode="reflect"):
        """The pictures enhance() will send through the model for this image (for prefetch)."""
        rgba = image.convert("RGBA")
        if processing == "ai_alpha":
            return [(rgba.getchannel("A").convert("RGB"), edge_mode)]
        if processing != "ai_rgb":
            return []
        out = [(rgba.convert("RGB"), edge_mode)]
        if self.config.get("alpha_mode", "lanczos") == "ai":
            out.append((rgba.getchannel("A").convert("RGB"), edge_mode))
        return out

    def enhance(self, image, processing="ai_rgb", edge_mode="reflect"):
        """As upscale_engine.Upscaler.enhance: same policies, same output size and mode (RGBA)."""
        rgba = image.convert("RGBA")
        size = rgba.width * self.scale, rgba.height * self.scale
        if processing == "resample_data":
            return Image.merge("RGBA", tuple(channel.resize(size, Image.Resampling.BILINEAR) for channel in rgba.split()))
        if processing == "ai_alpha":
            mask = self.rgb(rgba.getchannel("A").convert("RGB"), edge_mode).convert("L")
            output = Image.new("RGBA", size, (255, 255, 255, 255))
            output.putalpha(mask)
            return output
        if processing != "ai_rgb":
            raise ValueError(f"Unknown processing policy: {processing}")
        rgb = self.rgb(rgba.convert("RGB"), edge_mode).convert("RGBA")
        alpha_mode = self.config.get("alpha_mode", "lanczos")
        if alpha_mode == "ai":
            alpha = self.rgb(rgba.getchannel("A").convert("RGB"), edge_mode).convert("L")
        elif alpha_mode in ("lanczos", "bilinear"):
            alpha = rgba.getchannel("A").resize(size, Image.Resampling.LANCZOS if alpha_mode == "lanczos" else Image.Resampling.BILINEAR)
        else:
            raise ValueError("alpha_mode must be lanczos, bilinear or ai")
        rgb.putalpha(alpha)
        return rgb

    def close(self):
        shutil.rmtree(self.work, ignore_errors=True)
        if self.stage: shutil.rmtree(self.stage, ignore_errors=True)

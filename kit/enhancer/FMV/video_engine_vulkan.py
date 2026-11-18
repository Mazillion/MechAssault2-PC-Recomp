"""2x cutscene frames through realesrgan-ncnn-vulkan, for graphics cards without CUDA (AMD, Intel, ...).

The same frame math as video_engine.py (RealBasicVSR) after the model: the network's picture brought to the output size, then
blended with a Lanczos enlargement of the original at the same AI strength. The difference is the model: Real-ESRGAN x4plus works
on single frames (no temporal model), reduced to 2x with Lanczos (upscale_vulkan.VulkanUpscaler, the same tool, checks,
no-console runs and plain-path staging as the Vulkan textures). Each batch of frames goes through one start of the tool.
"""

from pathlib import Path
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))      # enhancer/: upscale_vulkan
from upscale_vulkan import MODEL, VulkanUpscaler                    # noqa: E402

CHUNK_SIZE, CONTEXT = 48, 0          # frames per tool start; no temporal context is needed by a single-frame model


def model_sha256(tool_dir) -> str:
    """The digest of the ncnn weights: part of the job signature, so a Vulkan frame is never taken for a RealBasicVSR one."""
    import hashlib
    with (Path(tool_dir) / "models" / f"{MODEL}.bin").open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def blend(enhanced: Image.Image, original: Image.Image, strength: float) -> Image.Image:
    """strength * AI + (1 - strength) * Lanczos, as video_engine.render_frame."""
    baseline = original.resize(enhanced.size, Image.Resampling.LANCZOS)
    out = strength * np.asarray(enhanced, dtype=np.float32) + (1.0 - strength) * np.asarray(baseline, dtype=np.float32)
    return Image.fromarray(np.rint(np.clip(out, 0, 255)).astype(np.uint8), "RGB")


class VulkanVideoEnhancer:
    def __init__(self, tool_dir, strength: float, scale=2, work=None, upscaler=None):
        if scale != 2:
            raise ValueError("The Vulkan cutscene path makes 2x videos only")
        self.strength = strength
        self.upscaler = upscaler or VulkanUpscaler(tool_dir, 2, {"boundary_padding": 16}, Path(work))

    def enhance(self, paths: list[Path], keep_start: int, keep_end: int) -> list[Image.Image]:
        frames = []
        for path in paths[keep_start:keep_end]:
            with Image.open(path) as image:
                frames.append(image.convert("RGB"))
        self.upscaler.prefetch([(frame, "reflect") for frame in frames])
        return [blend(self.upscaler.rgb(frame, "reflect"), frame, self.strength) for frame in frames]

    def close(self):
        self.upscaler.close()

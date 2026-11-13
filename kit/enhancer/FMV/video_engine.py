"""Bounded temporal RealBasicVSR batches with configurable final resolution."""

from pathlib import Path

import numpy as np
import torch
from PIL import Image
from torch.nn import functional as F

from realbasicvsr_model import load_model


def render_frame(prediction, original: np.ndarray, strength: float, scale: float) -> Image.Image:
    """Resize/blend an existing prediction; this helper does not run a model."""
    height, width = original.shape[:2]
    out = F.interpolate(prediction, size=(round(height * scale), round(width * scale)),
                        mode="bicubic", align_corners=False, antialias=True)
    out = out.clamp_(0, 1)[0].permute(1, 2, 0).cpu().numpy()
    baseline = np.asarray(Image.fromarray(original).resize(
        (round(width * scale), round(height * scale)), Image.Resampling.LANCZOS
    ), dtype=np.float32) / 255.0
    blended = strength * out + (1.0 - strength) * baseline
    return Image.fromarray(np.rint(np.clip(blended, 0, 1) * 255).astype(np.uint8))


class VideoEnhancer:
    def __init__(self, checkpoint: Path, strength: float, device="cuda", scale=2):
        if scale not in (2, 2.5, 3, 4):
            raise ValueError("Final scale must be 2, 2.5, 3, or 4")
        if device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA is unavailable. Run Setup.cmd to check the PyTorch installation.")
        self.device = device
        self.strength = strength
        self.scale = scale
        # FP32 is deliberate: recurrent flow and restoration are sensitive to precision.
        torch.set_num_threads(4)
        torch.backends.cudnn.benchmark = False
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.backends.cudnn.allow_tf32 = False
        self.model, self.info = load_model(checkpoint, device)

    @torch.inference_mode()
    def enhance(self, paths: list[Path], keep_start: int, keep_end: int) -> list[Image.Image]:
        originals = []
        for path in paths:
            with Image.open(path) as image:
                originals.append(np.array(image.convert("RGB"), copy=True))
        array = np.stack(originals)
        lqs = torch.from_numpy(array).permute(0, 3, 1, 2).unsqueeze(0)
        lqs = lqs.to(self.device, dtype=torch.float32).div_(255.0)
        outputs = self.model(lqs)
        results = []
        for index in range(keep_start, keep_end):
            results.append(render_frame(outputs[:, index], originals[index], self.strength, self.scale))
        del lqs, outputs
        return results

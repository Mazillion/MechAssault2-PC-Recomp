"""Native 2x/4x Real-ESRGAN inference with padded tiles and explicit alpha handling."""
from __future__ import annotations
import numpy as np
from PIL import Image


class Upscaler:
    def __init__(self, weights, scale, config):
        import torch
        from rrdbnet import RRDBNet
        self.torch = torch
        self.scale = scale
        self.config = config
        device = config.get("device", "cuda")
        if device.startswith("cuda") and not torch.cuda.is_available():
            raise RuntimeError("CUDA is unavailable. Run Setup.cmd --check, or set device to cpu in config.json")
        self.device = torch.device(device)
        self.half = bool(config.get("half_precision", True)) and self.device.type == "cuda"
        checkpoint = torch.load(weights, map_location="cpu", weights_only=True)
        params = checkpoint.get("params_ema", checkpoint.get("params"))
        if params is None:
            raise ValueError("Checkpoint has no params_ema or params")
        self.model = RRDBNet(scale=scale)
        self.model.load_state_dict(params, strict=True)
        self.model.eval().to(self.device)
        if self.half:
            self.model.half()
        print(f"Native {scale}x Real-ESRGAN ready on {self.device} ({'FP16' if self.half else 'FP32'}).", flush=True)

    def rgb(self, image: Image.Image, edge_mode="reflect") -> Image.Image:
        torch = self.torch
        array = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
        height, width = array.shape[:2]
        pad = int(self.config.get("boundary_padding", 16))
        if pad < 0 or edge_mode not in ("reflect", "wrap", "edge"):
            raise ValueError("Invalid boundary padding/edge mode")
        array = np.pad(array, ((pad, pad), (pad, pad), (0, 0)), mode=edge_mode)
        padded_h, padded_w = array.shape[:2]
        # Native 2x RRDBNet performs pixel-unshuffle; its input must be even.
        array = np.pad(array, ((0, padded_h % 2), (0, padded_w % 2), (0, 0)), mode="edge")
        tensor = torch.from_numpy(array.transpose(2, 0, 1).copy()).unsqueeze(0).to(self.device)
        if self.half:
            tensor = tensor.half()
        tile = int(self.config.get("tile_size", 192))
        overlap = int(self.config.get("tile_overlap", 32))
        if tile < 0 or overlap < 0:
            raise ValueError("Tile size and overlap must be nonnegative")
        _, _, full_h, full_w = tensor.shape
        output = np.empty((full_h * self.scale, full_w * self.scale, 3), dtype=np.uint8)
        step = tile or max(full_h, full_w)
        with torch.inference_mode():
            for y in range(0, full_h, step):
                for x in range(0, full_w, step):
                    right, bottom = min(x + step, full_w), min(y + step, full_h)
                    left_pad, top_pad = max(0, x - overlap), max(0, y - overlap)
                    right_pad, bottom_pad = min(full_w, right + overlap), min(full_h, bottom + overlap)
                    # Align all tile borders to the model's 2-pixel unshuffle grid.
                    left_pad -= left_pad % 2
                    top_pad -= top_pad % 2
                    right_pad = min(full_w, right_pad + right_pad % 2)
                    bottom_pad = min(full_h, bottom_pad + bottom_pad % 2)
                    patch = tensor[:, :, top_pad:bottom_pad, left_pad:right_pad]
                    result = self.model(patch)
                    crop = result[0, :, (y - top_pad) * self.scale:(bottom - top_pad) * self.scale,
                                  (x - left_pad) * self.scale:(right - left_pad) * self.scale]
                    pixels = crop.float().clamp(0, 1).mul(255).round().byte().permute(1, 2, 0).cpu().numpy()
                    output[y * self.scale:bottom * self.scale, x * self.scale:right * self.scale] = pixels
                    del result, crop
        start = pad * self.scale
        return Image.fromarray(output[start:start + height * self.scale, start:start + width * self.scale].copy())

    def enhance(self, image, processing="ai_rgb", edge_mode="reflect"):
        rgba = image.convert("RGBA")
        size = rgba.width * self.scale, rgba.height * self.scale
        if processing == "resample_data":
            # Interpolate channels independently: alpha may be data rather than transparency.
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

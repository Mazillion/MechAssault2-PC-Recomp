# Copyright (c) OpenMMLab. All rights reserved.
# Adapted from MMEditing v0.16.0 (Apache-2.0); see vendor/reference and NOTICE.txt.
# Changes: inference-only PyTorch layers replace MMCV wrappers, single-frame
# sequences are supported, and optical flow is evaluated in smaller batches.
"""RealBasicVSR inference architecture, with the official checkpoint key names."""

from pathlib import Path

import torch
from torch import nn
from torch.nn import functional as F


def flow_warp(x, flow, padding_mode="zeros"):
    if x.shape[-2:] != flow.shape[1:3]:
        raise ValueError("Feature and flow dimensions differ")
    h, w = x.shape[-2:]
    yy, xx = torch.meshgrid(
        torch.arange(h, device=x.device, dtype=x.dtype),
        torch.arange(w, device=x.device, dtype=x.dtype),
        indexing="ij",
    )
    grid = torch.stack((xx, yy), dim=2) + flow
    grid = torch.stack(
        (2 * grid[..., 0] / max(w - 1, 1) - 1,
         2 * grid[..., 1] / max(h - 1, 1) - 1), dim=3
    )
    return F.grid_sample(x, grid, mode="bilinear", padding_mode=padding_mode,
                         align_corners=True)


class ResidualBlockNoBN(nn.Module):
    def __init__(self, channels=64):
        super().__init__()
        self.conv1 = nn.Conv2d(channels, channels, 3, 1, 1)
        self.conv2 = nn.Conv2d(channels, channels, 3, 1, 1)
        self.relu = nn.ReLU(inplace=True)

    def forward(self, x):
        return x + self.conv2(self.relu(self.conv1(x)))


class ResidualBlocksWithInputConv(nn.Module):
    def __init__(self, inputs, channels=64, blocks=20):
        super().__init__()
        self.main = nn.Sequential(
            nn.Conv2d(inputs, channels, 3, 1, 1),
            nn.LeakyReLU(0.1, inplace=True),
            nn.Sequential(*(ResidualBlockNoBN(channels) for _ in range(blocks))),
        )

    def forward(self, x):
        return self.main(x)


class PixelShufflePack(nn.Module):
    def __init__(self, channels=64):
        super().__init__()
        self.upsample_conv = nn.Conv2d(channels, channels * 4, 3, 1, 1)

    def forward(self, x):
        return F.pixel_shuffle(self.upsample_conv(x), 2)


class ConvModule(nn.Module):
    """The no-normalization MMCV ConvModule configuration used by SPyNet."""

    def __init__(self, inputs, outputs, activate=True):
        super().__init__()
        self.conv = nn.Conv2d(inputs, outputs, 7, 1, 3)
        self.activate = nn.ReLU(inplace=True) if activate else nn.Identity()

    def forward(self, x):
        return self.activate(self.conv(x))


class SPyNetBasicModule(nn.Module):
    def __init__(self):
        super().__init__()
        self.basic_module = nn.Sequential(
            ConvModule(8, 32), ConvModule(32, 64), ConvModule(64, 32),
            ConvModule(32, 16), ConvModule(16, 2, activate=False),
        )

    def forward(self, x):
        return self.basic_module(x)


class SPyNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.basic_module = nn.ModuleList(SPyNetBasicModule() for _ in range(6))
        self.register_buffer("mean", torch.tensor([0.485, 0.456, 0.406]).view(1, 3, 1, 1))
        self.register_buffer("std", torch.tensor([0.229, 0.224, 0.225]).view(1, 3, 1, 1))

    def compute_flow(self, ref, supp):
        n, _, h, w = ref.shape
        ref, supp = [(ref - self.mean) / self.std], [(supp - self.mean) / self.std]
        for _ in range(5):
            ref.append(F.avg_pool2d(ref[-1], 2, 2, count_include_pad=False))
            supp.append(F.avg_pool2d(supp[-1], 2, 2, count_include_pad=False))
        ref, supp = ref[::-1], supp[::-1]
        flow = ref[0].new_zeros(n, 2, h // 32, w // 32)
        for level in range(6):
            up = flow if level == 0 else F.interpolate(
                flow, scale_factor=2, mode="bilinear", align_corners=True
            ) * 2.0
            flow = up + self.basic_module[level](torch.cat(
                (ref[level], flow_warp(supp[level], up.permute(0, 2, 3, 1), "border"), up), 1
            ))
        return flow

    def forward(self, ref, supp):
        h, w = ref.shape[-2:]
        size = ((h + 31) // 32 * 32, (w + 31) // 32 * 32)
        ref = F.interpolate(ref, size=size, mode="bilinear", align_corners=False)
        supp = F.interpolate(supp, size=size, mode="bilinear", align_corners=False)
        flow = F.interpolate(self.compute_flow(ref, supp), size=(h, w),
                             mode="bilinear", align_corners=False)
        flow[:, 0] *= w / size[1]
        flow[:, 1] *= h / size[0]
        return flow


class BasicVSRNet(nn.Module):
    def __init__(self, channels=64, blocks=20, flow_batch_size=2):
        super().__init__()
        self.mid_channels = channels
        self.flow_batch_size = flow_batch_size
        self.spynet = SPyNet()
        self.backward_resblocks = ResidualBlocksWithInputConv(channels + 3, channels, blocks)
        self.forward_resblocks = ResidualBlocksWithInputConv(channels + 3, channels, blocks)
        self.fusion = nn.Conv2d(channels * 2, channels, 1)
        self.upsample1 = PixelShufflePack(channels)
        self.upsample2 = PixelShufflePack(channels)
        self.conv_hr = nn.Conv2d(channels, channels, 3, 1, 1)
        self.conv_last = nn.Conv2d(channels, 3, 3, 1, 1)
        self.img_upsample = nn.Upsample(scale_factor=4, mode="bilinear", align_corners=False)
        self.lrelu = nn.LeakyReLU(0.1, inplace=True)

    def compute_flow(self, lrs):
        n, t, c, h, w = lrs.shape
        if t == 1:
            empty = lrs.new_empty(n, 0, 2, h, w)
            return empty, empty
        first = lrs[:, :-1].reshape(-1, c, h, w)
        second = lrs[:, 1:].reshape(-1, c, h, w)

        def estimate(a, b):
            # Splitting along batch has no effect on the per-pair architecture.
            return torch.cat([
                self.spynet(a[i:i + self.flow_batch_size], b[i:i + self.flow_batch_size])
                for i in range(0, len(a), self.flow_batch_size)
            ]).view(n, t - 1, 2, h, w)

        backward = estimate(first, second)
        mirror = t % 2 == 0 and torch.equal(lrs[:, :t // 2], lrs[:, t // 2:].flip(1))
        return (None if mirror else estimate(second, first)), backward

    def forward(self, lrs):
        n, t, _, h, w = lrs.shape
        if h < 64 or w < 64 or t < 1:
            raise ValueError("RealBasicVSR requires at least one frame and dimensions >= 64")
        forward, backward = self.compute_flow(lrs)
        outputs = []
        feat = lrs.new_zeros(n, self.mid_channels, h, w)
        for i in range(t - 1, -1, -1):
            if i < t - 1:
                feat = flow_warp(feat, backward[:, i].permute(0, 2, 3, 1))
            feat = self.backward_resblocks(torch.cat((lrs[:, i], feat), 1))
            outputs.append(feat)
        outputs.reverse()
        feat = torch.zeros_like(feat)
        for i in range(t):
            lr = lrs[:, i]
            if i:
                flow = forward[:, i - 1] if forward is not None else backward[:, -i]
                feat = flow_warp(feat, flow.permute(0, 2, 3, 1))
            feat = self.forward_resblocks(torch.cat((lr, feat), 1))
            out = self.lrelu(self.fusion(torch.cat((outputs[i], feat), 1)))
            out = self.lrelu(self.upsample1(out))
            out = self.lrelu(self.upsample2(out))
            out = self.conv_last(self.lrelu(self.conv_hr(out)))
            outputs[i] = out + self.img_upsample(lr)
        return torch.stack(outputs, dim=1)


class RealBasicVSRNet(nn.Module):
    def __init__(self):
        super().__init__()
        self.dynamic_refine_thres = 1.0  # Official inference default: 255 / 255.
        self.image_cleaning = nn.Sequential(
            ResidualBlocksWithInputConv(3), nn.Conv2d(64, 3, 3, 1, 1)
        )
        self.basicvsr = BasicVSRNet()

    def forward(self, lqs):
        for _ in range(3):
            residues = []
            for i in range(lqs.shape[1]):
                residue = self.image_cleaning(lqs[:, i])
                lqs[:, i] += residue
                residues.append(residue)
            if torch.stack(residues, dim=1).abs().mean() < self.dynamic_refine_thres:
                break
        return self.basicvsr(lqs)


def load_model(checkpoint: Path, device="cpu"):
    """Strictly load EMA generator weights. Loading alone performs no inference."""
    payload = torch.load(checkpoint, map_location="cpu", weights_only=True)
    state = payload["state_dict"]
    prefix = "generator_ema." if any(k.startswith("generator_ema.") for k in state) else "generator."
    weights = {k[len(prefix):]: v for k, v in state.items() if k.startswith(prefix)}
    if not weights:
        raise ValueError("No RealBasicVSR generator weights found")
    model = RealBasicVSRNet()
    model.load_state_dict(weights, strict=True)
    model.eval().requires_grad_(False)
    model.to(device)
    return model, {"weight_set": prefix[:-1], "tensor_count": len(weights),
                   "parameter_count": sum(p.numel() for p in model.parameters())}

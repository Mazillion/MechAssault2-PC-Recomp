"""MA2 archive and compiled picture reader; does not modify game archives.

Directory/picture format references:
https://github.com/LarvalExtract/MGF-Explorer/tree/master/src/MGF/Structures
Swizzle and compressed-volume addressing follow this project's nv2a_texture.c.
"""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, asdict
from io import BytesIO
from pathlib import Path
import hashlib
import json
import re
import struct
import zlib

import numpy as np
from PIL import Image

IMAGE_EXTENSIONS = {".tif", ".tiff", ".dds", ".tga", ".png", ".bmp"}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_hash(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".part")
    temp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temp.replace(path)


def save_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".part")
    image.save(temp, format="PNG")
    temp.replace(path)


def slug(name: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]+", "_", Path(name).stem).strip("_")[:65] or "texture"


class Mgf:
    """Read compressed/uncompressed archives, with a bounded decompression cache."""
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        self.blocks = OrderedDict()
        self.chunks = []
        if self.data[:4] == b"mgf ":
            self.size = len(self.data)
        elif self.data[4:8] == bytes.fromhex("0f5a73c5"):
            count, self.size = struct.unpack_from("<II", self.data, 8)
            self.block_size = struct.unpack_from("<I", self.data, 20)[0]
            if not self.block_size or count > 100000:
                raise ValueError("Invalid compressed archive header")
            self.chunks = [struct.unpack_from("<III", self.data, 32 + n * 12) for n in range(count)]
        else:
            raise ValueError(f"Unknown archive header: {path}")
        header = struct.unpack("<11I", self.read(0, 44))
        if header[0] != 0x2066676D or self.read(4, 1) != b"\x04":
            raise ValueError("Not an MA2 archive")
        nf, lf, of, nd, ld, od, ls, os = header[3:]
        if lf != nf * 32 or ld != nd * 24:
            raise ValueError("Invalid archive directory sizes")
        records, rows, strings = self.read(of, lf), self.read(od, ld), self.read(os, ls)
        self.records = [struct.unpack_from("<8I", records, i * 32) for i in range(nf)]
        self.rows = [struct.unpack_from("<6i", rows, i * 24) for i in range(nd)]
        self.names = [strings[r[4]:].split(b"\0", 1)[0].decode("ascii", errors="replace") for r in self.rows]
        self.files = {self.name(i).replace("\\", "/").strip("/"): r[5]
                      for i, r in enumerate(self.rows) if r[5] >= 0}

    def name(self, index: int) -> str:
        parts, seen = [], set()
        while index >= 0:
            if index in seen or index >= len(self.rows):
                raise ValueError("Invalid archive directory ancestry")
            seen.add(index)
            parts.append(self.names[index])
            index = self.rows[index][1]
        return "/".join(reversed(parts))

    def read(self, address: int, size: int) -> bytes:
        if address < 0 or size < 0 or address + size > self.size:
            raise ValueError("Archive range outside file")
        if not self.chunks:
            return self.data[address:address + size]
        result = []
        while size:
            number, offset = divmod(address, self.block_size)
            if number not in self.blocks:
                start, length, _checksum = self.chunks[number]
                block = zlib.decompress(self.data[start:start + length])
                self.blocks[number] = block
                if len(self.blocks) > 32:
                    self.blocks.popitem(last=False)
            self.blocks.move_to_end(number)
            part = self.blocks[number][offset:offset + size]
            if not part:
                raise ValueError("Compressed archive block is short")
            result.append(part)
            address += len(part)
            size -= len(part)
        return b"".join(result)

    def file(self, name: str) -> bytes:
        index = self.files.get(name)
        if index is None:
            key = next(n for n in self.files if n.casefold() == name.casefold())
            index = self.files[key]
        record = self.records[index]
        return self.read(record[6], record[3])


@dataclass
class Picture:
    width: int
    height: int
    flags: int
    levels: int
    frames: int
    depth: int
    payload_offset: int
    payload_bytes: int
    container: str = "MGIc/PIC"
    row_alignment: int = 1

    @property
    def compression(self):
        return (self.flags >> 8) & 15

    @property
    def pixel_format(self):
        return self.flags & 15

    @property
    def kind(self):
        if self.frames == 6 and self.depth == 1:
            return "cube"
        if self.depth > 1:
            return "volume"
        return "array" if self.frames > 1 else "2d"

    @property
    def format_name(self):
        if self.container != "MGIc/PIC":
            return self.container
        if self.compression:
            return {1: "DXT1", 3: "DXT3", 5: "DXT5"}.get(self.compression, "unknown")
        return {0: "ARGB8888", 1: "RGB565", 3: "ARGB4444", 5: "A8", 7: "A8L8"}.get(self.pixel_format, "unknown")

    @classmethod
    def parse(cls, data: bytes):
        if data[4:8] != b"MGIc" or data[32:36] != b"PIC ":
            with Image.open(BytesIO(data)) as image:
                return cls(image.width, image.height, 0, 1, 1, 1, 0, len(data), image.format)
        fields, cursor = {}, 48
        while cursor + 8 <= len(data):
            tag = data[cursor:cursor + 4]
            length = struct.unpack_from("<I", data, cursor + 4)[0]
            if length < 8 or cursor + length > len(data):
                raise ValueError(f"Invalid picture chunk {tag!r} at {cursor}")
            if tag == b"bits":
                offset, size = cursor + 8, length - 8
                break
            if length >= 12:
                fields[tag] = struct.unpack_from("<I", data, cursor + 8)[0]
            cursor += length
        else:
            raise ValueError("Missing picture pixel payload")
        picture = cls(fields[b"wdth"], fields[b"hgt "], fields[b"flgs"],
                      fields[b"mips"], fields[b"frms"], fields[b"dpth"], offset, size)
        if min(picture.width, picture.height, picture.frames, picture.depth, picture.levels) < 1:
            raise ValueError("Invalid picture dimensions")
        if max(picture.width, picture.height) > 16384 or picture.depth > 2048 or picture.frames > 256:
            raise ValueError("Picture dimensions exceed supported limits")
        if fields[b"size"] != size:
            raise ValueError("Picture size and payload chunk disagree")
        if not picture.compression and not picture.flags & 16:
            expected = sum(picture.level_size(level) for level in range(picture.levels)) * picture.frames
            if expected != size:
                picture.row_alignment = 64
                expected = sum(picture.level_size(level) for level in range(picture.levels)) * picture.frames
                if expected != size:
                    raise ValueError("Unsupported linear picture row-pitch layout")
        return picture

    def metadata(self):
        return {**asdict(self), "kind": self.kind, "format": self.format_name,
                "swizzled": bool(self.flags & 16)}

    def level_size(self, level: int):
        w, h, d = max(1, self.width >> level), max(1, self.height >> level), max(1, self.depth >> level)
        if self.compression:
            return ((w + 3) // 4) * ((h + 3) // 4) * d * (8 if self.compression == 1 else 16)
        bpp = {0: 4, 1: 2, 3: 2, 5: 1, 7: 2}.get(self.pixel_format)
        if bpp is None:
            raise ValueError(f"Unsupported pixel format {self.pixel_format}")
        pitch = ((w * bpp + self.row_alignment - 1) // self.row_alignment) * self.row_alignment
        return pitch * h * d

    def images(self, data: bytes):
        """Decode every top-level face/frame/slice; lower mips stay in the raw backup."""
        if self.container != "MGIc/PIC":
            with Image.open(BytesIO(data)) as image:
                return [image.convert("RGBA")]
        if self.compression not in (0, 1, 3, 5):
            raise ValueError(f"Unsupported compression {self.compression}")
        payload = data[self.payload_offset:self.payload_offset + self.payload_bytes]
        frame_bytes = sum(self.level_size(level) for level in range(self.levels))
        stride = frame_bytes
        # Retail picture allocation (0x2667F7) rounds every animated frame,
        # as well as cube faces, up to 128 bytes before multiplying by frames.
        if self.frames > 1 and len(payload) == ((frame_bytes + 127) & ~127) * self.frames:
            stride = (frame_bytes + 127) & ~127
        if stride * self.frames > len(payload):
            raise ValueError("Pixel payload is shorter than its mip/frame layout")
        images = []
        for frame in range(self.frames):
            top = payload[frame * stride:frame * stride + self.level_size(0)]
            if self.compression:
                block_bytes = 8 if self.compression == 1 else 16
                columns, rows = (self.width + 3) // 4, (self.height + 3) // 4
                if self.depth > 1:
                    # Each XY DXT tile stores up to four adjacent Z slices.
                    source = np.frombuffer(top, dtype=np.uint8).reshape(-1, block_bytes)
                    slices, cursor = [], 0
                    for first in range(0, self.depth, 4):
                        count = min(4, self.depth - first)
                        group = source[cursor:cursor + rows * columns * count].reshape(rows * columns, count, block_bytes)
                        slices.extend(group[:, z, :].tobytes() for z in range(count))
                        cursor += rows * columns * count
                else:
                    slices = [top]
                decoder = {1: 1, 3: 2, 5: 3}[self.compression]
                # Pillow's BCN decoder expects a complete block-sized rectangle.
                for raw in slices:
                    image = Image.frombytes("RGBA", (columns * 4, rows * 4), raw, "bcn", decoder)
                    images.append(image.crop((0, 0, self.width, self.height)))
            else:
                bpp = {0: 4, 1: 2, 3: 2, 5: 1, 7: 2}[self.pixel_format]
                raw = np.frombuffer(top, dtype=np.uint8)
                if self.row_alignment > 1:
                    pitch = ((self.width * bpp + self.row_alignment - 1) // self.row_alignment) * self.row_alignment
                    raw = raw.reshape(self.depth, self.height, pitch)[:, :, :self.width * bpp].copy()
                raw = raw.reshape(-1, bpp)
                if self.flags & 16:
                    offsets = swizzle_offsets(self.width, self.height, self.depth)
                    raw = raw[offsets.reshape(-1)]
                rgba = expand_pixels(raw, self.pixel_format).reshape(self.depth, self.height, self.width, 4)
                images.extend(Image.fromarray(rgba[z]) for z in range(self.depth))
        return images


def swizzle_offsets(width: int, height: int, depth: int = 1):
    if any(size & (size - 1) for size in (width, height, depth)):
        raise ValueError("Swizzled dimensions must be powers of two")
    z, y, x = np.indices((depth, height, width), dtype=np.uint32)
    result = np.zeros((depth, height, width), dtype=np.uint32)
    bit, position = 1, 0
    while bit < max(width, height, depth):
        for coordinates, size in ((x, width), (y, height), (z, depth)):
            if bit < size:
                result |= ((coordinates & bit) != 0).astype(np.uint32) << position
                position += 1
        bit <<= 1
    return result


def expand_pixels(raw: np.ndarray, format_code: int):
    result = np.full((len(raw), 4), 255, dtype=np.uint8)
    if format_code == 0:
        result[:] = raw[:, [2, 1, 0, 3]]
    elif format_code == 5:
        result[:, 3] = raw[:, 0]
    elif format_code in (1, 3, 7):
        value = raw[:, 0].astype(np.uint16) | (raw[:, 1].astype(np.uint16) << 8)
        if format_code == 1:
            for channel, shift, mask in ((0, 11, 31), (1, 5, 63), (2, 0, 31)):
                c = (value >> shift) & mask
                result[:, channel] = (c << (3 if mask == 31 else 2)) | (c >> (2 if mask == 31 else 4))
        elif format_code == 3:
            for channel, shift in ((0, 8), (1, 4), (2, 0), (3, 12)):
                result[:, channel] = ((value >> shift) & 15) * 17
        else:
            result[:, :3] = raw[:, :1]
            result[:, 3] = raw[:, 1]
    else:
        raise ValueError(f"Unsupported pixel format {format_code}")
    return result


# Effect sprites -- smoke, dust, steam, sparks, flames, glows, trails and beams -- are soft and
# semi-transparent, drawn blended over the scene. The AI model sharpens their soft colour into hard
# edges and invents detail inside them (a jump jet's smoke column showed dark boxes, crack patterns
# and straight seams along the sprites' edges). They are resampled instead, which keeps exactly the
# original look at the higher resolution. config.json's processing_overrides still win.
EFFECT_FOLDERS = ("texture_dump/specialeffects/",)


def effect_sprite(source_path):
    return source_path.replace("\\", "/").casefold().startswith(EFFECT_FOLDERS)

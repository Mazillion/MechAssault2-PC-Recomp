"""Build DDS files and an indexed external pack. Game integration is a separate step."""
from __future__ import annotations
import json
import os
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile
from PIL import Image, ImageChops, ImageStat
from ma2_assets import file_hash, write_json
from foliage_mips import POLICY, foliage_source, build_foliage_input, validate_foliage_mips


def frame_match_keys(root, originals, width, height):
    """The Xbox binds an animated array one frame at a time, as a 2D texture."""
    keys=[]
    for name in originals:
        with Image.open(root/name) as image:
            if image.size != (width,height):
                raise ValueError("Original animation frame dimensions changed")
            keys.append(hashlib.sha256(struct.pack('<5I',1,width,height,1,1)+image.convert('RGBA').tobytes()).hexdigest())
    return keys


def dds_info(path: Path):
    with path.open("rb") as stream:
        header = stream.read(148)
    if len(header) < 128 or header[:4] != b"DDS " or struct.unpack_from("<I", header, 4)[0] != 124:
        raise ValueError("Invalid DDS header")
    height, width = struct.unpack_from("<II", header, 12)
    depth, levels = struct.unpack_from("<II", header, 24)
    return {"width": width, "height": height, "depth": depth, "mip_levels": levels}


def validate_dds(path, picture, width, height, format_name):
    """Check type, format, every mip's byte count, and complete file length.

    DDS header definitions: https://learn.microsoft.com/windows/win32/direct3ddds/
    Only the two formats emitted by this packager are accepted.
    """
    info = dds_info(path)
    with path.open("rb") as stream:
        header = stream.read(148)
    if (info["width"], info["height"]) != (width, height):
        raise ValueError("DDS dimensions do not match current PNGs")
    if struct.unpack_from("<I", header, 76)[0] != 32:
        raise ValueError("Invalid DDS pixel-format header")
    caps = struct.unpack_from("<I", header, 112)[0]
    array_size, start = 1, 128
    if header[84:88] == b"DX10":
        if len(header) < 148:
            raise ValueError("Truncated DDS extended header")
        dxgi, dimension, flags, array_size, _ = struct.unpack_from("<5I", header, 128)
        expected_dxgi = {"BC7_UNORM": 98, "B8G8R8A8_UNORM": 87}[format_name]
        if dxgi != expected_dxgi or array_size < 1:
            raise ValueError("DDS format/array size mismatch")
        if dimension == 4 and array_size == 1:
            kind = "volume"
        elif dimension == 3:
            kind = "cube" if flags & 4 else "array" if array_size > 1 else "2d"
        else:
            raise ValueError("Unsupported DDS resource dimension")
        start = 148
    else:
        pixel_flags, _, bits, red, green, blue, alpha = struct.unpack_from("<7I", header, 80)
        if (format_name != "B8G8R8A8_UNORM" or pixel_flags & 0x41 != 0x41 or
                (bits, red, green, blue, alpha) != (32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)):
            raise ValueError("DDS uncompressed format mismatch")
        kind = "volume" if caps & 0x200000 else "cube" if caps & 0x200 else "2d"
        if kind == "cube" and caps & 0xFC00 != 0xFC00:
            raise ValueError("DDS cube is missing faces")
    if kind != picture["kind"]:
        raise ValueError("DDS resource type mismatch")
    if array_size != (picture["frames"] if kind == "array" else 1):
        raise ValueError("DDS array frame count mismatch")
    depth = picture["depth"] if kind == "volume" else 1
    if max(1, info["depth"]) != depth:
        raise ValueError("DDS depth mismatch")
    levels = max(width, height, depth).bit_length()
    if info["mip_levels"] != levels:
        raise ValueError("DDS mip chain is incomplete")
    payload = 0
    for level in range(levels):
        w, h, z = max(1, width >> level), max(1, height >> level), max(1, depth >> level)
        payload += (((w + 3) // 4) * ((h + 3) // 4) * 16 if format_name == "BC7_UNORM" else w * h * 4) * z
    payload *= 6 if kind == "cube" else array_size
    if path.stat().st_size != start + payload:
        raise ValueError("DDS payload is truncated or has an unexpected layout")
    if format_name == "BC7_UNORM":
        # All eight BC7 modes start with a unary bit in the first byte.
        # Zero denotes the reserved mode, not a valid black block. A killed
        # GPU conversion can leave a full-sized DDS with unwritten blocks.
        with path.open("rb") as stream:
            stream.seek(start)
            if 0 in stream.read()[::16]:
                raise ValueError("DDS contains invalid/unwritten BC7 blocks")
    return info


def validate_dds_pixels(path, outputs, picture, width, height, format_name):
    """Check every top-level image against its actual input, not only headers.

    BGRA8 is lossless; BC7 allows normal block quantization. This also detects
    complete-sized, all-zero uncompressed DDS files left by interrupted tools.
    Returns maximum mean channel error for each face/frame/slice.
    """
    info=validate_dds(path,picture,width,height,format_name)
    data=path.read_bytes();start=148 if data[84:88]==b'DX10' else 128
    bc=format_name=='BC7_UNORM';depth=picture['depth'] if picture['kind']=='volume' else 1
    expected=depth if picture['kind']=='volume' else picture['frames']
    if len(outputs)!=expected:raise ValueError('DDS input frame/slice count mismatch')
    def plane_size(w,h):return ((w+3)//4)*((h+3)//4)*16 if bc else w*h*4
    chain=sum(plane_size(max(1,width>>l),max(1,height>>l))*max(1,depth>>l) for l in range(info['mip_levels']))
    errors=[]
    for i,path in enumerate(outputs):
        offset=start+i*(plane_size(width,height) if picture['kind']=='volume' else chain)
        raw=data[offset:offset+plane_size(width,height)]
        decoded=Image.frombytes('RGBA',(width,height),raw,'bcn',7) if bc else Image.frombytes('RGBA',(width,height),raw,'raw','BGRA')
        with Image.open(path) as image:
            error=ImageStat.Stat(ImageChops.difference(decoded,image.convert('RGBA'))).mean
        if max(error)>(8 if bc else 0):raise ValueError(f'DDS frame/slice {i} differs from source PNG: {error}')
        errors.append(max(error))
    return errors


def recoverable_dds(path, outputs, picture, width, height, format_name):
    """Explicit recovery for files created before per-resource checkpoints existed.

    These legacy files have no recorded input hash. Require a complete matching
    layout and a DDS timestamp at least as recent as every input PNG. Subsequent
    runs use saved input/output SHA-256 hashes instead of timestamp inference.
    """
    if not path.is_file() or path.stat().st_mtime_ns < max(p.stat().st_mtime_ns for p in outputs):
        return False
    try:
        validate_dds_pixels(path, outputs, picture, width, height, format_name)
        return True
    except (OSError, ValueError, struct.error):
        return False


def read_saved_json(path):
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}


def build_pack(root: Path, manifest, jobs, scale, failed_inputs=(), recover_dds=False):
    texconv, assemble = root / "tools/texconv.exe", root / "tools/texassemble.exe"
    if not texconv.is_file() or not assemble.is_file():
        raise RuntimeError("DDS tools are missing; run Setup.cmd")
    pack = root / f"packs/{scale}x"
    (pack / "textures").mkdir(parents=True, exist_ok=True)
    previous = read_saved_json(pack / "index.json")
    checkpoint_root = pack / ".resume"
    checkpoint_root.mkdir(exist_ok=True)
    (root / "logs").mkdir(exist_ok=True)
    result = {"schema": 1, "scale": scale, "integration_status": "ma2_pc_live_texture_quality_v1", "resources": {}}
    failures, missing = [], []
    failed_inputs = set(failed_inputs)
    resources = dict(manifest.get("resources", {}))
    # Manually added source images become ordinary 2D pack resources too.
    for relative, job in jobs.items():
        if job["resource_id"] not in resources:
            resources[job["resource_id"]] = {"id": job["resource_id"], "name": job["source_path"], "images": [relative],
                "picture": {"kind": "2d", "width": job["width"], "height": job["height"], "depth": 1, "frames": 1}, "sources": []}
    for identity, resource in resources.items():
        originals = resource["images"]
        if not all(p in jobs for p in originals):
            continue
        # A failed rerun can leave an older PNG on disk. Keep it out of this pack.
        if any(p in failed_inputs for p in originals):
            missing.append(identity)
            continue
        outputs = [root / f"enhanced/{scale}x" / Path(p).relative_to("originals").with_suffix(".png") for p in originals]
        if not all(p.is_file() for p in outputs):
            missing.append(identity)
            continue
        try:
            picture = resource["picture"]
            kind = picture["kind"]
            first_job = jobs[originals[0]]
            width, height = first_job["width"] * scale, first_job["height"] * scale
            for output in outputs:
                with Image.open(output) as image:
                    if image.size != (width, height):
                        raise ValueError(f"Output dimensions changed: {output.name}")
            special = first_job["processing"] in ("ai_alpha", "resample_data") or kind == "volume" or width % 4 or height % 4
            format_name = "B8G8R8A8_UNORM" if special else "BC7_UNORM"
            signatures = [file_hash(p) for p in outputs]
            foliage = foliage_source(root, resource)
            mip_policy = {'mip_policy': POLICY, 'authored_mip_source_sha256': file_hash(foliage)} if foliage else {}
            checkpoint = checkpoint_root / f"{identity}.json"
            old = read_saved_json(checkpoint) or previous.get("resources", {}).get(identity)
            if foliage and old and old.get('mip_policy') == POLICY and old.get('dds_format') == 'B8G8R8A8_UNORM':
                format_name = 'B8G8R8A8_UNORM'
            final = pack / "textures" / f"{identity}.dds"
            reused = bool(old and old.get("source_png_hashes") == signatures and old.get("dds_format") == format_name and final.is_file() and file_hash(final) == old.get("dds_sha256"))
            if foliage and (not old or any(old.get(key) != value for key,value in mip_policy.items())):
                reused = False
            if reused:
                try:
                    validate_dds(final, picture, width, height, format_name)
                    if old.get("content_validation") != 1:
                        validate_dds_pixels(final, outputs, picture, width, height, format_name)
                    if foliage: validate_foliage_mips(final, foliage, scale, format_name)
                except ValueError:
                    reused = False
            recovered = False
            if not old and recover_dds and not foliage:
                recovered = recoverable_dds(final, outputs, picture, width, height, format_name)
                reused = recovered
            if not reused:
                with tempfile.TemporaryDirectory(prefix="dds-", dir=root / "logs") as temporary:
                    temporary = Path(temporary)
                    input_path = outputs[0]
                    if foliage:
                        input_path = temporary / 'authored-foliage.dds'
                        build_foliage_input(input_path, outputs[0], foliage, scale)
                    if kind in ("cube", "volume", "array"):
                        if kind == "volume" and picture["frames"] != 1:
                            raise ValueError("Volume arrays need a dedicated runtime layout")
                        input_path = temporary / "assembled.dds"
                        command = [str(assemble), kind, "-nologo", "-y", "-f", "B8G8R8A8_UNORM", "-o", str(input_path), *map(str, outputs)]
                        response = subprocess.run(command, capture_output=True, text=True)
                        if response.returncode:
                            raise RuntimeError(response.stdout + response.stderr)
                    destination = temporary / "converted"
                    destination.mkdir()
                    command = [str(texconv), "-nologo", "-y", "-f", format_name, "-m", "0", "-sepalpha", "-o", str(destination), str(input_path)]
                    response = subprocess.run(command, capture_output=True, text=True)
                    if response.returncode:
                        raise RuntimeError(response.stdout + response.stderr)
                    converted = destination / (input_path.stem + ".dds")
                    try:
                        validate_dds_pixels(converted, outputs, picture, width, height, format_name)
                        if foliage: validate_foliage_mips(converted, foliage, scale, format_name)
                    except ValueError:
                        if format_name != "BC7_UNORM":
                            raise
                        # DirectXTex's CPU codec avoids incomplete DirectCompute
                        # output while retaining the same DDS format and mips.
                        response = subprocess.run(command + ["-nogpu"], capture_output=True, text=True)
                        if response.returncode:
                            raise RuntimeError(response.stdout + response.stderr)
                        try:
                            validate_dds_pixels(converted, outputs, picture, width, height, format_name)
                            if foliage: validate_foliage_mips(converted, foliage, scale, format_name)
                        except ValueError:
                            if not foliage: raise
                            # A few tiny authored alpha mips cannot be represented
                            # faithfully by BC7. Keep those resources lossless.
                            format_name = 'B8G8R8A8_UNORM'
                            command[command.index('-f')+1] = format_name
                            response = subprocess.run(command, capture_output=True, text=True)
                            if response.returncode: raise RuntimeError(response.stdout+response.stderr)
                            validate_dds_pixels(converted, outputs, picture, width, height, format_name)
                            validate_foliage_mips(converted, foliage, scale, format_name)
                    converted.replace(final)
            result["resources"][identity] = {"name": resource["name"], "sources": resource["sources"],
                "kind": kind, "original_width": picture["width"], "original_height": picture["height"],
                "original_depth": picture["depth"], "frames": picture["frames"],
                "original_rgba_match_key": resource.get("original_rgba_match_key"),
                "dds_file": str(final.relative_to(pack)).replace("\\", "/"), "dds_format": format_name,
                "dds_sha256": file_hash(final), "source_png_hashes": signatures, "content_validation": 1, **dds_info(final), **mip_policy}
            if kind == "array":
                result["resources"][identity]["original_frame_match_keys"] = frame_match_keys(root, originals, picture["width"], picture["height"])
            if recovered or reused and old and old.get("recovered_after_interruption"):
                result["resources"][identity]["recovered_after_interruption"] = True
            # Persist this DDS before starting the next conversion. The final
            # index and ZIP can be rebuilt after a crash from these small records.
            write_json(checkpoint, result["resources"][identity])
            status = "recovered" if recovered else "already completed" if reused else "ready"
            print(f"DDS {status}: {resource['name']}", flush=True)
        except Exception as error:
            failures.append({"id": identity, "error": str(error)})
            print(f"DDS FAILED: {resource['name']}: {error}", flush=True)
    result["missing_resources"] = missing
    result["failures"] = failures
    write_json(pack / "index.json", result)
    if os.environ.get("ENHANCER_NO_ZIP"):   # the installer moves the DDS folder into the game
        print(f"Pack ready: {pack} ({len(result['resources'])} resources)", flush=True)
        return failures
    # ZIP is a delivery container. The renderer loads the extracted DDS directory.
    archive_path = pack / "enhanced-textures.zip"
    temporary = archive_path.with_suffix(".zip.part")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as archive:
        archive.write(pack / "index.json", "index.json")
        for record in result["resources"].values():
            archive.write(pack / record["dds_file"], record["dds_file"])
    temporary.replace(archive_path)
    print(f"Pack ready: {archive_path} ({len(result['resources'])} resources)", flush=True)
    return failures

"""Frame extraction, temporal batch planning, crash recovery, and video export."""

from contextlib import contextmanager
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image


ROOT = Path(__file__).resolve().parent
CHECKPOINT_SHA256 = "f2d73c0fe3e33869c812f608be77cbdbfc356443b718b78c9a6cb97027ce5de7"
DECODE_VERSION = 1


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def json_digest(value) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def read_json(path: Path, default=None):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError, OSError):
        return default


def atomic_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".part")
    with temp.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def save_png(path: Path, image: Image.Image):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".part")
    with temp.open("wb") as stream:
        image.save(stream, format="PNG", compress_level=3)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def frame_path(directory: Path, index: int) -> Path:
    return directory / f"frame_{index:06d}.png"


def valid_png(path: Path, record: dict, size: tuple[int, int]) -> bool:
    try:
        if not record or digest(path) != record["sha256"]:
            return False
        with Image.open(path) as image:
            if image.size != size or image.mode != "RGB":
                return False
            image.verify()
        return True
    except (OSError, ValueError, KeyError, SyntaxError):
        return False


def subprocess_options():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def probe(ffprobe: Path, source: Path, count_frames=True) -> dict:
    command = [str(ffprobe), "-v", "error"]
    if count_frames:
        command.append("-count_frames")
    command += ["-show_streams", "-show_format", "-of", "json", str(source)]
    result = subprocess.run(command, capture_output=True, text=True, check=True,
                            **subprocess_options())
    return json.loads(result.stdout)


def source_info(ffprobe: Path, source: Path) -> dict:
    data = probe(ffprobe, source)
    videos = [s for s in data["streams"] if s["codec_type"] == "video"]
    if len(videos) != 1:
        raise ValueError("Expected exactly one video stream")
    video = videos[0]
    count = int(video.get("nb_read_frames") or video.get("nb_frames") or 0)
    if count < 1:
        raise ValueError("FFprobe could not count the source video frames")
    fps = Fraction(video["avg_frame_rate"])
    if fps <= 0:
        raise ValueError("Source frame rate is invalid")
    return {"sha256": digest(source), "path": str(source), "width": video["width"],
            "height": video["height"], "frames": count, "fps": str(fps),
            "duration": float(Fraction(count, 1) / fps), "codec": video["codec_name"],
            "audio": [s for s in data["streams"] if s["codec_type"] == "audio"]}


@contextmanager
def run_lock(path: Path):
    """OS-held lock: a crashed process releases it even if the file remains."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        if path.stat().st_size == 0:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError("Another FMV runner is active. Close it before starting another.") from exc
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def read_exact(stream, length):
    blocks, remaining = [], length
    while remaining:
        block = stream.read(remaining)
        if not block:
            break
        blocks.append(block)
        remaining -= len(block)
    return b"".join(blocks)


def extract_frames(ffmpeg: Path, source: Path, info: dict, directory: Path, log: Path) -> dict:
    """Decode only the original video. No model is involved in this stage."""
    directory.mkdir(parents=True, exist_ok=True)
    metadata_path = directory / "frames.json"
    key = {"source_sha256": info["sha256"], "decode_version": DECODE_VERSION,
           "width": info["width"], "height": info["height"], "fps": info["fps"],
           "frames": info["frames"], "matrix": "bt601", "range": "tv-to-full"}
    cached = read_json(metadata_path, {})
    if cached.get("key") != key:
        cached = {"key": key, "complete": False, "images": []}
    old_records = {entry["index"]: entry for entry in cached["images"]}
    size = (info["width"], info["height"])
    valid = {i for i, rec in old_records.items() if 0 <= i < info["frames"]
             and valid_png(frame_path(directory, i), rec, size)}
    if cached.get("complete") and len(valid) == info["frames"]:
        print(f"Original frames: verified {len(valid):,} cached images.", flush=True)
        return cached
    print(f"Decoding {info['frames']:,} original frames ({len(valid):,} can be reused).", flush=True)
    command = [str(ffmpeg), "-hide_banner", "-loglevel", "warning", "-nostdin",
               "-i", str(source), "-map", "0:v:0", "-an", "-sn", "-dn",
               "-vf", "scale=iw:ih:in_range=tv:out_range=full:in_color_matrix=bt601,format=rgb24",
               "-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"]
    log.parent.mkdir(parents=True, exist_ok=True)
    records = []
    previous_thumb, previous_mad = None, 0.0
    with log.open("a", encoding="utf-8") as errors:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors,
                                   **subprocess_options())
        try:
            for index in range(info["frames"]):
                raw = read_exact(process.stdout, info["width"] * info["height"] * 3)
                if len(raw) != info["width"] * info["height"] * 3:
                    raise RuntimeError(f"Decoder stopped at frame {index}; see {log}")
                image = Image.frombytes("RGB", size, raw)
                thumb = np.asarray(image.resize((96, 64), Image.Resampling.BOX),
                                   dtype=np.float32) / 255.0
                mad = float(np.abs(thumb - previous_thumb).mean()) if previous_thumb is not None else 0.0
                # Similar to FFmpeg's scene metric: a jump in adjacent-frame difference.
                score = min(mad, abs(mad - previous_mad))
                previous_thumb, previous_mad = thumb, mad
                raw_sha = hashlib.sha256(raw).hexdigest()
                old = old_records.get(index, {})
                target = frame_path(directory, index)
                if index not in valid or old.get("rgb_sha256") != raw_sha:
                    save_png(target, image)
                records.append({"index": index, "sha256": digest(target),
                                "rgb_sha256": raw_sha, "scene_score": score})
                if (index + 1) % 32 == 0:
                    atomic_json(metadata_path, {"key": key, "complete": False, "images": records})
                if (index + 1) % 128 == 0 or index + 1 == info["frames"]:
                    print(f"  Original frames {index + 1:,}/{info['frames']:,}", flush=True)
            if process.stdout.read(1):
                raise RuntimeError("Decoder returned more frames than FFprobe counted")
            if process.wait() != 0:
                raise RuntimeError(f"FFmpeg decoding failed; see {log}")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait()
            process.stdout.close()
    cached = {"key": key, "complete": True, "images": records}
    atomic_json(metadata_path, cached)
    return cached


def plan_chunks(total: int, start: int, count: int, core: int, context: int,
                cuts: list[int]) -> list[dict]:
    if total < 1 or start < 0 or count < 1 or start + count > total or core < 1 or context < 0:
        raise ValueError("Invalid frame range or batch size")
    boundaries = sorted({0, total, *(int(c) for c in cuts if 0 < int(c) < total)})
    chunks = []
    for scene_start, scene_end in zip(boundaries, boundaries[1:]):
        left, right = max(start, scene_start), min(start + count, scene_end)
        while left < right:
            end = min(left + core, right)
            chunks.append({"start": left, "end": end,
                           "input_start": max(scene_start, left - context),
                           "input_end": min(scene_end, end + context)})
            left = end
    return chunks


def chunk_record_path(directory: Path, chunk: dict) -> Path:
    return directory / ".resume" / f"{chunk['start']:06d}-{chunk['end']:06d}.json"


def chunk_is_complete(directory: Path, chunk: dict, signature: str, size: tuple[int, int]) -> bool:
    record = read_json(chunk_record_path(directory, chunk), {})
    if record.get("signature") != signature or record.get("chunk") != chunk:
        return False
    images = record.get("images", [])
    if [r.get("index") for r in images] != list(range(chunk["start"], chunk["end"])):
        return False
    return all(valid_png(frame_path(directory, r["index"]), r, size) for r in images)


def chunk_key(chunk: dict) -> tuple[int, int, int, int]:
    return tuple(chunk[k] for k in ("start", "end", "input_start", "input_end"))


def reusable_batches(root: Path, settings: dict, chunks: list[dict], target: Path,
                     size: tuple[int, int]) -> list[dict]:
    """Find committed frames from jobs with identical inference and batch settings."""
    range_keys = {"start_frame", "frames"}
    target_processing = {k: v for k, v in settings.items() if k not in range_keys}
    wanted = {chunk_key(chunk) for chunk in chunks}
    sources = []
    folder = root / "enhanced" / f"{settings['scale']}x"
    for manifest_path in folder.glob("*/job.json"):
        directory = manifest_path.parent
        if directory.resolve() == target.resolve():
            continue
        manifest = read_json(manifest_path, {})
        previous_settings = manifest.get("settings", {})
        if ({k: v for k, v in previous_settings.items() if k not in range_keys}
                != target_processing):
            continue
        previous_signature = manifest.get("signature")
        if not previous_signature:
            continue
        available = {}
        for chunk in manifest.get("chunks", []):
            key = chunk_key(chunk)
            if key in wanted and chunk_is_complete(directory, chunk, previous_signature, size):
                available[key] = chunk
        if available:
            sources.append({"directory": directory, "signature": previous_signature,
                            "chunks": available})
    return sources


def copy_verified_png(source: Path, target: Path, record: dict, size: tuple[int, int]):
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_name(target.name + ".part")
    shutil.copyfile(source, temp)
    with temp.open("r+b") as stream:
        stream.flush()
        os.fsync(stream.fileno())
    if not valid_png(temp, record, size):
        raise RuntimeError(f"A cached frame changed while resuming: {source}")
    os.replace(temp, target)


def enhance_chunks(originals: Path, enhanced: Path, chunks: list[dict], signature: str,
                   size: tuple[int, int], engine_factory, encode_only=False,
                   reusable_jobs: list[dict] | None = None) -> dict:
    enhanced.mkdir(parents=True, exist_ok=True)
    completed = [chunk_is_complete(enhanced, c, signature, size) for c in chunks]
    reused = sum(c["end"] - c["start"] for c, done in zip(chunks, completed) if done)
    total = sum(c["end"] - c["start"] for c in chunks)
    reusable_jobs = reusable_jobs or []
    available = sum(chunk["end"] - chunk["start"] for chunk, done in zip(chunks, completed)
                    if not done and any(chunk_key(chunk) in job["chunks"] for job in reusable_jobs))
    print(f"Enhanced frames: {reused:,}/{total:,} already verified; "
          f"{available:,} additional matching frames are available.", flush=True)
    inherited = 0
    for chunk, done in zip(chunks, completed):
        if done:
            continue
        key = chunk_key(chunk)
        for job in reusable_jobs:
            if key not in job["chunks"]:
                continue
            prior = job["chunks"][key]
            record = read_json(chunk_record_path(job["directory"], prior), {})
            records = record.get("images", [])
            if [item.get("index") for item in records] != list(range(chunk["start"], chunk["end"])):
                continue
            for item in records:
                copy_verified_png(frame_path(job["directory"], item["index"]),
                                  frame_path(enhanced, item["index"]), item, size)
            atomic_json(chunk_record_path(enhanced, chunk),
                        {"signature": signature, "chunk": chunk, "images": records})
            inherited += len(records)
            break
    if inherited:
        print(f"Reused {inherited:,} verified frames from earlier jobs.", flush=True)
    completed = [chunk_is_complete(enhanced, c, signature, size) for c in chunks]
    total_cached = sum(c["end"] - c["start"] for c, done in zip(chunks, completed) if done)
    if encode_only and not all(completed):
        raise RuntimeError("Enhanced frames are missing or damaged. Rerun without --encode-only to resume.")
    engine, processed, started = None, 0, time.monotonic()
    for chunk, done in zip(chunks, completed):
        if done:
            continue
        if engine is None:
            engine = engine_factory()
        paths = [frame_path(originals, i) for i in range(chunk["input_start"], chunk["input_end"])]
        offset = chunk["start"] - chunk["input_start"]
        images = engine.enhance(paths, offset, offset + chunk["end"] - chunk["start"])
        if len(images) != chunk["end"] - chunk["start"]:
            raise RuntimeError("The model returned an incorrect frame count")
        records = []
        for index, image in zip(range(chunk["start"], chunk["end"]), images):
            if image.mode != "RGB" or image.size != size:
                raise RuntimeError("The model returned an incorrect image format or size")
            target = frame_path(enhanced, index)
            save_png(target, image)
            records.append({"index": index, "sha256": digest(target)})
        # The commit follows all durable image writes. An interrupted batch is regenerated.
        atomic_json(chunk_record_path(enhanced, chunk),
                    {"signature": signature, "chunk": chunk, "images": records})
        processed += len(records)
        seconds = time.monotonic() - started
        remaining = (total - total_cached - processed) * seconds / processed
        print(f"  Enhanced {total_cached + processed:,}/{total:,}; batch {chunk['start']}..{chunk['end'] - 1}; "
              f"approx. {remaining / 60:.1f} min remaining", flush=True)
    return {"reused_frames": reused, "new_frames": processed}


def seconds(frames: int, fps: str) -> str:
    return f"{float(Fraction(frames, 1) / Fraction(fps)):.12f}"


def audio_filter(audio: list[dict], start: int, count: int, fps: str):
    """A generic stereo preview plus each original decoded track separately."""
    filters, mappings = [], []
    for i in range(len(audio)):
        filters.append(
            f"[1:a:{i}]atrim=start={seconds(start, fps)}:end={seconds(start + count, fps)},"
            f"asetpts=PTS-STARTPTS,asplit=2[original{i}][to_mix{i}];"
            f"[to_mix{i}]aformat=sample_fmts=flt:channel_layouts=stereo[mix{i}]"
        )
    if audio:
        filters.append("".join(f"[mix{i}]" for i in range(len(audio))) +
                       f"amix=inputs={len(audio)}:duration=longest:dropout_transition=0:normalize=1[preview]")
        mappings = ["-map", "[preview]"]
        for i in range(len(audio)):
            mappings += ["-map", f"[original{i}]"]
    return ";".join(filters), mappings


def export_command(ffmpeg: Path, source: Path, frames: Path, output: Path, info: dict,
                   start: int, count: int, sar: str, crf: int, preset: str, scale=2,
                   title: str | None = None):
    command = [str(ffmpeg), "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
               "-framerate", info["fps"], "-start_number", str(start),
               "-i", str(frames / "frame_%06d.png"), "-i", str(source)]
    filters, audio_maps = audio_filter(info["audio"], start, count, info["fps"])
    if filters:
        command += ["-filter_complex", filters]
    command += ["-map", "0:v:0", *audio_maps,
                "-vf", f"trim=end_frame={count},setpts=PTS-STARTPTS,"
                f"scale=iw:ih:in_range=full:out_range=tv:out_color_matrix=bt601,setsar={sar}",
                # A video frame limit can prematurely end audio encoding in FFmpeg.
                # Trim the video in the filter and let each audio track finish its trim.
                "-t", seconds(count, info["fps"]),
                "-c:v", "libx264", "-preset", preset, "-crf", str(crf),
                "-pix_fmt", "yuv420p", "-fps_mode", "passthrough",
                "-color_range", "tv", "-colorspace", "smpte170m",
                "-color_primaries", "smpte170m", "-color_trc", "smpte170m"]
    if info["audio"]:
        # PCM float32 preserves the Bink decoder's float samples in the original tracks.
        command += ["-c:a", "pcm_f32le", "-metadata:s:a:0", "title=Stereo preview mix",
                    "-disposition:a:0", "default"]
        for i, track in enumerate(info["audio"], 1):
            command += [f"-metadata:s:a:{i}", f"title=Source track {i - 1} (Bink id {track.get('id', '?')})",
                        f"-disposition:a:{i}", "0"]
    video_title = title if title is not None else f"SPIntro - RealBasicVSR {scale}x"
    command += ["-map_metadata", "-1", "-metadata", f"title={video_title}",
                "-progress", "pipe:1", "-nostats", "-f", "matroska", str(output)]
    return command


def run_export(command: list[str], log: Path, count: int):
    log.parent.mkdir(parents=True, exist_ok=True)
    print("Encoding video and preserving audio...", flush=True)
    with log.open("a", encoding="utf-8") as errors:
        errors.write("\n" + subprocess.list2cmdline(command) + "\n")
        errors.flush()
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors, text=True,
                                   **subprocess_options())
        try:
            last = 0.0
            for line in process.stdout:
                if line.startswith("frame=") and time.monotonic() - last >= 5:
                    print(f"  Encoded {int(line.split('=', 1)[1]):,}/{count:,} frames", flush=True)
                    last = time.monotonic()
            if process.wait() != 0:
                errors.flush()
                detail = log.read_text(encoding="utf-8", errors="replace")[-1600:]
                raise RuntimeError(f"FFmpeg encoding failed; see {log}\n{detail}")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait()
            process.stdout.close()


def validate_export(ffprobe: Path, path: Path, size: tuple[int, int], count: int,
                    fps: str, audio_count: int, sar: str):
    data = probe(ffprobe, path)
    videos = [s for s in data["streams"] if s["codec_type"] == "video"]
    audios = [s for s in data["streams"] if s["codec_type"] == "audio"]
    if len(videos) != 1:
        raise RuntimeError("Export does not contain exactly one video stream")
    video = videos[0]
    if (video["width"], video["height"]) != size or int(video.get("nb_read_frames", 0)) != count:
        raise RuntimeError("Export dimensions or frame count do not match")
    if Fraction(video["avg_frame_rate"]) != Fraction(fps):
        raise RuntimeError("Export frame rate does not match the source")
    if Fraction(video.get("sample_aspect_ratio", "1:1").replace(":", "/")) != Fraction(sar):
        raise RuntimeError("Export display aspect ratio does not match")
    if len(audios) != audio_count:
        raise RuntimeError("One or more audio tracks are missing from the export")
    return {"size": list(size), "frames": count, "fps": fps, "sar": sar,
            "audio_tracks": len(audios), "sha256": digest(path)}


def export_video(ffmpeg: Path, ffprobe: Path, source: Path, frames: Path, output: Path,
                 info: dict, start: int, count: int, sar: str, crf: int, preset: str,
                 signature: str, log: Path, scale=2, title: str | None = None,
                 record_path: Path | None = None) -> Path:
    key = {"signature": signature, "crf": crf, "preset": preset, "sar": sar, "export_version": 1}
    if title is not None:
        key["title"] = title
    record_path = record_path or output.with_suffix(".json")
    previous = read_json(record_path, {})
    if output.is_file() and previous.get("key") == key and previous.get("sha256") == digest(output):
        print(f"Verified completed video: {output}", flush=True)
        return output
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.stem + ".part.mkv")
    command = export_command(ffmpeg, source, frames, temp, info, start, count,
                             sar, crf, preset, scale, title)
    run_export(command, log, count)
    metadata = validate_export(ffprobe, temp,
                               (round(info["width"] * scale), round(info["height"] * scale)),
                               count, info["fps"], len(info["audio"]) + bool(info["audio"]), sar)
    os.replace(temp, output)
    atomic_json(record_path, {"key": key, **metadata})
    print(f"Saved: {output}", flush=True)
    return output


def export_comparison(ffmpeg: Path, ffprobe: Path, originals: Path, enhanced_video: Path, output: Path,
                      info: dict, start: int, count: int, sar: str, log: Path, scale=2):
    w, h = round(info["width"] * scale), round(info["height"] * scale)
    temp = output.with_name(output.stem + ".part.mp4")
    key = {"enhanced_sha256": digest(enhanced_video), "comparison_version": 1}
    record_path = output.with_suffix(".json")
    previous = read_json(record_path, {})
    if output.is_file() and previous.get("key") == key and previous.get("sha256") == digest(output):
        print(f"Verified comparison: {output}", flush=True)
        return output
    time_base = str(1 / Fraction(info["fps"]))
    # Matroska timestamps are millisecond-granular. Rebuild both timelines from
    # the frame index before framesync; rounding must not introduce extra events.
    filters = (f"[0:v]scale={w}:{h}:flags=lanczos:in_range=full:out_range=tv:out_color_matrix=bt601,"
               f"setsar={sar},settb=expr={time_base},setpts=N[classic];"
               f"[1:v]setsar={sar},settb=expr={time_base},setpts=N[enhanced];"
               f"[classic][enhanced]hstack=inputs=2:shortest=1,trim=end_frame={count}[v]")
    command = [str(ffmpeg), "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
               "-framerate", info["fps"], "-start_number", str(start),
               "-i", str(originals / "frame_%06d.png"), "-i", str(enhanced_video),
               "-filter_complex", filters, "-map", "[v]", "-map", "1:a:0?",
               "-t", seconds(count, info["fps"]),
               "-c:v", "libx264", "-crf", "18", "-preset", "medium", "-pix_fmt", "yuv420p",
               "-r", info["fps"], "-fps_mode", "cfr", "-enc_time_base", time_base,
               "-video_track_timescale", str(Fraction(info["fps"]).numerator),
               "-c:a", "aac", "-b:a", "192k",
               "-colorspace", "smpte170m", "-color_primaries", "smpte170m",
               "-color_trc", "smpte170m", "-color_range", "tv", "-movflags", "+faststart",
               "-progress", "pipe:1", "-nostats", str(temp)]
    run_export(command, log, count)
    metadata = validate_export(ffprobe, temp, (w * 2, h), count, info["fps"],
                               int(bool(info["audio"])), sar)
    os.replace(temp, output)
    atomic_json(record_path, {"key": key, **metadata})
    print(f"Saved comparison (classic left, enhanced right): {output}", flush=True)


def export_resolution_comparison(ffmpeg: Path, ffprobe: Path, lower_frames: Path,
                                 higher_frames: Path, audio_video: Path, output: Path,
                                 info: dict, start: int, count: int, sar: str,
                                 lower_signature: str, higher_signature: str, log: Path,
                                 low_scale: float, high_scale: float):
    """Compare two enhanced image sequences at a common panel size."""
    panel_w = round(info["width"] * high_scale)
    panel_h = round(info["height"] * high_scale)
    temp = output.with_name(output.stem + ".part.mp4")
    key = {"lower_signature": lower_signature, "higher_signature": higher_signature,
           "audio_video_sha256": digest(audio_video), "low_scale": low_scale,
           "high_scale": high_scale, "comparison_version": 1}
    record_path = output.with_suffix(".json")
    previous = read_json(record_path, {})
    if output.is_file() and previous.get("key") == key and previous.get("sha256") == digest(output):
        print(f"Verified resolution comparison: {output}", flush=True)
        return output

    time_base = str(1 / Fraction(info["fps"]))
    # Both image streams use the source frame rate and are reset to exact frame
    # timestamps before hstack, avoiding drift from the encoded reference MKV.
    filters = (
        f"[0:v]scale={panel_w}:{panel_h}:flags=lanczos:in_range=full:out_range=tv:out_color_matrix=bt601,"
        f"setsar={sar},settb=expr={time_base},setpts=N,format=yuv420p[lower];"
        f"[1:v]scale={panel_w}:{panel_h}:flags=lanczos:in_range=full:out_range=tv:out_color_matrix=bt601,"
        f"setsar={sar},settb=expr={time_base},setpts=N,format=yuv420p[higher];"
        f"[lower][higher]hstack=inputs=2:shortest=1,trim=end_frame={count}[v]"
    )
    command = [str(ffmpeg), "-hide_banner", "-loglevel", "warning", "-nostdin", "-y",
               "-framerate", info["fps"], "-start_number", str(start),
               "-i", str(lower_frames / "frame_%06d.png"),
               "-framerate", info["fps"], "-start_number", str(start),
               "-i", str(higher_frames / "frame_%06d.png"),
               "-i", str(audio_video), "-filter_complex", filters,
               "-map", "[v]", "-map", "2:a:0?", "-t", seconds(count, info["fps"]),
               "-c:v", "libx264", "-crf", "18", "-preset", "medium", "-pix_fmt", "yuv420p",
               "-r", info["fps"], "-fps_mode", "cfr", "-enc_time_base", time_base,
               "-video_track_timescale", str(Fraction(info["fps"]).numerator),
               "-c:a", "aac", "-b:a", "192k",
               "-colorspace", "smpte170m", "-color_primaries", "smpte170m",
               "-color_trc", "smpte170m", "-color_range", "tv", "-movflags", "+faststart",
               "-map_metadata", "-1", "-metadata",
               f"title=SPIntro - Enhanced {low_scale}x vs {high_scale}x",
               "-progress", "pipe:1", "-nostats", str(temp)]
    run_export(command, log, count)
    metadata = validate_export(ffprobe, temp, (panel_w * 2, panel_h), count, info["fps"],
                               int(bool(info["audio"])), sar)
    os.replace(temp, output)
    atomic_json(record_path, {"key": key, **metadata})
    print(f"Saved {low_scale}x-left / {high_scale}x-right comparison: {output}", flush=True)
    return output

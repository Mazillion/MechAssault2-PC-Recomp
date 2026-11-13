"""Prepare and enhance a Bink movie. Run from an FMV command script."""

import argparse
from contextlib import nullcontext
from fractions import Fraction
import json
from pathlib import Path
import shutil
import sys
import traceback

from fmv_pipeline import (
    ROOT, CHECKPOINT_SHA256, atomic_json, chunk_is_complete, digest, enhance_chunks,
    export_comparison, export_resolution_comparison, export_video, extract_frames,
    json_digest, plan_chunks, read_json, reusable_batches, run_lock, source_info,
    validate_export,
)


def resolve(value: str) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (ROOT / path).resolve()


def parser():
    result = argparse.ArgumentParser(description="Resumable RealBasicVSR Bink-movie enhancement")
    result.add_argument("--config", default=str(ROOT / "config.json"))
    result.add_argument("--source", help="Override the configured source movie")
    result.add_argument("--output", help="Override the main MKV output path")
    result.add_argument("--title", help="Override the main MKV title metadata")
    result.add_argument("--metadata", help="Override the main MKV metadata sidecar path")
    result.add_argument("--lock-held", action="store_true", help=argparse.SUPPRESS)
    duration = result.add_mutually_exclusive_group()
    duration.add_argument("--frames", type=int, help="Enhance only this many frames, for a short test")
    duration.add_argument("--seconds", type=float, help="Clip duration, rounded to the nearest whole source frame")
    result.add_argument("--start-frame", type=int, default=0, help="Zero-based source frame at which to start")
    result.add_argument("--scale", type=float, choices=[2, 2.5, 3, 4],
                        help="Final resolution multiplier; default 2")
    result.add_argument("--strength", type=float, help="AI blend strength: 0..1; default 0.5")
    result.add_argument("--chunk-size", type=int, help="Output frames per temporal batch; default 8")
    result.add_argument("--context", type=int, help="Extra frames before/after each batch; default 4")
    result.add_argument("--device", choices=["cuda", "cpu"], help="CUDA by default; CPU is much slower")
    result.add_argument("--comparison", action="store_true", help="Also encode a classic-left/enhanced-right MP4")
    result.add_argument("--compare-scale", type=float, choices=[2, 2.5, 3, 4],
                        help="Also compare against a completed enhancement at a lower scale")
    result.add_argument("--encode-only", action="store_true", help="Verify and encode existing enhanced frames without AI")
    result.add_argument("--check", action="store_true", help="Validate setup only; do not extract or enhance")
    return result


def requested_frames(total, start, fps, frames=None, seconds=None):
    if seconds is not None:
        try:
            count = round(Fraction(str(seconds)) * Fraction(fps))
        except (ValueError, OverflowError) as exc:
            raise ValueError("Seconds must be a finite positive duration") from exc
    else:
        count = frames if frames is not None else total - start
    if start < 0 or count < 1 or start + count > total:
        raise ValueError(f"Frame range must fit within the {total:,}-frame intro")
    return count


def inference_code_hash(root: Path, scale: float):
    current = json_digest({name: digest(root / name) for name in
                           ("realbasicvsr_model.py", "video_engine.py", "fmv_pipeline.py", "enhance_intro.py")})
    # The 2x and 3x frame math is unchanged by multi-movie support.
    # Exact fingerprints permit verified earlier jobs at those scales to resume.
    # Any subsequent inference-code edit automatically invalidates this mapping.
    compatibility = read_json(root / "resume_compatibility.json", {})
    if current == compatibility.get("updated_code_sha256"):
        key = {2: "previous_2x_code_sha256", 3: "previous_3x_code_sha256"}.get(scale)
        if key and compatibility.get(key):
            return compatibility[key]
    return current


def normalize_scale(scale):
    scale = float(scale)
    return int(scale) if scale.is_integer() else scale


def main(argv=None):
    args = parser().parse_args(argv)
    config = json.loads(Path(args.config).read_text(encoding="utf-8"))
    for name in ("scale", "strength", "chunk_size", "context", "device"):
        value = getattr(args, name)
        if value is not None:
            config[name] = normalize_scale(value) if name == "scale" else value
    scale = normalize_scale(config["scale"])
    compare_scale = normalize_scale(args.compare_scale) if args.compare_scale is not None else None
    if scale not in (2, 2.5, 3, 4):
        raise ValueError("Final scale must be 2, 2.5, 3, or 4; the checkpoint itself is native 4x.")
    if compare_scale is not None and compare_scale >= scale:
        raise ValueError("The comparison scale must be lower than the enhanced video scale")
    if compare_scale is not None and args.comparison:
        raise ValueError("Choose either --comparison or --compare-scale, not both")
    if not 0 <= config["strength"] <= 1 or config["chunk_size"] < 1 or config["context"] < 0:
        raise ValueError("Strength must be 0..1, chunk size >=1, and context >=0")
    if not 0 < config["scene_cut_threshold"] <= 1:
        raise ValueError("Scene threshold must be greater than 0 and no greater than 1")
    if args.check:
        from setup_fmv import validate_setup
        validate_setup(config, download=False)
        return 0
    source = resolve(args.source) if args.source else resolve(config["source"])
    ffmpeg, ffprobe, checkpoint = (resolve(config[k]) for k in
                                  ("ffmpeg", "ffprobe", "checkpoint"))
    for path in (source, ffmpeg, ffprobe, checkpoint):
        if not path.is_file():
            raise FileNotFoundError(f"Missing {path}. Run FMV/Setup.cmd first.")
    if digest(checkpoint) != CHECKPOINT_SHA256:
        raise ValueError("Checkpoint checksum does not match the official downloaded weights. Run Setup.cmd.")
    lock = nullcontext() if args.lock_held else run_lock(ROOT / ".run.lock")
    with lock:
        info = source_info(ffprobe, source)
        if info["codec"] != "binkvideo":
            raise ValueError("This workflow expects an original Bink video")
        count = requested_frames(info["frames"], args.start_frame, info["fps"], args.frames, args.seconds)
        source_key = f"{source.stem}-{info['sha256'][:12]}"
        originals = ROOT / "originals" / source_key
        print(f"Source: {source}", flush=True)
        print(f"Output: {info['width'] * scale}x{info['height'] * scale}, {float(Fraction(info['fps'])):.5g} fps, "
              f"frames {args.start_frame}..{args.start_frame + count - 1}, "
              f"AI strength {config['strength']:.0%}", flush=True)
        print(f"Clip duration: {float(Fraction(count, 1) / Fraction(info['fps'])):.3f} seconds", flush=True)
        print("Completed batches are saved. Rerun the same command to resume.", flush=True)
        # Raw RGB is uncompressed in transit; on-disk originals/results are lossless PNG.
        # This rough allowance covers all originals and the selected enhanced results.
        allowance = info["frames"] * info["width"] * info["height"] * 3
        allowance += count * info["width"] * info["height"] * 3 * scale ** 2 + 1024 ** 3
        if shutil.disk_usage(ROOT).free < allowance and not args.encode_only:
            raise RuntimeError(f"Allow about {allowance / 1024 ** 3:.1f} GB free for frames and exports.")
        raw = extract_frames(ffmpeg, source, info, originals, ROOT / "logs" / "decode.log")
        cuts = sorted({r["index"] for r in raw["images"]
                       if r["index"] and r["scene_score"] >= config["scene_cut_threshold"]}
                      | set(config["extra_scene_cuts"]))
        chunks = plan_chunks(info["frames"], args.start_frame, count, config["chunk_size"],
                             config["context"], cuts)
        code_hash = inference_code_hash(ROOT, scale)
        settings = {"source_sha256": info["sha256"], "model_sha256": CHECKPOINT_SHA256,
                    "code_sha256": code_hash, "scale": scale, "strength": config["strength"],
                    "chunk_size": config["chunk_size"], "context": config["context"],
                    "device": config["device"], "precision": "float32", "cuts": cuts,
                    "start_frame": args.start_frame, "frames": count, "fps": info["fps"]}
        signature = json_digest(settings)
        job_name = f"{source.stem}-{scale}x-s{round(config['strength'] * 100):03d}-{signature[:12]}"
        enhanced = ROOT / "enhanced" / f"{scale}x" / job_name
        expected_size = (round(info["width"] * scale), round(info["height"] * scale))
        reference = None
        if compare_scale is not None:
            reference_settings = {**settings, "scale": compare_scale,
                                  "code_sha256": inference_code_hash(ROOT, compare_scale)}
            reference_signature = json_digest(reference_settings)
            reference_name = (f"{source.stem}-{compare_scale}x-s"
                              f"{round(config['strength'] * 100):03d}-{reference_signature[:12]}")
            reference_frames = ROOT / "enhanced" / f"{compare_scale}x" / reference_name
            reference_manifest = read_json(reference_frames / "job.json", {})
            if (reference_manifest.get("signature") != reference_signature
                    or reference_manifest.get("settings") != reference_settings
                    or reference_manifest.get("chunks") != chunks):
                raise RuntimeError(
                    f"A matching {compare_scale}x reference job is required before starting this comparison. "
                    "Run the reference enhancement for the same frames, blend, and batch settings first."
                )
            reference_size = (round(info["width"] * compare_scale),
                              round(info["height"] * compare_scale))
            incomplete = [chunk for chunk in reference_manifest.get("chunks", [])
                          if not chunk_is_complete(reference_frames, chunk,
                                                  reference_signature, reference_size)]
            if incomplete:
                raise RuntimeError(
                    f"The {compare_scale}x reference is missing or has damaged frames "
                    f"in batch {incomplete[0]['start']}..{incomplete[0]['end'] - 1}. "
                    "Complete that reference job before starting the comparison."
                )
            reference_video = ROOT / "output" / (reference_name + ".mkv")
            reference_record = read_json(reference_video.with_suffix(".json"), {})
            if (not reference_video.is_file()
                    or reference_record.get("key", {}).get("signature") != reference_signature
                    or reference_record.get("sha256") != digest(reference_video)):
                raise RuntimeError(
                    f"The matching {compare_scale}x reference video with preview audio is missing or stale. "
                    "Rerun its enhancement command before starting this comparison."
                )
            reference_sar = str(Fraction(config["display_aspect_ratio"])
                                * info["height"] / info["width"])
            validate_export(ffprobe, reference_video, reference_size, count, info["fps"],
                            len(info["audio"]) + bool(info["audio"]), reference_sar)
            reference = {"name": reference_name, "frames": reference_frames,
                         "video": reference_video, "signature": reference_signature}
            print(f"Comparison reference verified: {compare_scale}x at the same AI strength.", flush=True)
        prior_jobs = reusable_batches(ROOT, settings, chunks, enhanced, expected_size)
        if prior_jobs:
            prior_frame_count = sum(chunk["end"] - chunk["start"] for job in prior_jobs
                                    for chunk in job["chunks"].values())
            print(f"Found {prior_frame_count:,} verified frames in compatible earlier jobs.", flush=True)
        enhanced.mkdir(parents=True, exist_ok=True)
        atomic_json(enhanced / "job.json", {"signature": signature, "settings": settings,
                                           "source": info, "chunks": chunks})
        print(f"Temporal batches: {len(chunks):,}; detected scene boundaries: {len(cuts)}", flush=True)

        def engine_factory():
            from video_engine import VideoEnhancer
            print("Loading RealBasicVSR EMA weights in FP32...", flush=True)
            return VideoEnhancer(checkpoint, config["strength"], config["device"], scale=scale)

        enhance_chunks(originals, enhanced, chunks, signature, expected_size,
                       engine_factory, args.encode_only, prior_jobs)
        sar = str(Fraction(config["display_aspect_ratio"]) * info["height"] / info["width"])
        video = resolve(args.output) if args.output else ROOT / "output" / (job_name + ".mkv")
        metadata = resolve(args.metadata) if args.metadata else None
        export_video(ffmpeg, ffprobe, source, enhanced, video, info, args.start_frame, count,
                     sar, config["crf"], config["preset"], signature, ROOT / "logs" / "encode.log",
                     scale=scale, title=args.title, record_path=metadata)
        if compare_scale is not None:
            export_resolution_comparison(
                ffmpeg, ffprobe, reference["frames"], enhanced, reference["video"],
                ROOT / "output" / f"{source.stem}-{compare_scale}x-vs-{scale}x-"
                f"s{round(config['strength'] * 100):03d}-{signature[:12]}-comparison.mp4",
                info, args.start_frame, count, sar, reference["signature"], signature,
                ROOT / "logs" / "scale-comparison.log", low_scale=compare_scale,
                high_scale=scale,
            )
        elif args.comparison:
            export_comparison(ffmpeg, ffprobe, originals, video, ROOT / "output" / (job_name + "-comparison.mp4"),
                              info, args.start_frame, count, sar, ROOT / "logs" / "comparison.log", scale=scale)
        print("Finished. Original game files were not modified.", flush=True)
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nStopped. Rerun the same command to resume completed batches.", file=sys.stderr)
        sys.exit(130)
    except Exception as exc:
        (ROOT / "logs").mkdir(parents=True, exist_ok=True)
        (ROOT / "logs" / "last_error.txt").write_text(traceback.format_exc(), encoding="utf-8")
        print(f"\nError: {exc}\nSee logs/last_error.txt. Completed batches remain available.", file=sys.stderr)
        if "out of memory" in str(exc).lower():
            print("Try --chunk-size 4 --context 2. This creates a separate job with a smaller memory requirement.",
                  file=sys.stderr)
        sys.exit(1)

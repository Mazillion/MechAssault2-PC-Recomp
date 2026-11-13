"""Sequential, resumable upscale runner. Default: native 2x, DDS pack and preview."""
from __future__ import annotations
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys
import time

from PIL import Image
from ma2_assets import effect_sprite, file_hash, save_png, write_json

ROOT = Path(__file__).resolve().parent
PIPELINE_VERSION = 1


@contextmanager
def run_lock(path):
    # Windows locks are released by the OS even when the process is terminated.
    import msvcrt
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        if path.stat().st_size == 0:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise RuntimeError("Another run at this scale is active") from error
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def collect_jobs(manifest):
    jobs = {}
    for path in sorted((ROOT / "originals").rglob("*")):
        if not path.is_file() or path.suffix.casefold() not in (".png", ".jpg", ".jpeg", ".tif", ".tiff", ".bmp", ".tga"):
            continue
        relative = path.relative_to(ROOT).as_posix()
        existing = manifest.get("jobs", {}).get(relative)
        with Image.open(path) as image:
            width, height = image.size
        if existing:
            job = dict(existing)
        else:
            identity = file_hash(path)
            job = {"resource_id": identity, "source_path": relative, "format": path.suffix[1:].upper(),
                   "category": "Added image", "processing": "ai_rgb", "edge_mode": "reflect"}
        job.update(width=width, height=height, png_sha256=file_hash(path))
        if effect_sprite(job["source_path"]) and job["processing"].startswith("ai_"):
            job["processing"] = "resample_data"
        jobs[relative] = job
    return jobs


def job_signature(job, config, model_hash):
    value = {"pipeline": PIPELINE_VERSION, "input": job["png_sha256"], "processing": job["processing"],
             "edge_mode": job["edge_mode"], "config": config, "model": model_hash}
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def valid_completed(output, previous, signature, expected_size):
    if not previous or previous.get("status") != "completed" or previous.get("signature") != signature or not output.is_file():
        return False
    try:
        with Image.open(output) as image:
            if image.size != expected_size or image.mode != "RGBA":
                return False
            image.verify()
        return file_hash(output) == previous.get("output_sha256")
    except (OSError, ValueError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scale", type=int, choices=(2, 4), help="Separate native model and output directory for each scale")
    parser.add_argument("--force", action="store_true", help="Reprocess completed images")
    parser.add_argument("--limit", type=int, help="Process only this many images (useful for a first test)")
    parser.add_argument("--no-pack", action="store_true", help="Save PNGs without DDS/ZIP packaging")
    parser.add_argument("--no-preview", action="store_true")
    parser.add_argument("--pack-only", action="store_true", help="Repackage existing enhanced PNGs without AI inference")
    parser.add_argument("--recover-dds", action="store_true", help="Recover complete DDS files from a run interrupted before its index was saved")
    args = parser.parse_args()
    config = json.loads((ROOT / "config.json").read_text(encoding="utf-8"))
    scale = args.scale or int(config["scale"])
    config["scale"] = scale
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be positive")
    manifest_path = ROOT / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    jobs = collect_jobs(manifest)
    if not jobs:
        raise RuntimeError("No source images in originals. Run extract_textures.py or add images there first")
    overrides = config.get("processing_overrides", {})
    for relative, job in jobs.items():
        job.update(overrides.get(job["source_path"], overrides.get(relative, {})))
    if args.limit:
        jobs = dict(list(jobs.items())[:args.limit])
    destinations = {}
    for relative in jobs:
        destination = Path(relative).relative_to("originals").with_suffix(".png").as_posix().casefold()
        if destination in destinations:
            raise ValueError(f"Two inputs would overwrite the same PNG: {relative} and {destinations[destination]}; rename one source")
        destinations[destination] = relative
    output_root = ROOT / f"enhanced/{scale}x"
    output_root.mkdir(parents=True, exist_ok=True)
    progress_path = output_root / "progress.json"
    progress = json.loads(progress_path.read_text(encoding="utf-8")) if progress_path.exists() else {"schema": 1, "scale": scale, "jobs": {}}
    (ROOT / "logs").mkdir(exist_ok=True)
    failures, completed, skipped, elapsed = [], 0, 0, 0.0
    with run_lock(ROOT / f"logs/upscale-{scale}x.lock"):
        if not args.pack_only:
            model = ROOT / config["models"][str(scale)]
            if not model.is_file():
                raise RuntimeError("Model weights are missing; run Setup.cmd")
            model_hash = file_hash(model)
            engine = None
            for number, (relative, job) in enumerate(jobs.items(), 1):
                source = ROOT / relative
                output = output_root / Path(relative).relative_to("originals").with_suffix(".png")
                expected = job["width"] * scale, job["height"] * scale
                signature = job_signature(job, config, model_hash)
                if not args.force and valid_completed(output, progress["jobs"].get(relative), signature, expected):
                    print(f"[{number}/{len(jobs)}] Already completed: {source.name}", flush=True)
                    skipped += 1
                    continue
                print(f"[{number}/{len(jobs)}] {job['processing']}: {source.name} -> {expected[0]}x{expected[1]}", flush=True)
                started = time.monotonic()
                try:
                    with Image.open(source) as opened:
                        image = opened.convert("RGBA")
                    if job["processing"] == "resample_data":
                        result = Image.merge("RGBA", tuple(c.resize(expected, Image.Resampling.BILINEAR) for c in image.split()))
                    else:
                        if engine is None:
                            from upscale_engine import Upscaler
                            engine = Upscaler(model, scale, config)
                        result = engine.enhance(image, job["processing"], job["edge_mode"])
                    if result.size != expected or result.mode != "RGBA":
                        raise ValueError("Unexpected model output dimensions/mode")
                    save_png(result, output)
                    seconds = time.monotonic() - started
                    progress["jobs"][relative] = {"status": "completed", "signature": signature,
                        "output": output.relative_to(ROOT).as_posix(), "output_sha256": file_hash(output),
                        "processing": job["processing"], "seconds": round(seconds, 3),
                        "completed_at": datetime.now(timezone.utc).isoformat()}
                    completed += 1
                    elapsed += seconds
                    print(f"  Saved in {seconds:.1f}s", flush=True)
                except Exception as error:
                    progress["jobs"][relative] = {"status": "failed", "signature": signature, "error": str(error)}
                    failures.append(relative)
                    print(f"  FAILED: {error}", file=sys.stderr, flush=True)
                    if engine is not None and engine.device.type == "cuda":
                        engine.torch.cuda.empty_cache()
                write_json(progress_path, progress)
        print(f"PNG batch: {completed} newly completed, {skipped} skipped, {len(failures)} failed; {elapsed:.1f}s processing.", flush=True)
        pack_failures = []
        if config.get("generate_dds", True) and not args.no_pack:
            from pack_textures import build_pack
            pack_failures = build_pack(ROOT, manifest, jobs, scale, failed_inputs=failures, recover_dds=args.recover_dds)
        if config.get("generate_preview", True) and not args.no_preview:
            from preview import contact_sheet
            target = contact_sheet(ROOT, jobs, scale)
            print(f"Comparison preview: {target}", flush=True)
        write_json(ROOT / f"logs/last-run-{scale}x.json", {"completed": completed, "skipped": skipped, "failed_images": failures, "pack_failures": pack_failures, "seconds": elapsed})
    return bool(failures or pack_failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nStopped. Completed images are saved. Run the same command to resume.", file=sys.stderr)
        sys.exit(130)
    except Exception as error:
        print(f"Enhancer failed: {error}", file=sys.stderr)
        sys.exit(1)

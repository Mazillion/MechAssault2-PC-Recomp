"""Enhance every installed Bink movie once per distinct video stream."""

import argparse
from collections import defaultdict
from fractions import Fraction
import json
from pathlib import Path
import subprocess
import sys
import traceback

from enhance_intro import ROOT, inference_code_hash, resolve
from fmv_package import package_enhanced_fmvs
from fmv_pipeline import (
    CHECKPOINT_SHA256, atomic_json, chunk_is_complete, digest, export_video,
    json_digest, read_json, run_lock, source_info, subprocess_options,
)


def parser():
    result = argparse.ArgumentParser(
        description="Resumably enhance all game Bink movies at 2x, reusing identical localized video streams"
    )
    result.add_argument("--config", default=str(ROOT / "config.json"))
    result.add_argument("--movie-root", help="Movie folder; defaults to the configured SPIntro's Movie folder")
    result.add_argument("--output-root", help="Enhanced video folder; defaults to Movie/Enhanced/2x")
    result.add_argument("--metadata-root", help="Resume metadata folder; defaults to Enhancer/FMV/output/all-2x-metadata")
    result.add_argument("--strength", type=float, default=0.5, help="AI blend strength; default 0.5")
    result.add_argument("--device", choices=["cuda", "cpu"], help="Override the configured inference device")
    result.add_argument("--chunk-size", type=int, help="Override output frames per temporal batch")
    result.add_argument("--context", type=int, help="Override extra frames around each temporal batch")
    result.add_argument("--scan-only", action="store_true", help="Inventory movie files without enhancing them")
    result.add_argument("--no-zip", action="store_true", help="Skip the separate distribution ZIP")
    return result


def video_fingerprint(ffprobe: Path, source: Path, info: dict) -> tuple[str, int]:
    """Hash ordered Bink video packets and their timing/size properties, excluding audio."""
    command = [str(ffprobe), "-v", "error", "-select_streams", "v:0", "-show_packets",
               "-show_streams", "-show_entries",
               "stream=codec_name,width,height,avg_frame_rate,time_base,pix_fmt,extradata_hash:packet=data_hash",
               "-show_data_hash", "sha256", "-of", "json",
               str(source)]
    result = subprocess.run(command, capture_output=True, text=True, check=True,
                            **subprocess_options())
    probe_data = json.loads(result.stdout)
    packets = probe_data.get("packets", [])
    hashes = [packet.get("data_hash") for packet in packets]
    if not hashes or any(not packet_hash for packet_hash in hashes):
        raise RuntimeError(f"Could not fingerprint Bink video packets in {source}")
    streams = probe_data.get("streams", [])
    if not streams:
        raise RuntimeError(f"Could not read Bink video properties from {source}")
    video_stream = streams[0]
    identity = {"codec": info["codec"], "width": info["width"], "height": info["height"],
                "fps": info["fps"], "frames": info["frames"],
                "stream": {key: video_stream.get(key) for key in
                           ("time_base", "pix_fmt", "extradata_hash")},
                "video_packet_sha256": hashes}
    return json_digest(identity), len(hashes)


def source_title(asset: dict) -> str:
    return f"{asset['relative'].as_posix()} - RealBasicVSR 2x"


def find_job(asset: dict, expected: dict, chunks_root: Path):
    matches = []
    for manifest_path in chunks_root.glob("*/job.json"):
        manifest = read_json(manifest_path, {})
        settings = manifest.get("settings", {})
        if (manifest.get("source", {}).get("path") != str(asset["path"])
                or manifest.get("source", {}).get("sha256") != asset["info"]["sha256"]
                or any(settings.get(key) != value for key, value in expected.items())):
            continue
        matches.append((manifest_path.parent, manifest))
    if len(matches) != 1:
        raise RuntimeError(
            f"Expected one completed 2x job for {asset['relative']}, found {len(matches)}. "
            "The per-movie enhancement command may have stopped before saving its job manifest."
        )
    directory, manifest = matches[0]
    signature = manifest.get("signature")
    chunks = manifest.get("chunks", [])
    if not signature or not chunks:
        raise RuntimeError(f"The enhancement manifest is incomplete for {asset['relative']}")
    frame_size = (round(asset["info"]["width"] * 2), round(asset["info"]["height"] * 2))
    incomplete = [chunk for chunk in chunks
                  if not chunk_is_complete(directory, chunk, signature, frame_size)]
    if incomplete:
        raise RuntimeError(
            f"The enhanced frame cache is incomplete for {asset['relative']} "
            f"at batch {incomplete[0]['start']}..{incomplete[0]['end'] - 1}"
        )
    return directory, manifest


def main(argv=None):
    args = parser().parse_args(argv)
    if not 0 <= args.strength <= 1:
        raise ValueError("AI blend strength must be between 0 and 1")
    config_path = resolve(args.config)
    config = json.loads(config_path.read_text(encoding="utf-8"))
    configured_source = resolve(config["source"])
    movie_root = resolve(args.movie_root) if args.movie_root else configured_source.parent.parent.parent
    output_root = resolve(args.output_root) if args.output_root else movie_root / "Enhanced" / "2x"
    ffmpeg, ffprobe = (resolve(config[name]) for name in ("ffmpeg", "ffprobe"))
    for path in (movie_root, ffmpeg, ffprobe):
        if not path.exists():
            raise FileNotFoundError(f"Missing {path}")
    if not movie_root.is_dir():
        raise NotADirectoryError(movie_root)

    files = sorted((path for path in movie_root.rglob("*.bik")
                    if not path.is_relative_to(output_root)),
                   key=lambda path: path.as_posix().casefold())
    if not files:
        raise FileNotFoundError(f"No .bik files found under {movie_root}")
    groups = defaultdict(list)
    print(f"Inventorying {len(files)} Bink movies under {movie_root}", flush=True)
    for path in files:
        info = source_info(ffprobe, path)
        if info["codec"] != "binkvideo":
            raise RuntimeError(f"Expected a Bink video, found {info['codec']}: {path}")
        fingerprint, packets = video_fingerprint(ffprobe, path, info)
        asset = {"path": path, "relative": path.relative_to(movie_root), "info": info,
                 "fingerprint": fingerprint, "packets": packets}
        groups[fingerprint].append(asset)

    visual_groups = []
    for fingerprint, members in sorted(groups.items()):
        members.sort(key=lambda asset: asset["relative"].as_posix().casefold())
        english = [asset for asset in members if any(
            part.casefold() == "english" for part in asset["relative"].parts
        )]
        representative = english[0] if english else members[0]
        visual_groups.append({"id": fingerprint[:12], "fingerprint": fingerprint,
                              "representative": representative, "members": members})

    print(f"Found {len(files)} movie files containing {len(visual_groups)} distinct video streams.", flush=True)
    for group in visual_groups:
        print(f"  {group['id']}: {len(group['members'])} file(s); "
              f"{group['representative']['relative']}", flush=True)
    if args.scan_only:
        return 0

    device = args.device or config["device"]
    chunk_size = args.chunk_size if args.chunk_size is not None else config["chunk_size"]
    context = args.context if args.context is not None else config["context"]
    if chunk_size < 1 or context < 0:
        raise ValueError("Chunk size must be >=1 and context must be >=0")
    python = ROOT.parent / ".venv" / "Scripts" / "python.exe"
    if not python.is_file():
        python = Path(sys.executable)
    child_script = ROOT / "enhance_intro.py"
    metadata_root = (resolve(args.metadata_root) if args.metadata_root
                     else ROOT / "output" / "all-2x-metadata")
    enhanced_root = ROOT / "enhanced" / "2x"
    manifest_path = metadata_root / "manifest.json"
    expected_settings = {"model_sha256": CHECKPOINT_SHA256,
                         "code_sha256": inference_code_hash(ROOT, 2), "scale": 2,
                         "strength": args.strength, "chunk_size": chunk_size,
                         "context": context, "device": device, "precision": "float32",
                         "start_frame": 0}
    status = {asset["relative"].as_posix(): "pending"
              for group in visual_groups for asset in group["members"]}

    with run_lock(ROOT / ".run.lock"):
        for group in visual_groups:
            representative = group["representative"]
            relative = representative["relative"]
            representative_output = output_root / relative.with_suffix(".mkv")
            representative_metadata = metadata_root / relative.with_suffix(".json")
            command = [str(python), "-B", str(child_script), "--config", str(config_path),
                       "--source", str(representative["path"]), "--output", str(representative_output),
                       "--metadata", str(representative_metadata),
                       "--title", source_title(representative), "--lock-held", "--scale", "2",
                       "--strength", str(args.strength), "--chunk-size", str(chunk_size),
                       "--context", str(context), "--device", device]
            print(f"\nProcessing visual group {group['id']} from {relative}", flush=True)
            subprocess.run(command, cwd=ROOT, check=True)

            expected = {**expected_settings, "source_sha256": representative["info"]["sha256"],
                        "frames": representative["info"]["frames"],
                        "fps": representative["info"]["fps"]}
            enhanced_frames, job = find_job(representative, expected, enhanced_root)
            visual_signature = job["signature"]
            sar = str(Fraction(config["display_aspect_ratio"])
                      * representative["info"]["height"] / representative["info"]["width"])

            for asset in group["members"]:
                relative_name = asset["relative"].as_posix()
                output = output_root / asset["relative"].with_suffix(".mkv")
                if asset["path"] != representative["path"]:
                    metadata = metadata_root / asset["relative"].with_suffix(".json")
                    audio_signature = json_digest({"visual_signature": visual_signature,
                                                   "source_sha256": asset["info"]["sha256"],
                                                   "video_fingerprint": group["fingerprint"]})
                    export_video(ffmpeg, ffprobe, asset["path"], enhanced_frames, output,
                                 asset["info"], 0, asset["info"]["frames"], sar,
                                 config["crf"], config["preset"], audio_signature,
                                 ROOT / "logs" / "all-fmvs-encode.log", scale=2,
                                 title=source_title(asset), record_path=metadata)
                if not output.is_file():
                    raise RuntimeError(f"Expected output was not created: {output}")
                status[relative_name] = "complete"
                print(f"  Ready: {output}", flush=True)

            atomic_json(manifest_path, {
                "version": 1, "scale": 2, "strength": args.strength,
                "movie_root": str(movie_root), "output_root": str(output_root),
                "metadata_root": str(metadata_root),
                "distinct_video_streams": len(visual_groups),
                "files": len(files),
                "assets": [{"source": asset["relative"].as_posix(),
                            "source_sha256": asset["info"]["sha256"],
                            "video_fingerprint_sha256": asset["fingerprint"],
                            "video_group": group_for(asset, visual_groups),
                            "width": asset["info"]["width"],
                            "height": asset["info"]["height"],
                            "fps": asset["info"]["fps"],
                            "frames": asset["info"]["frames"],
                            "audio_tracks": len(asset["info"]["audio"]),
                            "output": asset["relative"].with_suffix(".mkv").as_posix(),
                            "status": status[asset["relative"].as_posix()]}
                           for group in visual_groups for asset in group["members"]],
            })

        if not args.no_zip:
            assets = [asset for group in visual_groups for asset in group["members"]]
            archive_path = ROOT / "packages" / "enhanced-fmvs-2x.zip"
            package_enhanced_fmvs(assets, output_root, metadata_root, archive_path,
                                  args.strength)

    print(f"\nFinished. {len(files)} outputs are under {output_root}", flush=True)
    return 0


def group_for(asset: dict, groups: list[dict]) -> str:
    for group in groups:
        if asset in group["members"]:
            return group["id"]
    raise RuntimeError("Movie was not assigned to a video group")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nStopped. Rerun this command to resume verified enhancement batches and outputs.", file=sys.stderr)
        sys.exit(130)
    except Exception as exc:
        (ROOT / "logs").mkdir(parents=True, exist_ok=True)
        (ROOT / "logs" / "last_error_all_fmvs.txt").write_text(traceback.format_exc(), encoding="utf-8")
        print(f"\nError: {exc}\nSee logs/last_error_all_fmvs.txt. Completed work is retained.", file=sys.stderr)
        sys.exit(1)

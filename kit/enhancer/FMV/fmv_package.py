"""Build a separate, resumable distribution ZIP for enhanced FMV exports."""

import hashlib
import json
import os
from pathlib import Path
import zipfile

from fmv_pipeline import atomic_json, json_digest, read_json


ARCHIVE_VERSION = 1
GAME_MOVIE_OUTPUT = Path("game_files") / "Movie" / "Enhanced" / "2x"


def package_enhanced_fmvs(assets: list[dict], output_root: Path, metadata_root: Path,
                         archive_path: Path, strength: float) -> bool:
    """Package verified MKVs, returning True when a new archive was written."""
    ordered_assets = sorted(assets, key=lambda asset: asset["relative"].as_posix().casefold())
    entries = []
    for asset in ordered_assets:
        relative = asset["relative"]
        output = output_root / relative.with_suffix(".mkv")
        record = read_json(metadata_root / relative.with_suffix(".json"), {})
        output_sha256 = record.get("sha256")
        if not output.is_file() or not output_sha256:
            raise RuntimeError(f"Missing completed video or checksum for {relative}: {output}")
        output_rel = (GAME_MOVIE_OUTPUT / relative.with_suffix(".mkv")).as_posix()
        entries.append({
            "source": relative.as_posix(),
            "source_sha256": asset["info"]["sha256"],
            "path": output_rel,
            "sha256": output_sha256,
            "bytes": output.stat().st_size,
            "width": round(asset["info"]["width"] * 2),
            "height": round(asset["info"]["height"] * 2),
            "fps": asset["info"]["fps"],
            "frames": asset["info"]["frames"],
            "audio_tracks": len(asset["info"]["audio"]) + bool(asset["info"]["audio"]),
        })

    fingerprint_data = {"archive_version": ARCHIVE_VERSION, "scale": 2,
                        "strength": strength, "files": entries}
    fingerprint = json_digest(fingerprint_data)
    manifest = {"package_fingerprint": fingerprint, "game": "MechAssault 2",
                "enhancement": {"model": "RealBasicVSR", "scale": 2,
                                "strength": strength}, "files": entries}
    state_path = metadata_root / "package-state.json"

    previous = read_json(state_path, {})
    if archive_path.is_file() and previous.get("fingerprint") == fingerprint:
        stat = archive_path.stat()
        if stat.st_size == previous.get("size") and stat.st_mtime_ns == previous.get("mtime_ns"):
            try:
                with zipfile.ZipFile(archive_path, "r") as archive:
                    archived_manifest = json.loads(archive.read("manifest.json"))
                if (archived_manifest.get("package_fingerprint") == fingerprint
                        and zipfile.is_zipfile(archive_path)):
                    print(f"Verified existing FMV package: {archive_path}", flush=True)
                    return False
            except (OSError, KeyError, ValueError, zipfile.BadZipFile):
                pass

    archive_path.parent.mkdir(parents=True, exist_ok=True)
    temp_path = archive_path.with_name(archive_path.stem + ".part" + archive_path.suffix)
    readme = (
        "MechAssault 2 enhanced FMV package\n"
        "\n"
        "This archive contains the enhanced 2x MKV exports only. It does not include\n"
        "the original BIK movies, model weights, source frames, or other game files.\n"
        "\n"
        "Extract at the PC Release root to place the videos under\n"
        "game_files/Movie/Enhanced/2x/. The relative 50/, 60/, and language folder\n"
        "structure mirrors the movie tree. See manifest.json for file hashes and\n"
        "source metadata.\n"
        "\n"
        "These MKVs are enhanced staging assets. The current game build requires\n"
        "movie-replacement support before it can play them.\n"
    )

    try:
        with zipfile.ZipFile(temp_path, "w", compression=zipfile.ZIP_STORED,
                             allowZip64=True) as archive:
            archive.writestr("README.txt", readme)
            for asset, entry in zip(ordered_assets, entries):
                relative = asset["relative"]
                output = output_root / relative.with_suffix(".mkv")
                actual_hash = hashlib.sha256()
                with output.open("rb") as source, archive.open(
                        entry["path"], "w", force_zip64=True) as destination:
                    while block := source.read(1024 * 1024):
                        actual_hash.update(block)
                        destination.write(block)
                if actual_hash.hexdigest() != entry["sha256"]:
                    raise RuntimeError(f"Output changed while packaging: {output}")
            archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")

        with zipfile.ZipFile(temp_path, "r") as archive:
            archived_manifest = json.loads(archive.read("manifest.json"))
            if archived_manifest.get("package_fingerprint") != fingerprint:
                raise RuntimeError("The generated FMV ZIP has an unexpected manifest")
            expected_names = {"README.txt", "manifest.json", *(entry["path"] for entry in entries)}
            if set(archive.namelist()) != expected_names:
                raise RuntimeError("The generated FMV ZIP is missing or contains unexpected files")
        os.replace(temp_path, archive_path)
    except Exception:
        temp_path.unlink(missing_ok=True)
        raise

    stat = archive_path.stat()
    atomic_json(state_path, {"fingerprint": fingerprint, "size": stat.st_size,
                             "mtime_ns": stat.st_mtime_ns})
    print(f"Created separate FMV package: {archive_path}", flush=True)
    return True

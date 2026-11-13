"""Extract originals only. This script never runs an AI model."""
from __future__ import annotations
import argparse
from collections import Counter
import csv
import hashlib
import json
import struct
from pathlib import Path
import sys

from ma2_assets import IMAGE_EXTENSIONS, Mgf, Picture, effect_sprite, sha256, file_hash, save_png, slug, write_json

ROOT = Path(__file__).resolve().parent
DEFAULT_GAME = ROOT.parent / "game_files"          # this folder copied into the install folder; else pass --game-dir
EXPORTER_VERSION = 3


def scan(game: Path):
    archives = sorted(game.glob("*.mgf"))
    if not archives:
        raise ValueError(f"No .mgf files in {game}")
    assets, errors, archive_info = {}, [], []
    entries, paths = 0, set()
    for position, path in enumerate(archives, 1):
        print(f"Inventory {position}/{len(archives)}: {path.name}", flush=True)
        archive = Mgf(path)
        archive_info.append({"name": path.name, "bytes": path.stat().st_size, "mtime_ns": path.stat().st_mtime_ns})
        for name in archive.files:
            if Path(name).suffix.casefold() not in IMAGE_EXTENSIONS:
                continue
            entries += 1
            paths.add(name.casefold())
            try:
                data = archive.file(name)
                identity = sha256(data)
                if identity not in assets:
                    picture = Picture.parse(data)
                    assets[identity] = {"id": identity, "name": name, "picture": picture.metadata(), "sources": []}
                assets[identity]["sources"].append({"archive": path.name, "path": name})
            except Exception as error:
                errors.append({"archive": path.name, "path": name, "error": str(error)})
    value = {"schema": 1, "game_directory": str(game.resolve()), "archives": archive_info,
             "image_entries": entries, "distinct_paths": len(paths), "unique_resources": len(assets),
             "formats": dict(Counter(a["picture"]["format"] for a in assets.values())),
             "assets": list(assets.values()), "errors": errors}
    write_json(ROOT / "catalog.json", value)
    print(f"Inventory: {entries} entries, {len(paths)} paths, {len(assets)} unique resources, {len(errors)} header errors", flush=True)
    return value


def load_catalog(game: Path, refresh: bool):
    path = ROOT / "catalog.json"
    if not refresh and path.exists():
        catalog = json.loads(path.read_text(encoding="utf-8"))
        now = [{"name": p.name, "bytes": p.stat().st_size, "mtime_ns": p.stat().st_mtime_ns} for p in sorted(game.glob("*.mgf"))]
        if catalog["game_directory"] == str(game.resolve()) and catalog["archives"] == now:
            return catalog
    return scan(game)


def export(game: Path, catalog, all_textures: bool):
    manifest_path = ROOT / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {"schema": 1, "resources": {}, "jobs": {}}
    selections = {}
    if not all_textures:
        samples = json.loads((ROOT / "samples.json").read_text(encoding="utf-8"))
        lookup = {(s["archive"].casefold(), s["path"].casefold()): a for a in catalog["assets"] for s in a["sources"]}
        for sample in samples:
            key = sample["archive"].casefold(), sample["path"].casefold()
            if key not in lookup:
                raise ValueError(f"Sample not found: {sample['archive']} / {sample['path']}")
            asset = lookup[key]
            if asset["picture"]["kind"] != "2d":
                raise ValueError("The 20-image starter selection must contain 2D resources")
            selections[asset["id"]] = sample
    selected = [a for a in catalog["assets"] if all_textures or a["id"] in selections]
    selected.sort(key=lambda a: list(selections).index(a["id"]) if not all_textures else a["name"].casefold())
    archive, last_archive, failures = None, None, []
    for position, asset in enumerate(selected, 1):
        source = asset["sources"][0]
        identity = asset["id"]
        previous = manifest["resources"].get(identity)
        if previous and previous.get("exporter_version") == EXPORTER_VERSION and previous.get("original_rgba_match_key") and all((ROOT / j).is_file() and file_hash(ROOT / j) == manifest["jobs"][j]["png_sha256"] for j in previous["images"]):
            print(f"Extract {position}/{len(selected)}: already present {asset['name']}", flush=True)
            continue
        try:
            if source["archive"] != last_archive:
                archive = Mgf(game / source["archive"])
                last_archive = source["archive"]
            data = archive.file(source["path"])
            if sha256(data) != identity:
                raise ValueError("Archive changed since inventory; rerun with --refresh")
            picture = Picture.parse(data)
            images = picture.images(data)
            match_hash = hashlib.sha256()
            match_hash.update(struct.pack("<5I", 1, picture.width, picture.height, picture.depth, picture.frames))
            for image in images:
                match_hash.update(image.tobytes())
            raw_path = ROOT / "raw" / f"{identity}.bin"
            raw_path.parent.mkdir(parents=True, exist_ok=True)
            raw_path.write_bytes(data)
            sample = selections.get(identity, {})
            name = sample.get("path", asset["name"])
            stem = slug(name) + "__" + identity[:16]
            processing = sample.get("processing", "ai_alpha" if picture.format_name == "A8" else "ai_rgb")
            # Packed lighting/data channels and cube/volume effects need a conservative default.
            if picture.format_name == "A8L8" or picture.kind in ("cube", "volume") or effect_sprite(name):
                processing = sample.get("processing", "resample_data")
            image_paths = []
            for index, image in enumerate(images):
                suffix = f"__{index:03}" if len(images) > 1 else ""
                relative = f"originals/{stem}{suffix}.png"
                save_png(image, ROOT / relative)
                image_paths.append(relative)
                manifest["jobs"][relative] = {"resource_id": identity, "subresource": index,
                    "width": image.width, "height": image.height, "png_sha256": file_hash(ROOT / relative),
                    "source_path": name, "format": picture.format_name, "category": sample.get("category", picture.kind),
                    "processing": processing, "edge_mode": sample.get("edge_mode", "reflect")}
            manifest["resources"][identity] = {**asset, "picture": picture.metadata(), "images": image_paths,
                "exporter_version": EXPORTER_VERSION,
                "original_rgba_match_key": match_hash.hexdigest(),
                "raw_file": str(raw_path.relative_to(ROOT)).replace("\\", "/")}
            write_json(manifest_path, manifest)
            print(f"Extract {position}/{len(selected)}: {name} ({len(images)} image(s))", flush=True)
        except Exception as error:
            failures.append({"id": identity, "path": asset["name"], "error": str(error)})
            print(f"FAILED: {asset['name']}: {error}", file=sys.stderr, flush=True)
    (ROOT / "previews").mkdir(exist_ok=True)
    with (ROOT / "previews/texture_inventory.csv").open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=["file", "source_path", "format", "width", "height", "processing", "category"])
        writer.writeheader()
        for path, job in manifest["jobs"].items():
            writer.writerow({"file": path, **{key: job[key] for key in writer.fieldnames[1:]}})
    write_json(ROOT / "logs/extraction.json", {"resources": len(manifest["resources"]), "images": len(manifest["jobs"]), "failures": failures})
    print(f"Originals ready: {len(manifest['resources'])} resources / {len(manifest['jobs'])} PNGs. No AI inference was run.", flush=True)
    return manifest, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, default=DEFAULT_GAME)
    parser.add_argument("--all", action="store_true", help="Export all catalogued texture resources, including faces/slices")
    parser.add_argument("--refresh", action="store_true", help="Rebuild the complete archive inventory")
    parser.add_argument("--inventory-only", action="store_true")
    args = parser.parse_args()
    catalog = load_catalog(args.game_dir, args.refresh)
    if args.inventory_only:
        return bool(catalog["errors"])
    _, failures = export(args.game_dir, catalog, args.all)
    return bool(failures or catalog["errors"])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"Extraction failed: {error}", file=sys.stderr)
        sys.exit(1)

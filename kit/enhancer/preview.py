"""Contact sheets of source pixels; no model inference."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def checker(size):
    image = Image.new("RGBA", size, (80, 80, 80, 255))
    draw = ImageDraw.Draw(image)
    for y in range(0, size[1], 12):
        for x in range(0, size[0], 12):
            if (x // 12 + y // 12) % 2:
                draw.rectangle((x, y, x + 11, y + 11), fill=(115, 115, 115, 255))
    return image


def contact_sheet(root: Path, jobs: dict, scale=None):
    items = list(jobs.items())[:40]
    if not items:
        return None
    cell_w, cell_h, columns = (420 if scale else 225), 254, (3 if scale else 5)
    rows = (len(items) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * cell_w, rows * cell_h + 46), (25, 27, 32))
    draw = ImageDraw.Draw(sheet)
    title = f"MechAssault 2 - Original / Enhanced {scale}x" if scale else "MechAssault 2 - 20 representative originals"
    draw.text((12, 14), title, fill="white")
    for index, (relative, job) in enumerate(items):
        x, y = (index % columns) * cell_w, 46 + (index // columns) * cell_h
        variants = [root / relative]
        if scale:
            variants.append(root / f"enhanced/{scale}x" / Path(relative).relative_to("originals").with_suffix(".png"))
        for number, path in enumerate(variants):
            if not path.is_file():
                continue
            with Image.open(path) as opened:
                image = opened.convert("RGBA")
            image.thumbnail((196, 184), Image.Resampling.NEAREST if number == 0 else Image.Resampling.LANCZOS)
            panel = checker((196, 184))
            panel.alpha_composite(image, ((196 - image.width) // 2, (184 - image.height) // 2))
            sheet.paste(panel.convert("RGB"), (x + 10 + number * 202, y + 8))
        draw.text((x + 10, y + 196), f"{index + 1:02}  {Path(job['source_path']).name[:36]}", fill="white")
        draw.text((x + 10, y + 211), f"{job['width']}x{job['height']}  {job['format']}  {job['processing']}", fill=(180, 195, 210))
        draw.text((x + 10, y + 227), job.get("category", "")[:54], fill=(195, 180, 140))
    target = root / "previews" / (f"comparison-{scale}x.jpg" if scale else "originals-contact-sheet.png")
    target.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(target)
    return target

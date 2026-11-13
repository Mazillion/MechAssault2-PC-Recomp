"""Game icon + yellow "SE" badge -> editor icon (all sizes of the original, plus 256)."""
import io, struct, sys
from PIL import Image, ImageDraw, ImageFont
src, dst, preview = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, 'rb').read()
n = struct.unpack_from('<HHH', d, 0)[2]
imgs = []
for i in range(n):
    w, h, c, r, p, b, sz, off = struct.unpack_from('<BBBBHHII', d, 6 + 16 * i)
    imgs.append(Image.open(io.BytesIO(d[off:off + sz])).convert('RGBA'))
big = max(imgs, key=lambda im: im.width)
imgs.append(big.resize((256, 256), Image.LANCZOS))
FONT = r'C:\Windows\Fonts\ariblk.ttf'
out = []
for im in sorted(imgs, key=lambda im: im.width):
    s = im.width
    im = im.copy()
    # Draw at 4x and downsample for clean edges at small sizes.
    k = 4 if s < 128 else 2
    layer = Image.new('RGBA', (s * k, s * k), (0, 0, 0, 0))
    dr = ImageDraw.Draw(layer)
    size = int(s * k * (0.6 if s <= 16 else 0.42 if s <= 32 else 0.36))
    font = ImageFont.truetype(FONT, size)
    stroke = max(k, int(size * 0.09))
    l, t, r_, b_ = dr.textbbox((0, 0), 'SE', font=font, stroke_width=stroke)
    x = s * k - (r_ - l) - (0 if s <= 16 else s * k * 0.02) - l
    y = s * k - (b_ - t) - (0 if s <= 16 else s * k * 0.02) - t
    dr.text((x, y), 'SE', font=font, fill=(255, 212, 0, 255), stroke_width=stroke, stroke_fill=(0, 0, 0, 255))
    im.alpha_composite(layer.resize((s, s), Image.LANCZOS))
    out.append(im)
# ICO: PNG-compressed entries.
blobs = []
for im in out:
    bio = io.BytesIO(); im.save(bio, 'PNG'); blobs.append(bio.getvalue())
hdr = struct.pack('<HHH', 0, 1, len(out))
off = 6 + 16 * len(out)
dirs = b''
for im, bl in zip(out, blobs):
    w = im.width if im.width < 256 else 0
    dirs += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(bl), off); off += len(bl)
open(dst, 'wb').write(hdr + dirs + b''.join(blobs))
sheet = Image.new('RGBA', (sum(im.width for im in out) + 10 * len(out), 256), (40, 40, 40, 255))
x = 0
for im in out:
    sheet.alpha_composite(im, (x, 256 - im.height)); x += im.width + 10
sheet.save(preview)
print('sizes', [im.width for im in out])

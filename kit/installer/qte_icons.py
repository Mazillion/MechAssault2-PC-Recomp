"""The PC quick-time-event prompt icons (interface/PC/QTE/*.tif in pc_assets/common.mgf).

The keyboard/mouse QTE prompts draw the game's own button sprites with the letter removed and the
current key binding printed on top. The 24 blank sprites are made here, on your PC, from your disc's
interface/Sprites/UI_Navigation/buttons/*.tif: each 32x32 DXT5 sprite is decoded, the letter is found
(interior pixels that differ from the button's face colour) and painted over by interpolating the
face colour across each row. The result is wrapped in the original MGIC header as uncompressed linear
A8R8G8B8 (header flags 0x80000000).

Nothing of the picture is stored in the project: data/qte-icons.json holds only file names and
SHA-256 values (of your disc's sprite, and of the icon this procedure makes from it), so a damaged
disc or a changed procedure is noticed.

  icons(game_mgf) -> {path: bytes}             (installer)
  python qte_icons.py record <disc common.mgf>  (release tooling: re-record the SHA-256 values)
"""
import hashlib, json, struct
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / 'data'
ROLES = ['button_A', 'button_X', 'button_Y', 'button_B', 'trigger_L', 'trigger_R']
STATES = ['idle', 'active', 'pressed', 'error']
SIZE = 32
FACE_TOLERANCE = 28          # max channel difference from the face colour that still counts as face


def sha(data):
    return hashlib.sha256(data).hexdigest()


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]


def decode_dxt5(data, width=SIZE, height=SIZE):
    """BGRA8 rows from DXT5 blocks (deterministic integer decode)."""
    out = bytearray(width * height * 4)
    blocks_x = width // 4
    for by in range(height // 4):
        for bx in range(blocks_x):
            block = data[(by * blocks_x + bx) * 16:(by * blocks_x + bx + 1) * 16]
            a0, a1 = block[0], block[1]
            if a0 > a1:
                alphas = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
            else:
                alphas = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
            alpha_bits = int.from_bytes(block[2:8], 'little')
            c0, c1 = struct.unpack_from('<HH', block, 8)
            p0, p1 = _rgb565(c0), _rgb565(c1)
            palette = [p0, p1, [(2 * a + b) // 3 for a, b in zip(p0, p1)], [(a + 2 * b) // 3 for a, b in zip(p0, p1)]]
            indices = struct.unpack_from('<I', block, 12)[0]
            for i in range(16):
                r, g, b = palette[(indices >> (2 * i)) & 3]
                o = ((by * 4 + i // 4) * width + bx * 4 + i % 4) * 4
                out[o:o + 4] = bytes((b, g, r, alphas[(alpha_bits >> (3 * i)) & 7]))
    return bytes(out)


def blank(bgra, w=SIZE):
    """Remove the letter from a decoded button sprite (BGRA rows); pure function of the input."""
    px = [list(bgra[i * 4:i * 4 + 4]) for i in range(w * w)]

    def solid(x, y):
        return 0 <= x < w and 0 <= y < w and px[y * w + x][3] >= 200

    inner = set(i for i in range(w * w)
                if all(solid(i % w + dx, i // w + dy) for dy in range(-2, 3) for dx in range(-2, 3)))
    out = [p[:] for p in px]
    if inner:
        key = Counter(tuple(v >> 4 for v in px[i][:3]) for i in sorted(inner)).most_common(1)[0][0]
        face_px = [px[i] for i in sorted(inner) if tuple(v >> 4 for v in px[i][:3]) == key]
        face = [sum(p[k] for p in face_px) // len(face_px) for k in range(3)]
        mask = set(i for i in inner if max(abs(px[i][k] - face[k]) for k in range(3)) > FACE_TOLERANCE)
        grown = set(mask)
        for i in mask:
            x, y = i % w, i // w
            grown |= {(y + dy) * w + x + dx for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                      if 0 <= x + dx < w and 0 <= y + dy < w and (y + dy) * w + x + dx in inner}
        for i in sorted(grown):
            x, y = i % w, i // w
            left = next((xx for xx in range(x - 1, -1, -1) if y * w + xx in inner and y * w + xx not in grown), None)
            right = next((xx for xx in range(x + 1, w) if y * w + xx in inner and y * w + xx not in grown), None)
            if left is not None and right is not None:
                t = (x - left) / (right - left)
                a, b = px[y * w + left], px[y * w + right]
                out[i][:3] = [int(a[k] * (1 - t) + b[k] * t + 0.5) for k in range(3)]
            else:
                out[i][:3] = face
    for p in out:                       # fully transparent texels carry no colour (no fringes when filtered)
        if p[3] == 0:
            p[:3] = [0, 0, 0]
    return bytes(v for p in out for v in p)


def wrap(original, payload):
    """The original sprite's MGIC header, re-labelled for uncompressed linear BGRA pixels."""
    if len(original) != 1173 or original[4:8] != b'MGIc':
        raise ValueError('unexpected original button sprite container')
    blob = bytearray(original[:149]); blob.extend(payload)
    struct.pack_into('<I', blob, 8, len(blob)); struct.pack_into('<I', blob, 40, len(blob) - 32)
    for key, value in ((b'flgs', 0x80000000), (b'size', len(payload))):
        pos = blob.find(key, 32, 149)
        if pos < 0: raise ValueError('missing MGIC attribute')
        struct.pack_into('<I', blob, pos + 8, value)
    bits = blob.find(b'bits', 32, 149)
    if bits < 0: raise ValueError('missing MGIC bits attribute')
    struct.pack_into('<I', blob, bits + 4, len(payload) + 8)
    return bytes(blob)


def _rows():
    for role in ROLES:
        for state in STATES:
            yield ('interface/PC/QTE/%s_%s.tif' % (role, state),
                   'interface/Sprites/UI_Navigation/buttons/%s_%s.tif' % (role, state))


def make_icon(original):
    return wrap(original, blank(decode_dxt5(original[149:149 + 1024])))


def icons(game_mgf):
    """{overlay path: icon bytes} from the disc's common.mgf (an inspect_mgf.Mgf)."""
    table = json.loads((DATA / 'qte-icons.json').read_text(encoding='utf-8'))
    out = {}
    for row in table:
        original = game_mgf.file(row['source'])
        if sha(original) != row['source_sha256']:
            raise SystemExit('The disc\'s %s is not the one this release was made from.' % row['source'])
        blob = make_icon(original)
        if sha(blob) != row['sha256']:
            raise SystemExit('QTE icon %s did not come out as expected.' % row['path'])
        out[row['path']] = blob
    return out


def record(game_common):
    """Release tooling: record source and result SHA-256 values (and update pc-overlay.json)."""
    import sys
    sys.path.insert(0, str(HERE))
    from inspect_mgf import Mgf
    game = Mgf(game_common)
    table = []
    for path, source in _rows():
        original = game.file(source)
        table.append({'path': path, 'source': source, 'source_sha256': sha(original), 'sha256': sha(make_icon(original))})
    (DATA / 'qte-icons.json').write_text(json.dumps(table, indent=1) + '\n', encoding='utf-8')
    overlay = DATA / 'pc-overlay.json'
    if overlay.is_file():
        spec = json.loads(overlay.read_text(encoding='utf-8'))
        for row in table:
            spec['changed'][row['path']] = row['sha256']
        overlay.write_text(json.dumps(spec, indent=1, sort_keys=True) + '\n', encoding='utf-8')
    print('recorded', len(table), 'icons')


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == 'record': record(sys.argv[2])
    else: sys.exit(__doc__)

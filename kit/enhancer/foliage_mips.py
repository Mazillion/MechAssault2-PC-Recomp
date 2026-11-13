"""Keep the game's deliberately fuller distant foliage in enhanced packs."""
from pathlib import Path
import struct
from PIL import Image, ImageChops, ImageStat
from ma2_assets import Picture

POLICY = 'authored_foliage_mips_v1'


def foliage_source(root, resource):
    picture = resource['picture']
    if ('/Trees/' not in resource['name'].replace('\\', '/') or
            picture['kind'] != '2d' or picture.get('format') not in ('DXT1', 'DXT3', 'DXT5')):
        return None
    with Image.open(root / resource['images'][0]) as image:
        if image.convert('RGBA').getchannel('A').getextrema()[0] == 255:
            return None
    return root / resource['raw_file']


def original_mips(path):
    raw = Path(path).read_bytes()
    picture = Picture.parse(raw)
    if picture.kind != '2d' or picture.compression not in (1, 3, 5):
        raise ValueError('Authored foliage requires a 2D DXT source')
    result, offset = [], picture.payload_offset
    for level in range(picture.levels):
        width, height = max(1, picture.width >> level), max(1, picture.height >> level)
        size = picture.level_size(level)
        result.append(Image.frombytes('RGBA', (((width+3)//4)*4, ((height+3)//4)*4),
            raw[offset:offset+size], 'bcn', {1:1, 3:2, 5:3}[picture.compression]).crop((0, 0, width, height)))
        offset += size
    return result


def compose_chain(enhanced, authored, scale):
    """Enhanced extra levels up close; original RGB and alpha at original sizes."""
    if scale not in (2, 4) or enhanced.size != tuple(n*scale for n in authored[0].size):
        raise ValueError('Foliage mip dimensions/scale do not match')
    extra = scale.bit_length()-1
    levels = [enhanced.resize((max(1, enhanced.width >> l), max(1, enhanced.height >> l)),
                             Image.Resampling.LANCZOS) for l in range(extra)] + authored
    while levels[-1].size != (1, 1):
        last = levels[-1]
        levels.append(last.resize((max(1, last.width//2), max(1, last.height//2)), Image.Resampling.BOX))
    return levels


def write_bgra_chain(path, levels):
    width, height = levels[0].size
    header = bytearray(128)
    header[:4] = b'DDS '
    struct.pack_into('<7I', header, 4, 124, 0x2100F, height, width, width*4, 0, len(levels))
    struct.pack_into('<8I', header, 76, 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    struct.pack_into('<I', header, 108, 0x401008)
    Path(path).write_bytes(bytes(header) + b''.join(image.tobytes('raw', 'BGRA') for image in levels))


def build_foliage_input(path, png, raw, scale):
    with Image.open(png) as image:
        levels = compose_chain(image.convert('RGBA'), original_mips(raw), scale)
    write_bgra_chain(path, levels)


def validate_foliage_mips(path, source, scale, format_name):
    """Check every authored level after compression, including alpha coverage."""
    data = Path(path).read_bytes()
    width, height = struct.unpack_from('<II', data, 12)[::-1]
    count = struct.unpack_from('<I', data, 28)[0]
    offset = 148 if data[84:88] == b'DX10' else 128
    extra = scale.bit_length()-1
    authored = original_mips(source)
    measurements = []
    for level in range(count):
        w, h = max(1, width >> level), max(1, height >> level)
        bc = format_name == 'BC7_UNORM'
        size = ((w+3)//4)*((h+3)//4)*16 if bc else w*h*4
        if extra <= level < extra+len(authored):
            decoded = (Image.frombytes('RGBA', (((w+3)//4)*4, ((h+3)//4)*4), data[offset:offset+size], 'bcn', 7).crop((0,0,w,h))
                       if bc else Image.frombytes('RGBA', (w,h), data[offset:offset+size], 'raw', 'BGRA'))
            expected = authored[level-extra]
            errors = ImageStat.Stat(ImageChops.difference(decoded, expected)).mean
            error = max(errors)
            def coverage(image):
                histogram = image.getchannel('A').histogram()
                return sum(histogram[128:])/(w*h)
            original, replacement = coverage(expected), coverage(decoded)
            # Tiny authored mips tolerate normal BC7 color quantization. Alpha
            # and the cutout silhouette use stricter, independent limits.
            if max(errors[:3]) > (12 if bc else 0) or errors[3] > (8 if bc else 0) or abs(original-replacement) > .03:
                raise ValueError(f'Authored foliage mip {level} changed: error={error:.3f}, coverage={original:.3f}/{replacement:.3f}')
            measurements.append({'width':w, 'height':h, 'original_coverage':original, 'enhanced_coverage':replacement, 'mean_error':error})
        offset += size
    if len(measurements) != len(authored):
        raise ValueError('Missing authored foliage mip levels')
    return measurements

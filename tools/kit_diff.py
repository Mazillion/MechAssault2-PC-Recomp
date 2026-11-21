"""List or compare the files packed inside single-file installers (the MA2KIT01 payload of installer-exe/pack_installer.py).

    py -3.13 tools/kit_diff.py A.exe                 list: size, sha256, name of every packed file
    py -3.13 tools/kit_diff.py A.exe B.exe [--json out.json]
                                                     compare: same / differ / only-in-A / only-in-B; for differing
                                                     files the number of differing bytes (same size) or the sizes

Files are decompressed with the Windows Compression API (LZMS), the same codec the packer uses.
Exit code 0 when the payloads are identical file for file, 1 otherwise.
"""
import ctypes, hashlib, json, struct, sys
from pathlib import Path

_cab = ctypes.WinDLL('cabinet')
LZMS = 5


def _decompress(blob, size):
    h = ctypes.c_void_p()
    if not _cab.CreateDecompressor(LZMS, None, ctypes.byref(h)):
        raise OSError('CreateDecompressor failed')
    try:
        out = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not _cab.Decompress(h, blob, len(blob), out, size, ctypes.byref(got)):
            raise OSError('Decompress failed')
        return out.raw[:got.value]
    finally:
        _cab.CloseDecompressor(h)


def read_kit(path):
    """Return {name: bytes} for every packed file, plus the kit id."""
    data = Path(path).read_bytes()
    if data[-8:] != b'MA2KEND1':
        raise SystemExit('%s: no MA2KIT payload' % path)
    offset, size = struct.unpack('<QQ', data[-40:-24])
    ident = data[-24:-8].decode('ascii')
    p = offset
    if data[p:p + 8] != b'MA2KIT01':
        raise SystemExit('%s: bad index' % path)
    count, = struct.unpack('<I', data[p + 8:p + 12]); p += 12
    entries = []
    for _ in range(count):
        n, = struct.unpack('<H', data[p:p + 2]); p += 2
        name = data[p:p + n].decode('utf-8'); p += n
        usize, csize, method = struct.unpack('<QQB', data[p:p + 17]); p += 17
        entries.append((name, usize, csize, method))
    files = {}
    for name, usize, csize, method in entries:
        blob = data[p:p + csize]; p += csize
        files[name] = _decompress(blob, usize) if method == 1 else blob
        if len(files[name]) != usize:
            raise SystemExit('%s: %s size mismatch' % (path, name))
    return files, ident


def sha(b):
    return hashlib.sha256(b).hexdigest()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out_json = sys.argv[sys.argv.index('--json') + 1] if '--json' in sys.argv else None
    if out_json in args:
        args.remove(out_json)
    if len(args) == 1:
        files, ident = read_kit(args[0])
        for k in sorted(files):
            print('%10d %s %s' % (len(files[k]), sha(files[k])[:16], k))
        print('%d files, kit id %s' % (len(files), ident))
        return 0
    a, ia = read_kit(args[0]); b, ib = read_kit(args[1])
    only_a = sorted(set(a) - set(b)); only_b = sorted(set(b) - set(a))
    same, diff = [], []
    for k in sorted(set(a) & set(b)):
        if a[k] == b[k]:
            same.append(k); continue
        if len(a[k]) == len(b[k]):
            n = sum(1 for x, y in zip(a[k], b[k]) if x != y)
            diff.append((k, '%d of %d bytes differ' % (n, len(a[k]))))
        else:
            diff.append((k, 'sizes %d vs %d' % (len(a[k]), len(b[k]))))
    print('A %s: %d files, kit id %s' % (Path(args[0]).name, len(a), ia))
    print('B %s: %d files, kit id %s' % (Path(args[1]).name, len(b), ib))
    print('%d same, %d differ, %d only in A, %d only in B' % (len(same), len(diff), len(only_a), len(only_b)))
    for k, d in diff:
        print('  differs: %s  (%s)' % (k, d))
    for k in only_a:
        print('  only A: %s' % k)
    for k in only_b:
        print('  only B: %s' % k)
    if out_json:
        Path(out_json).write_text(json.dumps({'same': len(same), 'differ': diff, 'only_a': only_a, 'only_b': only_b},
                                             indent=1), encoding='utf-8')
    return 0 if not (diff or only_a or only_b) else 1


if __name__ == '__main__':
    sys.exit(main())

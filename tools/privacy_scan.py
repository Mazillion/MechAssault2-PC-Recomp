"""Scan the repository (or built binaries) for things that must never be published.

    python tools/privacy_scan.py                     scan every tracked/untracked file in the repo
    python tools/privacy_scan.py --binaries a.exe b  scan built files (UTF-8 and UTF-16 strings); for the
                                                     single-file installer also every packed file, decompressed
                                                     (the kit, the bundled Python runtime and the editor);
                                                     for a .zip every member (and an installer in it)

It looks for: absolute user-profile paths (C:\\Users\\<name>, /home/<name>), the current Windows user
name and machine name, this checkout's own absolute path, e-mail addresses (other than GitHub noreply
addresses and the authors' addresses inside third-party licence notices), game files that must
never be committed (see .gitignore), and secrets that come from a game disc: XBE certificate keys
(LAN / signature keys as printed by xbe_parser), the studio's build path, and any long high-entropy
hex value (or 16-byte 0x.. list) within a few words after the word "key". Exit code 1 on any hit.
"""
import os, re, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
THIRD_PARTY_EMAILS = {'aquynh@gmail.com', 'erikd@mega-nerd.com', 'jseward@acm.org', 'info@indiantypefoundry.com',
                      'post@carrois.com'}
GAME_EXT = {'.xbe', '.iso', '.xiso', '.mgf', '.xwb', '.xsb', '.xgs', '.bik', '.wma', '.xmv', '.ggf', '.wdf', '.mgfont',
            '.tif', '.dds', '.pack'}
SKIP_DIRS = {'.git', 'dist', '__pycache__'}


def patterns(emails=True):
    # "Jos": the installer's documented example of an accented user name (C:/Users/José), not a real one;
    # "Player": the neutral user name in the recorded test install log; "MyUser": the example in Python's venv scripts.
    pats = [r'[A-Za-z]:\\Users\\(?!Public\b|Player\b|MyUser\b|<|%|Jos|\{)[^\\\s"\'<>]+',
            r'[A-Za-z]:/Users/(?!Public\b|Player\b|MyUser\b|<|%|Jos|\{)[^/\s"\'<>]+', r'/home/[a-z_][a-z0-9_-]+/',
            ]
    if emails:
        pats.append(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]*[A-Za-z]{2,}')
    for var in ('USERNAME', 'COMPUTERNAME'):
        v = os.environ.get(var)
        if v and len(v) > 2:
            pats.append(r'(?i)\b' + re.escape(v) + r'\b')
    pats.append(re.escape(str(REPO)))
    return [re.compile(p) for p in pats]


# Disc-derived secrets (certificate keys, the studio's debug path): never in the repo, a log or a binary.
SECRET_PATS = [re.compile(p) for p in (
    r'(?i)\b(?:LAN|Signature|Alternate Signature|HD|EEPROM) Keys?\s*[:=]\s*[0-9a-f]{16,}',
    r'(?i)Debug Path\s*:\s*[A-Za-z]:\\',
    r'(?i)[A-Za-z]:\\Development\\Source\\',
)]
KEY_HEX = re.compile(r'(?i)key(?!board|frame|word|stroke)[^\n]{0,40}?(?<![0-9a-f])([0-9a-f]{32,})(?![0-9a-f])')
KEY_BYTES = re.compile(r'(?i)key(?!board|frame|word|stroke)[^\n]{0,40}?((?:0x[0-9a-f]{2}\s*,\s*){15}0x[0-9a-f]{2})')


def secrets(t):
    """Disc-key-like values: explicit key/debug-path lines, or high-entropy hex right after the word "key"."""
    out = [m.group(0)[:80] for rx in SECRET_PATS for m in rx.finditer(t)]
    for rx in (KEY_HEX, KEY_BYTES):
        for m in rx.finditer(t):
            digits = re.sub(r'(?i)0x|[\s,]', '', m.group(1))
            if 'Audible' in m.group(0):                       # FFmpeg's published Audible AA/AAX option keys
                continue
            if len(set(digits.lower())) >= 10:                   # not 0000..., not a run of one byte
                out.append(m.group(0)[:80])
    return out


def texts(data):
    out = [data.decode('latin-1')]
    if b'\0' in data[:65536]:
        out += [data.decode('utf-16le', 'ignore'), data[1:].decode('utf-16le', 'ignore')]
    return out


def ok_match(m):
    s = m.group(0)
    if '@' in s and not s.startswith(('C:', 'c:')):
        low = s.lower()
        return (low in THIRD_PARTY_EMAILS or low.endswith('@users.noreply.github.com')
                or re.match(r'.*@[0-9.]+$', low) is not None or low.endswith(('.png', '.jpg', '.svg')))
    return False


def scan(paths, rel_to=None, emails=True, names_in_paths_only=False):
    """names_in_paths_only: for unmodified third-party files (the bundled python.org runtime), a word equal to the
    user or machine name is a hit only next to a path separator (a credit line naming a person is not a leak)."""
    pats, hits = patterns(emails), []
    names = set(rx.pattern for rx in pats if rx.pattern.startswith('(?i)'))
    for p in paths:
        data = Path(p).read_bytes()
        for t in texts(data):
            for s in secrets(t):
                hits.append((Path(p).relative_to(rel_to).as_posix() if rel_to else str(p), 'disc secret: ' + s))
            for rx in pats:
                for m in rx.finditer(t):
                    if names_in_paths_only and rx.pattern in names and not (
                            t[max(0, m.start() - 1):m.start()] in ('\\', '/') or t[m.end():m.end() + 1] in ('\\', '/')):
                        continue
                    if not ok_match(m):
                        name = Path(p).relative_to(rel_to).as_posix() if rel_to else str(p)
                        hits.append((name, m.group(0)[:80]))
                        break
    return sorted(set(hits))


def repo_files():
    """Files that would be committed (git: tracked + untracked, minus .gitignore), else everything."""
    r = subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '-z'], cwd=str(REPO), capture_output=True)
    if r.returncode == 0:
        for f in r.stdout.decode('utf-8').split('\0'):
            if f and (REPO / f).is_file():
                yield REPO / f
        return
    for p in REPO.rglob('*'):
        if p.is_file() and not (set(p.relative_to(REPO).parts) & SKIP_DIRS):
            yield p


def payload_members(path):
    """The files packed into a single-file installer (installer-exe/pack_installer.py format), decompressed, as
    (name, bytes); [] for any other file. The trailer is found at the end of the file or, for a signed file, just
    before the certificate table (the same rule as stub.cpp read_trailer)."""
    import ctypes, struct
    data = Path(path).read_bytes()

    def trailer(end):
        if end < 40 or data[end - 8:end] != b'MA2KEND1':
            return None
        off, size = struct.unpack_from('<QQ', data, end - 40)
        return (off, size) if off + size == end - 40 and data[off:off + 8] == b'MA2KIT01' else None
    found = trailer(len(data))
    if not found and data[:2] == b'MZ':
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        opt = pe + 24
        magic = struct.unpack_from('<H', data, opt)[0] if opt + 2 <= len(data) else 0
        if data[pe:pe + 4] == b'PE\0\0' and magic in (0x10B, 0x20B):
            cert = struct.unpack_from('<I', data, opt + (112 if magic == 0x20B else 96) + 32)[0]
            for pad in range(8):
                found = found or (trailer(cert - pad) if cert > pad else None)
    if not found:
        return []
    off = found[0]
    count = struct.unpack_from('<I', data, off + 8)[0]
    pos, entries = off + 12, []
    for _ in range(count):
        n = struct.unpack_from('<H', data, pos)[0]
        name = data[pos + 2:pos + 2 + n].decode('utf-8')
        size, packed, method = struct.unpack_from('<QQB', data, pos + 2 + n)
        entries.append((name, size, packed, method)); pos += 19 + n
    cab = ctypes.WinDLL('cabinet')
    cab.CreateDecompressor.argtypes = [ctypes.c_ulong, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    cab.Decompress.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.POINTER(ctypes.c_size_t)]
    cab.CloseDecompressor.argtypes = [ctypes.c_void_p]
    handle = ctypes.c_void_p()
    if not cab.CreateDecompressor(5, None, ctypes.byref(handle)):           # 5 = COMPRESS_ALGORITHM_LZMS
        raise OSError('CreateDecompressor')
    out = []
    try:
        for name, size, packed, method in entries:
            blob = data[pos:pos + packed]; pos += packed
            if method == 1:
                buf = ctypes.create_string_buffer(max(size, 1)); got = ctypes.c_size_t()
                if not cab.Decompress(handle, blob, len(blob), buf, size, ctypes.byref(got)) or got.value != size:
                    raise OSError('cannot decompress %s in %s' % (name, path))
                blob = buf.raw[:size]
            out.append((name, blob))
    finally:
        cab.CloseDecompressor(handle)
    return out


def scan_payload(path):
    """Every packed file of an installer, scanned like a binary (the kit, the bundled Python, the editor)."""
    hits, tmp = [], Path(os.environ.get('TEMP', '.')) / ('ma2-privacy-scan-%d' % os.getpid())
    members = payload_members(path)
    for name, blob in members:
        tmp.write_bytes(blob)
        hits += [('%s!%s' % (Path(path).name, name), what) for _, what in scan([tmp], emails=False, names_in_paths_only=name.startswith('python/'))]
    if tmp.exists():
        tmp.unlink()
    return hits, len(members)


def scan_zip(path):
    """A release .zip (the RecompLauncher zip): every member decompressed and scanned like a binary, an installer member with its
    packed files too. ([], 0) for any other file."""
    import zipfile
    if Path(path).suffix.lower() != '.zip' or not zipfile.is_zipfile(path):
        return [], 0
    hits, tmp = [], Path(os.environ.get('TEMP', '.')) / ('ma2-privacy-zip-%d' % os.getpid())
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        for name in names:
            tmp.write_bytes(z.read(name))
            where = '%s!%s' % (Path(path).name, name)
            hits += [(where, what) for _, what in scan([tmp], emails=False)]
            more, n = scan_payload(tmp)
            hits += [(where + what_name[len(tmp.name):], what) for what_name, what in more]
            if n:
                print('%s: %d packed files decompressed and scanned' % (where, n))
    if tmp.exists():
        tmp.unlink()
    return hits, len(names)


def main():
    a = sys.argv[1:]
    if a[:1] == ['--binaries']:
        hits = scan(a[1:], emails=False)    # compressed payloads make e-mail-like noise; paths/names still count
        for p in a[1:]:
            more, n = scan_payload(p)
            if n:
                print('%s: %d packed files decompressed and scanned' % (Path(p).name, n))
            hits += more
            more, n = scan_zip(p)
            if n:
                print('%s: %d zip members decompressed and scanned' % (Path(p).name, n))
            hits += more
    else:
        files = list(repo_files())
        hits = scan(files, REPO)
        hits += [(p.relative_to(REPO).as_posix(), 'game file type ' + p.suffix) for p in files
                 if p.suffix.lower() in GAME_EXT]
    for name, what in hits:
        print('HIT %s: %s' % (name, what))
    print('privacy scan: %d hit(s)' % len(hits))
    sys.exit(1 if hits else 0)


if __name__ == '__main__':
    main()

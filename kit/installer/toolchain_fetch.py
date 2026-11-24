"""The C++ toolchain for Linux (Wine / Proton): fetched the way FFmpeg is, every archive checked before use.

Microsoft's C++ Build Tools do not install under Wine, so there the game is compiled with the Windows builds of LLVM
(clang-cl, lld-link, llvm-rc, llvm-mt), the Microsoft C runtime and Windows SDK (laid out by xwin from Microsoft's own
package feed), CMake and Ninja. data/toolchain.json pins each part: its version, archive, URLs and SHA-256. An archive is
checked against its SHA-256 before a single file is taken out of it, and only the paths the spec names ("take") are ever
written, so a crafted archive cannot place files anywhere else. A part whose sha256 is still null is never downloaded.

The target folder (wine_host.toolchain_dir(), %LOCALAPPDATA%\\MechAssault2\\Toolchain inside the Wine prefix) gets
llvm\\, cmake\\, ninja\\ and xwin\\ (the CRT and SDK). xwin itself runs once, only when ensure(..., accept_licence=True) is
called: that is the player's yes to Microsoft's licence terms (the launcher's tick, or the installer's button).

Where the parts come from, in order:
  1. the target folder, when the part is already complete
  2. MA2_TOOLCHAIN_SOURCE: a folder holding the pinned archives (the same file names), or the parts already laid out
     (llvm\\, cmake\\, ninja\\, xwin\\); nothing is downloaded then (tests, players without internet)
  3. the published URLs (download.py: resume, retries, the per-user download cache)
"""
import hashlib, json, os, shutil, subprocess, tarfile, zipfile
from pathlib import Path, PurePosixPath
import download as dl
import wine_host

SPEC = json.loads((Path(__file__).resolve().parent / 'data' / 'toolchain.json').read_text(encoding='utf-8'))
PARTS = ('llvm', 'cmake', 'ninja')                 # archives; the 'xwin' part makes the CRT/SDK folder
MAX_BYTES = 2 << 30
MAGIC = {'.zip': b'PK\x03\x04', '.xz': b'\xfd7zXZ\x00', '.gz': b'\x1f\x8b'}


def download_bytes(spec=None):
    """What the player gets for the Linux toolchain, in bytes, from data/toolchain.json: the four pinned archives' recorded sizes
    (llvm, cmake, ninja, xwin) plus the measured size of the CRT/SDK that xwin lays out (xwin.splat_bytes). 1,680,568,089 for
    the 11-24 pins."""
    spec = spec or SPEC
    return sum(int(spec[n]['bytes']) for n in PARTS + ('xwin',)) + int(spec['xwin']['splat_bytes'])


def size_text(spec=None):
    """'1.7 GB' (decimal GB, one decimal): the size every player-facing text names, so it cannot drift from the pins."""
    return '%.1f GB' % (download_bytes(spec) / 1e9)


class FetchError(Exception):
    """The toolchain could not be had; the message says what to do."""


class Cancelled(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''): h.update(block)
    return h.hexdigest()


def _wanted(spec, name):
    """The path under the part's folder for archive member `name`, or None when the spec does not take it (or it escapes)."""
    n = name.replace('\\', '/').lstrip('./')
    root = spec.get('root') or ''
    if root:
        if not n.startswith(root + '/'): return None
        n = n[len(root) + 1:]
    p = PurePosixPath(n)
    if not n or p.is_absolute() or '..' in p.parts or ':' in n: return None
    for t in spec['take']:
        if (t.endswith('/') and n.startswith(t)) or n == t: return n
    return None


def unpack(archive, spec, target, cancelled=lambda: False):
    """Check `archive` against the spec's SHA-256, then write the members the spec takes into `target` (replaced as a whole)."""
    archive, target = Path(archive), Path(target)
    if spec.get('sha256') and sha256(archive) != spec['sha256']:
        raise FetchError('%s is not the published %s (its SHA-256 differs)' % (archive.name, spec['archive']))
    if not spec.get('sha256'):
        raise FetchError('%s %s is not pinned in this installer yet' % (spec['archive'], spec['version']))
    tmp = target.with_name(target.name + '.new')
    if tmp.exists(): shutil.rmtree(tmp)
    got = 0
    try:
        if archive.name.lower().endswith('.zip'):
            with zipfile.ZipFile(archive) as z:
                for info in z.infolist():
                    if cancelled(): raise Cancelled()
                    rel = _wanted(spec, info.filename)
                    if rel is None or info.is_dir(): continue
                    out = tmp / rel; out.parent.mkdir(parents=True, exist_ok=True)
                    with z.open(info) as src, open(out, 'wb') as dst: shutil.copyfileobj(src, dst, 1 << 20)
                    got += 1
        else:
            with tarfile.open(archive, 'r|*') as t:          # one pass through the stream: a 1 GB .tar.xz is read once
                for m in t:
                    if cancelled(): raise Cancelled()
                    rel = _wanted(spec, m.name)
                    if rel is None or not m.isfile(): continue
                    out = tmp / rel; out.parent.mkdir(parents=True, exist_ok=True)
                    src = t.extractfile(m)
                    with open(out, 'wb') as dst: shutil.copyfileobj(src, dst, 1 << 20)
                    got += 1
    except (Cancelled, FetchError):
        shutil.rmtree(tmp, ignore_errors=True); raise
    except Exception as e:
        shutil.rmtree(tmp, ignore_errors=True)
        raise FetchError('%s could not be read (%s: %s)' % (archive.name, type(e).__name__, e))
    missing = [t for t in spec['take'] if not t.endswith('/') and not (tmp / t).is_file() and not t.endswith('.TXT')]
    if missing or not got:
        shutil.rmtree(tmp, ignore_errors=True)
        raise FetchError('%s does not hold %s' % (archive.name, ', '.join(missing) or 'the expected files'))
    if target.exists(): shutil.rmtree(target)
    os.replace(tmp, target)


def _part_ok(root, name):
    miss = wine_host.toolchain_missing(root)
    return {'llvm': 'llvm', 'cmake': 'cmake', 'ninja': 'ninja', 'xwin': 'sdk'}[name] not in miss


def _from_source(src, name, spec, target, cancelled):
    """MA2_TOOLCHAIN_SOURCE: the pinned archive, or the part already laid out. True when the part was taken from there."""
    src = Path(wine_host.dos_path(src))
    laid = src / ('xwin' if name == 'xwin' else spec['dest'])
    if laid.is_dir():
        if (target / laid.name).exists(): shutil.rmtree(target / laid.name)
        shutil.copytree(laid, target / laid.name, symlinks=False)
        return True
    a = src / spec['archive']
    if a.is_file():
        unpack(a, spec, target / spec['dest'], cancelled); return True
    return False


def run_xwin(target, log, cancelled):
    """xwin.exe (unpacked under target\\xwin-tool) lays out the Microsoft CRT and Windows SDK in target\\xwin."""
    spec = SPEC['xwin']
    exe = Path(target) / spec['dest'] / 'xwin.exe'
    out, cache = Path(target) / 'xwin', Path(target) / 'xwin-cache'
    args = [str(exe), '--cache-dir', str(cache)] + list(spec['args']) + ['--output', str(out)]
    log('    xwin %s: the Microsoft C runtime and Windows SDK from Microsoft\'s package feed' % spec['version'])
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    tail = []
    for raw in p.stdout:
        if cancelled():
            p.kill(); raise Cancelled()
        line = raw.decode('utf-8', 'replace').rstrip()
        if line: tail = (tail + [line])[-10:]
    if p.wait() != 0:
        raise FetchError('xwin stopped (exit %s): %s' % (p.returncode, ' | '.join(tail[-3:])))
    shutil.rmtree(cache, ignore_errors=True)
    shutil.rmtree(Path(target) / spec['dest'], ignore_errors=True)


def ensure(target, log=print, cancelled=lambda: False, accept_licence=True):
    """Make `target` hold the complete toolchain (wine_host.toolchain_missing() empty). Raises FetchError or Cancelled."""
    target = Path(target); target.mkdir(parents=True, exist_ok=True)
    if not wine_host.toolchain_missing(target): return target
    log('  The C++ toolchain for Linux (LLVM clang %s, CMake, Ninja, the Microsoft C runtime and Windows SDK) is downloaded once and '
        'kept in %s; every archive is checked against its recorded SHA-256' % (SPEC['llvm']['version'], target))
    source = os.environ.get('MA2_TOOLCHAIN_SOURCE', '').strip().strip('"')
    for name in PARTS + ('xwin',):
        if _part_ok(target, name): continue
        if cancelled(): raise Cancelled()
        spec = SPEC[name]
        if source:
            if _from_source(source, name, spec, target, cancelled):
                log('    %s %s: from %s' % (name, spec['version'], source))
                if name != 'xwin' or _part_ok(target, name): continue
            elif name != 'xwin':
                raise FetchError('%s is not in %s (MA2_TOOLCHAIN_SOURCE)' % (spec['archive'], source))
        if name == 'xwin':
            if not accept_licence:
                raise FetchError('The Microsoft C runtime and Windows SDK need your yes to Microsoft\'s licence terms first.')
            xspec = dict(spec)
            if not (target / spec['dest'] / 'xwin.exe').is_file():
                _download(xspec, target, log, cancelled)
            run_xwin(target, log, cancelled)
            if not _part_ok(target, 'xwin'): raise FetchError('xwin finished, but the Windows SDK it laid out is incomplete')
            continue
        _download(spec, target, log, cancelled)
    miss = wine_host.toolchain_missing(target)
    if miss: raise FetchError('the toolchain is still incomplete: %s' % ', '.join(miss))
    log('    toolchain ready: %s' % target)
    return target


def _download(spec, target, log, cancelled):
    if not spec.get('sha256'):
        raise FetchError('%s %s is not pinned in this installer yet, so it is not downloaded. Put %s in the folder named by '
                         'MA2_TOOLCHAIN_SOURCE.' % (spec['archive'], spec['version'], spec['archive']))
    ext = Path(spec['archive']).suffix.lower()
    last = None
    for url in spec['urls']:
        log('    downloading %s' % url)
        try:
            dl.fetch_and_use(url, target / (spec['archive'] + '.download'), lambda a: unpack(a, spec, target / spec['dest'], cancelled),
                             log, cancelled, cache_key='toolchain-%s-%s' % (spec['sha256'][:16], spec['archive']),
                             max_bytes=MAX_BYTES, magic=MAGIC.get(ext))
            return
        except dl.Cancelled:
            raise Cancelled()
        except Cancelled:
            raise
        except Exception as e:
            last = e; log('    not used (%s)' % e)
    raise FetchError('Could not get %s: %s. Check the internet connection and press Resume.' % (spec['archive'], last))

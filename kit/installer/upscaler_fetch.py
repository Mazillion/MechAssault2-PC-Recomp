"""The Vulkan texture upscaler (realesrgan-ncnn-vulkan): downloaded on the player's PC, every file checked before use.

On a PC without an NVIDIA graphics card (or where PyTorch cannot use it) the enhanced textures are made by Real-ESRGAN's
own Windows build of realesrgan-ncnn-vulkan, which runs on any graphics card with Vulkan. The installer does not carry
it: the archive named in data/realesrgan-ncnn-vulkan.json is downloaded from Real-ESRGAN's GitHub release, its SHA-256
is checked, and only the four files the textures need are unpacked, each checked against its own SHA-256 and size.

Where it comes from, in order:
  1. the folder of an earlier run, when every file in it still checks out
  2. MA2_UPSCALER_SOURCE (or the installer's --upscaler option): the .zip, a folder with it extracted, or an
     http(s)/file URL; nothing else is tried then (installs without internet, and tests with a local copy)
  3. the .zip in the install folder or beside the installer exe (downloaded on another PC)
  4. the published URL in data/realesrgan-ncnn-vulkan.json
"""
import hashlib, json, os, shutil, urllib.parse, urllib.request, zipfile
from pathlib import Path

SPEC = json.loads((Path(__file__).resolve().parent / 'data' / 'realesrgan-ncnn-vulkan.json').read_text(encoding='utf-8'))
MAX_BYTES = 256 << 20                     # the archive is about 45 MB; a far bigger answer is not it


class FetchError(Exception):
    """The upscaler could not be had; the message says what to do."""


class MismatchError(FetchError):
    """A source was read, but it is not the expected build (another version, a damaged or altered copy)."""


class Cancelled(Exception):
    pass


def is_url(s):
    return urllib.parse.urlparse(s).scheme.lower() in ('http', 'https', 'file')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''): h.update(block)
    return h.hexdigest()


def verify(root):
    """Files that are missing under `root` or differ from the spec (empty: all good)."""
    root = Path(root)
    return [rel for rel, h in SPEC['files'].items()
            if not (root / rel).is_file() or (root / rel).stat().st_size != SPEC['sizes'][rel] or sha256(root / rel) != h]


def _replace_dir(tmp, target):
    if target.exists(): shutil.rmtree(target)
    os.replace(tmp, target)


def _not_the_build(bad, where):
    return MismatchError('%d file(s) in %s are missing or are not the expected realesrgan-ncnn-vulkan build, e.g. %s'
                         % (len(bad), where, ', '.join(bad[:3])))


def from_folder(folder, target):
    folder = Path(folder)
    bad = verify(folder)
    if bad: raise _not_the_build(bad, folder)
    tmp = target.with_name(target.name + '.new')
    if tmp.exists(): shutil.rmtree(tmp)
    for rel in SPEC['files']:
        (tmp / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(folder / rel, tmp / rel)
    _replace_dir(tmp, target)


def from_zip(zpath, target, cancelled=lambda: False):
    """Check the archive's SHA-256, then take exactly the files named in the spec out of it, checking each as it is
    written (only those paths are ever written). An archive that cannot be read is a FetchError, never another exception."""
    if sha256(zpath) != SPEC['archive_sha256']:
        raise MismatchError('%s is not the published %s (its SHA-256 differs)' % (Path(zpath).name, SPEC['archive']))
    tmp = target.with_name(target.name + '.new')
    if tmp.exists(): shutil.rmtree(tmp)
    bad = []
    try:
        with zipfile.ZipFile(zpath) as z:
            names = {n.replace('\\', '/'): n for n in z.namelist()}
            for rel, h in SPEC['files'].items():
                if cancelled(): raise Cancelled()
                member = names.get(rel)
                if member is None:
                    bad.append(rel); continue
                limit = SPEC['sizes'][rel]
                out = tmp / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                digest, got = hashlib.sha256(), 0
                with z.open(member) as src, open(out, 'wb') as dst:
                    for block in iter(lambda: src.read(1 << 20), b''):
                        got += len(block)
                        if got > limit: break
                        digest.update(block); dst.write(block)
                if digest.hexdigest() != h or got != limit: bad.append(rel)
    except Cancelled:
        shutil.rmtree(tmp, ignore_errors=True); raise
    except Exception as e:
        shutil.rmtree(tmp, ignore_errors=True)
        raise FetchError('%s could not be read as a zip archive (%s: %s)' % (Path(zpath).name, type(e).__name__, e))
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True); raise
    if bad:
        shutil.rmtree(tmp, ignore_errors=True)
        raise _not_the_build(bad, Path(zpath).name)
    _replace_dir(tmp, target)


class _HttpsOnlyRedirect(urllib.request.HTTPRedirectHandler):
    """An https address is never redirected to plain http."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if req.full_url.lower().startswith('https:') and not newurl.lower().startswith('https:'):
            raise FetchError('the server redirected %s to a non-HTTPS address' % req.full_url)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download(url, out, log=print, cancelled=lambda: False):
    """Download `url` to `out` (through out.part, so a stopped download never looks finished)."""
    part = out.with_name(out.name + '.part')
    req = urllib.request.Request(url, headers={'User-Agent': 'MechAssault2-Recomp-Installer'})
    opener = urllib.request.build_opener(_HttpsOnlyRedirect)
    try:
        with opener.open(req, timeout=60) as r, open(part, 'wb') as f:
            total = int(r.headers.get('Content-Length') or 0)
            if total > MAX_BYTES: raise FetchError('the server offers %d MB, far more than the upscaler' % (total >> 20))
            got = 0
            for block in iter(lambda: r.read(1 << 20), b''):
                if cancelled(): raise Cancelled()
                f.write(block); got += len(block)
                if got > MAX_BYTES: raise FetchError('the download is far larger than the upscaler')
        if total and got != total: raise FetchError('the download stopped early (%d of %d bytes)' % (got, total))
        os.replace(part, out)
    except BaseException:
        try: part.unlink()
        except OSError: pass
        raise


def ensure(target, log=print, cancelled=lambda: False, look_in=()):
    """A folder holding the checked upscaler: `target` itself when it already checks out, else filled from the first
    source that works (see the module notes). Raises FetchError (MismatchError when a source was read but held a
    different build), or Cancelled."""
    target = Path(target)
    if target.is_dir() and not verify(target):
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    log('  The Vulkan texture upscaler (%s, about %d MB) is downloaded on this PC, not carried by the installer; '
        'the archive and every file used are checked against their recorded SHA-256' % (SPEC['version'].split(' (')[0], SPEC['archive_bytes'] >> 20))
    override = os.environ.get('MA2_UPSCALER_SOURCE', '').strip().strip('"')
    if override:
        sources = [override]
    else:
        sources = [str(Path(d) / SPEC['archive']) for d in look_in if d and (Path(d) / SPEC['archive']).is_file()]
        sources += list(SPEC['urls'])
    tried, mismatched = [], []
    for src in sources:
        try:
            if is_url(src):
                log('    downloading %s' % src)
                z = target.parent / SPEC['archive']
                download(src, z, log, cancelled)
                try:
                    from_zip(z, target, cancelled)
                finally:
                    z.unlink(missing_ok=True)
            elif Path(src).is_dir():
                log('    from the folder %s' % src); from_folder(src, target)
            elif Path(src).is_file():
                log('    from %s' % src); from_zip(src, target, cancelled)
            else:
                raise FetchError('no such file or folder')
            log('    Vulkan upscaler: archive and %d files checked' % len(SPEC['files']))
            return target
        except Cancelled:
            raise
        except Exception as e:
            log('    not used (%s)' % e)
            tried.append('%s (%s)' % (src, e))
            if isinstance(e, MismatchError): mismatched.append(src)
    url = SPEC['urls'][0]
    if mismatched:
        raise MismatchError(
            'The Vulkan texture upscaler was found, but it is not the expected build (a different version, or a damaged '
            'or altered copy): %s. Delete that file, then download exactly %s and put it in the install folder, or check '
            'the internet connection, then try again. Tried: %s' % ('; '.join(mismatched), url, '; '.join(tried)))
    raise FetchError(
        'Could not get the Vulkan texture upscaler, which is downloaded while the enhanced textures are made. Check the '
        'internet connection and try again. Without internet on this PC: on another PC, download exactly %s, put %s in '
        'the install folder, and try again. Tried: %s' % (url, SPEC['archive'], '; '.join(tried) or 'nothing'))

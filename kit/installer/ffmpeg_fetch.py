"""FFmpeg for the cutscene player: downloaded on the player's PC, every file checked before use.

The installer does not carry FFmpeg. The build the game is made with (data/ffmpeg.json: gyan.dev's FFmpeg 9.0.2
"full" shared build, GPL-3.0) is downloaded from where it is published, and each of its files is checked against
the SHA-256 recorded in data/ffmpeg.json before anything uses it. A file that differs stops the install.

Where the files come from, in order:
  1. the folder of an earlier run, when every file in it still checks out
  2. MA2_FFMPEG_SOURCE (or the installer's --ffmpeg option): a .zip, an extracted folder, or an http(s)/file URL;
     nothing else is tried then (installs without internet, and tests with a local mirror)
  3. ffmpeg-9.0.2-full_build-shared.zip in the install folder or beside the installer exe (downloaded on another PC)
  4. the published URLs in data/ffmpeg.json, first to last
"""
import hashlib, json, os, shutil, urllib.parse, urllib.request, zipfile
from pathlib import Path

SPEC = json.loads((Path(__file__).resolve().parent / 'data' / 'ffmpeg.json').read_text(encoding='utf-8'))
NAME = SPEC['archive_root']
MAX_BYTES = 1 << 30                       # nothing near this size is expected; a bigger answer is not the build


class FetchError(Exception):
    """FFmpeg could not be had; the message says what to do."""


class MismatchError(FetchError):
    """A source was read, but its files are not the expected build (another version, a damaged or altered copy)."""


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
    """Files of the build that are missing under `root` or differ from data/ffmpeg.json (empty: all good)."""
    root = Path(root)
    return [rel for rel, h in SPEC['files'].items() if not (root / rel).is_file() or sha256(root / rel) != h]


def _replace_dir(tmp, target):
    if target.exists(): shutil.rmtree(target)
    os.replace(tmp, target)


def _not_the_build(bad, where):
    return MismatchError('%d file(s) in %s are missing or are not the expected FFmpeg build, e.g. %s'
                         % (len(bad), where, ', '.join(bad[:3])))


def from_folder(folder, target):
    """Copy the build from an extracted folder (the archive's top folder or the folder holding it)."""
    folder = Path(folder)
    if (folder / NAME).is_dir(): folder = folder / NAME
    bad = verify(folder)
    if bad:
        raise _not_the_build(bad, folder)
    tmp = target.with_name(target.name + '.new')
    if tmp.exists(): shutil.rmtree(tmp)
    for rel in SPEC['files']:
        (tmp / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(folder / rel, tmp / rel)
    _replace_dir(tmp, target)


def from_zip(zpath, target, cancelled=lambda: False):
    """Take exactly the files named in data/ffmpeg.json out of the archive, checking each one as it is written.
    Only those paths are ever written, so a crafted archive cannot place files anywhere else. Any archive that cannot
    be read (not a zip, damaged data, an unsupported or encrypted member) is a FetchError, never another exception."""
    want = SPEC.get('archive_sha256')
    if want and sha256(zpath) != want:
        raise MismatchError('%s is not the published %s (its SHA-256 differs)' % (Path(zpath).name, SPEC['archive']))
    tmp = target.with_name(target.name + '.new')
    if tmp.exists(): shutil.rmtree(tmp)
    bad = []
    try:
        with zipfile.ZipFile(zpath) as z:
            names = {n.replace('\\', '/'): n for n in z.namelist()}
            for rel, h in SPEC['files'].items():
                if cancelled(): raise Cancelled()
                member = names.get(NAME + '/' + rel) or names.get(rel)
                if member is None:
                    bad.append(rel); continue
                limit = (SPEC.get('sizes') or {}).get(rel)      # bytes: reading stops past it (no disk-filling archive)
                out = tmp / rel
                out.parent.mkdir(parents=True, exist_ok=True)
                digest, got = hashlib.sha256(), 0
                with z.open(member) as src, open(out, 'wb') as dst:
                    for block in iter(lambda: src.read(1 << 20), b''):
                        got += len(block)
                        if limit is not None and got > limit: break      # longer than the real file: not the build
                        digest.update(block); dst.write(block)
                if digest.hexdigest() != h or (limit is not None and got != limit): bad.append(rel)
    except Cancelled:
        shutil.rmtree(tmp, ignore_errors=True); raise
    except Exception as e:                              # BadZipFile, zlib.error, EOFError, NotImplementedError, RuntimeError...
        shutil.rmtree(tmp, ignore_errors=True)
        raise FetchError('%s could not be read as a zip archive (%s: %s)' % (Path(zpath).name, type(e).__name__, e))
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True); raise
    if bad:
        shutil.rmtree(tmp, ignore_errors=True)
        raise _not_the_build(bad, Path(zpath).name)
    _replace_dir(tmp, target)


class _HttpsOnlyRedirect(urllib.request.HTTPRedirectHandler):
    """An https address is never redirected to plain http (the files are checked anyway; this is defence in depth)."""
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
            if total > MAX_BYTES: raise FetchError('the server offers %d MB, far more than the FFmpeg build' % (total >> 20))
            got, step = 0, 16 << 20
            next_report = step
            for block in iter(lambda: r.read(1 << 20), b''):
                if cancelled(): raise Cancelled()
                f.write(block); got += len(block)
                if got > MAX_BYTES: raise FetchError('the download is far larger than the FFmpeg build')
                if got >= next_report:
                    log('    %d of %s MB' % (got >> 20, (total >> 20) or '?')); next_report += step
        if total and got != total: raise FetchError('the download stopped early (%d of %d bytes)' % (got, total))
        os.replace(part, out)
    except BaseException:
        try: part.unlink()
        except OSError: pass
        raise


def ensure(target, log=print, cancelled=lambda: False, look_in=()):
    """A folder holding the checked FFmpeg build: `target` itself when it already checks out, else filled from the
    first source that works (see the module notes). Raises FetchError with what to do (MismatchError when a source
    was read but held a different build), or Cancelled."""
    target = Path(target)
    if target.is_dir() and not verify(target):
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    log('  FFmpeg %s (cutscene playback) is downloaded on this PC, not carried by the installer; '
        'every file is checked against its recorded SHA-256' % SPEC['version'].split()[0])
    override = os.environ.get('MA2_FFMPEG_SOURCE', '').strip().strip('"')
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
            log('    FFmpeg: %d files checked' % len(SPEC['files']))
            return target
        except Cancelled:
            raise
        except Exception as e:                          # any failure of one source: say so and try the next one
            log('    not used (%s)' % e)
            tried.append('%s (%s)' % (src, e))
            if isinstance(e, MismatchError): mismatched.append(src)
    url = SPEC['urls'][0]
    if mismatched:
        raise MismatchError(
            'FFmpeg was found, but it is not the build the game needs (a different version, or a damaged or altered '
            'copy): %s. Delete that file, then download exactly %s (it must be this version, %s) and put it in the '
            'install folder, or check the internet connection, then press Resume. Tried: %s'
            % ('; '.join(mismatched), url, SPEC['version'].split()[0], '; '.join(tried)))
    raise FetchError(
        'Could not get FFmpeg, which plays the cutscenes and is downloaded during the install (it is not inside the '
        'installer). Check the internet connection and press Resume (finished steps are kept). Without internet on '
        'this PC: on another PC, download exactly %s, put %s in the install folder, and press Resume. Tried: %s'
        % (url, SPEC['archive'], '; '.join(tried) or 'nothing'))

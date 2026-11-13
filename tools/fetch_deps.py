"""Fetch the third-party binaries the installer kit needs, and verify every file by SHA-256.

The repository carries no third-party binaries. Packages are placed into kit/ exactly as listed (file by file,
with hashes):

  capstone  kit/toolkit/vendor/capstone          tools/deps.lock.json        (disassembler used by the toolkit;
                                                                            packed into the installer)
  ffmpeg    kit/source/game/vendor/ffmpeg-9.0.2-full_build-shared            (cutscene playback; GPL-3.0 build)
                                                 kit/installer/data/ffmpeg.json
            NOT packed into the installer: the installer downloads the same build on the player's PC and checks
            every file against the same hashes (kit/installer/ffmpeg_fetch.py). Fetch it here only to build or
            test the game locally (tests that need it say so).

Usage:
  python tools/fetch_deps.py                      download what is missing (needs internet), verify, install
  python tools/fetch_deps.py --from ffmpeg=<dir>  use an already-extracted copy instead of downloading
  python tools/fetch_deps.py --verify             only check what is in kit/ now (FFmpeg: only if present)
  python tools/fetch_deps.py --only capstone      just one package (a release build needs only capstone)

A file whose hash differs from the lock stops the build: the release must be made from exactly the
files the lock names.
"""
import argparse, hashlib, json, shutil, subprocess, sys, tempfile, urllib.request, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LOCK = json.loads((REPO / 'tools' / 'deps.lock.json').read_text(encoding='utf-8'))
LOCK['ffmpeg'] = json.loads((REPO / 'kit' / 'installer' / 'data' / 'ffmpeg.json').read_text(encoding='utf-8'))
PACKAGES = [k for k, v in LOCK.items() if isinstance(v, dict) and 'files' in v]   # 'python' only pins the runtime
OPTIONAL = {'ffmpeg'}                 # not packed: the installer downloads it on the player's PC


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def verify(name, root):
    spec = LOCK[name]
    root = Path(root)
    bad = [rel for rel, h in spec['files'].items() if not (root / rel).is_file() or sha(root / rel) != h]
    return bad


def install(name, src):
    spec = LOCK[name]
    bad = verify(name, src)
    if bad:
        raise SystemExit('%s: %d file(s) missing or different from tools/deps.lock.json, e.g. %s'
                         % (name, len(bad), ', '.join(bad[:3])))
    dest = REPO / spec['dest']
    if dest.exists():
        shutil.rmtree(dest)
    for rel in spec['files']:
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(Path(src) / rel, out)
    print('%s: %d files verified -> %s' % (name, len(spec['files']), spec['dest']))


def download(name, tmp):
    spec = LOCK[name]
    if 'pip' in spec:
        subprocess.run([sys.executable, '-m', 'pip', 'install', '--no-deps', '--only-binary=:all:',
                        '--target', str(tmp / 'pip'), spec['pip']], check=True)
        return tmp / 'pip' / spec['archive_root']
    for url in spec['urls']:
        try:
            print('downloading', url)
            zpath = tmp / spec['archive']
            with urllib.request.urlopen(url, timeout=60) as r, open(zpath, 'wb') as f:
                shutil.copyfileobj(r, f)
            got = sha(zpath)
            if spec.get('archive_sha256') and got != spec['archive_sha256']:
                raise ValueError('archive SHA-256 %s differs from the pinned %s' % (got, spec['archive_sha256']))
            print('  archive sha256 %s%s' % (got, '' if spec.get('archive_sha256') else
                                             '  (record it as "archive_sha256" in the pin file)'))
            with zipfile.ZipFile(zpath) as z:
                z.extractall(tmp / 'x')
            return tmp / 'x' / spec['archive_root']
        except Exception as e:                      # try the next mirror
            print('  failed:', e)
    raise SystemExit('%s: no download worked; get %s yourself and pass --from %s=<extracted folder>'
                     % (name, spec['archive'], name))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--from', dest='sources', action='append', default=[], metavar='NAME=DIR')
    ap.add_argument('--verify', action='store_true')
    ap.add_argument('--only', choices=PACKAGES)
    a = ap.parse_args()
    names = [a.only] if a.only else PACKAGES
    if a.verify:
        ok = True
        for name, spec in ((n, LOCK[n]) for n in names):
            dest = REPO / spec['dest']
            if name in OPTIONAL and not dest.exists() and not a.only:
                print("%s: not present (fine: the installer downloads it on the player's PC)" % name)
                continue
            bad = verify(name, dest)
            print('%s: %s' % (name, 'OK' if not bad else '%d file(s) missing/different' % len(bad)))
            ok &= not bad
        sys.exit(0 if ok else 1)
    given = dict(s.split('=', 1) for s in a.sources)
    with tempfile.TemporaryDirectory(prefix='ma2deps-') as t:
        for name in names:
            if name in given:
                install(name, given[name])
            elif not verify(name, REPO / LOCK[name]['dest']):
                print('%s: already present and verified' % name)
            else:
                install(name, download(name, Path(t)))


if __name__ == '__main__':
    main()

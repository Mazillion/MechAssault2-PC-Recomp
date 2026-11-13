"""Build the release: dist/MechAssault 2 Recomp Installer.exe (+ SHA256SUMS.txt) from this repository.

    py -3.13 tools/build_release.py [--build-dir D:\\ma2-build] [--skip-editor]

Steps (each stops the build on failure):
  1. versions   VERSION, kit/installer/data/release.json, ma2_release.h, editor/src/app/version.h and
                editor/res/editor.rc (numbers and strings) agree; Python is the version pinned in deps.lock
  2. deps       tools/fetch_deps.py --verify --only capstone: Capstone is in kit/ and matches the lock (FFmpeg is
                never packed: the installer downloads it on the player's PC, kit/installer/ffmpeg_fetch.py)
  3. editor     editor/build.cmd in a neutral build folder -> kit/editor/MA2 Scenario Editor.exe
  4. stub       installer-exe/build_stub.cmd in the same build folder
  5. pack       refuse any kit file git does not track (ignored, untracked or modified; the fetched deps and the
                built editor excepted), then installer-exe/pack_installer.py with the running Python 3.13 -> dist/
  6. check      tools/privacy_scan.py over the built binaries, then SHA-256 + size into dist/SHA256SUMS.txt

The compile steps run in a copy of the sources under --build-dir (default %SystemDrive%\\MA2PubBuild) so
no compiler ever sees your user-profile path: MSVC can embed the source folder in a binary.
Requirements: Windows 10/11 x64, Visual Studio 2019+ or Build Tools with the C++ x64 workload, Git
(optional, for the editor's version id), and a python.org Python 3.13 with Tcl/Tk (it is bundled).
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DIST = REPO / 'dist'
EDITOR_EXE = 'MA2 Scenario Editor.exe'


def step(msg):
    print('\n== ' + msg, flush=True)


def run(cmd, cwd, env=None):
    r = subprocess.run(cmd, cwd=str(cwd), env=env, shell=isinstance(cmd, str))
    if r.returncode:
        raise SystemExit('failed (%d): %s' % (r.returncode, cmd))


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()


def versions():
    v = [l.strip() for l in (REPO / 'VERSION').read_text(encoding='utf-8').splitlines() if l.strip()]
    rel = json.loads((REPO / 'kit' / 'installer' / 'data' / 'release.json').read_text(encoding='utf-8'))
    ed = (REPO / 'editor' / 'src' / 'app' / 'version.h').read_text(encoding='utf-8')
    m = re.search(r'MA2SE_VERSION\s+"([^"]+)"', ed)
    game = (REPO / 'kit' / 'source' / 'game' / 'src' / 'ma2_release.h').read_text(encoding='utf-8')
    g = re.search(r'MA2_PUBLIC_VERSION\s+"([^"]+)"', game)
    got = {'VERSION': v[0], 'release.json': rel['version'], 'editor': m and m.group(1), 'game': g and g.group(1)}
    nums = re.search(r'MA2_VERSION_MAJOR\s+(\d+).*?MA2_VERSION_MINOR\s+(\d+).*?MA2_VERSION_PATCH\s+(\d+)', game, re.S)
    if nums:
        got['game numeric'] = '.'.join(nums.groups())
    rc = (REPO / 'editor' / 'res' / 'editor.rc').read_text(encoding='utf-8')
    for key in ('FileVersion', 'ProductVersion'):
        s = re.search(r'VALUE "%s", "([^"\\]*)' % key, rc)
        got['editor.rc ' + key] = s and s.group(1)
    for key in ('FILEVERSION', 'PRODUCTVERSION'):
        s = re.search(r'^\s*%s (\d+),(\d+),(\d+),0' % key, rc, re.M)
        got['editor.rc ' + key] = s and '.'.join(s.groups())
    if len(set(got.values())) != 1:
        raise SystemExit('version mismatch: %r' % got)
    if rel.get('date') != v[1]:
        raise SystemExit('VERSION date %s differs from release.json %s' % (v[1], rel.get('date')))
    print('version %s (%s)' % (v[0], v[1]))
    return v[0]


# Files under kit/ that git does not track but the payload needs: fetched and hash-checked by tools/fetch_deps.py
# (step 2), or built from editor/ (step 3). Everything else in the payload must be a file git tracks, unchanged.
NOT_IN_GIT = ('kit/toolkit/vendor/capstone/', 'kit/editor/')


def kit_is_git(allow_dirty=False):
    """The release is reproducible from the repository: refuse to pack an ignored, untracked or modified kit file."""
    sys.path.insert(0, str(REPO / 'installer-exe'))
    import pack_installer
    sys.path.pop(0)
    packed = ['kit/' + rel for rel, p in pack_installer.files() if not rel.startswith('python/')]
    ff = [f for f in packed if '/vendor/ffmpeg' in f.lower() or Path(f).name.lower().startswith(('avcodec', 'avformat', 'ffmpeg.exe'))]
    if ff:
        raise SystemExit("refusing to pack FFmpeg (the installer downloads it on the player's PC): %s" % ', '.join(ff[:3]))
    r = subprocess.run(['git', 'ls-files', '-z', '--', 'kit'], cwd=str(REPO), capture_output=True)
    if r.returncode:
        raise SystemExit('Build from a git clone: the payload is checked against `git ls-files` (%s)' % r.stderr.decode(errors='replace').strip())
    tracked = set(f for f in r.stdout.decode('utf-8').split('\0') if f)
    extra = [f for f in packed if f not in tracked and not f.startswith(NOT_IN_GIT)]
    if extra:
        raise SystemExit('refusing to pack %d file(s) under kit/ that git does not track (ignored or untracked), e.g. %s. '
                         'Commit them, or delete them if they are local leftovers.' % (len(extra), ', '.join(extra[:5])))
    packed_dirs = tuple('kit/%s/' % d for d in pack_installer.KIT_DIRS)      # kit/README.md etc. are repo docs, not payload
    gone = sorted(f for f in tracked - set(packed) if f.startswith(packed_dirs))
    if gone:
        raise SystemExit('tracked kit files are missing on disk: %s' % ', '.join(gone[:5]))
    r = subprocess.run(['git', 'status', '--porcelain', '--', 'kit'], cwd=str(REPO), capture_output=True, text=True)
    dirty = [l[3:] for l in r.stdout.splitlines() if l and not l.startswith('??')]
    if dirty and not allow_dirty:
        raise SystemExit('refusing to pack modified kit files (commit them first, or pass --allow-dirty for a test build): %s'
                         % ', '.join(dirty[:5]))
    print('kit: %d files, all tracked by git%s (plus %d fetched or built)' % (
        sum(1 for f in packed if f in tracked), ', %d modified (--allow-dirty)' % len(dirty) if dirty else ', unmodified',
        sum(1 for f in packed if f.startswith(NOT_IN_GIT))))


def neutral_copy(build, rels):
    for rel in rels:
        src, dst = REPO / rel, build / rel
        if dst.exists():
            shutil.rmtree(dst) if dst.is_dir() else dst.unlink()
        if src.is_dir():
            shutil.copytree(src, dst, ignore=shutil.ignore_patterns('obj', 'out', '__pycache__', 'stub.exe'))
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build-dir', default=os.path.join(os.environ.get('SystemDrive', 'C:') + '\\', 'MA2PubBuild'))
    ap.add_argument('--skip-editor', action='store_true', help='reuse kit/editor/%s' % EDITOR_EXE)
    ap.add_argument('--allow-dirty', action='store_true', help='test builds only: pack modified (still tracked) kit files')
    a = ap.parse_args()
    want = json.loads((REPO / 'tools' / 'deps.lock.json').read_text(encoding='utf-8'))['python']['version']
    have = '%d.%d.%d' % sys.version_info[:3]
    if have != want:
        raise SystemExit('Run with Python %s (py -3.13 tools/build_release.py): it is the runtime the installer bundles, '
                         'pinned in tools/deps.lock.json. This is %s.' % (want, have))
    build = Path(a.build_dir)
    if not str(build).isascii() or os.environ.get('USERNAME', '\0').lower() in str(build).lower():
        raise SystemExit('--build-dir must be an ASCII path outside your user profile')
    build.mkdir(parents=True, exist_ok=True)

    step('1 versions'); version = versions()
    step('2 deps'); run([sys.executable, str(REPO / 'tools' / 'fetch_deps.py'), '--verify', '--only', 'capstone'], REPO)

    step('3 editor')
    if a.skip_editor and (REPO / 'kit' / 'editor' / EDITOR_EXE).is_file():
        print('reusing kit/editor')
    else:
        neutral_copy(build, ['editor', 'kit/source/game/vendor/imgui-1.92.5'])
        env = dict(os.environ)                        # the editor's About box shows the short commit id
        r = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=str(REPO), capture_output=True, text=True)
        if r.returncode == 0:
            env['MA2SE_GIT_ID'] = r.stdout.strip()
        run('"%s"' % (build / 'editor' / 'build.cmd'), build / 'editor', env)
        (REPO / 'kit' / 'editor').mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / 'editor' / 'out' / EDITOR_EXE, REPO / 'kit' / 'editor' / EDITOR_EXE)

    step('4 stub')
    neutral_copy(build, ['installer-exe', 'VERSION'])
    run('"%s"' % (build / 'installer-exe' / 'build_stub.cmd'), build / 'installer-exe')
    shutil.copy2(build / 'installer-exe' / 'stub.exe', REPO / 'installer-exe' / 'stub.exe')

    step('5 pack')
    kit_is_git(a.allow_dirty)
    DIST.mkdir(exist_ok=True)
    out = DIST / 'MechAssault 2 Recomp Installer.exe'
    env = dict(os.environ, MA2_PACK_OUT=str(out), MA2_PYTHON_HOME=sys.base_prefix)
    run([sys.executable, '-I', str(REPO / 'installer-exe' / 'pack_installer.py')], REPO, env)

    step('6 check')
    run([sys.executable, str(REPO / 'tools' / 'privacy_scan.py'), '--binaries', str(out),
         str(REPO / 'kit' / 'editor' / EDITOR_EXE), str(REPO / 'installer-exe' / 'stub.exe')], REPO)
    lines = ['%s *%s' % (sha(out), out.name)]
    (DIST / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n', encoding='ascii')
    print('\n%s  %d bytes\nsha256 %s\nversion %s' % (out, out.stat().st_size, sha(out), version))


if __name__ == '__main__':
    main()

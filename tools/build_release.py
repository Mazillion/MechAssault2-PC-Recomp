"""Build the release: dist/MechAssault 2 Recomp Installer.exe and its RecompLauncher zip (+ SHA256SUMS.txt) from this repository.

    py -3.13 tools/build_release.py [--build-dir D:\\ma2-build] [--skip-editor]

Steps (each stops the build on failure):
  1. versions   VERSION, kit/installer/data/release.json, ma2_release.h, editor/src/app/version.h and
                editor/res/editor.rc (numbers and strings) agree (a preview's "-preview.N" is in the strings only);
                Python is the version pinned in deps.lock
  2. deps       tools/fetch_deps.py --verify --only capstone: Capstone is in kit/ and matches the lock (FFmpeg is
                never packed: the installer downloads it on the player's PC, kit/installer/ffmpeg_fetch.py)
  3. editor     editor/build.cmd in a neutral build folder -> kit/editor/MA2 Scenario Editor.exe
  4. stub       installer-exe/build_stub.cmd in the same build folder; report-helper/build_report.cmd -> kit/report/MA2 Report.exe
                (its relay address comes from kit/installer/data/report.json, the one place it is set)
  5. pack       refuse any kit file git does not track (ignored, untracked or modified; the fetched deps and the
                built editor excepted), then installer-exe/pack_installer.py with the running Python 3.13 -> dist/
  6. zip        dist/MechAssault2-PC-Recomp-v<version>-Windows-x64.zip for RecompLauncher: recomplauncher.json (from the
                repository top) and the installer exe byte for byte, nothing else (never a pc_settings.ini: the launcher
                unpacks every update over the install). The installer exe itself stays the release's main download.
  7. check      tools/privacy_scan.py over the built binaries and the zip, then SHA-256 + size of both into dist/SHA256SUMS.txt

The compile steps run in a copy of the sources under --build-dir (default %SystemDrive%\\MA2PubBuild) so
no compiler ever sees your user-profile path: MSVC can embed the source folder in a binary.
Requirements: Windows 10/11 x64, Visual Studio 2019+ or Build Tools with the C++ x64 workload, Git
(optional, for the editor's version id), and a python.org Python 3.13 with Tcl/Tk (it is bundled).
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DIST = REPO / 'dist'
EDITOR_EXE = 'MA2 Scenario Editor.exe'
REPORT_EXE = 'MA2 Report.exe'
INSTALLER_EXE = 'MechAssault 2 Recomp Installer.exe'
LAUNCHER_JSON = 'recomplauncher.json'            # RecompLauncher's profile: how it installs, updates and starts the game


def launcher_zip_name(version):
    """The release zip RecompLauncher picks for Windows (it reads "Windows" and "x64" in the name). The .exe stays as it is."""
    return 'MechAssault2-PC-Recomp-v%s-Windows-x64.zip' % version


def check_launcher_zip(path, exe):
    """Problems with a RecompLauncher zip (empty list: fine): exactly recomplauncher.json and the installer at the top, the
    installer byte for byte `exe`, and a profile that parses and runs that installer as its setup step."""
    probs = []
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            if sorted(names) != sorted([LAUNCHER_JSON, INSTALLER_EXE]):
                probs.append('members %r, expected exactly %r' % (names, [LAUNCHER_JSON, INSTALLER_EXE]))
            if any(Path(n).name.lower() == 'pc_settings.ini' for n in names):
                probs.append("a pc_settings.ini is in the zip: every update would replace the player's settings")
            if INSTALLER_EXE in names and hashlib.sha256(z.read(INSTALLER_EXE)).hexdigest() != sha(exe):
                probs.append('the installer in the zip differs from %s' % Path(exe).name)
            if LAUNCHER_JSON in names:
                prof = json.loads(z.read(LAUNCHER_JSON).decode('utf-8'))
                if prof.get('format') != 1:
                    probs.append('recomplauncher.json: format must be 1')
                if (prof.get('prepare') or {}).get('program') != [INSTALLER_EXE]:
                    probs.append('recomplauncher.json: prepare.program must be [%r]' % INSTALLER_EXE)
                if INSTALLER_EXE not in ((prof.get('start') or {}).get('program') or []):      # 1.2.0: a file the zip has
                    probs.append("recomplauncher.json: start.program must also list %r (the built game is not in the zip: "
                                 "without a file of the release there, RecompLauncher's Developer tab shows a red problem)" % INSTALLER_EXE)
            bad = z.testzip()
            if bad:
                probs.append('damaged member %s' % bad)
    except (OSError, ValueError, zipfile.BadZipFile) as e:
        probs.append('cannot read %s: %s' % (Path(path).name, e))
    return probs


def make_launcher_zip(exe, profile, out, date):
    """`out`: the profile and the installer, every member dated the release date (the same inputs give the same zip).
    `date` is VERSION line 2 (YYYY.MM.DD). Stops the build when the result fails check_launcher_zip."""
    json.loads(Path(profile).read_text(encoding='utf-8'))          # a broken profile stops the build here, not on a player's PC
    when = tuple(int(x) for x in date.split('.')[:3]) + (0, 0, 0)
    tmp = Path(str(out) + '.tmp')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for src, name in ((profile, LAUNCHER_JSON), (exe, INSTALLER_EXE)):
            info = zipfile.ZipInfo(name, when)
            info.compress_type, info.external_attr = zipfile.ZIP_DEFLATED, 0o644 << 16
            with open(src, 'rb') as f, z.open(info, 'w') as dst:
                shutil.copyfileobj(f, dst, 1 << 20)
    probs = check_launcher_zip(tmp, exe)
    if probs:
        tmp.unlink()
        raise SystemExit('the RecompLauncher zip is wrong: %s' % '; '.join(probs))
    os.replace(tmp, out)
    return out


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
    # A preview build (tools/release.py preview) carries "1.2.3-preview.4" in the version strings; the version numbers
    # (game numeric, editor.rc FILEVERSION/PRODUCTVERSION) stay 1.2.3.
    core = re.match(r'^(\d+\.\d+\.\d+)(?:-preview\.\d+)?$', v[0] or '')
    if not core:
        raise SystemExit('VERSION line 1 must be X.Y.Z or X.Y.Z-preview.N, not %r' % v[0])
    numeric = {k: s for k, s in got.items() if k in ('game numeric', 'editor.rc FILEVERSION', 'editor.rc PRODUCTVERSION')}
    strings = {k: s for k, s in got.items() if k not in numeric}
    if len(set(strings.values())) != 1 or set(numeric.values()) - {core.group(1)}:
        raise SystemExit('version mismatch: %r' % got)
    if rel.get('date') != v[1]:
        raise SystemExit('VERSION date %s differs from release.json %s' % (v[1], rel.get('date')))
    print('version %s (%s)' % (v[0], v[1]))
    return v[0]


# Files under kit/ that git does not track but the payload needs: fetched and hash-checked by tools/fetch_deps.py
# (step 2), or built from editor/ (step 3). Everything else in the payload must be a file git tracks, unchanged.
NOT_IN_GIT = ('kit/toolkit/vendor/capstone/', 'kit/editor/', 'kit/report/')


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
    neutral_copy(build, ['report-helper'])
    relay = json.loads((REPO / 'kit' / 'installer' / 'data' / 'report.json').read_text(encoding='utf-8')).get('relay_url') or ''
    if relay and not relay.lower().startswith('https://'):
        raise SystemExit('kit/installer/data/report.json: relay_url must be an https:// address (or empty)')
    run('"%s"' % (build / 'report-helper' / 'build_report.cmd'), build / 'report-helper', dict(os.environ, MA2_RELAY_URL=relay))
    (REPO / 'kit' / 'report').mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / 'report-helper' / REPORT_EXE, REPO / 'kit' / 'report' / REPORT_EXE)

    step('5 pack')
    kit_is_git(a.allow_dirty)
    DIST.mkdir(exist_ok=True)
    out = DIST / INSTALLER_EXE
    env = dict(os.environ, MA2_PACK_OUT=str(out), MA2_PYTHON_HOME=sys.base_prefix)
    run([sys.executable, '-I', str(REPO / 'installer-exe' / 'pack_installer.py')], REPO, env)

    step('6 zip')
    date = [l.strip() for l in (REPO / 'VERSION').read_text(encoding='utf-8').splitlines() if l.strip()][1]
    for old in DIST.glob(launcher_zip_name('*')):
        old.unlink()                              # (an earlier version's zip is not this release's)
    zpath = make_launcher_zip(out, REPO / LAUNCHER_JSON, DIST / launcher_zip_name(version), date)
    print('%s: %s and %s' % (zpath.name, LAUNCHER_JSON, out.name))

    step('7 check')
    run([sys.executable, str(REPO / 'tools' / 'privacy_scan.py'), '--binaries', str(out), str(zpath),
         str(REPO / 'kit' / 'editor' / EDITOR_EXE), str(REPO / 'installer-exe' / 'stub.exe'), str(REPO / 'kit' / 'report' / REPORT_EXE)], REPO)
    lines = ['%s *%s' % (sha(p), p.name) for p in (out, zpath)]
    (DIST / 'SHA256SUMS.txt').write_text('\n'.join(lines) + '\n', encoding='ascii')
    for p in (out, zpath):
        print('\n%s  %d bytes\nsha256 %s' % (p, p.stat().st_size, sha(p)))
    print('version %s' % version)


if __name__ == '__main__':
    main()

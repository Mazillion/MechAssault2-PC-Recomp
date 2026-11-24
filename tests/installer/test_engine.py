"""Unit tests for precheck.py, progress.py and the install.py engine pieces (no compile, no disc).
Run with the kit's Python:  python -B test_engine.py
"""
import hashlib, json, os, re, shutil, struct, subprocess, sys, tempfile, time, unittest
from pathlib import Path

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
XBE = os.environ.get('MA2_TEST_XBE')            # your own disc's ma2.xbe (only test_extract_copies_a_folder needs it)
sys.path.insert(0, str(KIT / 'toolkit' / 'tools' / 'xiso'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
os.environ['MA2_SHORTCUT_DIR'] = tempfile.mkdtemp(prefix='ma2ux-lnk-', dir=str(WORK / 'tmp'))
import precheck, progress, install                                          # noqa: E402
from test_xdvdfs import _build_image                                        # noqa: E402
# Throwaway install folders: not under %TEMP% (the installer refuses it). The repo's tests/tmp (ignored by git), or
# MA2_TEST_BASE.
TEST_BASE = Path(os.environ.get('MA2_TEST_BASE') or Path(__file__).resolve().parents[2] / 'tests' / 'tmp') / 'eng'

def _test_dir(case, name):
    d = TEST_BASE / ('%s-%d' % (name, os.getpid()))
    shutil.rmtree(d, ignore_errors=True)
    try: d.mkdir(parents=True)
    except OSError as e: case.skipTest('cannot create %s (%s); set MA2_TEST_BASE' % (d, e))
    return d

def fake_xbe(title=0x4D53006B, region=7, version=6, pad=0):
    h = bytearray(0x800)
    h[0:4] = b'XBEH'
    struct.pack_into('<I', h, 0x104, 0x10000); struct.pack_into('<I', h, 0x118, 0x10000 + 0x184)
    struct.pack_into('<I', h, 0x184 + 8, title); struct.pack_into('<I', h, 0x184 + 0xA0, region); struct.pack_into('<I', h, 0x184 + 0xAC, version)
    return bytes(h) + b'\x01' * pad

def release_for(data):
    return {'xbe': 'ma2.xbe', 'xbe_sha256': hashlib.sha256(data).hexdigest(), 'xbe_bytes': len(data)}

class Precheck(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-', dir=str(WORK / 'tmp')))
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def img(self, name, data, **kw):
        p = self.tmp / name
        p.write_bytes(_build_image(files=(("ma2.xbe", data),), **kw)); return p

    def test_clean_path(self):
        self.assertEqual(precheck.clean_path('  "C:\\a b\\c.iso" '), 'C:\\a b\\c.iso')
        self.assertEqual(precheck.clean_path('\u201cD:\\x\u201d'), 'D:\\x')
        self.assertTrue(precheck.is_absolute('C:\\x') and precheck.is_absolute('\\\\srv\\share') and not precheck.is_absolute('x\\y'))

    def test_image_ok_and_redump_offset(self):
        data = fake_xbe(); rel = release_for(data)
        for base in (0, 0x18300000):
            if base: continue                  # a 400 MB sparse file is slow to build here; offset support is xdvdfs' own test
            r = precheck.validate_image(self.img('a.iso', data, base=base), rel)
            self.assertTrue(r.ok, r.message); self.assertIn('Disc image OK', r.message)

    def test_image_errors(self):
        data = fake_xbe(); rel = release_for(data)
        (self.tmp / 'e.iso').write_bytes(b''); self.assertEqual(precheck.validate_image(self.tmp / 'e.iso', rel).code, 'empty')
        (self.tmp / 'z.iso').write_bytes(b'PK\x03\x04' + b'0' * 100); r = precheck.validate_image(self.tmp / 'z.iso', rel)
        self.assertEqual(r.code, 'archive'); self.assertIn('compressed archive', r.message)
        (self.tmp / 's.7z').write_bytes(b'7z\xbc\xaf\x27\x1c' + b'0' * 100); self.assertEqual(precheck.validate_image(self.tmp / 's.7z', rel).code, 'archive')
        (self.tmp / 'c.cso').write_bytes(b'CISO' + b'0' * 100); self.assertEqual(precheck.validate_image(self.tmp / 'c.cso', rel).code, 'compressed_image')
        (self.tmp / 'junk.iso').write_bytes(os.urandom(5000)); r = precheck.validate_image(self.tmp / 'junk.iso', rel)
        self.assertEqual(r.code, 'not_xbox'); self.assertIn('isn\'t an original-Xbox disc image', r.message)
        self.assertEqual(precheck.validate_image(self.tmp / 'nope.iso', rel).code, 'missing')
        p = self.tmp / 'nox.iso'; p.write_bytes(_build_image(files=(("default.xbe", fake_xbe(title=0x12345678)),)))
        r = precheck.validate_image(p, rel); self.assertEqual(r.code, 'no_xbe'); self.assertIn('12345678', r.message)
        r = precheck.validate_image(self.img('t.iso', fake_xbe(title=0x11112222), ), rel); self.assertEqual(r.code, 'wrong_title'); self.assertIn('11112222', r.message)
        r = precheck.validate_image(self.img('v.iso', fake_xbe(version=3)), rel); self.assertEqual(r.code, 'wrong_version'); self.assertIn('different region or version', r.message)
        r = precheck.validate_image(self.img('r.iso', fake_xbe(region=1)), rel); self.assertEqual(r.code, 'wrong_version')
        r = precheck.validate_image(self.img('m.iso', fake_xbe(pad=64)), rel); self.assertEqual(r.code, 'modified'); self.assertIn('modified or patched', r.message)
        good = self.img('g.iso', data); b = good.read_bytes()
        (self.tmp / 'trunc.iso').write_bytes(b[:len(b) - 2048 * 3]); r = precheck.validate_image(self.tmp / 'trunc.iso', rel)
        self.assertIn(r.code, ('truncated', 'read_error', 'not_xbox'))
        # truncated mid xbe: image cut so that the xbe's sectors run past the end
        cut = self.tmp / 'cut.iso'; cut.write_bytes(_build_image(files=(("ma2.xbe", data), ("big.bin", b'x' * 100000)))[:-60000])
        r = precheck.validate_image(cut, rel); self.assertEqual(r.code, 'truncated'); self.assertIn('ends early', r.message)

    def test_folder_checks(self):
        c = lambda p, **kw: precheck.check_folder(p, **kw)
        self.assertIn('relative', c('MechAssault 2').codes()); self.assertIn('driveroot', c('D:\\').codes())
        self.assertIn('protected', c(os.environ.get('ProgramFiles', 'C:\\Program Files') + '\\MA2').codes())
        self.assertIn('temp', c(tempfile.gettempdir() + '\\MA2').codes())
        self.assertIn('empty', c('').codes())
        r = c('"%s"' % (self.tmp / 'new one')); self.assertTrue(r.ok, r.codes()); self.assertEqual(r.existing, 'none')
        (self.tmp / 'full').mkdir(); (self.tmp / 'full' / 'x.txt').write_text('x')
        r = c(str(self.tmp / 'full')); self.assertIn('notempty', r.codes()); self.assertTrue(r.suggest.endswith('MechAssault 2 Expanded'))
        r = c(str(self.tmp / ('José ' + 'x' * 5))); self.assertIn('tempbuild', r.codes())
        r = c(str(self.tmp / 'Lone Wolf (World) (En,Ja)' / 'MA2')); self.assertIn('tempbuild', r.codes()); self.assertTrue(r.ok, r.codes())
        self.assertNotIn('tempbuild', c(str(self.tmp / 'MechAssault 2 - plain_name.v2')).codes())
        real_lp = precheck.long_paths_enabled; precheck.long_paths_enabled = lambda: False          # (Windows' long paths off: the default)
        try: r = c(str(self.tmp / ('p' * 150))); self.assertIn('long', r.codes())
        finally: precheck.long_paths_enabled = real_lp
        r = c(str(self.tmp / ('p' * 60)), enhanced=True)
        r = c(str(self.tmp / 'q'), need_bytes=10 ** 18); self.assertIn('space', r.codes())
        os.environ['OneDrive'] = str(self.tmp / 'OD'); r = c(str(self.tmp / 'OD' / 'Games' / 'MA2')); self.assertIn('onedrive', r.codes()); del os.environ['OneDrive']
        r = c('C:\\Users\\x\\OneDrive\\Desktop\\MA2'); self.assertIn('onedrive', r.codes())
        # existing states
        d = self.tmp / 'inst'
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text('x')
        r = c(str(d)); self.assertEqual(r.existing, 'install'); self.assertTrue(r.ok)
        (d / '_work').mkdir(); (d / '_work' / 'state.json').write_text(json.dumps({'done': ['analyze']}))
        r = c(str(d)); self.assertEqual(r.existing, 'unfinished'); self.assertEqual(r.state['done'], ['analyze'])
        # read-only folder
        ro = self.tmp / 'ro'; ro.mkdir(); subprocess.run(['icacls', str(ro), '/deny', '%s:(WD,AD)' % os.environ['USERNAME']], capture_output=True)
        r = c(str(ro / 'sub')); subprocess.run(['icacls', str(ro), '/remove:d', os.environ['USERNAME']], capture_output=True)
        self.assertIn('readonly', r.codes())

    def test_ini_merge(self):
        defaults = '[Display]\nWidth=1\nNewKey=5\n\n[Bots]\n; note\nPathSlice=1\nSnap=1\n'
        live = '[Display]\r\nWidth=99\r\n; mine\r\n\r\n[Input]\r\nA=1\r\n'
        new, added = precheck.merge_ini(defaults, live)
        self.assertIn('Width=99', new); self.assertNotIn('Width=1', new); self.assertIn('NewKey=5', new)
        self.assertIn('[Bots]', new); self.assertIn('PathSlice=1', new); self.assertIn('; mine', new)
        self.assertEqual(sorted(added), ['Bots.pathslice', 'Bots.snap', 'Display.newkey'])
        self.assertTrue(new.index('NewKey=5') < new.index('[Input]'))
        again, added2 = precheck.merge_ini(defaults, new); self.assertEqual(added2, []); self.assertEqual(again, new)
        self.assertIn('TextureQuality=2', precheck.set_ini_value('a=1\nTextureQuality=0\n', 'TextureQuality', 2))

    def test_redact(self):
        u = os.environ['USERNAME']
        out = precheck.support_bundle(['C:\\Users\\%s\\x' % u, 'hello'], 'build', True, '1', 'k', [], '')
        self.assertNotIn(u, out) if len(u) > 2 else None; self.assertIn('hello', out)
        self.assertIn('Failed stage: build', out)
        la = os.environ['LOCALAPPDATA']                     # N2: profile folders become tokens, other accounts <user>
        t = precheck.redact('log file: %s\\MechAssault2\\Logs\\install.log; old C:\\Users\\Some One\\x.iso' % la)
        self.assertIn('%LOCALAPPDATA%\\MechAssault2', t); self.assertIn('C:\\Users\\<user>\\x.iso', t); self.assertNotIn(la, t)

    def test_support_url_placeholder(self):
        """W-52 / D-11: one MA2_SUPPORT_URL; empty means the fallback wording, set means the URL, in the bundle and the advice."""
        public_url = precheck.MA2_SUPPORT_URL; precheck.MA2_SUPPORT_URL = ''       # check the empty form too
        out = precheck.support_bundle([], 'build', True, '1', 'k', [], '')
        self.assertIn(precheck.SUPPORT_FALLBACK, out.splitlines()[0]); self.assertIn(precheck.SUPPORT_FALLBACK, precheck.report_hint())
        precheck.MA2_SUPPORT_URL = 'https://example.invalid/report'
        try:
            self.assertIn('https://example.invalid/report', precheck.support_bundle([], '', True, '1', 'k', [], '').splitlines()[0])
            self.assertIn('Report a problem', precheck.report_hint())
        finally:
            precheck.MA2_SUPPORT_URL = public_url

    def test_internet_check_falls_back_to_urllib(self):
        """W-61: behind an HTTP proxy a raw socket to aka.ms fails although downloads work; urllib follows the proxy."""
        import socket, urllib.error, urllib.request
        real = (socket.create_connection, urllib.request.urlopen)
        class Resp:
            def close(self): pass
        try:
            socket.create_connection = lambda *a, **k: (_ for _ in ()).throw(OSError('blocked'))
            urllib.request.urlopen = lambda *a, **k: Resp()
            self.assertTrue(precheck.internet_ok(), 'urllib reached Microsoft')
            urllib.request.urlopen = lambda *a, **k: (_ for _ in ()).throw(urllib.error.HTTPError('https://aka.ms', 404, 'x', {}, None))
            self.assertTrue(precheck.internet_ok(), 'an HTTP error page is still an answer')
            urllib.request.urlopen = lambda *a, **k: (_ for _ in ()).throw(urllib.error.URLError('no route'))
            self.assertFalse(precheck.internet_ok(), 'offline')
            socket.create_connection = lambda *a, **k: Resp()
            urllib.request.urlopen = lambda *a, **k: (_ for _ in ()).throw(AssertionError('not needed when the socket works'))
            self.assertTrue(precheck.internet_ok())
        finally:
            socket.create_connection, urllib.request.urlopen = real

    def test_estimates_and_recommend(self):
        G = precheck.GB
        self.assertTrue(10 <= precheck.estimate_minutes(24, 20 * G) <= 20)
        self.assertGreater(precheck.estimate_minutes(4, 4 * G), precheck.estimate_minutes(24, 20 * G))
        mk = lambda name, gb, v='nvidia': [{'name': name, 'vram': int(gb * G), 'vendor': v}]
        self.assertEqual(precheck.recommend_textures(mk('RTX 4070', 12))['rec'], '4x')
        self.assertEqual(precheck.recommend_textures(mk('RTX 3060 Ti', 8))['rec'], '2x')
        r = precheck.recommend_textures(mk('GTX 1650', 4)); self.assertEqual((r['rec'], r['can']), ('original', False))
        # 1.1.2: other graphics cards make the textures through Vulkan (not the videos); Original stays the advice
        r = precheck.recommend_textures(mk('Intel UHD 620', 1, 'intel')); self.assertEqual((r['rec'], r['can'], r['videos'], r['engine']), ('original', True, True, 'vulkan')); self.assertIn('Vulkan', r['reason'])      # 1.1.3: videos through Vulkan too
        self.assertEqual(precheck.recommend_textures([])['rec'], 'original')

    def test_machine_facts(self):
        self.assertTrue(precheck.windows_info()['ok'])
        self.assertIsInstance(precheck.on_battery(), bool)
        self.assertIn(precheck.smart_app_control(), (None, 'enforce', 'eval', 'off'))
        self.assertIn(precheck.d3d11_ok(), (True, False, None))
        g = precheck.detect_gpus(); self.assertIsInstance(g, list)
        print('  gpus:', [(x['name'], round(x['vram'] / precheck.GB, 1)) for x in g], 'd3d11', precheck.d3d11_ok(), 'sac', precheck.smart_app_control())

class Progress(unittest.TestCase):
    def test_model(self):
        now = [0.0]
        steps = ['check', 'disc', 'extract', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets', 'install', 'finish']
        m = progress.ProgressModel(steps, clock=lambda: now[0])
        self.assertIsNone(m.eta())
        for s, dur in (('check', 1), ('disc', 1), ('extract', 20), ('analyze', 85)):
            m.step(s); now[0] += dur
        m.step('recompile')
        self.assertIsNotNone(m.eta()); self.assertAlmostEqual(m.pace(), 1.0, delta=0.1)
        now[0] += 225; m.step('generate'); now[0] += 80; m.step('patch'); now[0] += 3; m.step('build')
        f0 = m.fraction(); self.assertGreater(f0, 0.4)
        now[0] += 100; m.feed('  [100/215] Building C object x.obj')
        self.assertEqual(m.detail['build'], 'Compiling 100 of 215 files'); self.assertGreater(m.fraction(), f0)
        eta = m.eta(); self.assertTrue(60 < eta < 900, eta)
        self.assertEqual(m.stage_status('analyze'), 'done'); self.assertEqual(m.stage_status('compile'), 'active')
        self.assertEqual(m.stage_status('menus'), 'pending')
        m.step('assets'); m.complete(); self.assertAlmostEqual(m.fraction(), 1.0)
        self.assertEqual(progress.fmt_eta(30), 'Less than a minute left'); self.assertEqual(progress.fmt_eta(540), 'About 9 min left')
        self.assertEqual(progress.fmt_eta(3 * 3600), 'About 3.0 h left'); self.assertEqual(progress.fmt_eta(None), 'Estimating...')

    def test_kept_and_enhanced(self):
        m = progress.ProgressModel(['check', 'analyze', 'finish', 'textures-2x'])
        m.step('check', True); m.step('analyze', True); self.assertEqual(m.stage_status('analyze'), 'kept')
        m.step('textures-2x'); m.feed('  1200/5000 textures'); self.assertEqual(m.detail['textures-2x'], 'Enhanced 1200 of 5000')

class GenPatch(unittest.TestCase):
    """W-53: the patch files carry only added lines (no recompiled game code), and still reproduce the release byte for byte."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-gp-', dir=str(WORK / 'tmp')))
        import gen_patch; self.gp = gen_patch
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _trees(self):
        base, final = self.tmp / 'base', self.tmp / 'final'
        base.mkdir(); final.mkdir()
        (base / 'a.c').write_bytes(b'one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix\r\n')
        (final / 'a.c').write_bytes(b'one\r\nTWO\r\nthree\r\nfour\r\nSEVEN\r\nEIGHT\r\nsix\r\nlast')          # replace, insert, append, no final newline
        (base / 'b.c').write_bytes(b'x\r\ny\r\n'); (final / 'b.c').write_bytes(b'x\r\ny\r\n')                 # unchanged
        (base / 'gone.c').write_bytes(b'bye\n')                                                                # removed
        (final / 'new.h').write_bytes(b'#pragma once\n')                                                       # added
        (base / 'hi.c').write_bytes('caf\xe9 \x0c page\n'.encode('latin-1')); (final / 'hi.c').write_bytes('caf\xe9 \x0c page 2\n'.encode('latin-1'))
        return base, final

    def test_round_trip_and_no_original_lines_in_the_patch(self):
        base, final = self._trees(); out = self.tmp / 'p'
        (self.tmp / 'p' / 'diff').mkdir(parents=True); (self.tmp / 'p' / 'diff' / 'old.c.diff').write_text('stale')
        self.gp.make(base, final, out)
        self.assertEqual(sorted(p.name for p in (out / 'diff').iterdir()), ['a.c.json', 'hi.c.json'])
        self.assertEqual(list(out.rglob('*.diff')), [], 'a stale .diff from an earlier make must not survive')
        text = ''.join(p.read_text(encoding='ascii') for p in (out / 'diff').iterdir())
        for line in ('one', 'three', 'four', 'five', 'bye', 'x', 'y'): self.assertNotIn(line, text.replace('\\r\\n', ' '), 'no base line may be in the patch: ' + line)
        work = self.tmp / 'work'; shutil.copytree(base, work)
        self.gp.apply(work, out, log=lambda s: None)
        for f in final.iterdir(): self.assertEqual((work / f.name).read_bytes(), f.read_bytes(), f.name)
        self.assertFalse((work / 'gone.c').exists())

    def test_moved_generated_lines_are_references(self):
        base, final = self.tmp / 'base', self.tmp / 'final'; base.mkdir(); final.mkdir()
        (base / 'm.c').write_bytes(b'top\r\n    call_a();\r\nmid\r\nend\r\n')
        (final / 'm.c').write_bytes(b'top\r\nmine();\r\nmid\r\n        call_a();\r\n    call_a();\r\nend\r\n')
        out = self.tmp / 'p'; self.gp.make(base, final, out)
        text = (out / 'diff' / 'm.c.json').read_text(encoding='ascii')
        self.assertNotIn('call_a', text, 'a moved or re-indented generated line is stored as a line number')
        self.assertIn('mine', text)
        work = self.tmp / 'work'; shutil.copytree(base, work); self.gp.apply(work, out, log=lambda s: None)
        self.assertEqual((work / 'm.c').read_bytes(), (final / 'm.c').read_bytes())

    def test_wrong_base_or_format_is_refused(self):
        base, final = self._trees(); out = self.tmp / 'p'; self.gp.make(base, final, out)
        work = self.tmp / 'w1'; shutil.copytree(base, work); (work / 'a.c').write_bytes(b'different')
        with self.assertRaises(SystemExit) as e: self.gp.apply(work, out, log=lambda s: None)
        self.assertIn('does not match this release', str(e.exception))
        m = json.loads((out / 'manifest.json').read_text()); m.pop('format'); (out / 'manifest.json').write_text(json.dumps(m))
        work = self.tmp / 'w2'; shutil.copytree(base, work)
        with self.assertRaises(SystemExit) as e: self.gp.apply(work, out, log=lambda s: None)
        self.assertIn('unknown format', str(e.exception))

    def test_stopped_apply_resumes_and_part_patched_code_is_named(self):
        """N4: each file is written through .tmp + rename, a stopped apply resumes, and code that matches neither the
        base nor the result after a stopped run is reported as that (not as a wrong disc)."""
        base, final = self._trees(); out = self.tmp / 'p'; self.gp.make(base, final, out)
        work = self.tmp / 'w'; shutil.copytree(base, work)
        real, calls = self.gp.replay, []
        def stop_after_one(*a, **k):
            if calls: raise KeyboardInterrupt('installer killed')
            calls.append(1); return real(*a, **k)
        self.gp.replay = stop_after_one
        try:
            with self.assertRaises(KeyboardInterrupt): self.gp.apply(work, out, log=lambda s: None)
        finally:
            self.gp.replay = real
        self.assertEqual(list(work.glob('*.tmp')), [], 'no half-written file is left')
        logs = []; self.gp.apply(work, out, log=logs.append)
        for f in final.iterdir(): self.assertEqual((work / f.name).read_bytes(), f.read_bytes(), f.name)
        self.assertIn('already patched by an earlier run', logs[-1])
        work2 = self.tmp / 'w2'; shutil.copytree(base, work2)
        (work2 / 'a.c').write_bytes((final / 'a.c').read_bytes()); (work2 / 'hi.c').write_bytes(b'changed after the stop')
        with self.assertRaises(SystemExit) as e: self.gp.apply(work2, out, log=lambda s: None)
        self.assertIn('stopped', str(e.exception)); self.assertNotIn('disc image', str(e.exception))
        self.assertEqual(self.gp.base_mismatches(work2, out), ['hi.c'])

    def test_the_kit_has_no_unified_diffs(self):
        diffs = list((KIT / 'patches').rglob('*.diff')) if (KIT / 'patches').is_dir() else []
        self.assertEqual(diffs, [])
        m = json.loads((KIT / 'patches' / 'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(m.get('format'), self.gp.FORMAT)
        self.assertEqual(sorted(p.name for p in (KIT / 'patches' / 'diff').iterdir()), sorted(f + '.json' for f in m['patched']))
        if (WORK.parent / 'gen_patch.py').is_file():
            self.assertEqual((KIT / 'installer' / 'gen_patch.py').read_bytes(), (WORK.parent / 'gen_patch.py').read_bytes(), 'both copies of gen_patch.py are the same')

    def test_one_sentence_about_game_data(self):
        for text in (precheck.FAN_LINE, __import__('gui').TECH, (WORK.parent / 'installer-exe' / 'stub.rc').read_text(encoding='utf-8'), install.__doc__):
            self.assertIn(precheck.NO_GAME_DATA, ' '.join(text.split()))
            self.assertIn(precheck.SHADER_LINE, ' '.join(text.split()))

class Engine(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-eng-', dir=str(WORK / 'tmp')))
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_classify(self):
        self.assertEqual(install.classify(['fatal error C1060: compiler is out of heap space'], 'CMake + Ninja'), 'out_of_memory')
        self.assertEqual(install.classify(['LINK : fatal error LNK1104: cannot open file x.exe'], 'CMake + Ninja'), 'access_denied')
        self.assertEqual(install.classify(['FAILED: foo.obj'], 'CMake + Ninja'), 'build_failed')
        # 1.1.2: a compiler / linker error is named (not blamed on antivirus); access problems keep their own kind
        lnk = ["LINK : fatal error LNK1117: syntax error in option 'MANIFESTINPUT:E:/MechAssault 2 - Lone Wolf (World) (En,Ja,Fr,De,Es,It)/x/app.manifest'",
               'ninja: build stopped: subcommand failed.']
        self.assertEqual(install.classify(lnk, 'CMake + Ninja'), 'compile_error')
        self.assertTrue(install.compiler_error(lnk).startswith('LINK : fatal error LNK1117'))
        self.assertEqual(install.classify(['x.c(3): error C2065: y: undeclared identifier', 'FAILED: x.obj'], 'CMake + Ninja'), 'compile_error')
        self.assertEqual(install.classify(['CMake Error at CMakeLists.txt:5 (project):'], 'CMake + Ninja'), 'compile_error')
        self.assertEqual(install.classify(['LINK : fatal error LNK1168: cannot open x.exe for writing'], 'CMake + Ninja'), 'access_denied')
        self.assertEqual(install.classify(['fatal error C1060: compiler is out of heap space'], 'CMake + Ninja'), 'out_of_memory')
        self.assertEqual(install.classify(['There is not enough space on the disk'], 'unpack'), 'disk_full')

    def test_relation_and_stamp(self):
        d = self.tmp / 'inst'
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text('x')
        self.assertEqual(install.install_relation(d), 'older')
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': install.KIT_ID, 'version': install.VERSION})); self.assertEqual(install.install_relation(d), 'same')
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': 'zz', 'version': '2099.01.01'})); self.assertEqual(install.install_relation(d), 'newer')
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': 'zz', 'version': '2020.01.01'})); self.assertEqual(install.install_relation(d), 'older')

    def test_version_file(self):
        """W-06c: KITSRC/VERSION (public version, date) is what release.json, the installer and the stub's resources carry."""
        ver = [l.strip() for l in (WORK.parent / 'VERSION').read_text(encoding='utf-8').splitlines() if l.strip()]
        self.assertEqual(len(ver), 2); self.assertRegex(ver[0], r'^\d+\.\d+\.\d+$'); self.assertRegex(ver[1], r'^\d{4}\.\d{2}\.\d{2}$')
        self.assertEqual((install.PUBLIC_VERSION, install.VERSION), (ver[0], ver[1]))
        self.assertEqual(install.build_label(), 'MechAssault 2 Recomp %s (build %s)' % (ver[0], install.KIT_ID[:8]))
        rc = (WORK.parent / 'installer-exe' / 'stub.rc').read_text(encoding='utf-8')
        self.assertIn('MA2_VER_STR', rc); self.assertNotIn('2026,10,30', rc)

    def test_docs_tokens(self):
        """W-70/W-71: README.txt, Controls.txt and Docs/*.txt are copied with {{VERSION}} {{BUILD}} {{MA2_SUPPORT_URL}} filled in; the old PC_CONTROLS.md goes."""
        kit = self.tmp / 'kit'; d = self.tmp / 'inst'
        (kit / 'defaults' / 'Docs').mkdir(parents=True); d.mkdir()
        (kit / 'defaults' / 'README.txt').write_bytes(b'MechAssault 2 - PC recompilation, version {{VERSION}} (build {{BUILD}})\r\nHelp: {{MA2_SUPPORT_URL}}\r\n')
        (kit / 'defaults' / 'Controls.txt').write_bytes(b'Controls {{VERSION}}\n')
        (kit / 'defaults' / 'Docs' / 'Install guide.txt').write_bytes(b'Report at {{MA2_SUPPORT_URL}}\n')
        (kit / 'defaults' / 'Docs' / 'pic.bin').write_bytes(b'{{VERSION}}\x00')
        (d / 'PC_CONTROLS.md').write_text('old')
        real_kit = install.KIT; install.KIT = kit
        try:
            inst = install.Installer(None, d, log=lambda s: None); inst.install_docs()
            readme = (d / 'README.txt').read_bytes()
            self.assertEqual(readme, ('MechAssault 2 - PC recompilation, version %s (build %s)\r\nHelp: %s\r\n' % (install.PUBLIC_VERSION, install.KIT_ID[:8], precheck.MA2_SUPPORT_URL or precheck.SUPPORT_FALLBACK)).encode())
            self.assertEqual((d / 'Controls.txt').read_bytes(), ('Controls %s\n' % install.PUBLIC_VERSION).encode())
            self.assertFalse((d / 'PC_CONTROLS.md').exists())
            self.assertIn(precheck.MA2_SUPPORT_URL or precheck.SUPPORT_FALLBACK, (d / 'Docs' / 'Install guide.txt').read_text())
            self.assertEqual((d / 'Docs' / 'pic.bin').read_bytes(), b'{{VERSION}}\x00')                  # only text files are rendered
            precheck.MA2_SUPPORT_URL = 'https://example.invalid/r'
            try:
                inst.install_docs(); self.assertIn('https://example.invalid/r', (d / 'README.txt').read_text())
            finally:
                precheck.MA2_SUPPORT_URL = ''
        finally:
            install.KIT = real_kit

    def test_shipped_docs_use_only_known_tokens(self):
        """The documents in kit/defaults may contain only the three tokens install.py fills in."""
        known = {'{{VERSION}}', '{{BUILD}}', '{{MA2_SUPPORT_URL}}'}; bad = []
        for f in (KIT / 'defaults').rglob('*'):
            if f.is_file() and f.suffix.lower() in ('.txt', '.md'):
                for tok in re.findall(r'\{\{[^}]*\}\}', f.read_text(encoding='utf-8', errors='replace')):
                    if tok not in known: bad.append((f.name, tok))
        self.assertEqual(bad, [])

    def test_verify_disc_files(self):
        """W-55: sizes, then SHA-256; a damaged or missing file stops the install with kind 'disc' and its name."""
        d = self.tmp / 'inst'; gf = d / 'game_files'
        files = {'a.mgf': b'alpha' * 1000, 'Audio/b.xwb': b'beta' * 500, 'c.bin': b'gamma' * 10}
        for rel, data in files.items():
            (gf / rel).parent.mkdir(parents=True, exist_ok=True); (gf / rel).write_bytes(data)
        manifest = {rel: [len(data), hashlib.sha256(data).hexdigest()] for rel, data in files.items()}
        logs = []; inst = install.Installer(None, d, log=logs.append)
        inst.verify_disc_files(manifest); self.assertTrue(any('all 3 disc files match' in l for l in logs))
        (gf / 'Audio' / 'b.xwb').write_bytes(b'beta' * 499 + b'betX')                    # same size, one byte differs
        with self.assertRaises(install.Stop) as e: inst.verify_disc_files(manifest)
        self.assertEqual(e.exception.kind, 'disc'); self.assertIn('damaged: Audio/b.xwb.', str(e.exception)); self.assertIn('Dump the disc again', str(e.exception))
        (gf / 'a.mgf').write_bytes(b'short'); (gf / 'c.bin').unlink()                  # wrong size, and missing
        with self.assertRaises(install.Stop) as e: inst.verify_disc_files(manifest)
        for name in ('a.mgf', 'Audio/b.xwb', 'c.bin'): self.assertIn(name, str(e.exception))
        many = {'f%d.bin' % i: [1, 'x'] for i in range(8)}
        with self.assertRaises(install.Stop) as e: inst.verify_disc_files(many)
        self.assertIn('(and 3 more)', str(e.exception))
        inst.cancelled = True
        with self.assertRaises(install.Stop) as e: inst.verify_disc_files({'c.bin': [1, 'x'], 'a.mgf': [5, hashlib.sha256(b'short').hexdigest()]})
        self.assertEqual(e.exception.kind, 'cancelled')

    def test_the_real_disc_manifest(self):
        m = json.loads((KIT / 'installer' / 'data' / 'disc-manifest.json').read_text(encoding='utf-8'))
        self.assertIn(install.RELEASE['xbe'], m); self.assertEqual(m[install.RELEASE['xbe']], [install.RELEASE['xbe_bytes'], install.RELEASE['xbe_sha256']])
        self.assertGreater(len(m), 500); self.assertIn('boggeddown.mgf', m)
        self.assertTrue(all(isinstance(v[0], int) and len(v[1]) == 64 for v in m.values()))

    def test_asset_errors_say_the_disc_may_be_damaged(self):
        import pc_assets
        d = self.tmp / 'inst'; d.mkdir(); inst = install.Installer(None, d, log=lambda s: None); inst.state = {}
        real = pc_assets.build
        try:
            for exc in (SystemExit('pc_assets/common.mgf does not match the release.'), ValueError('bad archive')):
                pc_assets.build = lambda *a, **k: (_ for _ in ()).throw(exc)
                with self.assertRaises(install.Stop) as e: inst.assets()
                self.assertEqual(e.exception.kind, 'disc'); self.assertIn('damaged', str(e.exception))
        finally:
            pc_assets.build = real

    def test_enhancer_licenses_travel_with_the_content(self):
        """W-56: Licenses/Enhancer holds enhancer/LICENSES/* and, for videos, FMV-NOTICE.txt."""
        if not (KIT / 'source' / 'game' / 'vendor' / install.FFMPEG / 'bin').is_dir():
            self.skipTest('FFmpeg is fetched, not in git: run tools/fetch_deps.py first')
        d = self.tmp / 'inst'; d.mkdir(); inst = install.Installer(None, d, log=lambda s: None)
        inst.install_enhancer_licenses(videos=False)
        names = sorted(p.name for p in (d / 'Licenses' / 'Enhancer').iterdir())
        self.assertEqual(names, sorted(p.name for p in (KIT / 'enhancer' / 'LICENSES').iterdir() if p.name != install.VULKAN_NOTICE))   # 1.1.2: that one only for Vulkan textures
        self.assertTrue(names, 'the kit carries the enhancer licence texts')
        inst.install_enhancer_licenses(videos=True)
        self.assertEqual((d / 'Licenses' / 'Enhancer' / 'FMV-NOTICE.txt').read_bytes(), (KIT / 'enhancer' / 'FMV' / 'NOTICE.txt').read_bytes())
        # an update rewrites Licenses/: with enhanced textures installed the notices are put back
        (d / 'game_files' / 'texture_packs' / '2x').mkdir(parents=True); (d / 'game_files' / 'texture_packs' / '2x' / 'index.json').write_text('{}')
        shutil.rmtree(d / 'Licenses')
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text('x')
        inst.state = {'update': False, 'done': []}; inst.old_stamp = None
        inst.vcvars, inst.cmake, inst.ninja, inst.vs = install.find_visual_studio()
        (inst.work).mkdir(parents=True, exist_ok=True); (inst.work / 'build').mkdir(); (inst.work / 'build' / 'mechassault2_recomp.exe').write_text('new')
        inst.install()
        self.assertTrue((d / 'Licenses' / 'Enhancer').is_dir(), 'install() keeps the enhancer notices when enhanced textures are present')
        self.assertFalse((d / 'Licenses' / 'Enhancer' / 'FMV-NOTICE.txt').exists(), 'no video notice without enhanced videos')

    def test_folder_as_disc(self):
        """W-58: a folder copied from the disc (or its ma2.xbe) is accepted like an image, with the same verdicts."""
        data = fake_xbe(); rel = release_for(data)
        good = self.tmp / 'gf'; good.mkdir(); (good / 'ma2.xbe').write_bytes(data); (good / 'x.mgf').write_bytes(b'x')
        r = precheck.validate_image(good, rel); self.assertTrue(r.ok); self.assertEqual(r.info['folder'], str(good)); self.assertIn('Game folder OK', r.message)
        r = precheck.validate_image('"%s"' % good, rel); self.assertTrue(r.ok)                                      # quoted ("Copy as path")
        r = precheck.validate_image(good / 'ma2.xbe', rel); self.assertTrue(r.ok); self.assertEqual(r.info['folder'], str(good))
        up = self.tmp / 'up'; up.mkdir(); (up / 'MA2.XBE').write_bytes(data); self.assertTrue(precheck.validate_image(up, rel).ok)   # name case does not matter
        none = self.tmp / 'empty'; none.mkdir(); r = precheck.validate_image(none, rel); self.assertEqual(r.code, 'no_xbe'); self.assertIn('no ma2.xbe', r.message)
        for name, blob, code in (('t', fake_xbe(title=0x11112222), 'wrong_title'), ('v', fake_xbe(version=3), 'wrong_version'), ('m', fake_xbe(pad=64), 'modified')):
            d = self.tmp / name; d.mkdir(); (d / 'ma2.xbe').write_bytes(blob); r = precheck.validate_image(d, rel); self.assertEqual(r.code, code, name)
        self.assertIn('game folder', precheck.validate_image(self.tmp / 't', rel).message)

    def test_extract_copies_a_folder(self):
        src = self.tmp / 'disc'; (src / 'Audio').mkdir(parents=True)
        if not XBE: self.skipTest('set MA2_TEST_XBE to your own ma2.xbe')
        shutil.copy2(XBE, src / 'ma2.xbe')
        (src / 'a.mgf').write_bytes(b'a' * 1000); (src / 'Audio' / 'b.xwb').write_bytes(b'b' * 500)
        d = self.tmp / 'inst'; logs = []
        inst = install.Installer(src / 'ma2.xbe', d, log=logs.append)                      # the xbe itself names its folder
        self.assertEqual(inst.xiso, src.resolve())
        inst.work.mkdir(parents=True); inst.verify_disc_files = lambda manifest=None: logs.append('verified')
        inst.extract()
        self.assertEqual((d / 'game_files' / 'Audio' / 'b.xwb').read_bytes(), b'b' * 500); self.assertEqual((d / 'game_files' / 'a.mgf').read_bytes(), b'a' * 1000)
        self.assertIn('verified', logs); self.assertTrue(any('copied 3 files' in l for l in logs), logs)
        inst.cancelled = True
        with self.assertRaises(install.Stop) as e: inst.copy_folder(src, self.tmp / 'again')
        self.assertEqual(e.exception.kind, 'cancelled')

    def test_network_and_gpu_failures_are_named(self):
        """W-62: a failed download is 'network', not 'unexpected'; an unsupported card is 'gpu'."""
        for text in ('urllib.error.URLError: <urlopen error [Errno 11001] getaddrinfo failed>', 'TimeoutError: timed out', 'ConnectionResetError: [WinError 10054]',
                     'urllib.error.HTTPError: HTTP Error 404: Not Found', 'ERROR: Could not find a version that satisfies the requirement torch',
                     'ERROR: No matching distribution found for torch', 'Setup failed: Could not download RealBasicVSR.pth from https://x (boom)'):
            self.assertEqual(install.classify([text], 'Enhancer setup'), 'network', text)
        self.assertEqual(install.classify(['Setup failed: PyTorch does not support this graphics card yet (NVIDIA GeForce RTX 5090).'], 'Enhancer setup'), 'gpu')
        self.assertEqual(install.classify(['something odd'], 'x'), 'unexpected')
        # through a real (fake) tool run: the Stop carries the kind
        inst = install.Installer(None, self.tmp / 'd', log=lambda s: None)
        with self.assertRaises(install.Stop) as e:
            inst.run_cmd([sys.executable, '-c', 'import sys; print("urllib.error.URLError: <urlopen error getaddrinfo failed>"); sys.exit(1)'], title='Enhancer setup')
        self.assertEqual(e.exception.kind, 'network')
        gui = __import__('gui'); self.assertEqual(gui.FAILURES['network'][0], 'A download failed.'); self.assertIn('gpu', gui.FAILURES)

    def test_pytorch_build_follows_the_graphics_card(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('enh_setup', KIT / 'enhancer' / 'setup.py'); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        label, args, check = m.torch_plan(['NVIDIA GeForce RTX 5090'])
        self.assertIn('cu128', ' '.join(args)); self.assertIn('torch', args); self.assertNotIn('==', ' '.join(args))
        for name in ('NVIDIA GeForce RTX 5070 Ti', 'NVIDIA RTX PRO 6000 Blackwell Workstation Edition', 'NVIDIA RTX 5060'):
            self.assertIn('cu128', ' '.join(m.torch_plan([name])[1]), name)
        for names in (['NVIDIA GeForce RTX 4070'], ['NVIDIA GeForce RTX 3090'], ['NVIDIA GeForce GTX 1660'], ['NVIDIA GeForce RTX 3050'], []):
            self.assertEqual(m.torch_plan(names)[1], ['torch==2.6.0', '--index-url', 'https://download.pytorch.org/whl/cu124'], names)
        self.assertIsInstance(m.nvidia_gpu_names(), list)

    def test_venv_home_is_repaired(self):
        cfg = self.tmp / 'pyvenv.cfg'; gone = self.tmp / 'gone-python'
        cfg.write_text('home = %s\ninclude-system-site-packages = true\nversion = 3.13.1\n' % gone)
        self.assertTrue(install.repair_venv_home(cfg, Path(sys.executable).parent))
        text = cfg.read_text(); self.assertIn('home = %s' % Path(sys.executable).parent, text); self.assertIn('include-system-site-packages = true', text)
        self.assertFalse(install.repair_venv_home(cfg, Path(sys.executable).parent), 'a home that exists is left alone')
        self.assertFalse(install.repair_venv_home(self.tmp / 'missing.cfg', Path(sys.executable).parent))

    def test_fmv_download_failure_names_the_file(self):
        import importlib.util, urllib.error, urllib.request
        sys.path.insert(0, str(KIT / 'enhancer' / 'FMV'))
        try:
            spec = importlib.util.spec_from_file_location('setup_fmv_t', KIT / 'enhancer' / 'FMV' / 'setup_fmv.py'); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        except Exception as e:
            self.skipTest('setup_fmv needs the enhancer environment (%s)' % e)
        finally:
            sys.path.remove(str(KIT / 'enhancer' / 'FMV'))
        real = urllib.request.urlopen
        urllib.request.urlopen = lambda *a, **k: (_ for _ in ()).throw(urllib.error.URLError('getaddrinfo failed'))
        try:
            with self.assertRaises(RuntimeError) as e: m.download_file('https://example.invalid/RealBasicVSR.pth', self.tmp / 'RealBasicVSR.pth')
        finally:
            urllib.request.urlopen = real
        self.assertIn('RealBasicVSR.pth', str(e.exception)); self.assertIn('Check your internet connection', str(e.exception)); self.assertFalse((self.tmp / 'RealBasicVSR.pth.part').exists())

    def test_step_titles_are_plain(self):
        """W-117: the titles that reach the window and the log name no tool; the tool names stay in the indented log lines."""
        inst = install.Installer(None, self.tmp / 'd', ['2x'], True, log=lambda s: None); inst.state = {'done': []}
        titles = [title for _, title, _ in inst.steps()]
        self.assertEqual(titles, ['Checking requirements', 'Checking the disc image', 'Extracting game files', 'Analysing the game program', 'Translating the game program to C',
                                  'Generating helper code', 'Applying the PC fixes and features', 'Compiling the game', 'Building the PC menus', 'Installing the game files',
                                  'Making 2x enhanced textures', 'Making the enhanced cutscene videos', 'Finishing'])
        for title in titles:
            for jargon in ('xboxrecomp', 'MSVC', 'bringup', 'Enhancer', 'ma2.xbe', 'generators'): self.assertNotIn(jargon, title)
        self.assertEqual(inst.step_names(), ['check', 'disc', 'extract', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets', 'install', 'textures-2x', 'videos', 'finish'])

    def test_previous_restore(self):
        d = self.tmp / 'i'; d.mkdir()
        (d / 'MechAssault 2.exe').write_text('old'); (d / 'pc_assets').mkdir(); (d / 'pc_assets' / 'a').write_text('old-assets')
        inst = install.Installer(None, d, log=lambda s: None); inst.state = {'update': True, 'done': ['assets', 'install', 'analyze']}
        inst.work.mkdir()
        inst.keep_previous(d / 'MechAssault 2.exe'); inst.keep_previous(d / 'pc_assets')
        (d / 'MechAssault 2.exe').write_text('new'); (d / 'pc_assets').mkdir(); (d / 'pc_assets' / 'a').write_text('new-assets')
        self.assertTrue((d / 'MechAssault 2.exe.previous').exists())
        inst.restore_previous()
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'old'); self.assertEqual((d / 'pc_assets' / 'a').read_text(), 'old-assets')
        self.assertEqual(inst.state['done'], ['analyze']); self.assertFalse((d / 'MechAssault 2.exe.previous').exists())
        # commit path
        inst.keep_previous(d / 'MechAssault 2.exe'); (d / 'MechAssault 2.exe').write_text('new2'); inst.commit_previous()
        self.assertFalse((d / 'MechAssault 2.exe.previous').exists()); self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new2')

    def test_build_root_special_chars(self):
        for name, ascii_ok in (('MA2 plain', True), ('100% MA2', False), ('a;b', False), ('wow!x', False), ('MA2 Jos\u00e9', False),
                               ('MechAssault 2 - Lone_Wolf v1.2', True), ('Test (World) (En,Ja)', False), ('MA2 & co', False),
                               ('Test (World) (En,Ja)/MA2 & co', False), ('a,b', False), ('a(b)', False), ("O'Neil", False)):
            inst = install.Installer(None, self.tmp / name, log=lambda s: None)
            root = inst.build_root()
            self.assertEqual(root == inst.work, ascii_ok, name)
            self.assertTrue(install.plain_path(root), root)                # every build runs under a plain path
            if root != inst.work: shutil.rmtree(root, ignore_errors=True)

    def test_shortcut(self):
        lnk = self.tmp / 'x.lnk'
        install.create_shortcut(lnk, sys.executable, self.tmp, 'test', sys.executable)
        out = subprocess.run(['powershell', '-NoProfile', '-Command', "(New-Object -ComObject WScript.Shell).CreateShortcut('%s').TargetPath" % lnk],
                             capture_output=True, text=True).stdout.strip()
        self.assertEqual(os.path.normcase(out), os.path.normcase(sys.executable))

    def test_relative_and_quoted_paths(self):
        inst = install.Installer('"rel.iso"', '"%s"' % (self.tmp / 'd'), log=lambda s: None)
        self.assertEqual(inst.dest, (self.tmp / 'd').resolve()); self.assertEqual(inst.xiso, (Path.cwd() / 'rel.iso').resolve())

    def test_mkdir_error_and_early_failure_keeps_log(self):
        os.environ['LOCALAPPDATA'] = str(self.tmp / 'LA')
        lines = []
        inst = install.Installer(self.tmp / 'nope.iso', self.tmp / 'newdest', log=lines.append, jobs=2)
        with self.assertRaises(install.Stop) as cm: inst.run()
        self.assertEqual(cm.exception.kind, 'disc')                      # missing image -> the disc advice
        self.assertFalse((self.tmp / 'newdest').exists())               # nothing left behind
        logs = list((self.tmp / 'LA' / 'MechAssault2' / 'Logs').glob('install-*.log'))
        self.assertEqual(len(logs), 1); self.assertIn('Choose your MechAssault 2 disc image', logs[0].read_text(encoding='utf-8'))
        self.assertIn('Choose your', str(cm.exception))
        # unwritable destination
        blocker = self.tmp / 'afile'; blocker.write_text('x')
        inst = install.Installer(None, blocker / 'sub', log=lambda s: None)
        with self.assertRaises(install.Stop) as cm: inst.run()
        self.assertEqual(cm.exception.kind, 'access_denied'); self.assertIn('Cannot create', str(cm.exception))

    def test_kit_mismatch_resume_drops_build_steps(self):
        d = self.tmp / 'r'; (d / '_work').mkdir(parents=True)
        (d / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc', 'extract', 'analyze', 'recompile'], 'kit_id': 'old'}))
        (d / 'game_files').mkdir(); (d / 'game_files' / install.RELEASE['xbe']).write_text('x')      # extract's output exists (else it runs again)
        os.environ['LOCALAPPDATA'] = str(self.tmp / 'LA2')
        inst = install.Installer(self.tmp / 'x.iso', d, log=lambda s: None); inst.until = 'check'
        inst.ffmpeg_dir = lambda: None                                    # (the requirements check fetches FFmpeg: not here)
        inst.run()
        st = json.loads((d / '_work' / 'state.json').read_text())
        self.assertEqual(st['done'], ['check', 'disc', 'extract'])

    def _update_fixture(self, name):
        d = WORK / 'tmp' / ('upd-' + name); shutil.rmtree(d, ignore_errors=True)
        for f, data in (('MechAssault 2.exe', 'old-exe'), ('pc_assets/common.mgf', 'old-assets'), ('MA2 Scenario Editor.exe', 'old-editor'),
                        ('avcodec-63.dll', 'old-dll'), ('pc_settings.ini', '[Display]\nTextureQuality=0\nMine=1\n')):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text(data)
        if not XBE: self.skipTest('set MA2_TEST_XBE to your own ma2.xbe')
        (d / 'game_files').mkdir(); shutil.copy2(XBE, d / 'game_files' / 'ma2.xbe')
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': 'oldkit', 'version': '2026.10.22', 'defaults_sha256': 'not-the-current-sha'}))
        live = Path(os.environ['LOCALAPPDATA']) / 'MechAssault2'; live.mkdir(parents=True, exist_ok=True)
        (live / 'pc_settings.ini').write_text('[Display]\nTextureQuality=0\nWindowWidth=2000\n')
        inst = install.Installer(None, d, update=True, log=lambda s: None, shortcuts=('desktop', 'start', 'editor'), switch_textures=True)
        inst._start_logs(); inst.old_stamp = precheck.read_stamp(d)
        inst.state = {'update': True, 'done': ['verify', 'analyze', 'build', 'assets'], 'options': inst.opts}
        inst.work.mkdir(parents=True, exist_ok=True); inst.logfile = open(inst.work / 'install.log', 'a')
        inst.game_ok = True
        (inst.work / 'build').mkdir(); (inst.work / 'build' / 'mechassault2_recomp.exe').write_text('new-exe')
        inst.vcvars, inst.cmake, inst.ninja, inst.vs = install.find_visual_studio()
        return d, inst, live

    def test_update_swap_commit_and_settings(self):
        if not (KIT / 'source' / 'game' / 'vendor' / install.FFMPEG / 'bin').is_dir():
            self.skipTest('FFmpeg is fetched, not in git: run tools/fetch_deps.py first')
        d, inst, live = self._update_fixture('ok')
        (d / 'pc_assets' / 'common.mgf').write_text('new-assets')       # the assets step already replaced it
        inst.keep_previous(d / 'pc_assets')                              # (keep_previous before rebuild, as assets() does)
        (d / 'pc_assets').mkdir(); (d / 'pc_assets' / 'common.mgf').write_text('new-assets')
        inst.install()
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new-exe'); self.assertEqual((d / 'MechAssault 2.exe.previous').read_text(), 'old-exe')
        self.assertTrue((d / 'avcodec-63.dll.previous').exists())
        self.assertIn(b'gyan.dev', (d / 'Licenses' / 'FFmpeg-README.txt').read_bytes())                  # W-54: the FFmpeg build's own README beside the GPL text
        self.assertEqual((d / 'pc_settings.ini').read_text(), '[Display]\nTextureQuality=0\nMine=1\n')      # user-edited ini kept
        self.assertTrue((d / 'pc_settings.default.ini').is_file()); self.assertTrue(any('pc_settings.default.ini' in n for n in inst.notes))
        lnks = sorted(p.name for p in Path(os.environ['MA2_SHORTCUT_DIR']).rglob('*.lnk')); self.assertIn('MechAssault 2 (Recomp).lnk', lnks); self.assertIn('MA2 Scenario Editor.lnk', lnks)
        # a texture pack was added by the (skipped) textures step: pretend, then finish
        (d / 'game_files' / 'texture_packs' / '2x').mkdir(parents=True); (d / 'game_files' / 'texture_packs' / '2x' / 'index.json').write_text('{}')
        inst.state['done'].append('textures-2x'); inst.keep_work = True
        inst.finish()
        self.assertFalse((d / 'MechAssault 2.exe.previous').exists()); self.assertFalse((d / 'pc_assets.previous').exists())
        text = (live / 'pc_settings.ini').read_text()
        self.assertIn('WindowWidth=2000', text); self.assertIn('TextureQuality=1', text)                  # user value kept; texture switched on
        self.assertTrue(list(live.glob('pc_settings.ini.bak-*')), 'a backup of the live ini is made')
        self.assertIn('[Bots]', text)                                                                      # new keys added, add-only
        for key, line in precheck._ini_keys((KIT / 'defaults' / 'pc_settings.ini').read_text(encoding='utf-8'))[0]['Bots'].items(): self.assertIn(line.strip(), text, key)
        stamp = precheck.read_stamp(d)
        self.assertEqual(stamp['kit_id'], install.KIT_ID); self.assertEqual(stamp['version'], install.VERSION); self.assertEqual(stamp['options']['textures'], ['2x'])
        self.assertEqual(install.install_relation(d), 'same')
        # W-06c: the stamp names the public version, the release exe and the exe this install really holds
        self.assertEqual(stamp['public_version'], install.PUBLIC_VERSION)
        self.assertEqual(stamp['built_sha256'], install.sha256(d / 'MechAssault 2.exe')); self.assertIn('release_sha256', stamp); self.assertNotIn('game_sha256', stamp)
        inst._stop_logs()

    def test_update_failure_puts_the_old_game_back(self):
        d, inst, live = self._update_fixture('fail')
        inst.install()
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new-exe')
        inst.restore_previous()                                           # what _run() does on any Stop or cancel
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'old-exe'); self.assertEqual((d / 'avcodec-63.dll').read_text(), 'old-dll')
        self.assertFalse(list(d.glob('*.previous'))); self.assertNotIn('install', inst.state['done'])
        self.assertEqual((live / 'pc_settings.ini').read_text(), '[Display]\nTextureQuality=0\nWindowWidth=2000\n')   # live settings untouched
        inst._stop_logs()

    # -- 1.1.2: optional content never undoes the game install
    def _xbe_into(self, d):
        if not XBE: self.skipTest('set MA2_TEST_XBE to your own ma2.xbe')
        xbe = XBE
        (d / 'game_files').mkdir(parents=True, exist_ok=True); shutil.copy2(xbe, d / 'game_files' / 'ma2.xbe')

    def _resume_state(self, d, done, update):
        (d / '_work').mkdir(parents=True, exist_ok=True)
        (d / '_work' / 'state.json').write_text(json.dumps({'update': update, 'done': done, 'kit_id': install.KIT_ID,
                                                            'release': install.RELEASE['release_executable_sha256'],
                                                            'options': {'textures': ['2x'], 'videos': False}}))

    def _runner(self, d, update, fail=None, logs=None):
        """A real Installer.run() on `d` with the compiler check and the optional step replaced (no compile, no download)."""
        inst = install.Installer(None, d, textures=['2x'], update=update, log=(logs.append if logs is not None else lambda s: None),
                                 shortcuts=('desktop', 'start', 'editor'))
        inst.vcvars, inst.cmake, inst.ninja, inst.vs = install.find_visual_studio()
        inst.check = lambda: None
        def textures(scale):
            if fail: raise fail
            p = d / 'game_files' / 'texture_packs' / scale; p.mkdir(parents=True, exist_ok=True); (p / 'index.json').write_text('{}')
        inst.textures = textures
        return inst

    def test_update_optional_failure_keeps_the_new_game(self):
        d, fx, live = self._update_fixture('optfail'); fx._stop_logs(); fx.logfile.close()
        self._resume_state(d, ['check', 'verify', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets'], True)
        logs = []
        stop = install.Stop('Enhancer setup failed (exit code 1). A program it started stopped from outside: its window was closed or '
                            'security software ended it.', 'unexpected')
        inst = self._runner(d, True, fail=stop, logs=logs)
        with self.assertRaises(install.Stop) as cm: inst.run()
        e = cm.exception
        self.assertEqual(e.kind, 'extras'); self.assertEqual(e.cause, 'unexpected')
        self.assertTrue(str(e).startswith('MechAssault 2 is installed. The 2x enhanced textures could not be made: Enhancer setup failed'), str(e))
        self.assertIn('try again later with Resume or Add enhanced content', str(e))
        # the new game stays: no restore, no .previous left, the stamp says this version
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new-exe'); self.assertFalse(list(d.glob('*.previous')))
        self.assertFalse(any('previous version was put back' in l for l in logs), logs)
        stamp = precheck.read_stamp(d)
        self.assertEqual(stamp['kit_id'], install.KIT_ID); self.assertEqual(stamp['version'], install.VERSION)
        self.assertEqual(stamp['built_sha256'], install.sha256(d / 'MechAssault 2.exe')); self.assertEqual(stamp['options']['textures'], [])
        self.assertTrue(stamp.get('shortcuts')); self.assertEqual(install.install_relation(d), 'same')
        self.assertIn('[Bots]', (live / 'pc_settings.ini').read_text())                 # the update's new settings keys are in
        st = precheck.read_state(d)
        self.assertTrue(st['core_done']); self.assertIn('install', st['done']); self.assertNotIn('textures-2x', st['done']); self.assertEqual(st['previous'], [])
        # Resume: only the optional step (and finish) run; the game is not rebuilt or reinstalled
        plan, ran = [], []
        inst2 = self._runner(d, True)
        inst2.on_plan = plan.extend; inst2.on_step = lambda i, n, title, name, skipped: ran.append(name)
        inst2.install = lambda: self.fail('the game must not be installed again')
        inst2.run()
        self.assertEqual(plan, ['check', 'textures-2x', 'finish']); self.assertEqual(ran, plan)
        stamp2 = precheck.read_stamp(d)
        self.assertEqual(stamp2['options']['textures'], ['2x']); self.assertEqual(stamp2['built_sha256'], stamp['built_sha256'])
        self.assertEqual(stamp2['installed'], stamp['installed']); self.assertEqual(stamp2['version'], install.VERSION)
        self.assertIsNone(precheck.read_state(d), 'finished: no Resume offered any more')
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new-exe')

    def test_update_core_failure_still_puts_the_old_game_back(self):
        d, fx, live = self._update_fixture('corefail'); fx._stop_logs(); fx.logfile.close()
        self._resume_state(d, ['check', 'verify', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets'], True)
        inst = self._runner(d, True)
        def blocked(): raise install.Stop('Can\'t write a shortcut.', 'access_denied')
        inst.make_shortcuts = blocked                                     # the last thing install() does: the exe and DLLs are already swapped
        with self.assertRaises(install.Stop) as cm: inst.run()
        self.assertEqual(cm.exception.kind, 'access_denied')
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'old-exe'); self.assertEqual((d / 'avcodec-63.dll').read_text(), 'old-dll')
        self.assertFalse(list(d.glob('*.previous')))
        self.assertEqual(precheck.read_stamp(d)['kit_id'], 'oldkit')
        st = precheck.read_state(d); self.assertNotIn('install', st['done']); self.assertFalse(st.get('core_done'))

    def test_fresh_install_optional_failure_or_cancel_keeps_the_game(self):
        for kind in ('network', 'cancelled'):
            d = WORK / 'tmp' / ('fresh-opt-' + kind); shutil.rmtree(d, ignore_errors=True)
            (d / 'pc_assets').mkdir(parents=True); (d / 'pc_assets' / 'common.mgf').write_text('assets')
            self._xbe_into(d)
            self._resume_state(d, ['check', 'disc', 'extract', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets'], False)
            (d / '_work' / 'build').mkdir(); (d / '_work' / 'build' / 'mechassault2_recomp.exe').write_text('new-exe')
            inst = self._runner(d, False, fail=install.Stop('Cancelled.' if kind == 'cancelled' else 'A download failed.', kind))
            with self.assertRaises(install.Stop) as cm: inst.run()
            self.assertEqual(cm.exception.kind, 'extras'); self.assertEqual(cm.exception.cause, kind)
            if kind == 'cancelled': self.assertIn('were not made: the install was stopped', str(cm.exception))
            else: self.assertIn('could not be made: A download failed.', str(cm.exception))
            self.assertTrue(precheck.is_install(d)); self.assertEqual(install.install_relation(d), 'same')
            self.assertEqual(precheck.read_stamp(d)['version'], install.VERSION)
            self.assertIn('TextureQuality=0', (d / 'pc_settings.ini').read_text(encoding='utf-8'))
            self.assertEqual(precheck.read_stamp(d)['defaults_sha256'], install.sha256(d / 'pc_settings.ini'))
            self.assertTrue(precheck.read_state(d)['core_done'])
            shutil.rmtree(d, ignore_errors=True)

    def test_addons_failure_says_the_game_is_installed(self):
        d = WORK / 'tmp' / 'addon-opt'; shutil.rmtree(d, ignore_errors=True)
        for f in ('MechAssault 2.exe', 'pc_assets/common.mgf'): (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text('x')
        self._xbe_into(d)
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': install.KIT_ID, 'version': install.VERSION}))
        inst = self._runner(d, False, fail=install.Stop('PyTorch does not support this graphics card yet.', 'gpu'))
        with self.assertRaises(install.Stop) as cm: inst.run()
        self.assertEqual(cm.exception.kind, 'extras'); self.assertEqual(cm.exception.cause, 'gpu')
        self.assertTrue(precheck.read_state(d)['addons_only']); self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'x')
        shutil.rmtree(d, ignore_errors=True)

    def test_exit_code_meanings(self):
        self.assertIn('stopped from outside: its window was closed or security software ended it', install.exit_meaning(3221225786))
        self.assertEqual(install.exit_meaning(-1073741510), install.exit_meaning(3221225786))           # the same code, signed
        self.assertEqual(install.exit_text(3221225786)[:35], 'exit code 3221225786 = 0xC000013A: ')
        self.assertEqual(install.exit_meaning(1), ''); self.assertEqual(install.exit_text(1), 'exit code 1')
        self.assertTrue(install.explain_codes("Setup failed: Command '['python.exe', '-m', 'pip']' returned non-zero exit status 3221225786."))
        # a real child stopped with STATUS_CONTROL_C_EXIT: readable reason + the antivirus hint
        lines = []
        inst = install.Installer(None, self.tmp / 'rc', log=lines.append)
        with self.assertRaises(install.Stop) as cm:
            inst.run_cmd([sys.executable, '-c', 'import os; os._exit(-1073741510)'], title='Enhancer setup', hint=install.ENHANCER_HINT)
        msg = str(cm.exception)
        self.assertIn('exit code 3221225786 = 0xC000013A: stopped from outside', msg); self.assertIn('Antivirus', msg); self.assertEqual(cm.exception.code, 3221225786)
        self.assertTrue(any('stopped from outside' in l for l in lines))
        # a tool that reports its child's code in its output (setup.py) gets the meaning too
        with self.assertRaises(install.Stop) as cm:
            inst.run_cmd([sys.executable, '-c', 'import sys; print("pip install numpy==2.2.3 stopped from outside (exit code 3221225786 = 0xC000013A)."); sys.exit(1)'],
                         title='Enhancer setup', hint=install.ENHANCER_HINT)
        self.assertIn('A program it started stopped from outside', str(cm.exception)); self.assertIn('Antivirus', str(cm.exception))
        # an ordinary failure keeps the plain text (no hint)
        with self.assertRaises(install.Stop) as cm:
            inst.run_cmd([sys.executable, '-c', 'import sys; sys.exit(3)'], title='tool', hint=install.ENHANCER_HINT)
        self.assertIn('(exit code 3)', str(cm.exception)); self.assertNotIn('Antivirus', str(cm.exception))

    def test_enhancer_setup_runs_tools_without_a_console(self):
        import importlib.util
        src = KIT / 'enhancer' / 'setup.py'
        text = src.read_text(encoding='utf-8')
        self.assertEqual(text.count('subprocess.run('), text.count('creationflags=NO_CONSOLE'), 'every tool setup.py starts gets NO_CONSOLE')
        self.assertIn('with_pip=False', text); self.assertNotIn('with_pip=True', text)                                                 # venv's own pip step would start a console tool
        spec = importlib.util.spec_from_file_location('ma2_enhancer_setup', src); mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        self.assertTrue(mod.NO_CONSOLE & subprocess.CREATE_NO_WINDOW and mod.NO_CONSOLE & subprocess.CREATE_NEW_PROCESS_GROUP)
        self.assertIn('stopped from outside', mod.explain(3221225786))
        with self.assertRaises(RuntimeError) as cm: mod.run([sys.executable, '-c', 'import os; os._exit(-1073741510)'], 'pip install numpy==2.2.3')
        self.assertIn('pip install numpy==2.2.3 stopped from outside', str(cm.exception)); self.assertIn('exit code 3221225786 = 0xC000013A', str(cm.exception))
        self.assertIn('Antivirus', str(cm.exception)); self.assertIn('game itself is installed', str(cm.exception))
        self.assertEqual(install.explain_codes(str(cm.exception)), [install.exit_meaning(3221225786)])    # install.py reads it from setup.py's output
        self.assertEqual(mod.run([sys.executable, '-c', 'import sys; sys.exit(5)'], '', capture=True), 5)
        self.assertEqual(install.NO_CONSOLE & subprocess.CREATE_NO_WINDOW, subprocess.CREATE_NO_WINDOW)

    def test_patch_after_a_stopped_run_translates_again(self):
        """N4: a run stopped inside 'patch' left part-patched code; the resume translates the program again first."""
        import gen_patch
        base, final = self.tmp / 'base', self.tmp / 'final'; base.mkdir(); final.mkdir()
        (base / 'a.c').write_bytes(b'one\r\ntwo\r\n'); (final / 'a.c').write_bytes(b'one\r\nmine();\r\ntwo\r\n')
        kit = self.tmp / 'kit'; gen_patch.make(base, final, kit / 'patches')
        proj = self.tmp / 'proj'; gen = proj / 'src' / 'recomp' / 'gen'; shutil.copytree(base, gen)
        (gen / 'a.c').write_bytes(b'one\nmine();\ntwo\n')                   # patched, then FastCode changed its line ends
        d = self.tmp / 'inst'; d.mkdir(); inst = install.Installer(None, d, log=lambda s: None)
        inst.state = {'done': ['recompile', 'generate'], 'patch_started': True}; inst.save = lambda: None
        inst.project = lambda: proj; inst.fastcode = lambda g: None; steps = []
        def recompile(): steps.append('recompile'); shutil.rmtree(gen); shutil.copytree(base, gen)
        inst.recompile = recompile; inst.generate = lambda: steps.append('generate')
        saved = install.KIT; install.KIT = kit
        try: inst.patch()
        finally: install.KIT = saved
        self.assertEqual(steps, ['recompile', 'generate'])
        self.assertEqual((gen / 'a.c').read_bytes(), (final / 'a.c').read_bytes())
        self.assertNotIn('patch_started', inst.state)
        inst.state['patch_started'] = True; (gen / 'a.c').write_bytes(b'not this release')   # no stopped run: a real mismatch
        inst.state.pop('patch_started'); steps.clear(); install.KIT = kit
        try:
            with self.assertRaises(install.Stop) as e: inst.patch()
        finally: install.KIT = saved
        self.assertEqual(steps, []); self.assertIn('does not match this release', str(e.exception))

    def test_compiler_state(self):
        s = install.compiler_state(); print('  compiler:', s['state'], s.get('name'), s.get('code'))
        self.assertIn(s['state'], ('ready', 'incomplete', 'missing'))

class Diagnostics(unittest.TestCase):
    """W-63: one zip with what a developer needs; text redacted; dumps and recordings only when asked."""
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix='ma2ux-diag-', dir=str(WORK / 'tmp')))
        self.saved = os.environ.get('LOCALAPPDATA'); os.environ['LOCALAPPDATA'] = str(self.base / 'LA')
        self.user = os.environ.get('USERNAME', 'someone')
        me = self.base / 'LA' / 'MechAssault2'; logs = me / 'Logs'; logs.mkdir(parents=True)
        for i in range(5):
            f = logs / ('2026111%d-%d.log' % (i, 100 + i)); f.write_text('game log %d for C:\\Users\\%s\\Games\\x\n' % (i, self.user)); os.utime(f, (1000 + i, 1000 + i))
        (logs / 'install-20261110-100000.log').write_text('installer log of %s\n' % self.user); (logs / 'installer-ui-20261110-100000.log').write_text('ui log\n')
        (logs / 'buildtools-20261110-100000.log').write_text('not wanted\n')
        (logs / 'crash-4242.dmp').write_bytes(b'MDMP' + self.user.encode() + b'\x00' * 100); (logs / 'hang-1.dmp').write_bytes(b'MDMP-old'); os.utime(logs / 'hang-1.dmp', (5, 5))
        (logs / 'huge-1.botrec').write_bytes(b'rec'); (me / 'pc_settings.ini').write_text('[Display]\nWidth=1280\n; %s\n' % self.user)
        self.dest = self.base / 'inst'; self.dest.mkdir(); (self.dest / precheck.STAMP).write_text(json.dumps({'version': '2026.11.10', 'shortcuts': ['C:\\Users\\%s\\Desktop\\a.lnk' % self.user]}))
    def tearDown(self):
        if self.saved is None: os.environ.pop('LOCALAPPDATA', None)
        else: os.environ['LOCALAPPDATA'] = self.saved
        shutil.rmtree(self.base, ignore_errors=True)

    def zip_members(self, path):
        import zipfile
        with zipfile.ZipFile(path) as z: return {n: z.read(n) for n in z.namelist()}

    def test_default_zip(self):
        res = precheck.collect_diagnostics(self.dest, self.base / 'Desktop', '1.0.2', 'abcd1234', [])
        self.assertTrue(res['path'].name.startswith('MA2-diagnostics-') and res['path'].suffix == '.zip')
        m = self.zip_members(res['path'])
        self.assertEqual(sorted(m), ['logs/20261112-102.log', 'logs/20261113-103.log', 'logs/20261114-104.log', 'logs/buildtools-20261110-100000.log',   # 1.1.2: the Build Tools log too
                                     'logs/install-20261110-100000.log',
                                     'logs/installer-ui-20261110-100000.log', 'pc_settings.ini', 'recomp-install.json', 'system.txt'])
        if len(self.user) > 2:
            for name, data in m.items(): self.assertNotIn(self.user.encode(), data, name)
        self.assertIn(b'%USERNAME%', m['logs/install-20261110-100000.log'])
        self.assertIn(b'MechAssault 2 Recomp 1.0.2 (build abcd1234)', m['system.txt']); self.assertIn(b'Screen: ', m['system.txt']); self.assertIn(b'Windows build', m['system.txt'])
        self.assertIn(b'Width=1280', m['pc_settings.ini'])

    def test_dump_and_recording_only_when_asked(self):
        m = self.zip_members(precheck.collect_diagnostics(self.dest, self.base / 'D1', '1', 'k', [], include_dump=True, include_botrec=True)['path'])
        self.assertIn('dumps/crash-4242.dmp', m); self.assertNotIn('dumps/hang-1.dmp', m, 'only the newest dump'); self.assertIn('botrec/huge-1.botrec', m)
        self.assertIn(b'crash-4242.dmp', m['system.txt'])
        # a dump over 50 MB is skipped with a note
        real = Path.stat
        big = self.base / 'LA' / 'MechAssault2' / 'Logs' / 'crash-4242.dmp'
        with open(big, 'r+b') as f: f.truncate(51 * 1024 * 1024)
        res = precheck.collect_diagnostics(self.dest, self.base / 'D2', '1', 'k', [], include_dump=True)
        self.assertNotIn('dumps/crash-4242.dmp', self.zip_members(res['path'])); self.assertTrue(any('larger than 50 MB' in n for n in res['notes']))

    def test_missing_pieces_are_noted_not_fatal(self):
        shutil.rmtree(self.base / 'LA'); (self.base / 'LA').mkdir()
        res = precheck.collect_diagnostics(self.base, self.base / 'Desktop', '1', 'k', [])
        m = self.zip_members(res['path']); self.assertEqual(sorted(m), ['system.txt'])
        self.assertTrue(any('no game logs' in n for n in res['notes'])); self.assertIn(b'Note: ', m['system.txt'])

class DiagnosticsAnywhere(unittest.TestCase):
    """1.1.1: Collect diagnostics works with nothing installed and on a failed or interrupted install; the user name never reaches a text member."""
    def setUp(self):
        self.base = Path(tempfile.mkdtemp(prefix='ma2ux-diag2-', dir=str(WORK / 'tmp')))
        self.saved = {k: os.environ.get(k) for k in ('LOCALAPPDATA', 'USERNAME', 'USERPROFILE')}
        os.environ['LOCALAPPDATA'] = str(self.base / 'LA')           # under C:\\Users\\<user>\\...: the notes name this folder
        self.user = os.environ.get('USERNAME', 'someone')
    def tearDown(self):
        for k, v in self.saved.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
        shutil.rmtree(self.base, ignore_errors=True)
    def members(self, res):
        import zipfile
        with zipfile.ZipFile(res['path']) as z: return {n: z.read(n) for n in z.namelist()}
    def no_user(self, m):
        if len(self.user) > 2:
            for name, data in m.items(): self.assertNotIn(self.user.encode().lower(), data.lower(), name)

    def test_nothing_installed_nothing_else(self):
        """No folder, no logs, no settings, no LOCALAPPDATA folder at all: a zip with system.txt that says what was not found."""
        for dest in (self.base / 'does-not-exist', self.base):
            res = precheck.collect_diagnostics(dest, self.base / 'Desktop', '1.1.1', 'abcd1234', [])
            m = self.members(res)
            self.assertEqual(sorted(m), ['system.txt'])
            txt = m['system.txt'].decode()
            self.assertIn('MechAssault 2 Recomp 1.1.1 (build abcd1234)', txt); self.assertIn('Windows build', txt)
            self.assertIn('recomp-install.json was not found', txt); self.assertIn('no game logs found', txt); self.assertIn('no pc_settings.ini', txt)
            self.assertNotIn('Failure:', txt); self.assertNotIn('Unfinished install', txt)
            self.no_user(m)                                          # the notes name %LOCALAPPDATA%, which is under the profile

    def test_nothing_installed_but_logs_and_settings_exist(self):
        me = self.base / 'LA' / 'MechAssault2'; (me / 'Logs').mkdir(parents=True)
        (me / 'Logs' / '20261110-1.log').write_text('log of %s\n' % self.user); (me / 'pc_settings.ini').write_text('[Display]\nWidth=1280\n')
        (self.base / 'empty').mkdir()
        m = self.members(precheck.collect_diagnostics(self.base / 'empty', self.base / 'Desktop', '1', 'k', []))
        self.assertEqual(sorted(m), ['logs/20261110-1.log', 'pc_settings.ini', 'system.txt']); self.no_user(m)

    def test_failed_install_names_the_step_and_keeps_the_newest_install_log(self):
        logs = self.base / 'LA' / 'MechAssault2' / 'Logs'; logs.mkdir(parents=True)
        for i, name in enumerate(('install-20261110-100000.log', 'install-20261111-100000.log')):
            (logs / name).write_text('installer log %d of %s\n' % (i, self.user)); os.utime(logs / name, (1000 + i, 1000 + i))
        dest = self.base / 'inst'; (dest / '_work').mkdir(parents=True)
        (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc', 'extract'], 'last_title': 'Compiling the game', 'xiso': 'D:\\secret folder\\my disc.iso'}))
        res = precheck.collect_diagnostics(dest, self.base / 'Desktop', '1.1.1', 'k', [], stage='build', failure='build_failed: Building the game failed in C:\\Users\\%s\\x' % self.user,
                                           log_lines=['[3/9] compile', 'FAILED: C:\\Users\\%s\\y' % self.user])
        m = self.members(res); txt = m['system.txt'].decode()
        self.assertIn('logs/install-20261111-100000.log', m); self.assertNotIn('logs/install-20261110-100000.log', m, 'only the newest install log')
        self.assertIn('Failed stage: build', txt); self.assertIn('Failure: build_failed: Building the game failed', txt)
        self.assertIn('Unfinished install: 2 step(s) finished (disc, extract); last step: Compiling the game', txt)
        self.assertIn('Last log lines:', txt); self.assertIn('FAILED: ', txt)
        self.assertNotIn(b'secret folder', b''.join(m.values()), 'the disc path in state.json is not copied')
        self.no_user(m)

    def test_extras_only_when_something_exists(self):
        self.assertEqual(precheck.diagnostics_extras(), {'dump': None, 'botrec': None})
        logs = self.base / 'LA' / 'MechAssault2' / 'Logs'; logs.mkdir(parents=True)
        self.assertEqual(precheck.diagnostics_extras(), {'dump': None, 'botrec': None})
        for n, t in (('crash-1.dmp', 10), ('hang-2.dmp', 20), ('a.botrec', 5)):
            (logs / n).write_bytes(b'x'); os.utime(logs / n, (t, t))
        ex = precheck.diagnostics_extras()
        self.assertEqual((ex['dump'].name, ex['botrec'].name), ('hang-2.dmp', 'a.botrec'))

class FpuCheck(unittest.TestCase):
    """W-65: check_fpu.py passes on the assembled tree and fails when any of the five FPU C2 fixes is reverted."""
    def setUp(self):
        sys.path.insert(0, str(WORK.parent))
        try: import check_fpu
        except ImportError: self.skipTest('check_fpu.py is a maintainer tool (not in this repository)')
        self.cf = check_fpu
        self.gen = WORK.parent / 'stage' / 'game' / 'src' / 'recomp' / 'gen'
        if not (self.gen / 'recomp_0077.c').is_file(): self.skipTest('no assembled stage tree')
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-fpu-', dir=str(WORK / 'tmp')))
        for n in ('recomp_0077.c', 'recomp_0079.c'): shutil.copy2(self.gen / n, self.tmp / n)
    def tearDown(self):
        sys.path.remove(str(WORK.parent)); shutil.rmtree(getattr(self, 'tmp', '.'), ignore_errors=True)

    def test_passes_on_the_current_tree(self):
        self.assertEqual(self.cf.check(self.tmp), [])
        r = subprocess.run([sys.executable, '-B', str(WORK.parent / 'check_fpu.py'), str(self.tmp)], capture_output=True, text=True); self.assertEqual(r.returncode, 0, r.stderr)

    def test_fails_on_each_reverted_site(self):
        for name, what, target in self.cf.SITES:
            for n in ('recomp_0077.c', 'recomp_0079.c'): shutil.copy2(self.gen / n, self.tmp / n)
            p = self.tmp / name; text = p.read_bytes().decode('latin-1')
            old = 'if (0) goto %s;' % target; self.assertEqual(text.count(old), 1, target)
            p.write_bytes(text.replace(old, 'if ((g_fp_cmp == 2) /* sahf */) goto %s;' % target).encode('latin-1'))
            problems = self.cf.check(self.tmp)
            self.assertTrue(problems and any(target in x for x in problems), (target, problems))
            r = subprocess.run([sys.executable, '-B', str(WORK.parent / 'check_fpu.py'), str(self.tmp)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1); self.assertIn(target, r.stderr)

    def test_missing_file_is_a_problem(self):
        (self.tmp / 'recomp_0079.c').unlink(); self.assertTrue(any('recomp_0079.c cannot be read' in p for p in self.cf.check(self.tmp)))

class WorkFolders(unittest.TestCase):
    """W-59: the ASCII build folder is recorded, owned, cleaned up when stale, and never left looking like an interrupted install."""
    def setUp(self):
        self.base = _test_dir(self, 'wf')
    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def _inst(self, name='MA2 Jos\u00e9'):
        d = self.base / name; inst = install.Installer(None, d, log=lambda s: None)
        return d, inst

    def test_build_folder_is_recorded_and_start_over_removes_it(self):
        d, inst = self._inst(); inst.work.mkdir(parents=True); inst.state = {'done': []}
        root = inst.build_root(); self.assertNotEqual(root, inst.work, 'a non-ASCII install path builds elsewhere')
        try:
            self.assertTrue(root.name.startswith('MA2RecompBuild-') and str(root).isascii())
            (root / 'owner.txt').write_text(str(d), encoding='utf-8'); (root / 'junk.obj').write_text('x')
            inst.state['build_root'] = str(root); inst.save()
            self.assertEqual(precheck.read_state(d)['build_root'], str(root))
            install.start_over(d)
            self.assertFalse(inst.work.exists()); self.assertFalse(root.exists(), 'start over removes the build folder too')
            self.assertTrue(d.exists(), 'the install folder itself stays')
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_only_our_folders_are_removed(self):
        mine = self.base / 'MA2RecompBuild-test'; mine.mkdir(); (mine / 'owner.txt').write_text(str(self.base / 'A'))
        self.assertFalse(install.remove_build_folder(mine, owner=self.base / 'B'), 'a folder owned by another install is kept')
        plain = self.base / 'MA2RecompBuild-noowner'; plain.mkdir()
        self.assertFalse(install.remove_build_folder(plain)); self.assertTrue(plain.exists(), 'no owner.txt: not ours')
        other = self.base / 'Documents'; other.mkdir(); (other / 'owner.txt').write_text('x')
        self.assertFalse(install.remove_build_folder(other)); self.assertTrue(other.exists(), 'wrong name: never touched')
        self.assertTrue(install.remove_build_folder(mine, owner=self.base / 'A')); self.assertFalse(mine.exists())

    def test_stale_build_folders_are_cleaned_at_the_start(self):
        live = self.base / 'LiveInstall'; live.mkdir()
        stale = self.base / 'MA2RecompBuild-aaaa1111'; stale.mkdir(); (stale / 'owner.txt').write_text(str(self.base / 'DeletedInstall'))
        kept = self.base / 'MA2RecompBuild-bbbb2222'; kept.mkdir(); (kept / 'owner.txt').write_text(str(live))
        foreign = self.base / 'MA2RecompBuild-cccc3333'; foreign.mkdir()
        gone = install.clean_stale_build_folders([self.base, self.base / 'missing-base'])
        self.assertEqual(gone, [stale]); self.assertFalse(stale.exists()); self.assertTrue(kept.exists()); self.assertTrue(foreign.exists())

    def test_finish_when_the_work_folder_cannot_be_removed(self):
        d, inst = self._inst('MA2 ascii'); inst.work.mkdir(parents=True)
        (inst.work / 'state.json').write_text('{"done": ["check"]}'); (inst.work / 'bigfile').write_text('x')
        notes = []; logs = []; inst.log = logs.append
        real_rmtree, real_sleep = shutil.rmtree, time.sleep
        shutil.rmtree = lambda *a, **k: (_ for _ in ()).throw(PermissionError('held by a scanner'))
        install.time.sleep = lambda s: None
        try:
            # the finish() tail, as finish() runs it: rename the state file first, then remove
            state_file = inst.work / 'state.json'; state_file.replace(inst.work / 'state.finished.json')
            inst.remove_work_folders([inst.work])
        finally:
            shutil.rmtree = real_rmtree; install.time.sleep = real_sleep
        self.assertFalse((inst.work / 'state.json').exists(), 'no state.json left: Welcome will not offer Resume')
        self.assertTrue(any('Could not remove' in n and str(inst.work) in n and 'you can delete it' in n for n in inst.notes), inst.notes)
        self.assertTrue(any('could not remove' in l for l in logs))
        self.assertIsNone(precheck.read_state(d), 'detect_existing sees no interrupted install')
        # and the normal case
        inst2 = install.Installer(None, self.base / 'MA2 two', log=lambda s: None); inst2.work.mkdir(parents=True)
        inst2.remove_work_folders([inst2.work]); self.assertFalse(inst2.work.exists()); self.assertEqual(inst2.notes, [])

class Uninstall(unittest.TestCase):
    """W-60: uninstall deletes only what is ours, only inside a real install, and never the player's scenarios."""
    def setUp(self):
        self.base = _test_dir(self, 'un')
        self.local = self.base / 'LocalAppData'; self.docs = self.base / 'Documents'; self.lnk = self.base / 'Shortcuts'
        self.saved_env = {k: os.environ.get(k) for k in ('LOCALAPPDATA', 'MA2_DOCUMENTS_DIR')}
        os.environ['LOCALAPPDATA'] = str(self.local); os.environ['MA2_DOCUMENTS_DIR'] = str(self.docs)
        (self.docs / 'MechAssault 2' / 'Scenarios').mkdir(parents=True); (self.docs / 'MechAssault 2' / 'Scenarios' / 'mine.ma2map').write_text('my map')
    def tearDown(self):
        for k, v in self.saved_env.items():
            if v is None: os.environ.pop(k, None)
            else: os.environ[k] = v
        shutil.rmtree(self.base, ignore_errors=True)

    def make_install(self, name='Games/MA2', extra_shortcuts=True):
        d = self.base / name
        for f, data in (('MechAssault 2.exe', 'exe'), ('game_files/ma2.xbe', 'xbe'), ('game_files/a.mgf', 'a'), ('pc_assets/common.mgf', 'c'), ('Licenses/x.txt', 'l'),
                        ('MA2 Scenario Editor.exe', 'e'), ('README.txt', 'r')):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text(data)
        made = []
        for n in ('MechAssault 2 (Recomp).lnk', 'MA2 Scenario Editor.lnk'):
            p = self.lnk / 'desktop' / n; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('lnk'); made.append(str(p))
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': 'k', 'version': '2026.11.10', 'shortcuts': made}))
        me = self.local / 'MechAssault2'
        for f in ('pc_settings.ini', 'Saved Games/p1.sav', 'Logs/g.log', 'ShaderCache/s.bin', 'NavCache/n.bin'):
            (me / f).parent.mkdir(parents=True, exist_ok=True); (me / f).write_text('x')
        (self.local / 'xboxrecomp' / 'shader-cache').mkdir(parents=True, exist_ok=True); (self.local / 'xboxrecomp' / 'shader-cache' / 'old.bin').write_text('x')
        (me / 'installer.json').write_text(json.dumps({'dest': str(d), 'kit_id': 'k'}))
        return d, made

    def test_uninstall_keeps_settings_saves_and_scenarios(self):
        d, made = self.make_install(); logs = []
        res = install.uninstall(d, False, log=logs.append)
        self.assertFalse(d.exists()); self.assertEqual(res['failed'], [])
        for p in made: self.assertFalse(Path(p).exists(), p)
        me = self.local / 'MechAssault2'
        self.assertTrue((me / 'pc_settings.ini').is_file() and (me / 'Saved Games' / 'p1.sav').is_file() and (me / 'Logs' / 'g.log').is_file(), 'settings, saves and logs are kept')
        self.assertFalse((me / 'ShaderCache').exists() or (me / 'NavCache').exists() or (self.local / 'xboxrecomp' / 'shader-cache').exists(), 'caches go')
        self.assertEqual((self.docs / 'MechAssault 2' / 'Scenarios' / 'mine.ma2map').read_text(), 'my map')
        self.assertIn('were kept in', res['text']); self.assertNotIn('Build Tools', res['text'])
        self.assertNotIn('dest', json.loads((me / 'installer.json').read_text()), 'the install is forgotten')

    def test_uninstall_with_settings_and_saves(self):
        d, made = self.make_install()
        res = install.uninstall(d, True, log=lambda s: None)
        self.assertFalse(d.exists()); self.assertFalse((self.local / 'MechAssault2').exists(), 'the tick box also deletes settings, saves and logs')
        self.assertTrue((self.docs / 'MechAssault 2' / 'Scenarios' / 'mine.ma2map').is_file(), 'Documents\\MechAssault 2\\Scenarios is never deleted')
        self.assertIn('were deleted', res['text'])

    def test_build_tools_sentence_only_when_we_installed_them(self):
        d, _ = self.make_install(); install.mark_build_tools_installed()
        inst = install.Installer(None, d, log=lambda s: None); inst.remember_install()                         # keeps the flag
        self.assertTrue(precheck.read_json(self.local / 'MechAssault2' / 'installer.json')['build_tools_installed_by_us'])
        res = install.uninstall(d, False, log=lambda s: None)
        self.assertIn("Microsoft's C++ Build Tools were installed by this installer", res['text']); self.assertIn('Windows Settings > Apps', res['text'])

    def test_refuses_what_is_not_ours(self):
        stranger = self.base / 'Other'; (stranger / 'photos').mkdir(parents=True); (stranger / 'photos' / 'a.jpg').write_text('x')
        with self.assertRaises(install.Stop) as e: install.uninstall(stranger, True, log=lambda s: None)
        self.assertIn('not a MechAssault 2 Recomp install', str(e.exception)); self.assertTrue((stranger / 'photos' / 'a.jpg').is_file())
        # an install folder that holds the scenarios, the settings or Documents: nothing is deleted
        for name in ('Documents', 'LocalAppData', ''):
            d, _ = self.make_install('Games/MA2')
            container = {'Documents': self.docs, 'LocalAppData': self.local, '': self.base}[name]
            for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
                (container / f).parent.mkdir(parents=True, exist_ok=True); (container / f).write_text('x')
            with self.assertRaises(install.Stop, msg=name): install.uninstall(container, True, log=lambda s: None)
            self.assertTrue((self.docs / 'MechAssault 2' / 'Scenarios' / 'mine.ma2map').is_file(), name)
        inside = self.docs / 'MechAssault 2' / 'Scenarios' / 'Inst'
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (inside / f).parent.mkdir(parents=True, exist_ok=True); (inside / f).write_text('x')
        with self.assertRaises(install.Stop): install.uninstall(inside, False, log=lambda s: None)

    def test_a_junction_is_removed_not_followed(self):
        d, _ = self.make_install(); target = self.base / 'precious'; target.mkdir(); (target / 'keep.txt').write_text('keep')
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', str(d / 'game_files' / 'link'), str(target)], capture_output=True, text=True, creationflags=0x08000000)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        res = install.uninstall(d, False, log=lambda s: None)
        self.assertFalse(d.exists()); self.assertEqual((target / 'keep.txt').read_text(), 'keep', 'what a junction points to is never touched')

    def test_a_file_that_cannot_be_deleted_is_reported(self):
        d, _ = self.make_install(); real = os.unlink
        def unlink(p, *a, **k):
            if str(p).endswith('MechAssault 2.exe'): raise PermissionError(13, 'Access is denied')
            return real(p, *a, **k)
        os.unlink = unlink
        try:
            res = install.uninstall(d, False, log=lambda s: None)
        finally:
            os.unlink = real
        self.assertTrue(res['failed'] and 'MechAssault 2.exe' in res['failed'][0][0]); self.assertTrue(res['text'].startswith('Not everything could be deleted'))
        self.assertIn('Close MechAssault 2', res['text'])

    def test_plan_lists_exactly_what_goes(self):
        d, made = self.make_install(); plan = install.uninstall_plan(d, True)
        self.assertEqual(sorted(map(str, plan['shortcuts'])), sorted(made)); self.assertEqual(plan['dest'], d.resolve())
        self.assertEqual(plan['user_data'], self.local / 'MechAssault2'); self.assertEqual(len(plan['caches']), 3)
        self.assertIsNone(install.uninstall_plan(d, False)['user_data']); self.assertTrue(d.exists(), 'a plan deletes nothing')

if __name__ == '__main__':
    unittest.main(verbosity=2)

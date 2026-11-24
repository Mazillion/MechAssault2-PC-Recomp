"""1.2.1 Linux through Wine/Proton, installer side: Wine detection, paths, the LLVM toolchain in place of MSVC, the install stamp's
host, reports, d3dcompiler_47.dll by Wine version, recomplauncher:// in the prefix, and that NONE of it changes Windows.

Runs on Windows; MA2_TEST_WINE=<version> pretends to be Wine where a test needs it (the real Wine run is separate, see
docs/linux-proton.md).
"""
import hashlib, io, json, os, shutil, sys, tarfile, tempfile, unittest, zipfile
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
KIT = REPO / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
import wine_host, precheck, toolchain_fetch, install                       # noqa: E402


class Wine(object):
    """Pretend to be Wine <version> for the block (wine_host reads MA2_TEST_WINE every call)."""
    def __init__(self, v='9.0'): self.v = v
    def __enter__(self): self.old = os.environ.get('MA2_TEST_WINE'); os.environ['MA2_TEST_WINE'] = self.v
    def __exit__(self, *a):
        if self.old is None: os.environ.pop('MA2_TEST_WINE', None)
        else: os.environ['MA2_TEST_WINE'] = self.old


class OnWindows(unittest.TestCase):
    """Windows behaviour stays exactly 1.2.0's."""
    def setUp(self):
        os.environ.pop('MA2_TEST_WINE', None); os.environ.pop('MA2_TOOLCHAIN', None)

    def test_not_wine(self):
        self.assertIsNone(wine_host.wine_version()); self.assertFalse(wine_host.is_wine()); self.assertFalse(wine_host.use_clang())
        self.assertEqual(wine_host.describe(), ''); self.assertEqual(precheck.wine_line(), '')
        self.assertFalse(wine_host.needs_own_d3dcompiler())
        self.assertEqual(wine_host.register_launcher_link(), '')

    def test_paths_unchanged(self):
        for p in (r'C:\Games\MechAssault 2 Expanded', '/srv/x', r'\\server\share\x', '%TEMP%'):
            self.assertEqual(wine_host.dos_path(p), p)
        self.assertEqual(precheck.clean_path('/srv/games/x'), '/srv/games/x')
        self.assertEqual(precheck.clean_path(' "C:\\G\\M" '), 'C:\\G\\M')

    def test_build_script_and_crt_are_msvc(self):
        src = (KIT / 'installer' / 'install.py').read_text(encoding='utf-8')
        body = src.split('    def build(self', 1)[1].split('    def assets(self', 1)[0]
        clang, msvc = body.split("            if wine_host.use_clang():" + chr(10), 1)[1].split("            script.write_text(", 1)
        self.assertIn('CMAKE_TOOLCHAIN_FILE', clang); self.assertNotIn('CMAKE_TOOLCHAIN_FILE', msvc)     # the toolchain file only under clang
        self.assertIn("'chcp 65001 >nul'", msvc); self.assertIn("'call \"%s\" >nul || exit /b 1' % self.vcvars", msvc)   # 1.2.0's script
        self.assertIn("crt = [] if wine_host.use_clang() else sorted((self.vs / 'VC' / 'Redist'", src)

    def test_space_plan_build_tools_8gb(self):
        src = (KIT / 'installer' / 'install.py').read_text(encoding='utf-8')
        self.assertIn("(3 if wine_host.use_clang() else 8) * GB, 0, 'buildtools')", src)

    def test_sitecustomize_does_nothing_here(self):
        import _winapi
        had = hasattr(_winapi, 'CopyFile2')
        ns = {}
        exec(compile((KIT / 'installer' / 'data' / 'sitecustomize.py').read_text(encoding='utf-8'), 'sitecustomize', 'exec'), ns)
        self.assertEqual(hasattr(_winapi, 'CopyFile2'), had)

    def test_sitecustomize_is_packed(self):
        sys.path.insert(0, str(REPO / 'installer-exe'))
        try:
            import pack_installer
            names = [r for r, _ in pack_installer.files()]
        finally:
            sys.path.pop(0)
        self.assertIn('python/Lib/sitecustomize.py', names)
        self.assertFalse([n for n in names if 'd3dcompiler' in n.lower()])         # owner 11-24: never a Microsoft DLL in the kit


class UnderWine(unittest.TestCase):
    def test_detect_and_describe(self):
        with Wine('9.0'):
            self.assertTrue(wine_host.is_wine()); self.assertTrue(wine_host.use_clang())
            self.assertIn('Wine 9.0', wine_host.describe()); self.assertTrue(precheck.wine_line().startswith('Running under: Wine 9.0'))
            with mock.patch.dict(os.environ, {'SteamAppId': '1', 'SteamDeck': '1'}):
                self.assertIn('Proton', wine_host.describe()); self.assertIn('Steam Deck', wine_host.describe())

    def test_linux_paths(self):
        with Wine('9.0'):
            self.assertEqual(wine_host.dos_path('/srv/deck/Games/Recomp Library/MechAssault 2 Expanded'), r'Z:\srv\deck\Games\Recomp Library\MechAssault 2 Expanded')
            self.assertEqual(precheck.clean_path(' /srv/deck/x '), r'Z:\srv\deck\x')
            self.assertEqual(precheck.clean_path(r'Z:\srv\deck\x.iso'), r'Z:\srv\deck\x.iso')     # RecompLauncher's {input:...}
            self.assertEqual(precheck.clean_path(r'C:\Games\MechAssault 2 Expanded'), r'C:\Games\MechAssault 2 Expanded')

    def test_d3dcompiler_by_wine_version(self):
        self.assertEqual(wine_host.WINE_BUILTIN_D3DCOMPILER_MIN, (10, 0))
        for v, need in (('9.0', True), ('9.22', True), ('10.0', False), ('10.0-rc3', False), ('11.14', False)):
            with Wine(v):
                self.assertEqual(wine_host.needs_own_d3dcompiler(), need, v)

    def test_d3dcompiler_check(self):
        d = Path(tempfile.mkdtemp())
        try:
            self.assertFalse(wine_host.d3dcompiler_state(d)['found'])
            fake = bytearray(2 << 20); fake[:2] = b'MZ'; fake[0x3C:0x40] = (0x80).to_bytes(4, 'little')
            fake[0x80:0x84] = b'PE\0\0'; fake[0x84:0x86] = (0x8664).to_bytes(2, 'little')
            (d / 'd3dcompiler_47.dll').write_bytes(bytes(fake))
            self.assertTrue(wine_host.d3dcompiler_state(d)['found'])
            fake[0x40:0x40 + 20] = b'Wine builtin DLL\0\0\0\0'; (d / 'd3dcompiler_47.dll').write_bytes(bytes(fake))
            self.assertFalse(wine_host.d3dcompiler_state(d)['found'])                 # Wine's own placeholder is not the real one
            (d / 'd3dcompiler_47.dll').write_bytes(b'MZ' + b'\0' * 100)
            self.assertFalse(wine_host.d3dcompiler_state(d)['found'])
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_d3dcompiler_message_only_on_old_wine(self):
        d = Path(tempfile.mkdtemp())
        try:
            inst = install.Installer(None, d / 'G', log=lambda s: None)
            inst.save = lambda: None
            with Wine('10.0'):
                inst.notes = []; inst.wine_d3dcompiler(); self.assertEqual(inst.notes, []); self.assertEqual(inst.state['d3dcompiler'], 'wine')
            with Wine('9.0'):
                inst.notes = []; inst.wine_d3dcompiler()
                self.assertEqual(len(inst.notes), 1); self.assertIn('d3dcompiler_47.dll', inst.notes[0]); self.assertIn('9.0', inst.notes[0])
                self.assertEqual(inst.state['d3dcompiler'], 'missing')
            inst.notes = []; inst.wine_d3dcompiler(); self.assertEqual(inst.notes, [])          # Windows: never
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_compiler_state_names_llvm(self):
        with Wine('9.0'), mock.patch.dict(os.environ, {'MA2_WINE_TOOLCHAIN': tempfile.mkdtemp()}):
            st = install.compiler_state()
            self.assertEqual((st['state'], st['clang']), ('missing', True))
            self.assertIn('LLVM', st['detail']); self.assertIn('--install-build-tools', st['detail'])

    def test_env_script(self):
        root = Path(tempfile.mkdtemp())
        try:
            text = wine_host.env_script(root, root / 'e.cmd').read_text(encoding='utf-8')
            for k in ('INCLUDE=', 'LIB=', 'MA2_LLVM=', 'MA2_XWIN=', 'exit /b 0'): self.assertIn(k, text)
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_toolchain_file(self):
        t = wine_host.toolchain_file().read_text(encoding='utf-8')
        for k in ('clang-cl.exe', 'lld-link.exe', 'llvm-rc.exe', 'llvm-mt.exe', 'CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded"',
                  'CMAKE_TRY_COMPILE_CONFIGURATION Release', '-fwrapv'):
            self.assertIn(k, t)

    def test_preseed_is_msvc_only(self):
        with Wine('9.0'):
            inst = install.Installer(None, Path(tempfile.mkdtemp()) / 'G', log=lambda s: None)
            self.assertEqual(inst.preseed_kind(4 << 30), 'none')


class ToolchainFetch(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_pins(self):
        for name in ('llvm', 'cmake', 'ninja', 'xwin'):
            s = toolchain_fetch.SPEC[name]
            self.assertRegex(s['sha256'], r'^[0-9a-f]{64}$', name)
            self.assertTrue(all(u.startswith('https://github.com/') for u in s['urls']), name)
        self.assertIn('--disable-symlinks', toolchain_fetch.SPEC['xwin']['args'])
        self.assertIn('--accept-license', toolchain_fetch.SPEC['xwin']['args'])
        self.assertNotIn('ship', toolchain_fetch.SPEC.get('d3dcompiler', {}))

    def zip_with(self, members):
        p = self.tmp / 'a.zip'
        with zipfile.ZipFile(p, 'w') as z:
            for n, data in members: z.writestr(n, data)
        return p

    def test_unpack_checks_sha_and_takes_only_named(self):
        a = self.zip_with([('root/bin/tool.exe', b'MZtool'), ('root/share/x.txt', b'x'), ('root/other.txt', b'no'), ('../evil.txt', b'e')])
        spec = {'archive': 'a.zip', 'version': '1', 'root': 'root', 'take': ['bin/tool.exe', 'share/'], 'sha256': hashlib.sha256(a.read_bytes()).hexdigest()}
        toolchain_fetch.unpack(a, spec, self.tmp / 'out')
        got = sorted(p.relative_to(self.tmp / 'out').as_posix() for p in (self.tmp / 'out').rglob('*') if p.is_file())
        self.assertEqual(got, ['bin/tool.exe', 'share/x.txt'])
        self.assertFalse((self.tmp / 'evil.txt').exists())
        bad = dict(spec, sha256='0' * 64)
        with self.assertRaises(toolchain_fetch.FetchError): toolchain_fetch.unpack(a, bad, self.tmp / 'out2')
        with self.assertRaises(toolchain_fetch.FetchError): toolchain_fetch.unpack(a, dict(spec, sha256=None), self.tmp / 'out3')

    def test_unpack_tar(self):
        p = self.tmp / 'a.tar.gz'
        with tarfile.open(p, 'w:gz') as t:
            for n, data in (('r/xwin.exe', b'MZx'), ('r/README', b'r')):
                i = tarfile.TarInfo(n); i.size = len(data); t.addfile(i, io.BytesIO(data))
        spec = {'archive': 'a.tar.gz', 'version': '1', 'root': 'r', 'take': ['xwin.exe'], 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
        toolchain_fetch.unpack(p, spec, self.tmp / 'o')
        self.assertEqual((self.tmp / 'o' / 'xwin.exe').read_bytes(), b'MZx'); self.assertFalse((self.tmp / 'o' / 'README').exists())

    def test_no_licence_no_xwin(self):
        with Wine('9.0'), mock.patch.dict(os.environ, {'MA2_TOOLCHAIN_SOURCE': ''}), \
                mock.patch.object(toolchain_fetch, '_part_ok', lambda root, name: name != 'xwin'):
            with self.assertRaises(toolchain_fetch.FetchError) as c:
                toolchain_fetch.ensure(self.tmp, log=lambda s: None, accept_licence=False)
            self.assertIn('licence', str(c.exception))


class LauncherProfile(unittest.TestCase):
    def test_linux_tick(self):
        m = json.loads((REPO / 'recomplauncher.json').read_text(encoding='utf-8'))
        o = {x['id']: x for x in m['prepare']['options']}
        self.assertEqual(o['linux_build_tools']['os'], ['linux'])
        self.assertEqual(o['vs_build_tools']['os'], ['windows'])
        self.assertEqual(m['prepare']['args'][-1], '{install}/MechAssault 2 Expanded')    # a /home/... path under Proton: clean_path turns it into Z:\


class Stamp(unittest.TestCase):
    def test_host_only_under_wine(self):
        src = (KIT / 'installer' / 'install.py').read_text(encoding='utf-8')
        self.assertIn("if wine_host.is_wine(): stamp['host'] = 'wine'", src)
        self.assertIn("else: stamp.pop('host', None)", src)


if __name__ == '__main__':
    unittest.main(verbosity=2)

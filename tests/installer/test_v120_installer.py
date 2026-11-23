"""1.2.0 installer fixes from report MA2-H3DVVH.

1. Only the CMake and Ninja that come with the chosen Visual Studio are used. A Visual Studio without "C++ CMake tools for
   Windows" stops with the add-this-component message, even when another toolchain's cmake/ninja is on PATH (that one broke
   the resource compiler with RC1107).
2. Logs are copied into a report without a byte-order mark, and the stub writes its log as plain UTF-8 bytes.
"""
import os, re, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / 'kit' / 'installer'))
import install, precheck                                                  # noqa: E402


class CMakeFromVisualStudioOnly(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2-120-'))
        self.vs = self.tmp / 'VS'
        for rel in ('VC/Auxiliary/Build/vcvars64.bat', 'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'):
            (self.vs / rel).parent.mkdir(parents=True, exist_ok=True); (self.vs / rel).write_bytes(b'x')
        self.path_tools = self.tmp / 'retcomm' / 'bin'                     # someone else's toolchain on PATH
        self.path_tools.mkdir(parents=True)
        for exe in ('cmake.exe', 'ninja.exe'): (self.path_tools / exe).write_bytes(b'x')
        self.sdk = mock.patch.object(install, 'windows_sdk', return_value=Path('windows.h')); self.sdk.start()
        self.path = mock.patch.dict(os.environ, {'PATH': str(self.path_tools) + os.pathsep + os.environ.get('PATH', '')}); self.path.start()

    def tearDown(self):
        mock.patch.stopall()
        import shutil; shutil.rmtree(self.tmp, ignore_errors=True)

    def test_cmake_on_path_is_never_used(self):
        with self.assertRaises(install.Stop) as c:
            install.find_visual_studio(self.vs)
        e = c.exception
        self.assertEqual(e.missing, ['cmake'])
        self.assertIn('C++ CMake tools for Windows', str(e))
        self.assertNotIn(str(self.path_tools), str(e))
        state = install.compiler_state(self.vs)                            # the Systems page offers to add exactly that component
        self.assertEqual((state['state'], state['add']), ('incomplete', ['Microsoft.VisualStudio.Component.VC.CMake.Project']))

    def test_the_bundled_tools_are_used(self):
        cm = self.vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake'
        for rel in ('CMake/bin/cmake.exe', 'Ninja/ninja.exe'):
            (cm / rel).parent.mkdir(parents=True, exist_ok=True); (cm / rel).write_bytes(b'x')
        vcvars, cmake, ninja, vs = install.find_visual_studio(self.vs)
        self.assertEqual((cmake, ninja), (cm / 'CMake/bin/cmake.exe', cm / 'Ninja/ninja.exe'))

    def test_no_which_fallback_left(self):
        src = (REPO / 'kit' / 'installer' / 'install.py').read_text(encoding='utf-8')
        self.assertIsNone(re.search(r"which\(\s*['\"](cmake|ninja)", src))

    def test_tool_version_never_raises(self):
        self.assertEqual(install.tool_version(self.tmp / 'missing.exe'), '?')

    # -- the installer adds the missing CMake tools itself (owner, 11-23)
    def bundle(self):
        cm = self.vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake'
        for rel in ('CMake/bin/cmake.exe', 'Ninja/ninja.exe'):
            (cm / rel).parent.mkdir(parents=True, exist_ok=True); (cm / rel).write_bytes(b'x')
        return 0

    def add(self, run, busy=lambda: False):
        lines = []
        with mock.patch.object(install, 'vs_installer_exe', return_value=Path('vs_installer.exe')):
            why = install.add_vs_parts(self.vs, ['cmake'], lines.append, run=run, busy=busy, sleep=lambda s: None)
        return why, lines

    def test_add_step_adds_the_component_and_then_finds_it(self):
        why, lines = self.add(self.bundle)
        self.assertEqual(why, '')
        self.assertTrue(any('adding C++ CMake tools for Windows to Visual Studio' in l for l in lines), lines)
        self.assertIn('  C++ CMake tools for Windows added', lines)
        self.assertEqual(install.find_visual_studio(self.vs)[1], self.vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')

    def test_the_exact_command(self):
        got = {}
        def fake_run(exe, args, *a, **k):
            got.update(exe=exe, args=args); self.bundle(); return 0
        with mock.patch.object(install, 'vs_installer_exe', return_value=Path('vs_installer.exe')), \
             mock.patch.object(install, 'run_elevated', fake_run):
            self.assertEqual(install.add_vs_parts(self.vs, ['cmake'], lambda s: None, busy=lambda: False, sleep=lambda s: None), '')
        self.assertEqual(got['args'], ['modify', '--installPath', str(self.vs), '--add', 'Microsoft.VisualStudio.Component.VC.CMake.Project',
                                       '--passive', '--norestart'])

    def test_installer_that_returns_early_is_waited_for(self):
        state = {'n': 0}
        def busy():
            state['n'] += 1
            if state['n'] == 3: self.bundle()                              # setup.exe finishes a little later
            return True
        why, lines = self.add(lambda: 0, busy)
        self.assertEqual(why, '')

    def test_failures_give_a_reason_never_a_build(self):
        def declined(): raise install.Stop('declined at the administrator prompt', 'compiler', 'uac')
        self.assertIn('declined', self.add(declined)[0])
        self.assertIn('exit code 1', self.add(lambda: 1)[0])
        self.assertIn('C++ CMake tools', self.add(lambda: 0)[0])          # ran, but the tools did not appear
        with mock.patch.object(install, 'vs_installer_exe', return_value=None):
            self.assertIn('not found', install.add_vs_parts(self.vs, ['cmake'], lambda s: None))

    def run_check(self, add_result):
        dest = self.tmp / 'game'
        inst = install.Installer(None, dest, log=lambda s: None)
        inst.state = {'done': []}
        calls = []
        def fake_add(vs, missing, log, cancelled):
            calls.append((vs, missing))
            if add_result == '': self.bundle()
            return add_result
        with mock.patch.object(install, 'add_vs_parts', fake_add), \
             mock.patch.object(install, 'vs_instances', return_value=[{'installationPath': str(self.vs)}]), \
             mock.patch.object(inst, 'need_disc', return_value=False), mock.patch.object(inst, 'game_installed', return_value=False), \
             mock.patch.object(inst, 'save'), mock.patch.object(inst, 'remember_install'), mock.patch.object(inst, 'plan_reuse'), \
             mock.patch.object(inst, 'check_manifest'), mock.patch.object(inst, 'ffmpeg_dir'), \
             mock.patch.object(install, 'tool_version', return_value='version 4'):
            try:
                inst.check()
            except install.Stop as e:
                return calls, inst, e
            except Exception:
                pass                                                       # later requirement checks: not this test's business
        return calls, inst, None

    def test_check_adds_then_continues(self):
        calls, inst, stop = self.run_check('')
        self.assertEqual(calls, [(self.vs, ['cmake'])])
        self.assertEqual(Path(inst.cmake), self.vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe')
        self.assertNotIn(str(self.path_tools), str(inst.cmake))

    def test_check_stops_with_the_usual_message_when_adding_fails(self):
        calls, inst, stop = self.run_check('declined at the administrator prompt')
        self.assertEqual(calls, [(self.vs, ['cmake'])])
        self.assertIsNotNone(stop)
        self.assertEqual((stop.kind, stop.missing), ('compiler', ['cmake']))
        self.assertIn('C++ CMake tools for Windows', str(stop))
        self.assertFalse(hasattr(inst, 'cmake') and inst.cmake)


class ByteOrderMark(unittest.TestCase):
    def test_a_log_with_a_bom_is_copied_without_it(self):
        tmp = Path(tempfile.mkdtemp(prefix='ma2-120-bom-'))
        try:
            p = tmp / 'stub-1.log'
            p.write_bytes(b'\xef\xbb\xbf22:35:03 MechAssault 2 Recomp Installer starting\r\n')
            self.assertEqual(Path(p).read_text(encoding='utf-8-sig')[0], '2')
            src = (REPO / 'kit' / 'installer' / 'precheck.py').read_text(encoding='utf-8')
            self.assertIn("read_text(encoding='utf-8-sig', errors='replace')", src)
        finally:
            import shutil; shutil.rmtree(tmp, ignore_errors=True)

    def test_stub_writes_utf8_bytes_without_a_mark(self):
        stub = (REPO / 'installer-exe' / 'stub.cpp').read_text(encoding='utf-8')
        self.assertIn('L"ab", _SH_DENYWR', stub)
        self.assertNotIn('L"a, ccs=UTF-8"', stub)

    def test_triage_tolerates_any_console(self):
        tri = (REPO / 'relay' / 'tools' / 'triage.py').read_text(encoding='utf-8')
        self.assertIn('decode("utf-8-sig", "replace")', tri)
        self.assertIn('reconfigure(errors="replace")', tri)


if __name__ == '__main__':
    unittest.main(verbosity=2)

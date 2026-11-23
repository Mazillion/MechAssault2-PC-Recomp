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

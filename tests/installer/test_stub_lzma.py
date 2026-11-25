"""The stub's own LZMA decoder (installer-exe/lzma_dec.h, packing method 2) gives back every file bit for bit, and refuses
damaged data without crashing. Method 2 is what lets the installer unpack under Wine and Proton (MA2_PACK_CODEC=lzma).

    python tests/installer/test_stub_lzma.py      (Windows with the MSVC x64 tools; skipped otherwise)
"""
import lzma, os, random, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

REPO = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
NO_WINDOW = 0x08000000


def vcvars():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if os.name != 'nt' or not vswhere.is_file(): return None
    r = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], capture_output=True, text=True,
                       creationflags=NO_WINDOW)
    p = Path(r.stdout.strip() or '.') / 'VC/Auxiliary/Build/vcvars64.bat'
    return p if p.is_file() else None


class LzmaRoundTrip(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        vc = vcvars()
        if not vc: raise unittest.SkipTest('no MSVC x64 tools')
        cls.tmp = Path(tempfile.mkdtemp(prefix='ma2-lzma-'))
        script = cls.tmp / 'b.cmd'
        script.write_text('@echo off\r\nset "PATH=%%ProgramFiles(x86)%%\Microsoft Visual Studio\Installer;%%PATH%%"\r\n'
                          'call "%s" >nul || exit /b 1\r\ncl /nologo /O2 /EHsc /std:c++17 /I "%s" "%s" /Fe:"%s" >nul || exit /b 1\r\n'
                          % (vc, REPO / 'installer-exe', HERE / 'lzma_roundtrip.cpp', cls.tmp / 'rt.exe'), encoding='utf-8')
        r = subprocess.run(['cmd.exe', '/d', '/c', str(script)], cwd=cls.tmp, capture_output=True, text=True, creationflags=NO_WINDOW)
        if r.returncode: raise RuntimeError('build failed: %s' % r.stdout[-2000:])

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(getattr(cls, 'tmp', ''), ignore_errors=True)

    def run_list(self, files):
        lines = []
        for i, (name, data) in enumerate(files):
            o = self.tmp / ('o%04d' % i); o.write_bytes(data)
            p = self.tmp / ('p%04d' % i); p.write_bytes(lzma.compress(data, format=lzma.FORMAT_ALONE, preset=9 | lzma.PRESET_EXTREME))
            lines.append('%s\t%s' % (p, o))
        (self.tmp / 'list.txt').write_text('\n'.join(lines), encoding='utf-8')
        r = subprocess.run([str(self.tmp / 'rt.exe'), str(self.tmp / 'list.txt')], capture_output=True, text=True, creationflags=NO_WINDOW)
        return r.returncode, r.stdout

    def test_kit_files_and_edge_cases(self):
        rnd = random.Random(7)
        files = [('empty-ish', b'x' * 64), ('zeros', bytes(1 << 20)), ('random', bytes(rnd.getrandbits(8) for _ in range(200000))),
                 ('repeats', (b'MechAssault 2 ' * 9000) + bytes(range(256)) * 300)]
        for p in sorted((REPO / 'kit' / 'installer').rglob('*.py'))[:80]:
            files.append((p.name, p.read_bytes()))
        code, out = self.run_list(files)
        self.assertEqual(code, 0, out)
        self.assertIn('ok=%d bad=0' % len(files), out)


if __name__ == '__main__':
    unittest.main()

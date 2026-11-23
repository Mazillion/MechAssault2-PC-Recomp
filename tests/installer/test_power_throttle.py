"""1.1.8: the EcoQoS opt-out falls back to speed only on Windows 10 (MA2-2LXVOZ logged "opt-out failed (87)").

Checks main.c uses the fallback helper, and builds + runs tests/game/power_throttle_test.c when the MSVC x64 tools are found
(else that test is skipped).
"""
import subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SRC = REPO / 'kit' / 'source' / 'game' / 'src'
sys.path.insert(0, str(HERE))
from test_update_notice import vcvars, NO_WINDOW                         # noqa: E402


class PowerThrottle(unittest.TestCase):
    def test_main_uses_the_fallback(self):
        main = (SRC / 'main.c').read_text(encoding='utf-8')
        self.assertIn('pc_power_optout(pc_power_set_throttling', main)
        self.assertNotIn('PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION', main)
        (SRC / 'pc_power_throttle.h').read_bytes().decode('ascii')

    def test_c_unit_test(self):
        bat = vcvars()
        if not bat:
            self.skipTest('MSVC x64 tools not found')
        with tempfile.TemporaryDirectory() as out:
            exe = Path(out) / 'power_throttle_test.exe'
            script = Path(out) / 'build.cmd'
            script.write_text('@echo off\r\ncall "%s" >nul || exit /b 9\r\n'
                              'cl /nologo /W3 /WX tests\game\power_throttle_test.c /Fo"%s\\\\" /Fe"%s"\r\n' % (bat, out, exe),
                              encoding='ascii')
            r = subprocess.run(['cmd', '/d', '/c', str(script)], cwd=REPO, capture_output=True, text=True, creationflags=NO_WINDOW,
                               timeout=600)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
            r = subprocess.run([str(exe)], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertRegex(r.stdout, r'power_throttle_test: \d+ checks, 0 failed')


if __name__ == '__main__':
    unittest.main(verbosity=2)

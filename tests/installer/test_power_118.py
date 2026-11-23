"""1.1.8 power: the EcoQoS opt-out falls back to speed only on Windows 10 (MA2-2LXVOZ logged "opt-out failed (87)"), and the
power mode, plan and clocks in the logs ([POWER], [HEALTH] pmode/pplan/mhz) and the report summary.

Checks main.c uses the fallback helper and precheck.power_scheme fails safe, and builds + runs tests/game/power_throttle_test.c
and tests/game/power_state_test.c when the MSVC x64 tools are found (else those tests are skipped).
"""
import subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SRC = REPO / 'kit' / 'source' / 'game' / 'src'
RT = REPO / 'kit' / 'source' / 'runtime' / 'src'
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / 'kit' / 'installer'))
from test_update_notice import vcvars, NO_WINDOW                         # noqa: E402
import precheck                                                           # noqa: E402


class PowerThrottle(unittest.TestCase):
    def test_main_uses_the_fallback(self):
        main = (SRC / 'main.c').read_text(encoding='utf-8')
        self.assertIn('pc_power_optout(pc_power_set_throttling', main)
        self.assertNotIn('PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION', main)
        (SRC / 'pc_power_throttle.h').read_bytes().decode('ascii')

    def test_power_state_is_logged(self):
        self.assertIn(r'"[POWER] %s cpus=%u\n", txt',(RT / 'platform' / 'pc_cpu_topo.c').read_text(encoding='utf-8'))
        health = (RT / 'apu' / 'apu_core.c').read_text(encoding='utf-8')
        self.assertIn('nvs=%ld %s', health)
        self.assertIn('pc_power_state_read(&pw)', health)
        header = (RT / 'platform' / 'pc_power_state.h').read_bytes().decode('ascii')
        self.assertNotIn('PowerReadFriendlyName', header)                 # a plan's own name can carry any text: never read

    def test_power_scheme_fails_safe(self):
        self.assertEqual(precheck.power_scheme(_fail=True),
                         {'mode': 'unknown', 'plan': 'unknown', 'mhz_avg': None, 'mhz_min': None, 'mhz_max': None})
        s = precheck.power_scheme()
        self.assertTrue(s['plan'] in precheck.POWER_PLANS.values() or s['plan'].startswith('custom-') or s['plan'] == 'unknown', s)
        self.assertIn(s['mode'], set(precheck.POWER_MODES.values()) | {'unknown'})

    def c_test(self, name):
        bat = vcvars()
        if not bat:
            self.skipTest('MSVC x64 tools not found')
        with tempfile.TemporaryDirectory() as out:
            exe = Path(out) / (name + '.exe')
            script = Path(out) / 'build.cmd'
            script.write_text('@echo off\r\ncall "%s" >nul || exit /b 9\r\n'
                              'cl /nologo /W3 /WX tests\\game\\%s.c /Fo"%s\\\\" /Fe"%s"\r\n' % (bat, name, out, exe),
                              encoding='ascii')
            r = subprocess.run(['cmd', '/d', '/c', str(script)], cwd=REPO, capture_output=True, text=True, creationflags=NO_WINDOW,
                               timeout=600)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
            r = subprocess.run([str(exe)], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertRegex(r.stdout, name + r': \d+ checks, 0 failed')

    def test_throttle_c_unit_test(self):
        self.c_test('power_throttle_test')

    def test_power_state_c_unit_test(self):
        self.c_test('power_state_test')


if __name__ == '__main__':
    unittest.main(verbosity=2)

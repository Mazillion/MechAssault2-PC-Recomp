"""11-25: no installer test may read or write the real %LOCALAPPDATA%\\MechAssault2 (see isolate.py).

    py -3.13 -B tests/installer/test_isolation.py

- every suite in tests/installer imports isolate before anything that can reach the installer code;
- each suite, imported in a fresh Python whose LOCALAPPDATA is the REAL folder (as when it is run directly), ends with every
  installer path (precheck.local_dir / diagnostics_dir, report.store_path, download's cache, wine_host.toolchain_dir,
  LOCALAPPDATA itself) outside the real %LOCALAPPDATA%\\MechAssault2;
- the audit hook refuses opening or listing a path in the real folder (it refuses before Windows is asked, so nothing
  there is read: the probe names a file that does not exist).
Nothing here runs any suite's tests, and nothing in the real folder is opened, listed or changed."""
import os, re, subprocess, sys, unittest
from pathlib import Path
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
NO_WINDOW = 0x08000000
SUITES = sorted(p for p in HERE.glob('*.py') if p.name.startswith(('test_', 'ui_flow')) and p.name != 'test_fake.py')
# the first import of anything that is not the standard library: installer modules, the rig, other suites, mocks, tools
LOCAL_IMPORT = re.compile(r'^\s*(?:import|from)\s+(?!isolate\b)(rig|precheck|install|report|gui\w*|download|selfupdate|wine_host|'
                          r'toolchain_fetch|ffmpeg_fetch|ui_theme|mock_\w+|privacy_scan|test_\w+|ui_flow\w*)\b')

PROBE = r'''
import os, sys, json, importlib
sys.path[:0] = [%(here)r, %(kit)r]
importlib.import_module(%(mod)r)
import isolate, precheck, report, download, wine_host
paths = {'LOCALAPPDATA': os.environ.get('LOCALAPPDATA', ''), 'local_dir': str(precheck.local_dir()),
         'diagnostics_dir': str(precheck.diagnostics_dir()), 'store_path': str(report.store_path()),
         'download_cache': str(download._local()), 'toolchain_dir': str(wine_host.toolchain_dir())}
print('PATHS ' + json.dumps({k: [v, isolate.inside_real(v) or isolate.inside_real(os.path.join(v, 'MechAssault2'))] for k, v in paths.items()}))
'''


class Isolation(unittest.TestCase):
    def test_every_suite_imports_isolate_first(self):
        self.assertGreater(len(SUITES), 20)
        bad = []
        for p in SUITES + [HERE / 'rig.py']:
            lines = p.read_text(encoding='utf-8').splitlines()
            iso = next((i for i, l in enumerate(lines) if re.match(r'^import isolate\b', l)), None)
            first = next((i for i, l in enumerate(lines) if LOCAL_IMPORT.match(l)), None)
            if iso is None or (first is not None and first < iso):
                bad.append('%s (isolate at %s, first local import at %s)' % (p.name, iso, first))
        self.assertEqual(bad, [], 'import isolate before any installer, rig or suite import')

    def test_no_suite_resolves_into_the_real_folder(self):
        if not isolate.REAL_LOCAL:
            self.skipTest('no real Local AppData folder on this PC')
        env = {k: v for k, v in os.environ.items() if not k.startswith('MA2_') or k.startswith('MA2_TEST_')}
        env.setdefault('MA2_TEST_XISO', str(HERE / 'no-disc.xiso'))          # ui_flow computes its disc paths at import
        env.setdefault('MA2_TEST_GAME_FILES', str(HERE / 'no-game-files'))
        env['LOCALAPPDATA'] = isolate.REAL_LOCAL                    # as in a direct run: the real folder
        bad = []
        for p in SUITES:
            code = PROBE % {'here': str(HERE), 'kit': str(REPO / 'kit' / 'installer'), 'mod': p.stem}
            r = subprocess.run([sys.executable, '-B', '-c', code], cwd=str(REPO), env=env, capture_output=True, text=True,
                               encoding='utf-8', errors='replace', creationflags=NO_WINDOW, timeout=300)
            line = next((l for l in r.stdout.splitlines() if l.startswith('PATHS ')), None)
            if line is None:
                bad.append('%s: import failed (%s)' % (p.name, (r.stderr.strip().splitlines() or ['?'])[-1][:200]))
                continue
            got = __import__('json').loads(line[6:])
            hits = ['%s=%s' % (k, v[0]) for k, v in got.items() if v[1]]
            if hits: bad.append('%s: %s' % (p.name, ', '.join(hits)))
            if 'isolate: ' in r.stderr: bad.append('%s: %s' % (p.name, r.stderr.strip()[:300]))
        self.assertEqual(bad, [])

    def test_the_hook_refuses_the_real_folder(self):
        if not isolate.REAL:
            self.skipTest('no real Local AppData folder on this PC')
        real = os.path.join(isolate.REAL_LOCAL, 'MechAssault2')
        probe = os.path.join(real, 'isolate-probe-%d-does-not-exist.txt' % os.getpid())
        n = len(isolate.BLOCKED)
        try:
            with self.assertRaises(PermissionError):
                open(probe, 'rb')
            with self.assertRaises(PermissionError):
                open(probe, 'w')
            with self.assertRaises(PermissionError):
                os.listdir(real)
            with self.assertRaises(PermissionError):
                os.mkdir(os.path.join(real, 'Logs', 'isolate-probe'))
            self.assertEqual(len(isolate.BLOCKED), n + 4)
            self.assertTrue(isolate.inside_real(real.upper()))
            self.assertFalse(isolate.inside_real(os.path.join(isolate.REAL_LOCAL, 'MechAssault2-other')))
            self.assertFalse(isolate.inside_real(os.environ['LOCALAPPDATA']))
        finally:
            del isolate.BLOCKED[n:]                                # expected refusals: not reported at exit


if __name__ == '__main__':
    unittest.main(verbosity=2)

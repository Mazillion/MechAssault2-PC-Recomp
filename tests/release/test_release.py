"""Unit tests for tools/release.py (no build, no install, no network).

    py -3.13 -B tests/release/test_release.py
"""
import json, shutil, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import release  # noqa: E402

PLACES = ['VERSION', 'kit/installer/data/release.json', 'kit/source/game/src/ma2_release.h', 'editor/src/app/version.h',
          'editor/res/editor.rc', 'kit/defaults/Docs/Changelog.txt', '.github/ISSUE_TEMPLATE/bug_report.yml']


class Strings(unittest.TestCase):
    def test_texty(self):
        for s in ('Report a problem', 'Back', 'SEND REPORT', 'What happened?', 'Copy number'):
            self.assertTrue(release.texty(s), s)
        for s in ('utf-8', 'normal', 'kit/installer', r'^\d+$', 'Content-Type', '--crash "%s"', '%s', '-alpha', '42'):
            self.assertFalse(release.texty(s), s)
        self.assertFalse(release.texty('BUTTON', 'c'))
        self.assertTrue(release.texty('SEND', 'py'))

    def test_c_literals(self):
        src = ('#include "x.h"\n// "not this"\n/* "nor this" */\nstatic const wchar_t *a = L"Send "\n    L"report";\n'
               'fprintf(stderr, "[REPORT] sent %s\\n", id);\nchar c = \'"\'; const char *b = u8"Close the game";\n')
        got = release.c_literals(src)
        texts = [(s, k) for _, s, k in got]
        self.assertIn(('Send report', 'text'), texts)
        self.assertIn(('[REPORT] sent %s\\n', 'log'), texts)
        self.assertIn(('Close the game', 'text'), texts)
        self.assertFalse(any('not this' in s or 'nor this' in s or s == 'x.h' for s, _ in texts))
        self.assertEqual([ln for ln, s, _ in got if s == 'Send report'], [4])

    def test_py_literals(self):
        src = ('"""Module doc."""\ndef f():\n    """Function doc."""\n    self.log("ui: thing failed")\n'
               '    return "Open folder", f"Updated to {v}. Kept."\n')
        got = {s: k for _, s, k in release.py_literals(src)}
        self.assertNotIn('Module doc.', got); self.assertNotIn('Function doc.', got)
        self.assertEqual(got['ui: thing failed'], 'log')
        self.assertEqual(got['Open folder'], 'text')
        self.assertIn('Updated to {...}. Kept.', got)

    def test_numbers(self):
        n = [x for _, x in release.numbers_in([('a', 'kept for 60 days, up to 20 MB, 3 minutes; 12%')])]
        self.assertEqual(n, ['60 days', '20 MB', '3 minutes', '12%'])

    def test_switches(self):
        s = release.switches('pc_switch("RECOMP_AUDIO_SELFGUARD","Audio","SelfGuard",1); ma2_pc_ini_get_int("Bots", "Red", 3);'
                             ' getenv("RECOMP_X"); ap.add_argument(\'--clean-build\')')
        self.assertEqual(s['[Audio] SelfGuard (env RECOMP_AUDIO_SELFGUARD)'], '1')
        self.assertEqual(s['[Bots] Red'], '3')
        self.assertIn('env RECOMP_X', s); self.assertIn('installer option --clean-build', s)


class Versions(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='release-test-'))
        for p in PLACES:
            (self.tmp / p).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / p, self.tmp / p)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def read(self):
        return release.read_versions(lambda p: (self.tmp / p).read_text(encoding='utf-8') if (self.tmp / p).is_file() else None)

    def test_repo_agrees(self):
        self.assertEqual(release.check_versions(self.read()), [])

    def test_preview_stamp_strings_only(self):
        v = self.read()['VERSION']
        raw = (self.tmp / 'editor/res/editor.rc').read_bytes()
        release.stamp_versions(self.tmp, v + '-preview.3', strings_only=True)
        got = self.read()
        self.assertEqual(got['release.json version'], v + '-preview.3')
        self.assertEqual(got['game numbers'], v)
        self.assertEqual(release.check_versions(got), [])
        self.assertEqual((self.tmp / 'editor/res/editor.rc').read_bytes().count(b'\r\n'), raw.count(b'\r\n'))   # line endings kept

    def test_set_and_mismatch(self):
        release.stamp_versions(self.tmp, '9.8.7', '2030.01.02')
        got = self.read()
        self.assertEqual(got['game numbers'], '9.8.7'); self.assertEqual(got['bug_report placeholder'], '9.8.7')
        self.assertEqual(got['release.json date'], '2030.01.02')
        probs = release.check_versions(got)
        self.assertEqual(len(probs), 1); self.assertIn('Changelog', probs[0])         # the entry is written by hand

    def test_bad_version(self):
        with self.assertRaises(SystemExit):
            release.stamp_versions(self.tmp, '1.2')


class Sheet(unittest.TestCase):
    def test_signed_part_ignores_line_numbers_and_appendix(self):
        a = 'x `gui.py:10` "Back"\n' + release.SIGNED_END + '\ncommit aaa\n'
        b = 'x `gui.py:99` "Back"\n' + release.SIGNED_END + '\ncommit bbb\n'
        c = 'x `gui.py:10` "Back!"\n' + release.SIGNED_END + '\n'
        self.assertEqual(release.sheet_hash(a), release.sheet_hash(b))
        self.assertNotEqual(release.sheet_hash(a), release.sheet_hash(c))

    def test_ini_map(self):
        self.assertEqual(release.ini_map('[Audio]\n; c\nSelfGuard = 1\n'), {'[Audio] SelfGuard': '1'})


class Gate(unittest.TestCase):
    def test_plan(self):
        task = release.plan_steps('task')
        self.assertNotIn('ab_fresh', task); self.assertIn('suites', task)
        rc = release.plan_steps('rc')
        for s in ('signoff', 'ab_fresh', 'update', 'fastpath', 'fallback', 'fault'):
            self.assertIn(s, rc)
        only = release.plan_steps('task', only=['relay_installed'])
        self.assertEqual(only, ['clone', 'deps', 'build', 'ab_fresh', 'relay_installed'])
        self.assertNotIn('ui_flow', release.plan_steps('task', skip=['ui_flow']))
        self.assertIn('fault', release.plan_steps('task', extra=['fault']))

    def test_closed_port(self):
        import socket
        port = release.closed_port()
        with self.assertRaises(OSError):
            socket.create_connection(('127.0.0.1', port), timeout=1)

    def test_manifest_and_copy_skip_links(self):
        tmp = Path(tempfile.mkdtemp(prefix='release-test-'))
        try:
            (tmp / 'a' / '_work').mkdir(parents=True); (tmp / 'a' / 'x.txt').write_text('1')
            (tmp / 'a' / '_work' / 'big.bin').write_text('2')
            m = release.manifest(tmp / 'a')
            self.assertEqual(list(m), ['x.txt'])
            self.assertEqual(release.copy_tree(tmp / 'a', tmp / 'b'), [])
            self.assertTrue((tmp / 'b' / '_work' / 'big.bin').is_file())
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_fault_list(self):
        data = json.loads((REPO / 'tests' / 'installer' / 'gate_faults.json').read_text(encoding='utf-8'))
        ids = [f['id'] for f in data['faults']]
        self.assertEqual(len(ids), len(set(ids)))
        for f in data['faults']:
            self.assertIn(f['module'], ('__main__', 'precheck', 'download', 'selfupdate'))
            self.assertIn(f['when'], ('install', 'diagnostics', 'both', 'never'))
            self.assertIn(f['exc'], ('OSError', 'RuntimeError'))

    def test_sitecustomize_compiles(self):
        compile(release.SITECUSTOMIZE % {'hit': 'h.txt', 'faults': []}, 'sitecustomize.py', 'exec')


if __name__ == '__main__':
    unittest.main()

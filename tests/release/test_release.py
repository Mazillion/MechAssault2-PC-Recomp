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


class InstallerName(unittest.TestCase):
    """1.2.0 (replaced in place): the installer is "MechAssault 2 Expanded Installer.exe", published on GitHub with dots for
    spaces; the old name's copy in an install folder is replaced by an update."""
    def test_names_agree_with_the_installer(self):
        sys.path.insert(0, str(REPO / 'kit' / 'installer'))
        try:
            import selfupdate
        finally:
            sys.path.pop(0)
        self.assertEqual(release.INSTALLER_NAME, 'MechAssault 2 Expanded Installer.exe')
        self.assertEqual(release.INSTALLER_NAME, selfupdate.ASSET)
        self.assertEqual(tuple(release.OLD_INSTALLER_NAMES), tuple(selfupdate.OLD_ASSETS))
        self.assertEqual(release.published_name(release.INSTALLER_NAME), 'MechAssault.2.Expanded.Installer.exe')
        self.assertEqual(release.published_name(release.INSTALLER_NAME), selfupdate.github_asset_name(selfupdate.ASSET))
        for n in (release.INSTALLER_NAME,) + tuple(release.OLD_INSTALLER_NAMES):
            self.assertTrue(release.match_any(n, release.IDENTITY), n)
        self.assertEqual(release.FAULT_MAY_LACK, [release.INSTALLER_NAME])

    def test_build_makes_this_name(self):
        import build_release
        if build_release.INSTALLER_EXE in release.OLD_INSTALLER_NAMES:
            self.skipTest('tools/build_release.py still builds "%s" (its rename is a separate change)' % build_release.INSTALLER_EXE)
        self.assertEqual(build_release.INSTALLER_EXE, release.INSTALLER_NAME)

    def gate(self):
        import types
        return types.SimpleNamespace(cfg={'identity_files': [], 'ab_allowed_removed': []})

    def test_ab_previous_copy_under_the_old_name(self):
        old, new = release.OLD_INSTALLER_NAMES[0], release.INSTALLER_NAME
        prev = {'MechAssault 2.exe': 'a', old: 'x'}
        only_p, only_c, diff, det, bad = release.ab_verdict(self.gate(), prev, {'MechAssault 2.exe': 'a', new: 'y'}, None, None)
        self.assertEqual((only_p, only_c, bad), ([old], [new], []))
        self.assertIn('renamed', ' '.join(det))
        # the new copy missing: the old one going away is a real removal
        only_p, only_c, diff, det, bad = release.ab_verdict(self.gate(), prev, {'MechAssault 2.exe': 'a'}, None, None)
        self.assertEqual(bad, [old])
        self.assertEqual(release.ab_verdict(self.gate(), {'x.txt': 'a'}, {}, None, None)[4], ['x.txt'])

    def test_suites_must_prove_the_dotted_download(self):
        tmp = Path(tempfile.mkdtemp(prefix='release-test-'))
        try:
            log = tmp / 'suite.txt'
            t = [tmp / 'test_a.py', tmp / (release.SELFUPDATE_SUITE + '.py')]
            self.assertIn('missing', release.selfupdate_dotted_problem(t[:1], log))
            self.assertIn('did not pass', release.selfupdate_dotted_problem(t, log))           # no log
            name = release.SELFUPDATE_DOTTED_TEST
            for text, ok in (('%s (__main__.AssetName.%s) ... ok\n' % (name, name), True),
                             ('%s (__main__.AssetName.%s) ... <frozen os>:1: ResourceWarning: x\nResourceWarning: y\nok\n' % (name, name), True),
                             ('%s (__main__.AssetName.%s) ... FAIL\ntest_b (__main__.X.test_b) ... ok\n' % (name, name), False),
                             ('%s (__main__.AssetName.%s) ... skipped \'x\'\n' % (name, name), False),
                             ('test_b (__main__.X.test_b) ... ok\n', False)):
                log.write_text('$ py\n' + text, encoding='utf-8')
                self.assertEqual(release.selfupdate_dotted_problem(t, log) == '', ok, text)
            self.assertIn('MechAssault.2.Expanded.Installer.exe', release.selfupdate_dotted_problem(t[:1], log))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_suite_has_the_dotted_test(self):
        src = (REPO / 'tests' / 'installer' / (release.SELFUPDATE_SUITE + '.py')).read_text(encoding='utf-8')
        self.assertIn('def %s(self)' % release.SELFUPDATE_DOTTED_TEST, src)
        self.assertIn('unittest.main(verbosity=2)', src)        # the gate reads the per-test lines


class LauncherZip(unittest.TestCase):
    """The RecompLauncher zip (build_release step 6): its name, members, checks, privacy scan, and the gate step."""
    def setUp(self):
        import build_release
        self.br = build_release
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2-zip-'))
        self.exe = self.tmp / self.br.INSTALLER_EXE; self.exe.write_bytes(b'MZ' + b'installer bytes' * 1000)
        self.prof = REPO / 'recomplauncher.json'
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_name_and_members(self):
        self.assertEqual(self.br.launcher_zip_name('1.1.8'), 'MechAssault2-PC-Recomp-v1.1.8-Windows-x64.zip')
        z = self.br.make_launcher_zip(self.exe, self.prof, self.tmp / 'a.zip', '2026.11.30')
        import zipfile
        with zipfile.ZipFile(z) as zf:
            self.assertEqual(sorted(zf.namelist()), sorted([self.br.INSTALLER_EXE, 'recomplauncher.json']))
            self.assertEqual(zf.read(self.br.INSTALLER_EXE), self.exe.read_bytes())
            self.assertEqual({i.date_time for i in zf.infolist()}, {(2026, 11, 30, 0, 0, 0)})
        again = self.br.make_launcher_zip(self.exe, self.prof, self.tmp / 'b.zip', '2026.11.30')
        self.assertEqual(z.read_bytes(), again.read_bytes())                        # same inputs, same zip
        self.assertEqual(self.br.check_launcher_zip(z, self.exe), [])

    def test_check_refuses_extras_settings_and_another_installer(self):
        import zipfile
        bad = self.tmp / 'bad.zip'
        with zipfile.ZipFile(bad, 'w') as zf:
            zf.write(self.prof, 'recomplauncher.json'); zf.writestr(self.br.INSTALLER_EXE, b'other'); zf.writestr('pc_settings.ini', b'[Display]')
        probs = ' | '.join(self.br.check_launcher_zip(bad, self.exe))
        self.assertIn('members', probs); self.assertIn('pc_settings.ini', probs); self.assertIn('differs', probs)
        self.assertTrue(self.br.check_launcher_zip(self.tmp / 'missing.zip', self.exe))

    def test_check_wants_a_start_program_the_zip_has(self):
        import json, zipfile
        prof = json.loads(self.prof.read_text(encoding='utf-8'))
        prof['start']['program'] = ['MechAssault 2/MechAssault 2.exe']             # preview 1: red in the Developer tab
        bad = self.tmp / 'nostart.zip'
        with zipfile.ZipFile(bad, 'w') as zf:
            zf.writestr('recomplauncher.json', json.dumps(prof)); zf.write(self.exe, self.br.INSTALLER_EXE)
        self.assertIn('start.program must also list', ' | '.join(self.br.check_launcher_zip(bad, self.exe)))

    def test_broken_profile_stops_the_build(self):
        p = self.tmp / 'recomplauncher.json'; p.write_text('{"format": 1,', encoding='utf-8')
        with self.assertRaises(ValueError):
            self.br.make_launcher_zip(self.exe, p, self.tmp / 'c.zip', '2026.11.30')
        self.assertFalse((self.tmp / 'c.zip').exists())

    def test_privacy_scan_opens_the_zip(self):
        import privacy_scan, zipfile
        z = self.tmp / 'p.zip'
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
            zf.writestr('recomplauncher.json', 'built in C:' + chr(92) + 'Users' + chr(92) + 'somebody' + chr(92) + 'Desktop')
        hits, n = privacy_scan.scan_zip(z)
        self.assertEqual(n, 1); self.assertTrue(hits and hits[0][0].endswith('!recomplauncher.json'), hits)
        self.assertEqual(privacy_scan.scan_zip(self.exe), ([], 0))

    def test_gate_step(self):
        self.assertIn('launcher_zip', release.plan_steps('task')); self.assertIn('launcher_zip', release.plan_steps('rc'))
        self.assertEqual(release.plan_steps('task', only=['launcher_zip']), ['clone', 'deps', 'build', 'launcher_zip'])


if __name__ == '__main__':
    unittest.main()

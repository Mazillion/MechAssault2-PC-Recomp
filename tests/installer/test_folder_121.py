"""1.2.1 (report TN6DL3): the install folder became non-empty between Setup's check and pressing Install, and the player got the
generic "Something went wrong". Now:
  - the window checks the folder again when Install is pressed: a problem goes back to Setup with the existing folder notice and
    its fix link, and nothing starts;
  - the engine's own check (files arrive after that) stops with kind 'folder', which the window routes to the same Setup notice;
  - RecompLauncher's hidden step (--recomplauncher --dest) stops cleanly on it: its folder words, exit code 1, no traceback;
  - the folder check's write test uses a file name of its own on every call (concurrent checks never share one) and never
    leaves it behind; a write-test file is never counted as the player's files;
  - a sent installer report no longer says "a dump was asked for" (nobody asked); Collect diagnostics still does.
The window is transparent, off screen and never activated (rig.py). Nothing here touches the real %LOCALAPPDATA%, saves or
C:\\Games: every path is in tests/tmp. Run with Python 3.13:  python -B test_folder_121.py
"""
import contextlib, io, os, shutil, sys, tempfile, threading, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import isolate                                                               # noqa: E402  (never the real %LOCALAPPDATA%\MechAssault2; first)
import rig                                                                   # noqa: E402  (transparent, off-screen window)
from rig import pump, until                                                  # noqa: E402
import precheck, install, selfupdate                                         # noqa: E402
gui = rig.gui

REC = WORK / 'installer' / 'data' / 'recorded-install.txt'
NOTEMPTY = 'This folder already has other files. Choose an empty folder or a new name.'     # check_folder's existing words


def screen_text(app):
    cv = app.cv
    return ' | '.join(cv.itemcget(i, 'text') for i in cv.find_withtag('s') if cv.type(i) == 'text')


def probes(folder):
    return [p.name for p in Path(folder).iterdir() if p.name.startswith('.ma2probe')] if Path(folder).is_dir() else []


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2fold-121-', dir=str(WORK / 'tmp')))
        self.dest = self.tmp / 'Games' / precheck.FOLDER_NAME
        mock.patch.dict(os.environ, {'LOCALAPPDATA': str(self.tmp / 'appdata'), 'MA2_SHORTCUT_DIR': str(self.tmp / 'lnk')}).start()
        mock.patch.object(selfupdate, '_switched_off', False).start()
        # the engine's start-up tidy-up looks at real drive roots for old build folders: never in a test
        mock.patch.object(install, 'clean_stale_build_folders', lambda bases: []).start()

    def tearDown(self):
        mock.patch.stopall()
        shutil.rmtree(self.tmp, ignore_errors=True)


class Window(Base):
    """Setup -> a file arrives -> INSTALL: back to Setup with the folder notice, nothing started."""
    def make(self):
        root, app = rig.make(scale=1.0); pump(root, 0.4)
        self.dest.mkdir(parents=True)
        app.set_dest(str(self.dest)); self.assertTrue(until(root, lambda: app.folder is not None, 20))
        app.disc = str(self.tmp / 'disc.xiso'); app.image = precheck.ImageResult(True, 'ok', '', '', {})
        app.facts['compiler'] = {'state': 'ready'}; app.facts['sac'] = None
        app.render(); pump(root, 0.2)
        def gone():
            try:
                if app.worker and app.inst: app.inst.cancel(); until(root, lambda: app.worker is None, 30)
                app.close()
            except Exception: pass
        self.addCleanup(gone)
        return root, app

    def test_folder_fills_before_install_is_pressed(self):
        with mock.patch.dict(os.environ, {'MA2_INSTALLER_FAKE': str(REC), 'MA2_FAKE_SPEED': '400'}):
            root, app = self.make()
            self.assertEqual((app.screen, app.mode, app.folder.existing), ('setup', 'install', 'empty'))
            self.assertTrue(app.folder.ok, app.folder.codes())
            self.assertEqual((app.primary.text, app.primary.enabled), ('INSTALL', True))
            starts = []; real_start = app.start; app.start = lambda: starts.append(1)
            (self.dest / 'something.txt').write_text('x')                      # TN6DL3: files arrive while the window waits
            app.primary.invoke(); pump(root, 0.3)
            self.assertEqual(starts, [], 'no install starts')
            self.assertIsNone(app.worker); self.assertIsNone(app.inst)
            self.assertEqual(app.screen, 'setup')
            self.assertEqual(app.folder.first_error().code, 'notempty')
            t = screen_text(app)
            self.assertIn(NOTEMPTY, t)                                          # the existing strip ...
            self.assertIn('fix', app.w)                                         # ... and its fix link
            self.assertEqual(app.w['fix'].text, 'Use %s' % precheck.short(str(self.dest / precheck.FOLDER_NAME), 30))
            self.assertEqual(app.primary.text, 'CHOOSE FOLDER')
            self.assertNotIn('Something went wrong', t)
            self.assertEqual(sorted(p.name for p in self.dest.iterdir()), ['something.txt'], 'nothing written into the folder')
            # the fix link picks the suggested new folder: INSTALL again, and now it starts
            app.w['fix'].invoke(); self.assertTrue(until(root, lambda: app.folder is not None and app.folder.ok, 20), app.folder and app.folder.codes())
            app.render(); pump(root, 0.2)
            self.assertEqual((app.primary.text, app.primary.enabled), ('INSTALL', True))
            app.start = real_start
            app.primary.invoke(); self.assertTrue(until(root, lambda: app.screen == 'progress', 10), app.screen)

    def test_engine_folder_stop_goes_back_to_setup(self):
        """The window's check passed, then files arrived before the engine's own check (the real engine, TN6DL3 replayed): its
        'folder' stop shows Setup's folder notice, not "Something went wrong"."""
        root, app = self.make()
        def arrive(bases):
            (self.dest / 'arrived late.txt').write_text('x'); return []
        os.environ.pop('MA2_INSTALLER_FAKE', None)
        with mock.patch.object(install, 'clean_stale_build_folders', arrive):     # runs just before the engine's folder check
            app.primary.invoke()
            self.assertTrue(until(root, lambda: app.worker is None and app.failed is not None, 30), (app.screen, app.failed))
            pump(root, 0.3)
        self.assertEqual(app.failed[0], 'folder')
        self.assertEqual(app.inst.stage, '')                                   # stopped before step 1
        self.assertEqual(app.screen, 'setup')
        t = screen_text(app)
        self.assertIn(NOTEMPTY, t); self.assertIn('fix', app.w); self.assertNotIn('Something went wrong', t)
        self.assertEqual(app.primary.text, 'CHOOSE FOLDER')
        self.assertFalse((self.dest / '_work').exists(), 'the engine made nothing in the folder')

    def test_other_problems_also_go_back(self):
        """Any folder problem the check reports at Install (here: the folder became a file) goes back to Setup the same way."""
        root, app = self.make()
        starts = []; app.start = lambda: starts.append(1)
        self.dest.rmdir(); self.dest.write_text('x')
        app.primary.invoke(); pump(root, 0.3)
        self.assertEqual((starts, app.screen, app.folder.first_error().code), ([], 'setup', 'notdir'))
        self.assertIn('That is a file, not a folder.', screen_text(app))

    def test_a_slow_check_does_not_hold_the_window(self):
        """A folder check that does not answer in time: the install goes on (the engine checks too)."""
        root, app = self.make()
        gate = threading.Event(); self.addCleanup(gate.set)
        real = precheck.check_folder
        starts = []; app.start = lambda: starts.append(1)
        with mock.patch.object(gui.App, 'FOLDER_RECHECK_WAIT', 0.2), \
             mock.patch.object(precheck, 'check_folder', lambda *a, **k: (gate.wait(5), real(*a, **k))[1]):
            app.primary.invoke(); pump(root, 0.1)
        self.assertEqual(starts, [1])


class Engine(Base):
    """install.py: a folder found non-empty or unusable during the install is a 'folder' stop."""
    def run_engine(self, dest):
        inst = install.Installer(None, dest, log=lambda s: None)
        with self.assertRaises(install.Stop) as cm: inst.run()
        return cm.exception

    def test_not_empty_is_a_folder_stop(self):
        self.dest.mkdir(parents=True); (self.dest / 'a.txt').write_text('x')
        e = self.run_engine(self.dest)
        self.assertEqual((e.kind, e.code), ('folder', 'notempty'))
        self.assertIn('is not empty', str(e))
        self.assertFalse((self.dest / '_work').exists())

    def test_a_file_is_a_folder_stop(self):
        self.dest.parent.mkdir(parents=True); self.dest.write_text('x')
        e = self.run_engine(self.dest)
        self.assertEqual((e.kind, e.code), ('folder', 'notdir'))
        self.assertEqual(str(e), 'That is a file, not a folder.')

    def test_a_write_test_file_is_not_the_players(self):
        """A write test of the window's folder check (another thread) in the folder at that moment never stops the install."""
        self.dest.mkdir(parents=True); (self.dest / '.ma2probe1234-5-6').write_text('x')
        class Past(Exception): pass
        def past(dest): raise Past()                                           # called right after the folder check
        r = precheck.check_folder(str(self.dest))
        self.assertEqual(r.existing, 'empty'); self.assertNotIn('notempty', r.codes())
        inst = install.Installer(None, self.dest, log=lambda s: None)
        with mock.patch.object(install, 'install_relation', past):
            with self.assertRaises(Past): inst.run()
        self.assertTrue((self.dest / '.ma2probe1234-5-6').is_file())          # (not ours to delete: another check's)

    def test_stop_kind_is_documented(self):
        self.assertIn('folder', install.Stop.__doc__)


class Launcher(Base):
    """--recomplauncher --dest: a folder that filled stops cleanly with the folder words and exit code 1."""
    def main(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, 'argv', ['install.py'] + list(argv)), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err), \
             mock.patch.object(selfupdate, 'cleanup_old', lambda log=None: 0):        # (never the real %TEMP%'s update folders)
            with self.assertRaises(SystemExit) as cm: install.main()
        return cm.exception.code, out.getvalue(), err.getvalue()

    def check_clean(self, code, out, err):
        self.assertEqual(code, 1, (out, err))
        self.assertNotIn('Traceback', out + err)
        self.assertNotIn('unexpected', (out + err).lower())
        self.assertIn(install.LAUNCHER_STOPPED, err)
        self.assertTrue(out.strip().splitlines()[-1].startswith('progress '), out)

    def test_not_empty(self):
        self.dest.mkdir(parents=True); (self.dest / 'a.txt').write_text('x')
        code, out, err = self.main('--recomplauncher', '--dest', str(self.dest), '--xiso', str(self.tmp / 'disc.xiso'))
        self.check_clean(code, out, err)
        self.assertIn('INSTALL STOPPED: The install folder %s is not empty.' % self.dest.resolve(), err)
        self.assertEqual([p.name for p in self.dest.iterdir()], ['a.txt'])

    def test_a_file(self):
        self.dest.parent.mkdir(parents=True); self.dest.write_text('x')
        code, out, err = self.main('--recomplauncher', '--dest', str(self.dest), '--xiso', str(self.tmp / 'disc.xiso'))
        self.check_clean(code, out, err)
        self.assertIn('INSTALL STOPPED: That is a file, not a folder.', err)

    def test_plain_command_line(self):
        self.dest.mkdir(parents=True); (self.dest / 'a.txt').write_text('x')
        code, out, err = self.main('--dest', str(self.dest), '--xiso', str(self.tmp / 'disc.xiso'))
        self.assertEqual(code, 1, (out, err)); self.assertNotIn('Traceback', out + err)
        self.assertIn('INSTALL STOPPED: The install folder', err)


class Probe(Base):
    """The folder check's write test: a name of its own per call, never left behind."""
    def test_concurrent_checks_use_unique_names_and_leave_nothing(self):
        self.dest.mkdir(parents=True)
        names, lock = [], threading.Lock()
        real_open = os.open
        def spy(path, *a, **k):
            if '.ma2probe' in str(path):
                with lock: names.append(Path(path).name)
            return real_open(path, *a, **k)
        results, errors = [], []
        start = threading.Barrier(16)
        def worker():
            try:
                start.wait()
                for _ in range(15):
                    r = precheck.check_folder(str(self.dest), 1)
                    with lock: results.append((r.existing, r.codes()))
                    self.assertTrue(precheck.can_write(self.dest))
            except Exception as e:
                errors.append(e)
        with mock.patch.object(precheck.os, 'open', spy):
            ts = [threading.Thread(target=worker) for _ in range(16)]
            for t in ts: t.start()
            for t in ts: t.join(60)
        self.assertEqual(errors, [])
        self.assertEqual(len(results), 16 * 15)
        self.assertTrue(all(ex == 'empty' and 'readonly' not in c and 'notempty' not in c for ex, c in results), results[:5])
        self.assertEqual(len(names), 2 * 16 * 15)
        self.assertEqual(len(set(names)), len(names), 'every write test has a file name of its own')
        self.assertEqual(probes(self.dest), [], 'no write-test file left behind')
        self.assertEqual(list(self.dest.iterdir()), [])

    def test_a_held_probe_is_still_removed(self):
        """The first delete fails (as when an antivirus scan holds the new file): it is tried again, nothing is left."""
        self.dest.mkdir(parents=True)
        real = os.unlink; calls = []
        def flaky(p, *a, **k):
            calls.append(p)
            if len(calls) == 1: raise PermissionError(32, 'in use')
            return real(p, *a, **k)
        with mock.patch.object(precheck.os, 'unlink', flaky):
            self.assertTrue(precheck.can_write(self.dest))
        self.assertEqual(len(calls), 2); self.assertEqual(probes(self.dest), [])

    def test_unwritable_folder_says_so(self):
        with mock.patch.object(precheck.os, 'open', side_effect=PermissionError(13, 'Access denied')):
            r = precheck.check_folder(str(self.dest))
        self.assertEqual(r.first_error().code, 'readonly'); self.assertEqual(r.first_error().text, precheck.READONLY_TEXT)

    def test_default_folder_probe_leaves_nothing(self):
        drive = self.tmp / 'C'; (drive / 'Games').mkdir(parents=True)
        with mock.patch.dict(os.environ, {'SystemDrive': str(drive)}), mock.patch.object(precheck, 'old_default_install', lambda: None):
            self.assertEqual(precheck.default_install_folder(), str(drive / 'Games' / precheck.FOLDER_NAME))
        self.assertEqual(probes(drive / 'Games'), [])


class DumpNote(Base):
    """'a dump was asked for but none exists': not in a sent installer report (it always looks; nobody asked), still in Collect
    diagnostics when the player ticked the dump."""
    def test_note(self):
        (self.tmp / 'appdata' / 'MechAssault2' / 'Logs').mkdir(parents=True)
        rep = precheck.collect_diagnostics(self.tmp / 'none', self.tmp / 'R', '1.2.1', 'k', [], include_dump=True, name_prefix='MA2-report')
        self.assertFalse(any('dump was asked for' in n for n in rep['notes']), rep['notes'])
        diag = precheck.collect_diagnostics(self.tmp / 'none', self.tmp / 'D', '1.2.1', 'k', [], include_dump=True)
        self.assertIn('a dump was asked for but none exists', diag['notes'])
        plain = precheck.collect_diagnostics(self.tmp / 'none', self.tmp / 'P', '1.2.1', 'k', [], include_dump=False)
        self.assertFalse(any('dump was asked for' in n for n in plain['notes']))


if __name__ == '__main__':
    unittest.main(verbosity=2)

"""1.2.0 installer: RecompLauncher's "show the installer window" tick (--recomplauncher --window) and the start fallback.
The window locked to the launcher's folder (no folder question, no shortcuts, no uninstall, no Play, no self-update), pre-ticked
from the launcher's ticks; the progress lines while it is open (at least once a minute, never going back); the exit code when it
closes (0 built or already built, 1 closed before finishing or stopped, 2 optional content failed); a fallback to the hidden step
when the window cannot start; real hidden-console runs closed by the test hook (MA2_LAUNCHER_UI_TEST, never the desktop's mouse
or keyboard); and the manifest (the tick, start.program's fallback, MA2_RECOMPLAUNCHER_DEST).
The window is transparent, off screen and never activated (rig.py). Run with Python 3.13:  python -B test_v120_launcher_window.py
  [--shots <folder>]  also saves pictures of the window in launcher mode (PNG).
Needs MA2_TEST_XISO (your own disc image) for the disc-ready pictures; without it those checks are skipped.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
REPO = WORK.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import rig                                                                   # noqa: E402  (transparent, off-screen window)
from rig import pump, until, snap                                            # noqa: E402
import precheck, install, selfupdate                                         # noqa: E402
gui = rig.gui

LINE = re.compile(r'^progress (0\.\d\d|1) \S.*$')
NW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
XISO = os.environ.get('MA2_TEST_XISO') or ''
REC = WORK / 'installer' / 'data' / 'recorded-install.txt'
SHOTS = None


def fake_install(dest, kit_id=None, version=None):
    for rel in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
        (dest / rel).parent.mkdir(parents=True, exist_ok=True); (dest / rel).write_bytes(b'x')
    (dest / precheck.STAMP).write_text(json.dumps({'kit_id': kit_id or install.KIT_ID, 'version': version or install.VERSION}), encoding='utf-8')


class Clock:
    def __init__(self): self.t = 0.0
    def __call__(self): return self.t


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-120w-', dir=str(WORK / 'tmp')))
        self.dest = self.tmp / 'library' / 'MechAssault 2'
        mock.patch.dict(os.environ, {'LOCALAPPDATA': str(self.tmp / 'appdata'), 'MA2_SHORTCUT_DIR': str(self.tmp / 'lnk')}).start()
        mock.patch.object(selfupdate, '_switched_off', False).start()
    def tearDown(self):
        mock.patch.stopall()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def args(self, *extra):
        argv = ['install.py', '--recomplauncher', '--dest', str(self.dest)] + list(extra)
        import argparse
        real = argparse.ArgumentParser.parse_args
        got = {}
        def grab(p, *a, **k):
            got['a'] = real(p, *a, **k); raise SystemExit('parsed')
        with mock.patch.object(sys, 'argv', argv), mock.patch.object(argparse.ArgumentParser, 'parse_args', grab):
            with self.assertRaises(SystemExit): install.main()
        return got['a']


class Progress(Base):
    def test_waiting_lines_once_a_minute_never_back(self):
        out, clock = [], Clock()
        p = install.LauncherProgress(out.append, clock=clock)
        p.wait(install.LAUNCHER_WAITING)
        self.assertEqual(out, ['progress 0.00 ' + install.LAUNCHER_WAITING])
        clock.t = 30; p.emit(); self.assertEqual(len(out), 1)                    # nothing new inside the minute
        clock.t = 61; p.emit(); self.assertEqual(len(out), 2)                    # but a line at least once a minute
        p.wait(None); p.plan(['check', 'build', 'finish']); p.step(2, 3, 'Building', 'build', False)
        self.assertNotIn(install.LAUNCHER_WAITING, out[-1])
        p.model.fraction = lambda: 0.6; clock.t = 200; p.emit()
        p.wait(install.LAUNCHER_WAITING)                                          # the run ended: the bar stays where it got to
        self.assertTrue(out[-1].startswith('progress 0.60 '), out[-1])
        p.wait(None); p.plan(['check', 'finish'])                                  # Try again: a new model starts low
        nums = [float(l.split()[1]) for l in out]
        self.assertEqual(nums, sorted(nums))
        self.assertTrue(all(LINE.match(l) for l in out), out)


class ExitCodes(Base):
    def test_exit_follows_what_is_in_the_folder(self):
        self.assertEqual(install.launcher_exit(self.dest, None), 1)                  # nothing built: closed before finishing
        self.assertEqual(install.launcher_exit(self.dest, 'done'), 1)                # (a run said done but nothing is there)
        fake_install(self.dest)
        self.assertEqual(install.launcher_exit(self.dest, None), 0)                  # already built, the player only looked
        self.assertEqual(install.launcher_exit(self.dest, 'done'), 0)
        self.assertEqual(install.launcher_exit(self.dest, 'extras'), 2)              # the game is there, optional content failed
        (self.dest / '_work').mkdir(); (self.dest / '_work' / 'state.json').write_text('{"done": []}', encoding='utf-8')
        self.assertEqual(install.launcher_exit(self.dest, 'stopped'), 1)             # an unfinished run (HD content, update)
        shutil.rmtree(self.dest); fake_install(self.dest, kit_id='older000', version='2000.01.01')
        self.assertEqual(install.launcher_exit(self.dest, None), 1)                  # the update was not run

    def run_window(self, result, *extra, textures=()):
        out, err, seen = [], [], {}
        def window(dest, xiso, tex, videos, report=None, waiting=None):
            seen.update(dest=dest, xiso=xiso, textures=tex, videos=videos, waiting=waiting)
            if callable(result): return result(dest)
            return result
        def fake_print(*x, **k):
            (err if k.get('file') is sys.stderr else out).append(' '.join(map(str, x)))
        a = self.args('--window', *extra)
        with mock.patch.object(gui, 'launcher_window', window), mock.patch('builtins.print', fake_print), \
             mock.patch.object(install, 'Installer', side_effect=AssertionError('the hidden step ran')):
            code = install.run_launcher(a, list(textures))
        return code, out, err, seen

    def test_window_args_and_codes(self):
        code, out, err, seen = self.run_window({'last': None, 'ran': False}, '--xiso=C:/discs/MA2.xiso', '--videos', textures=['4x', '2x'])
        self.assertEqual(code, 1)
        self.assertEqual((seen['xiso'], seen['textures'], seen['videos']), ('C:/discs/MA2.xiso', ['2x', '4x'], True))
        self.assertEqual(Path(seen['dest']), self.dest.resolve())
        self.assertEqual(seen['waiting'], install.LAUNCHER_WAITING)
        self.assertIn('with the installer window', out[0])
        self.assertEqual(out[1], 'progress 0.00 ' + install.LAUNCHER_WAITING)            # a line as soon as the window opens
        self.assertEqual(err[-1], install.LAUNCHER_STOPPED)                               # closed before finishing
        self.assertTrue(out[-1].startswith('progress ') and out[-1].endswith(install.LAUNCHER_STOPPED))

        def built(dest): fake_install(Path(dest)); return {'last': 'done', 'ran': True}
        code, out, err, _ = self.run_window(built)
        self.assertEqual((code, out[-1], err), (0, 'progress 1 Ready to play', []))
        code, out, err, _ = self.run_window({'last': None, 'ran': False})                  # opened on a built folder, closed
        self.assertEqual((code, out[-1]), (0, 'progress 1 Already built'))
        code, out, err, _ = self.run_window({'last': 'extras', 'ran': True, 'extras': 'MechAssault 2 is installed. HD textures failed.'})
        self.assertEqual((code, err[-1].strip()), (2, 'MechAssault 2 is installed. HD textures failed.'))

    def test_a_window_that_cannot_start_falls_back_to_the_hidden_step(self):
        made = {}
        class Engine:
            def __init__(eng, xiso, dest, *a, **kw):
                made.update(kw=kw); eng.notes = []; eng.session_log = 'x.log'; eng.on_plan = eng.on_reuse = lambda *a: None
            def run(eng): pass
        a = self.args('--window', '--xiso=C:/discs/MA2.xiso')
        out = []
        with mock.patch.object(gui, 'launcher_window', lambda *x, **k: None), mock.patch.object(install, 'Installer', Engine), \
             mock.patch('builtins.print', lambda *x, **k: out.append(' '.join(map(str, x)))):
            code = install.run_launcher(a, [])
        self.assertEqual(code, 0)
        self.assertEqual(made['kw']['shortcuts'], ())
        self.assertIn('RecompLauncher setup: the installer window could not start; setting up without it', out)
        self.assertEqual(out[-1], 'progress 1 Ready to play')

    def test_unticked_never_opens_the_window(self):
        a = self.args('--xiso=C:/discs/MA2.xiso')
        self.assertFalse(a.window)
        with mock.patch.object(gui, 'launcher_window', side_effect=AssertionError('window')), \
             mock.patch.object(install, 'Installer') as eng, mock.patch('builtins.print'):
            eng.return_value.notes = []
            self.assertEqual(install.run_launcher(a, []), 0)


def footer_overlaps(app):
    """Footer items (texts and widgets) that run into each other."""
    cv, dp = app.cv, app.k.dp
    top = dp(app.H) - dp(64)
    boxes = []
    for i in cv.find_withtag('s'):
        if cv.type(i) not in ('text', 'window'): continue
        b = cv.bbox(i)
        if b and b[1] >= top - 2: boxes.append((b[0], b[2], cv.itemcget(i, 'text') if cv.type(i) == 'text' else cv.type(i)))
    boxes.sort()
    return [(a, b) for a, b in zip(boxes, boxes[1:]) if b[0] < a[1] - 4]


class Window(Base):
    """The window itself (transparent, off screen), as gui.launcher_window builds it."""
    def make(self, xiso=None, textures=(), videos=False, report=None):
        import tkinter as tk, ui_theme
        ui_theme.load_fonts()
        root = tk.Tk(); rig.KEEP.append(root)
        root.withdraw(); root.attributes('-alpha', 0.0); root.attributes('-toolwindow', True)
        app = gui.App(root, scale=1.0, launcher=dict(dest=str(self.dest), xiso=xiso, textures=list(textures), videos=videos,
                                                     report=report, waiting=install.LAUNCHER_WAITING))
        app.launches = []; app.launch = lambda what, path: app.launches.append((what, str(path)))
        try:
            h = app.hwnd; import ctypes
            ctypes.windll.user32.SetWindowLongW(h, -20, ctypes.windll.user32.GetWindowLongW(h, -20) | 0x08000000)
        except Exception: pass
        root.deiconify(); root.attributes('-alpha', 0.0)
        real = root.destroy; root.destroy = lambda: (app.stop_pump(), real())
        pump(root, 0.4)
        def gone():
            try:
                if not app.closing:
                    if app.worker and app.inst: app.inst.cancel(); until(root, lambda: app.worker is None, 30)
                    app.close()
            except Exception: pass                                              # already closed by the test
        self.addCleanup(gone)
        return root, app

    def shot(self, root, name):
        if SHOTS: snap(root, SHOTS / name)

    def test_fresh_folder_is_locked_and_pre_ticked(self):
        root, app = self.make(xiso=XISO or None, textures=['2x', '4x'], videos=True)
        self.assertEqual((app.screen, app.mode), ('setup', 'install'))
        self.assertEqual(precheck.clean_path(app.dest), precheck.clean_path(str(self.dest)))
        self.assertNotIn('dest', app.w)                                         # no folder question
        self.assertEqual((app.texture, app.videos), ('4x', True))               # the launcher's ticks, the window decides now
        self.assertEqual(app.sc, {'desktop': False, 'start': False, 'editor': False})
        if XISO:
            until(root, lambda: app.image is not None and not app.disc_checking and app.folder is not None, 30)
            self.assertTrue(app.image.ok, app.image.code)
            app.facts['sac'] = None
            if app.folder: app.folder.findings = [f for f in app.folder.findings if f.level != 'warn']
            app.render(); pump(root, 0.2)
            self.assertEqual(footer_overlaps(app), [])                          # Ultra HD + movies pre-ticked: the longest total
            self.assertNotIn('f_diag', app.w)                                   # Help > Collect diagnostics instead
            self.shot(root, '01-launcher-window-setup.png')
        app.go('options'); pump(root, 0.2)
        self.assertNotIn('o_dest', app.w); self.assertFalse(any(k.startswith('o_sc_') for k in app.w))      # no shortcuts section
        self.shot(root, '02-launcher-window-options.png')

    def test_a_run_feeds_the_launcher_and_ends_on_done(self):
        out = []
        report = install.LauncherProgress(out.append)
        report.wait(install.LAUNCHER_WAITING)
        with mock.patch.dict(os.environ, {'MA2_INSTALLER_FAKE': str(REC), 'MA2_FAKE_SPEED': '600', 'MA2_FAKE_STAMP': '1'}):
            root, app = self.make(report=report)
            if not XISO:                                                         # no disc: the run starts without the disc check
                app.disc = str(self.tmp / 'disc.xiso'); app.image = precheck.ImageResult(True, 'ok', '', '', {})
            else:
                app.set_disc(XISO); until(root, lambda: app.image is not None and not app.disc_checking, 30)
            until(root, lambda: app.folder is not None, 20)
            app.start(); pump(root, 0.3)
            self.assertTrue(getattr(app.inst, 'launcher', False))              # the folder is never remembered
            self.assertEqual(app.last_params['shortcuts'], [])
            self.shot(root, '03-launcher-window-installing.png')
            self.assertTrue(until(root, lambda: app.screen == 'done', 120), app.screen)
        prog = [l for l in out if l.startswith('progress ')]
        self.assertTrue(len(prog) > 5 and all(LINE.match(l) for l in prog), prog[:5])
        nums = [float(l.split()[1]) for l in prog]
        self.assertEqual(nums, sorted(nums))
        self.assertEqual(prog[-1].split(' ', 2)[2], install.LAUNCHER_WAITING)  # done: waiting for the player to close it
        self.assertEqual(app.lw['last'], 'done')
        self.assertEqual(app.primary.text, 'DONE')                              # no LAUNCH: the launcher plays the game
        self.assertNotIn('LAUNCH', ' '.join(str(getattr(w, 'text', '')) for w in app.w.values()))
        self.shot(root, '04-launcher-window-done.png')
        app.primary.invoke(); pump(root, 0.2)
        self.assertTrue(app.closing)
        self.assertEqual(install.launcher_exit(self.dest, app.lw['last']), 0)

    def test_built_folder_offers_repair_without_uninstall_or_play(self):
        fake_install(self.dest)
        root, app = self.make(textures=['2x'])
        self.assertEqual(app.mode, 'maintain')
        self.assertEqual(app.action(), 'addons')                                # Set Up Again with a new tick: add only that
        for key in ('dest', 'uninstall', 'play', 'selfupdate'): self.assertNotIn(key, app.w)
        self.assertIn('report', app.w)
        until(root, lambda: app.folder is not None, 20); app.render(); pump(root, 0.2)
        self.assertEqual(footer_overlaps(app), [])
        self.shot(root, '05-launcher-window-installed.png')
        st = json.loads((self.dest / precheck.STAMP).read_text(encoding='utf-8'))
        (self.dest / 'game_files/texture_packs/2x').mkdir(parents=True); (self.dest / 'game_files/texture_packs/2x/index.json').write_text('{}')
        root2, app2 = self.make(textures=['2x'])
        self.assertEqual((app2.texture, app2.action()), ('original', 'repair'))  # a pack already there is not asked for again

    def test_newer_install_is_updated_as_the_hidden_step(self):
        fake_install(self.dest, kit_id='newer000', version='2999.01.01')
        root, app = self.make()
        self.assertEqual((app.mode, app.action()), ('update', 'update'))
        self.assertTrue(app.params()['update'])

    def test_close_while_running_asks_first(self):
        with mock.patch.dict(os.environ, {'MA2_INSTALLER_FAKE': str(REC), 'MA2_FAKE_SPEED': '5'}):
            root, app = self.make()
            app.disc = str(self.tmp / 'disc.xiso'); app.image = precheck.ImageResult(True, 'ok', '', '', {})
            until(root, lambda: app.folder is not None, 20)
            app.start(); pump(root, 0.3)
            app.close(); pump(root, 0.2)
            self.assertFalse(app.closing); self.assertEqual(app.confirm, 'cancel')
            app.cancel_install(); until(root, lambda: app.worker is None, 30)
            self.assertEqual(app.lw['last'], 'stopped')
            app.close(); pump(root, 0.1)
            self.assertTrue(app.closing)


class Hidden(Base):
    """install.py --recomplauncher --window in its own process, no console, stdout piped, closed by the test hook."""
    def run_proc(self, steps, *extra, env_extra=None):
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('MA2_', 'RECOMP_', 'PYTHON'))}
        temp = self.tmp / 'temp'; temp.mkdir(exist_ok=True)
        env.update(LOCALAPPDATA=str(self.tmp / 'appdata'), TEMP=str(temp), TMP=str(temp), RECOMPLAUNCHER='1',
                   MA2_SHORTCUT_DIR=str(self.tmp / 'lnk'), MA2_DOCUMENTS_DIR=str(self.tmp / 'docs'), MA2_SELF_UPDATE='0',
                   MA2_UI_DISC_SEARCH='', MA2_LAUNCHER_UI_TEST=steps)
        env.update(env_extra or {})
        r = subprocess.run([sys.executable, '-B', str(REPO / 'kit' / 'installer' / 'install.py'), '--recomplauncher', '--window',
                            '--dest', str(self.dest)] + list(extra), env=env, capture_output=True, stdin=subprocess.DEVNULL,
                           creationflags=NW, timeout=300)
        return r.returncode, r.stdout.decode('utf-8', 'replace').splitlines(), r.stderr.decode('utf-8', 'replace').splitlines()

    def test_closed_before_finishing_is_1(self):
        code, out, err = self.run_proc('until:setup;close')
        self.assertEqual(code, 1, err)
        self.assertEqual(err[-1], install.LAUNCHER_STOPPED)
        self.assertTrue(out[-1].startswith('progress 0.00 ') and out[-1].endswith(install.LAUNCHER_STOPPED), out)
        self.assertTrue(all(l.startswith(('progress ', 'RecompLauncher setup:')) for l in out), out)       # no "ui:" lines on stdout

    def test_built_folder_closed_is_0(self):
        fake_install(self.dest)
        code, out, err = self.run_proc('until:setup;close')
        self.assertEqual((code, out[-1]), (0, 'progress 1 Already built'), err)
        self.assertFalse((self.tmp / 'lnk').exists())

    def test_a_run_to_done_then_closed_is_0(self):
        if not XISO: self.skipTest('MA2_TEST_XISO not set: the window waits for a valid disc')
        code, out, err = self.run_proc('until:setup;primary;until:done;close', '--xiso=%s' % XISO,
                                       env_extra={'MA2_INSTALLER_FAKE': str(REC), 'MA2_FAKE_SPEED': '2000', 'MA2_FAKE_STAMP': '1'})
        self.assertEqual((code, out[-1]), (0, 'progress 1 Ready to play'), err)
        prog = [l for l in out if l.startswith('progress ')]
        self.assertTrue(all(LINE.match(l) for l in prog), prog)
        nums = [float(l.split()[1]) for l in prog]
        self.assertEqual(nums, sorted(nums))


class Manifest(unittest.TestCase):
    def setUp(self):
        self.m = json.loads((REPO / 'recomplauncher.json').read_text(encoding='utf-8'))

    def test_the_window_tick(self):
        opts = self.m['prepare']['options']
        o = opts[-1]                                                            # last on the setup screen, after the content ticks
        self.assertEqual((o['id'], o['args'], o['default']), ('installer_window', ['--window'], False))
        self.assertTrue(o['label'].startswith('PLACEHOLDER:') and o['note'].startswith('PLACEHOLDER:'))      # not approved yet
        self.assertNotIn('required', o); self.assertNotIn('license', o)
        self.assertNotIn('--window', self.m['prepare']['args'])                # unticked: the hidden step, unchanged

    def test_start_falls_back_to_the_installer_in_the_release(self):
        s, p = self.m['start'], self.m['prepare']
        self.assertEqual(s['program'][0], 'MechAssault 2/MechAssault 2.exe')     # tried first: Play starts the built game
        self.assertEqual(s['program'][1:], p['program'])                       # the file in the zip: the Developer tab finds it
        dest = p['args'][p['args'].index('--dest') + 1]
        self.assertEqual(s['env']['MA2_RECOMPLAUNCHER_DEST'], dest)
        self.assertFalse(any(k.upper().startswith('RECOMPLAUNCHER') for k in s['env']))
        src = (REPO / 'kit' / 'installer' / 'gui.py').read_text(encoding='utf-8')
        self.assertIn("os.environ.get('MA2_RECOMPLAUNCHER_DEST'", src)          # the installer opens locked to that folder

    def test_installer_without_args_opens_locked_to_the_launchers_folder(self):
        calls = []
        with mock.patch.dict(os.environ, {'MA2_RECOMPLAUNCHER_DEST': 'D:/RL/MechAssault 2'}),              mock.patch.object(gui, 'launcher_window', lambda *a, **k: (calls.append(a), {'last': None})[1]),              mock.patch.object(gui, 'start_new', side_effect=AssertionError('the standalone window opened')),              mock.patch.object(gui, 'open_ui_log', lambda: (None, None)), mock.patch.object(gui, 'STARTUP_LOG', (None, None)),              mock.patch.object(selfupdate, '_switched_off', False):
            gui.main()
            self.assertFalse(selfupdate.enabled())
        self.assertEqual(calls, [('D:/RL/MechAssault 2', None, [], False)])


if __name__ == '__main__':
    if '--shots' in sys.argv:
        i = sys.argv.index('--shots'); SHOTS = Path(sys.argv[i + 1]); SHOTS.mkdir(parents=True, exist_ok=True); del sys.argv[i:i + 2]
    unittest.main(verbosity=2)

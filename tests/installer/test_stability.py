"""1.1.2 stability fixes: Tcl/Tk variables cleared by the stub, the window's start-up log, the build environment, parallel
compiles on small-memory PCs, a quarantined game exe, diagnostics (Build Tools log, Desktop refused), Start over after a
stopped update.  Run with the kit's Python:  python -B test_stability.py
"""
import faulthandler, inspect, json, os, shutil, sys, tempfile, unittest, zipfile
from pathlib import Path

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
import precheck, install                                                     # noqa: E402

G = precheck.GB


class Stability(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-stab-', dir=str(WORK / 'tmp')))
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_stub_clears_tcl_tk(self):
        src = (WORK.parent / 'installer-exe' / 'stub.cpp').read_text(encoding='utf-8', errors='replace')
        block = src[src.index('L"PYTHONHOME"'):]; block = block[:block.index('SetEnvironmentVariableW(v, nullptr)')]
        for v in ('TCL_LIBRARY', 'TK_LIBRARY', 'TCLLIBPATH', 'PYTHONHOME', 'PYTHONPATH'):
            self.assertIn('L"%s"' % v, block)

    def test_window_start_up_log(self):
        src = (KIT / 'installer' / 'gui.py').read_text(encoding='utf-8')
        self.assertLess(src.index('STARTUP_LOG = startup_log()'), src.index('import tkinter'), 'the log is open before tkinter and the kit modules load')
        self.assertLess(src.index('STARTUP_LOG = startup_log()'), src.index('import install, precheck'))
        self.assertIn('MessageBoxW', src[src.index('import tkinter'):src.index('def message_box') if 'def message_box' in src else None])
        sys.path.insert(0, str(KIT / 'installer'))
        import gui
        out, err = sys.stdout, sys.stderr
        local = os.environ['LOCALAPPDATA']; os.environ['LOCALAPPDATA'] = str(self.tmp / 'la')
        try:
            path, log = gui.startup_log()
            print('hello from the window')
            self.assertTrue(faulthandler.is_enabled())
        finally:
            faulthandler.disable(); sys.stdout, sys.stderr = out, err; os.environ['LOCALAPPDATA'] = local
        log.close()
        self.assertEqual(path.parent, self.tmp / 'la' / 'MechAssault2' / 'Logs'); self.assertTrue(path.name.startswith('installer-ui-'))
        self.assertIn('hello from the window', path.read_text(encoding='utf-8'))

    def test_build_env_drops_injected_flags(self):
        self.assertEqual(install.BUILD_ENV_DROP, ('CL', '_CL_', 'LINK', '_LINK_'))
        src = inspect.getsource(install.Installer.build)
        self.assertLess(src.index('for v in BUILD_ENV_DROP: env.pop(v, None)'), src.index("title='CMake + Ninja'"))

    def test_build_jobs_small_memory(self):
        self.assertEqual(precheck.build_jobs(8, 6 * G, 7.8 * G), 2)                # an 8 GB laptop: was 3
        self.assertEqual(precheck.build_jobs(4, 3 * G, 7.8 * G), 2)
        self.assertEqual(precheck.build_jobs(8, 1.5 * G, 7.8 * G), 1)              # little free memory still wins
        self.assertEqual(precheck.build_jobs(12, 9 * G, 11.8 * G), 4)              # a 12 GB PC: unchanged
        self.assertEqual(precheck.build_jobs(24, 20 * G, 32 * G), 12)              # unchanged
        self.assertEqual(precheck.build_jobs(16, 5 * G), 2)                        # total unknown: estimated from free memory

    def test_missing_exe_after_build_is_quarantine(self):
        src = inspect.getsource(install.Installer.build)
        tail = src[src.index("if not (build / 'mechassault2_recomp.exe').is_file():"):]
        self.assertIn("'access_denied')", tail); self.assertIn('quarantined', tail)
        self.assertIn('antivirus', __import__('gui').FAILURES['access_denied'][1].lower())

    def test_diagnostics_build_tools_log_and_refused_desktop(self):
        local = os.environ['LOCALAPPDATA']; os.environ['LOCALAPPDATA'] = str(self.tmp / 'la')
        real = zipfile.ZipFile
        try:
            logs = precheck.logs_dir(); logs.mkdir(parents=True)
            (logs / 'buildtools-20261118-100000.log').write_text('vs_buildtools exit 1603\n', encoding='utf-8')
            desk = self.tmp / 'Desktop'; desk.mkdir()
            res = precheck.collect_diagnostics(self.tmp / 'none', desk, gpus=[])
            self.assertIn('logs/buildtools-20261118-100000.log', res['members']); self.assertEqual(res['path'].parent, desk)
            def guarded(file, *a, **k):                                   # Controlled Folder Access: the Desktop refuses new files
                if Path(file).parent == desk: raise PermissionError(13, 'Access is denied')
                return real(file, *a, **k)
            zipfile.ZipFile = guarded
            res = precheck.collect_diagnostics(self.tmp / 'none', desk, gpus=[])
            self.assertEqual(res['path'].parent, self.tmp / 'la' / 'MechAssault2' / 'Diagnostics'); self.assertTrue(res['path'].is_file())
            self.assertTrue(any('could not be written to' in n for n in res['notes']))
        finally:
            zipfile.ZipFile = real; os.environ['LOCALAPPDATA'] = local

    def make_stopped_update(self, core_done=False):
        d = self.tmp / ('inst-%d' % core_done); (d / '_work').mkdir(parents=True)
        (d / 'MechAssault 2.exe').write_text('new'); (d / 'MechAssault 2.exe.previous').write_text('old')
        state = {'update': True, 'done': ['check', 'verify', 'build'], 'core_done': core_done,
                 'previous': [[str(d / 'MechAssault 2.exe'), str(d / 'MechAssault 2.exe.previous')]]}
        (d / '_work' / 'state.json').write_text(json.dumps(state), encoding='utf-8')
        return d

    def test_start_over_after_a_stopped_update_puts_the_old_game_back(self):
        d = self.make_stopped_update()
        self.assertTrue(precheck.read_state(d).get('previous'))
        install.start_over(d)
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'old'); self.assertFalse((d / 'MechAssault 2.exe.previous').exists())
        self.assertFalse((d / '_work').exists())
        d = self.make_stopped_update(core_done=True)                       # the new game was already final: nothing to put back
        install.start_over(d)
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new'); self.assertFalse((d / '_work').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)

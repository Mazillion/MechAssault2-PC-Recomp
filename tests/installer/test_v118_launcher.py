"""1.1.8 installer: RecompLauncher's setup step (--recomplauncher) and the recomplauncher.json the release zip carries.
What to do in the folder (install, update, resume, already built), the "progress <0..1> <text>" lines, no installer copy / shortcuts /
self-update / remembered folder, the exit codes, a real hidden run of install.py, and the manifest against the installer.
Run with the kit's Python (or Python 3.13):  python -B test_v118_launcher.py
Optional: MA2_TEST_RECOMPLAUNCHER_SCHEMA=<the schema from recomplauncher.com> also validates the manifest (needs jsonschema).
"""
import json, os, re, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
REPO = WORK.parent
REAL_KIT = REPO / 'kit'
sys.path.insert(0, str(REAL_KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
import precheck, install, selfupdate                                         # noqa: E402

LINE = re.compile(r'^progress (0\.\d\d|1) \S.*$')
NW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def fake_install(dest, kit_id='dev', version=None):
    for rel in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
        (dest / rel).parent.mkdir(parents=True, exist_ok=True); (dest / rel).write_bytes(b'x')
    (dest / precheck.STAMP).write_text(json.dumps({'kit_id': kit_id, 'version': version or install.VERSION}), encoding='utf-8')


class Clock:
    def __init__(self): self.t = 0.0
    def __call__(self): return self.t


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-118-', dir=str(WORK / 'tmp')))
        self.dest = self.tmp / 'library' / 'MechAssault 2'
        mock.patch.object(selfupdate, '_switched_off', False).start()           # run_launcher switches it off for the process
    def tearDown(self):
        mock.patch.stopall()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def args(self, *extra):
        argv = ['install.py', '--recomplauncher', '--dest', str(self.dest)] + list(extra)
        with mock.patch.object(sys, 'argv', argv):
            import argparse
            real = argparse.ArgumentParser.parse_args
            got = {}
            def grab(p, *a, **k):
                got['a'] = real(p, *a, **k); raise SystemExit('parsed')
            with mock.patch.object(argparse.ArgumentParser, 'parse_args', grab):
                with self.assertRaises(SystemExit): install.main()
        return got['a']


class Plan(Base):
    def test_empty_or_missing_folder_is_an_install(self):
        self.assertEqual(install.launcher_plan(self.dest), 'install')
        self.dest.mkdir(parents=True)
        self.assertEqual(install.launcher_plan(self.dest), 'install')

    def test_this_version_installed_is_built(self):
        fake_install(self.dest, kit_id=install.KIT_ID)
        self.assertEqual(install.launcher_plan(self.dest), 'built')

    def test_another_installer_build_is_an_update_either_way(self):
        fake_install(self.dest, kit_id='older000', version='2000.01.01')
        self.assertEqual(install.launcher_plan(self.dest), 'update')
        fake_install(self.dest, kit_id='newer000', version='2999.01.01')             # the player went back to an older release
        self.assertEqual(install.launcher_plan(self.dest), 'update')

    def test_unfinished_run_is_resumed_whatever_is_there(self):
        fake_install(self.dest, kit_id=install.KIT_ID)
        (self.dest / '_work').mkdir(); (self.dest / '_work' / 'state.json').write_text('{"done": []}', encoding='utf-8')
        self.assertEqual(install.launcher_plan(self.dest), 'resume')


class Progress(Base):
    def lines(self):
        out, clock = [], Clock()
        return out, clock, install.LauncherProgress(out.append, clock=clock)

    def test_lines_follow_the_window_stages_and_never_go_back(self):
        out, clock, p = self.lines()
        names = ['check', 'disc', 'extract', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets', 'install', 'finish']
        p.plan(names)
        for i, n in enumerate(names, 1):
            p.step(i, len(names), n, n, False)
            clock.t += 30
            if n == 'build':
                for k in range(1, 11): p.log('[%d/10] Building C object gen/x%d.c.obj' % (k, k))
            p.log('    done in 30s')
        p.reuse('fast')                                                          # changes the weights: the bar must not jump back
        p.emit(); p.finish('Ready to play')
        self.assertTrue(all(LINE.match(l) for l in out), out)
        nums = [float(l.split()[1]) for l in out]
        self.assertEqual(nums, sorted(nums))
        self.assertEqual(out[-1], 'progress 1 Ready to play')
        self.assertTrue(any('Compiling 10 of 10 files' in l for l in out), out)
        self.assertTrue(any(l.endswith('Reading your disc') for l in out), out)
        self.assertLess(max(nums[:-1]), 1.0)

    def test_an_update_with_the_kept_build_never_goes_back_a_stage(self):
        """As the real 1.1.7 -> 1.1.8 update ran: verify (outside the stages), translation kept, a short build."""
        out, clock, p = self.lines()
        names = ['check', 'verify', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets', 'install', 'finish']
        p.plan(names)
        for i, n in enumerate(names, 1):
            kept = n in ('analyze', 'recompile', 'generate')
            p.step(i, len(names), n, n, kept)
            if not kept:
                clock.t += 5; p.emit()
                if n == 'build':
                    for k in (1, 2, 3): p.log('[%d/3] Building C object a%d.obj' % (k, k))
        p.finish('Ready to play')
        titles = [t for _, t, _ in p.model.stages]
        seen = [l.split(' ', 2)[2] for l in out]
        order = [titles.index(t) for t in seen if t in titles]
        self.assertEqual(order, sorted(order), seen)

    def test_a_line_at_least_once_a_minute_even_without_change(self):
        out, clock, p = self.lines()
        p.plan(['check', 'build'])
        p.step(2, 2, 'Compiling', 'build', False)
        p.log('[1/5000] Building C object a.obj')
        n = len(out); p.emit(); self.assertEqual(len(out), n)                # nothing new: nothing printed
        clock.t += 61; p.emit(); self.assertEqual(len(out), n + 1)

    def test_a_broken_model_never_stops_the_install(self):
        out, clock, p = self.lines()
        p.plan(['check'])
        with mock.patch.object(p.model, 'fraction', side_effect=RuntimeError('boom')):
            p.emit(); p.log('anything')
        self.assertEqual(out, ['progress 0.00 Reading your disc'])


class Run(Base):
    """run_launcher with a stand-in engine: what it is asked to do, and what reaches stdout."""
    def run_with(self, engine_run=None, *extra, textures=()):
        made, out = {}, []
        class Engine:
            def __init__(eng, xiso, dest, *a, **kw):
                made.update(xiso=xiso, dest=dest, kw=kw, args=a); eng.notes = []; eng.session_log = 'x.log'
                eng.on_plan = eng.on_reuse = lambda *a: None; eng.kw = kw
            def run(eng):
                if engine_run: engine_run(eng)
        a = self.args(*extra)
        with mock.patch.object(install, 'Installer', Engine), mock.patch('builtins.print', lambda *x, **k: out.append(' '.join(map(str, x)))):
            code = install.run_launcher(a, list(textures))
        return code, made, out

    def test_set_up_again_with_a_new_tick_adds_only_that(self):
        fake_install(self.dest, kit_id=install.KIT_ID)                           # this version, no packs
        code, made, out = self.run_with(None, '--xiso=x.iso', '--videos', textures=['2x'])
        self.assertEqual(code, 0)
        self.assertFalse(made['kw']['update'])                                   # addons only: no update, no rebuild
        self.assertEqual(made['args'][0:2], (['2x'], True))
        self.assertIn('RecompLauncher setup: adding enhanced textures 2x and enhanced cutscenes to the installed game', out)
        st = json.loads((self.dest / precheck.STAMP).read_text(encoding='utf-8'))
        st['options'] = {'textures': ['2x'], 'videos': True}                     # what settle writes once the packs are on disk
        (self.dest / precheck.STAMP).write_text(json.dumps(st), encoding='utf-8')
        code, made, out = self.run_with(None, '--xiso=x.iso', '--videos', textures=['2x'])
        self.assertEqual((code, made, out[-1]), (0, {}, 'progress 1 Already built'))
        code, made, out = self.run_with(None, '--xiso=x.iso', textures=['4x'])   # another pack: only that one
        self.assertEqual(code, 0); self.assertIn('adding enhanced textures 4x to the installed game', out[1])

    def test_missing_extras_reads_the_stamp(self):
        self.assertEqual(install.launcher_missing_extras(self.dest, ['2x'], True), ['enhanced textures 2x', 'enhanced cutscenes'])
        fake_install(self.dest, kit_id=install.KIT_ID)
        st = json.loads((self.dest / precheck.STAMP).read_text(encoding='utf-8'))
        st['options'] = {'textures': ['2x', '4x'], 'videos': False}
        (self.dest / precheck.STAMP).write_text(json.dumps(st), encoding='utf-8')
        self.assertEqual(install.launcher_missing_extras(self.dest, ['4x'], False), [])
        self.assertEqual(install.launcher_missing_extras(self.dest, [], True), ['enhanced cutscenes'])

    def test_install_update_and_built(self):
        code, made, out = self.run_with(None, '--xiso=C:/discs/MA2.xiso')
        self.assertEqual(code, 0)
        self.assertFalse(made['kw']['update']); self.assertEqual(made['kw']['shortcuts'], ())
        self.assertEqual(out[-1], 'progress 1 Ready to play')
        self.assertFalse(selfupdate.enabled())                                  # no installer copy, no Update shortcut, no check
        fake_install(self.dest, kit_id='older000', version='2000.01.01')
        code, made, out = self.run_with(None)                                    # keeps_copy: no disc on an update
        self.assertEqual(code, 0); self.assertTrue(made['kw']['update'])
        fake_install(self.dest, kit_id=install.KIT_ID)
        code, made, out = self.run_with(None, '--xiso=C:/discs/MA2.xiso')
        self.assertEqual((code, made, out[-1]), (0, {}, 'progress 1 Already built'))

    def test_both_texture_ticks_make_both_packs(self):
        code, made, out = self.run_with(None, '--xiso=x.iso', textures=['4x', '2x', '2x'])
        self.assertEqual((code, made['args'][0]), (0, ['2x', '4x']))
        a = self.args('--xiso=x.iso', '--textures', '2x', '--textures', '4x')         # two ticks = the flag twice: both kept
        self.assertEqual(a.textures, ['2x', '4x'])

    def test_a_stop_ends_with_the_approved_sentence(self):
        def stop(eng): raise install.Stop('it stopped', 'build_failed')
        code, made, out = self.run_with(stop, '--xiso=x')
        self.assertEqual(code, 1)
        self.assertEqual(out[-1], install.LAUNCHER_STOPPED)                       # the last stderr line
        self.assertTrue(out[-2].startswith('progress ') and out[-2].endswith(install.LAUNCHER_STOPPED), out[-2])
        self.assertRegex(out[-2], LINE)
        code, made, out = self.run_with(None)                                    # nothing to install or update
        self.assertEqual((code, out[-1]), (1, install.LAUNCHER_STOPPED))

    def test_engine_events_become_progress_lines(self):
        def engine(eng):
            eng.on_plan(['check', 'build']); eng.kw['step'](1, 2, 'Checking', 'check', False)
            eng.kw['log']('[3/9] Building C object y.obj')
        code, made, out = self.run_with(engine, '--xiso=x.iso')
        prog = [l for l in out if l.startswith('progress ')]
        self.assertTrue(prog and all(LINE.match(l) for l in prog), out)
        self.assertIn('[3/9] Building C object y.obj', out)                      # the engine's own lines go out too

    def test_no_disc_and_nothing_to_update_stops(self):
        code, made, out = self.run_with(None)
        self.assertEqual((code, made), (1, {}))
        self.assertFalse(self.dest.exists())

    def test_exit_codes(self):
        def stop(kind):
            def f(eng): raise install.Stop('it stopped', kind)
            return f
        self.assertEqual(self.run_with(stop('build_failed'), '--xiso=x')[0], 1)
        self.assertEqual(self.run_with(stop('extras'), '--xiso=x')[0], 2)
        def crash(eng): raise RuntimeError('boom')
        with mock.patch('traceback.print_exc'):
            self.assertEqual(self.run_with(crash, '--xiso=x')[0], 1)

    def test_the_launchers_folder_is_not_remembered(self):
        self.dest.mkdir(parents=True)
        inst = install.Installer(None, self.dest, log=lambda s: None)
        inst.launcher = True; inst.remember_install(toolchain='vs')
        j = precheck.read_json(precheck.local_dir() / 'installer.json')
        self.assertEqual(j.get('toolchain'), 'vs'); self.assertNotIn('dest', j)
        inst.launcher = False; inst.remember_install()
        self.assertEqual(precheck.read_json(precheck.local_dir() / 'installer.json')['dest'], str(inst.dest))

    def test_without_the_flag_nothing_changes(self):
        a = self.args()
        self.assertTrue(a.recomplauncher)
        with mock.patch.object(sys, 'argv', ['install.py', '--dest', str(self.dest)]), \
             mock.patch.object(install, 'run_launcher', side_effect=AssertionError('called')),              mock.patch.object(selfupdate, 'cleanup_old'), \
             mock.patch.object(install.Installer, 'run', side_effect=install.Stop('stop here', 'disc')):
            with self.assertRaises(SystemExit) as e: install.main()
        self.assertEqual(e.exception.code, 1)


class Hidden(Base):
    def test_a_real_hidden_run_prints_the_line_and_exits_0(self):
        """install.py --recomplauncher in its own process, no window, stdout piped (as RecompLauncher runs it)."""
        fake_install(self.dest, kit_id=install.KIT_ID)
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('MA2_', 'RECOMP_', 'PYTHON'))}
        temp = self.tmp / 'temp'; temp.mkdir()
        env.update(LOCALAPPDATA=str(self.tmp / 'appdata'), TEMP=str(temp), TMP=str(temp), RECOMPLAUNCHER='1',
                   MA2_SHORTCUT_DIR=str(self.tmp / 'lnk'), MA2_DOCUMENTS_DIR=str(self.tmp / 'docs'))
        r = subprocess.run([sys.executable, '-B', str(REAL_KIT / 'installer' / 'install.py'), '--recomplauncher',
                            '--xiso=%s' % (self.tmp / 'no disc.xiso'), '--dest', str(self.dest)], env=env, capture_output=True,
                           stdin=subprocess.DEVNULL, creationflags=NW, timeout=120)
        out = r.stdout.decode('utf-8', 'replace').splitlines()
        self.assertEqual(r.returncode, 0, r.stderr.decode('utf-8', 'replace'))
        self.assertEqual(out[-1], 'progress 1 Already built')
        self.assertFalse((self.tmp / 'lnk').exists())


class Manifest(unittest.TestCase):
    def setUp(self):
        self.m = json.loads((REPO / 'recomplauncher.json').read_text(encoding='utf-8'))
        self.src = (REAL_KIT / 'installer' / 'install.py').read_text(encoding='utf-8')

    def test_programs_and_paths_match_the_installer(self):
        p, s = self.m['prepare'], self.m['start']
        self.assertEqual(p['program'], ['MechAssault 2 Recomp Installer.exe'])
        self.assertEqual(s['program'], ['MechAssault 2/MechAssault 2.exe'])
        self.assertIn('--recomplauncher', p['args'])
        self.assertIn('--xiso={input:game}', p['args'])                       # one argument: keeps_copy leaves it out
        self.assertTrue(p['keeps_copy'])
        dest = p['args'][p['args'].index('--dest') + 1]
        self.assertEqual(dest, '{install}/MechAssault 2')
        self.assertEqual(self.m['folders']['game'][0]['path'], dest)
        self.assertEqual(sorted(p['done']), sorted([dest + '/MechAssault 2.exe', dest + '/' + precheck.STAMP]))

    def test_every_option_arg_is_an_installer_option(self):
        known = set(re.findall(r"add_argument\('(--[\w-]+)'", self.src))
        for a in self.m['prepare']['args'] + [x for o in self.m['prepare']['options'] for x in o['args']]:
            if a.startswith('--'): self.assertIn(a.split('=')[0], known)
        req = {o['id'] for o in self.m['prepare']['options'] if o.get('required')}
        self.assertEqual(req, {'vs_build_tools', 'ffmpeg'})

    def test_settings_keys_are_game_ini_keys(self):
        ini = (REAL_KIT / 'defaults' / 'pc_settings.ini').read_text(encoding='utf-8')
        keys = set()
        section = None
        for line in ini.splitlines():
            m = re.match(r'^\[(\w+)\]', line)
            if m: section = m.group(1)
            m = re.match(r'^(\w+)=', line)
            if m: keys.add('%s/%s' % (section, m.group(1)))
        for s in self.m['settings']:
            for k in list(s.get('defaults', {})) + [k for c in s['choices'] for k in c['set']]:
                self.assertIn(k, keys)

    def test_plain_player_texts(self):
        texts = [self.m['inputs'][0]['label'], self.m['prepare']['label']] + list(self.m['exit_codes'].values())
        texts += [o['label'] for o in self.m['prepare']['options']]
        texts += [s[k] for s in self.m['settings'] for k in ('label', 'help')] + [c['label'] for s in self.m['settings'] for c in s['choices']]
        notes = [o['note'] for o in self.m['prepare']['options']]     # 1.2.0: the owner's approved notes are one short paragraph
        for t in texts + notes:
            self.assertNotRegex(t.lower(), r'reddit|discord|github|ninja|cmake|xdvdfs|\bini\b')
            self.assertLessEqual(len(t), 300 if t in notes else 130, t)

    def test_against_the_schema_when_given(self):
        path = os.environ.get('MA2_TEST_RECOMPLAUNCHER_SCHEMA')
        if not path: self.skipTest('MA2_TEST_RECOMPLAUNCHER_SCHEMA not set')
        try:
            import jsonschema
        except ImportError:
            self.skipTest('jsonschema is not installed')
        errors = list(jsonschema.Draft7Validator(json.loads(Path(path).read_text(encoding='utf-8'))).iter_errors(self.m))
        self.assertEqual([e.message for e in errors], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

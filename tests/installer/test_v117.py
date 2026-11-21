"""1.1.7 installer: the kept build and quick updates (fingerprint, cache.json, source sync that keeps unchanged files' times, every
fallback to the 1.1.6 clean path), recomp-manifest.json (copy only what differs, Repair lists changed or missing files), the earlier
version kept in _previous until the new one has had a good launch, --rollback, Uninstall of the kept build folder, the low-disk
rule, and fault injection on every new method (each must end exactly as 1.1.6 does).
Run with the kit's Python:  python -B test_v117.py
"""
import json, os, re, shutil, subprocess, sys, tempfile, time, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
REPO = WORK.parent
REAL_KIT = REPO / 'kit'
sys.path.insert(0, str(REAL_KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
os.environ.pop('MA2_TEST_FINGERPRINT_SALT', None); os.environ.pop('MA2_KEEP_BUILD', None); os.environ.pop('MA2_FAST_UPDATE', None)
import precheck, install, progress                                           # noqa: E402

G = precheck.GB
XBE = os.environ.get('MA2_TEST_XBE')
boom = lambda *a, **k: (_ for _ in ()).throw(RuntimeError('injected fault'))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-117-', dir=str(WORK / 'tmp')))
        install._TREE_MEMO.clear()
        self.lines = []
        # The kept FastCode files are checked against the release's hashes, which fake generated code cannot have: a file named in
        # fastcode.json inside a 'gen' folder hashes as the release says (everything else is the real SHA-256).
        spec = json.loads((REAL_KIT / 'patches' / 'fastcode' / 'fastcode.json').read_text(encoding='utf-8'))
        real = install.sha256
        self.shap = mock.patch.object(install, 'sha256', lambda p: spec['final'].get(Path(p).name, real(p)) if Path(p).parent.name == 'gen' else real(p))
        self.shap.start()
    def tearDown(self):
        mock.patch.stopall()
        install._TREE_MEMO.clear()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake_vs(self, name='vs', msvc='14.50.1'):
        vs = self.tmp / name
        b = vs / 'VC/Tools/MSVC' / msvc / 'bin/Hostx64/x64'; b.mkdir(parents=True, exist_ok=True)
        for n in ('cl.exe', 'c1.dll', 'c2.dll', 'link.exe'): (b / n).write_bytes(n.encode())
        (vs / 'VC/Auxiliary/Build').mkdir(parents=True, exist_ok=True)
        (vs / 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt').write_text(msvc + '\n')
        for n in ('cmake.exe', 'ninja.exe'): (vs / n).write_bytes(n.encode())
        return vs

    def installer(self, name='game', **kw):
        d = self.tmp / name; d.mkdir(parents=True, exist_ok=True)
        inst = install.Installer(None, d, log=self.lines.append, **kw)
        vs = self.fake_vs()
        inst.vcvars, inst.cmake, inst.ninja, inst.vs = vs / 'vcvars64.bat', vs / 'cmake.exe', vs / 'ninja.exe', vs
        inst.state = {'done': [], 'update': True}
        inst.work.mkdir(parents=True, exist_ok=True)
        return inst

    def complete_cache(self, inst, **over):
        """A finished install's cache.json for `inst` (as record_cache writes it), with the patched generated code in place."""
        gen = inst.work / install.GEN_REL; gen.mkdir(parents=True, exist_ok=True)
        spec = json.loads((install.KIT / 'patches' / 'fastcode' / 'fastcode.json').read_text(encoding='utf-8'))
        for f in spec['final']: (gen / f).write_text('fast ' + f)
        (gen / 'recomp_extra.c').write_text('int x;')
        fp = inst.fingerprint()
        data = {'complete': True, 'fingerprint': fp, 'gen': install.tree_hash(gen, memo=False), 'root': str(inst.work),
                'lowmem': {'kind': fp['lowmem'], 'units': {}, 'ladder': False, 'oom_jobs': None}}
        for k, v in over.items(): data[k] = v
        install.write_cache(inst.work, data)
        (inst.work / 'build').mkdir(exist_ok=True); (inst.work / 'build' / 'build.ninja').write_text('')
        return data


# ---------------------------------------------------------------------------- fingerprint and sync

class Fingerprint(Base):
    def test_tree_hash_sees_content_names_and_ignores_pycache(self):
        t = self.tmp / 't'; (t / 'a').mkdir(parents=True); (t / 'a' / 'x.py').write_text('1')
        h1 = install.tree_hash(t, memo=False)
        (t / 'a' / '__pycache__').mkdir(); (t / 'a' / '__pycache__' / 'x.pyc').write_bytes(b'junk'); (t / 'y.pyc').write_bytes(b'j')
        self.assertEqual(install.tree_hash(t, memo=False), h1)
        (t / 'a' / 'x.py').write_text('2'); h2 = install.tree_hash(t, memo=False); self.assertNotEqual(h1, h2)
        (t / 'a' / 'x.py').rename(t / 'a' / 'z.py'); self.assertNotEqual(install.tree_hash(t, memo=False), h2)
        self.assertEqual(install.tree_hash(self.tmp / 'missing'), '')

    def test_gen_fingerprint_covers_toolkit_bringup_xbe_patches_and_patch_tool(self):
        fp = install.gen_fingerprint()
        self.assertEqual(set(fp), {'schema', 'python', 'toolkit', 'bringup', 'xbe', 'patches'})
        self.assertEqual(fp['xbe'], install.RELEASE['xbe_sha256'])
        self.assertTrue(fp['patches'].endswith(':' + install.sha256(install.HERE / 'gen_patch.py')))
        self.assertEqual(install.gen_fingerprint(), fp)                       # stable

    def test_compiler_identity_changes_with_the_compiler(self):
        vs = self.fake_vs()
        a = install.compiler_identity(vs / 'vcvars64.bat', vs / 'cmake.exe', vs / 'ninja.exe', vs)
        self.assertEqual(a['msvc'], '14.50.1'); self.assertIsNotNone(a['cl']); self.assertIsNotNone(a['c2'])
        self.assertEqual(install.compiler_identity(vs / 'vcvars64.bat', vs / 'cmake.exe', vs / 'ninja.exe', vs), a)
        b2 = vs / 'VC/Tools/MSVC/14.50.1/bin/Hostx64/x64/c2.dll'; b2.write_bytes(b'a newer code generator')
        self.assertNotEqual(install.compiler_identity(vs / 'vcvars64.bat', vs / 'cmake.exe', vs / 'ninja.exe', vs), a)
        vs2 = self.fake_vs('vs2', '14.51.0')
        self.assertNotEqual(install.compiler_identity(vs2 / 'vcvars64.bat', vs2 / 'cmake.exe', vs2 / 'ninja.exe', vs2)['msvc'], a['msvc'])

    def test_salt_env_changes_only_the_named_parts(self):
        inst = self.installer(); fp = inst.fingerprint()
        with mock.patch.dict(os.environ, MA2_TEST_FINGERPRINT_SALT='compiler=1'):
            fp2 = inst.fingerprint()
        self.assertNotEqual(fp2['compiler'], fp['compiler']); self.assertEqual(fp2['gen'], fp['gen'])
        install._TREE_MEMO.clear()
        with mock.patch.dict(os.environ, MA2_TEST_FINGERPRINT_SALT='patches=1'):
            fp3 = inst.fingerprint()
        self.assertNotEqual(fp3['gen']['patches'], fp['gen']['patches']); self.assertEqual(fp3['compiler'], fp['compiler'])


class Sync(Base):
    def test_sync_writes_only_changed_files_keeps_times_and_deletes_extras(self):
        a = self.tmp / 'a'; d = self.tmp / 'd'
        for n, t in (('x.c', 'one'), ('sub/y.c', 'two'), ('sub/z.h', 'three')):
            (a / n).parent.mkdir(parents=True, exist_ok=True); (a / n).write_text(t)
        want = install.tree_files(a)
        res = install.sync_tree({k: v for k, v in want.items()}, d)
        self.assertEqual(sorted(res['written']), ['sub/y.c', 'sub/z.h', 'x.c'])
        old = time.time() - 3600
        for p in d.rglob('*'):
            if p.is_file(): os.utime(p, (old, old))
        (a / 'sub' / 'y.c').write_text('TWO'); (d / 'stale.c').write_text('gone'); (d / 'olddir').mkdir(); (d / 'olddir' / 'q.c').write_text('q')
        t0 = time.time()
        res = install.sync_tree(install.tree_files(a), d)
        self.assertEqual(res['written'], ['sub/y.c']); self.assertEqual(sorted(res['deleted']), ['olddir/q.c', 'stale.c']); self.assertEqual(res['same'], 2)
        self.assertAlmostEqual((d / 'x.c').stat().st_mtime, old, delta=1)              # unchanged: its time stays
        self.assertAlmostEqual((d / 'sub' / 'z.h').stat().st_mtime, old, delta=1)
        self.assertGreaterEqual((d / 'sub' / 'y.c').stat().st_mtime, t0 - 2)          # changed: new time (Ninja rebuilds it)
        self.assertEqual((d / 'sub' / 'y.c').read_text(), 'TWO'); self.assertFalse((d / 'olddir').exists())
        self.assertFalse(list(d.rglob('*.sync')))

    def test_same_size_different_bytes_is_written(self):
        a = self.tmp / 'a'; a.mkdir(); (a / 'f').write_text('abc')
        d = self.tmp / 'd'; d.mkdir(); (d / 'f').write_text('abd')
        self.assertEqual(install.sync_tree(install.tree_files(a), d)['written'], ['f'])

    def test_cancel_stops_the_sync(self):
        a = self.tmp / 'a'; a.mkdir(); (a / 'f').write_text('abc')
        with self.assertRaises(install.Stop) as cm:
            install.sync_tree(install.tree_files(a), self.tmp / 'd', cancelled=lambda: True)
        self.assertEqual(cm.exception.kind, 'cancelled')


# ---------------------------------------------------------------------------- the decision: fast, rebuild or the 1.1.6 path

class Decision(Base):
    def test_fast_when_nothing_changed(self):
        inst = self.installer(); self.complete_cache(inst)
        self.assertEqual(inst.reuse_mode(inst.fingerprint())[0], 'fast')

    def test_every_trigger(self):
        cases = [
            ('not an update', None, lambda inst: inst.state.update(update=False)),
            ('clean build asked', None, lambda inst: setattr(inst, 'clean_build', True)),
            ('no cache', None, lambda inst: (inst.work / install.CACHE_FILE).unlink()),
            ('unreadable cache', None, lambda inst: (inst.work / install.CACHE_FILE).write_text('{not json')),
            ('other schema', None, lambda inst: (inst.work / install.CACHE_FILE).write_text(json.dumps({'schema': 99, 'complete': True}))),
            ('incomplete', None, lambda inst: install.write_cache(inst.work, dict(install.read_cache(inst.work), complete=False))),
            ('gen code changed', None, lambda inst: (inst.work / install.GEN_REL / 'recomp_extra.c').write_text('int y;')),
            ('gen file added', None, lambda inst: (inst.work / install.GEN_REL / 'recomp_9999.c').write_text('int y;')),
            ('toolkit changed', None, lambda inst: self.patch_cache_fp(inst, 'gen', 'toolkit', 'other')),
            ('bringup changed', None, lambda inst: self.patch_cache_fp(inst, 'gen', 'bringup', 'other')),
            ('patches changed', None, lambda inst: self.patch_cache_fp(inst, 'gen', 'patches', 'other')),
            ('python changed', None, lambda inst: self.patch_cache_fp(inst, 'gen', 'python', '3.0')),
            ('compiler changed', 'rebuild', lambda inst: self.patch_cache_fp(inst, None, 'compiler', {'vs': 'old'})),
            ('build folder changed', 'rebuild', lambda inst: self.patch_cache_fp(inst, None, 'root', 'C:/elsewhere')),
            ('memory setting changed', 'rebuild', lambda inst: self.patch_cache(inst, lowmem={'kind': 'heavy'})),
            ('low-memory ladder', 'rebuild', lambda inst: self.patch_cache(inst, lowmem={'kind': inst.fingerprint()['lowmem'], 'ladder': True})),
            ('oom jobs', 'rebuild', lambda inst: self.patch_cache(inst, lowmem={'kind': inst.fingerprint()['lowmem'], 'oom_jobs': 1})),
        ]
        for name, want, change in cases:
            with self.subTest(name):
                inst = self.installer(name.replace(' ', '-')); self.complete_cache(inst)
                change(inst)
                self.assertEqual(inst.reuse_mode(inst.fingerprint())[0], want, name)

    def test_fastcode_mismatch_means_the_1_1_6_path(self):
        self.shap.stop()
        inst = self.installer(); c = self.complete_cache(inst)
        spec = json.loads((install.KIT / 'patches' / 'fastcode' / 'fastcode.json').read_text(encoding='utf-8'))
        f = next(iter(spec['final']))
        # the recorded tree hash still matches (recorded after the change): only the FastCode check can see it
        self.assertNotEqual(install.sha256(inst.work / install.GEN_REL / f), spec['final'][f])
        self.assertEqual(inst.reuse_mode(inst.fingerprint()), (None, 'the kept FastCode files do not match'))

    def patch_cache(self, inst, **kw):
        c = install.read_cache(inst.work); c.update(kw); install.write_cache(inst.work, c)

    def patch_cache_fp(self, inst, sub, key, value):
        c = install.read_cache(inst.work)
        (c['fingerprint'][sub] if sub else c['fingerprint'])[key] = value
        install.write_cache(inst.work, c)


class Plan(Base):
    def fastcode_ok(self, inst):
        import contextlib
        return contextlib.nullcontext()          # (Base already makes the kept FastCode files match)

    def test_fast_marks_the_translation_kept_and_the_cache_unfinished(self):
        inst = self.installer(); self.complete_cache(inst); modes = []; inst.on_reuse = modes.append
        with self.fastcode_ok(inst):
            self.assertEqual(inst.plan_reuse(), 'fast')
        for s in install.REUSED_STEPS: self.assertIn(s, inst.state['done'])
        self.assertEqual(inst.state['reuse_root'], str(inst.work)); self.assertEqual(modes, ['fast'])
        self.assertFalse(install.read_cache(inst.work)['complete'])
        self.assertTrue((inst.work / install.GEN_REL / 'recomp_extra.c').is_file())

    def test_none_drops_the_kept_build_and_runs_everything(self):
        inst = self.installer(); self.complete_cache(inst); (inst.work / 'src').mkdir(); (inst.work / 'build.cmd').write_text('x')
        (inst.work / 'deps' / install.FFMPEG).mkdir(parents=True)
        inst.clean_build = True
        self.assertIsNone(inst.plan_reuse())
        self.assertFalse((inst.work / 'toolkit').exists()); self.assertFalse((inst.work / 'build').exists()); self.assertFalse((inst.work / 'src').exists())
        self.assertTrue((inst.work / 'deps' / install.FFMPEG).is_dir())         # the FFmpeg download stays (checked file by file anyway)
        for s in install.REUSED_STEPS: self.assertNotIn(s, inst.state['done'])
        self.assertTrue(any('not used, everything is made again (a rebuild from scratch was asked for)' in l for l in self.lines), self.lines)

    def test_rebuild_keeps_the_translation_but_builds_clean(self):
        inst = self.installer(); self.complete_cache(inst); Decision.patch_cache_fp(None, inst, None, 'compiler', {'vs': 'old'})
        with self.fastcode_ok(inst):
            self.assertEqual(inst.plan_reuse(), 'rebuild')
        self.assertIn('patch', inst.state['done']); self.assertNotIn('reuse_preseed', inst.state)

    def test_a_resume_keeps_the_first_decision(self):
        inst = self.installer(); inst.state['reuse'] = 'fast'; inst.resumed = True
        self.assertEqual(inst.plan_reuse(), 'fast')
        inst2 = self.installer('g2'); inst2.resumed = True; self.complete_cache(inst2)
        self.assertIsNone(inst2.plan_reuse()); self.assertTrue((inst2.work / install.GEN_REL).is_dir())    # (nothing deleted on a Resume)

    def test_fault_fingerprint_or_decision_throwing_is_the_1_1_6_path(self):
        for what in ('fingerprint', 'reuse_mode', 'read_cache', 'write_cache', 'tree_hash'):
            with self.subTest(what):
                inst = self.installer(what); self.complete_cache(inst)
                target = (install, what) if what in ('read_cache', 'write_cache', 'tree_hash') else (install.Installer, what)
                with mock.patch.object(*target, boom):
                    self.assertIsNone(inst.plan_reuse())
                for s in install.REUSED_STEPS: self.assertNotIn(s, inst.state['done'])
                self.assertIsNone(inst.state['reuse'])

    def test_fault_plan_reuse_itself_throwing_in_check(self):
        inst = self.installer(); self.complete_cache(inst)
        with mock.patch.object(install.Installer, 'plan_reuse', boom), mock.patch.object(install.precheck, 'smart_app_control', lambda: 'off'):
            inst.state['done'] = []; inst.game_ok = True; inst.check()
        self.assertIsNone(inst.state['reuse']); self.assertFalse(set(install.REUSED_STEPS) & set(inst.state['done']))
        self.assertTrue(any('(the kept build is not used: injected fault)' in l for l in self.lines), self.lines)

    def test_fault_the_window_hook(self):
        inst = self.installer(); inst.on_reuse = boom
        self.assertIsNone(inst.plan_reuse())


# ---------------------------------------------------------------------------- the build step

GEN = ['recomp_0000.c', 'recomp_0001.c']


class Build(Base):
    def setUp(self):
        super().setUp()
        self.kit = self.tmp / 'kit'
        for top in ('source/runtime', 'source/game'):
            (self.kit / top / 'src').mkdir(parents=True)
            (self.kit / top / 'src' / 'a.c').write_text('int a;'); (self.kit / top / 'CMakeLists.txt').write_text('# x')
        (self.kit / 'source/game/bringup').mkdir(); (self.kit / 'source/game/bringup/gen.py').write_text('#')
        self.ff = self.tmp / 'ffmpeg'; (self.ff / 'bin').mkdir(parents=True); (self.ff / 'bin' / 'avcodec-63.dll').write_text('dll')
        self.p = mock.patch.object(install, 'KIT', self.kit); self.p.start()
        self.calls = []

    def tearDown(self):
        super().tearDown()

    def inst(self, outcomes):
        inst = self.installer()
        inst.project = lambda: inst.work / 'toolkit' / install.PROJECT
        gen = inst.project() / 'src' / 'recomp' / 'gen'; gen.mkdir(parents=True, exist_ok=True)
        for n in GEN: (gen / n).write_text('int %s;' % n[:-2])
        inst._ffmpeg = self.ff; inst.jobs = 4
        def run_cmd(args, cwd=None, env=None, **k):
            b = Path(cwd) / 'build'; b.mkdir(exist_ok=True)
            self.calls.append({'objs': sorted(p.name for p in b.glob('*.obj')), 'src': sorted(p.relative_to(Path(cwd) / 'src').as_posix() for p in (Path(cwd) / 'src').rglob('*') if p.is_file())})
            out = outcomes.pop(0)
            if isinstance(out, BaseException): raise out
            (b / 'build.ninja').write_text(''); (b / 'old.obj').write_bytes(b'')
            (b / 'mechassault2_recomp.exe').write_bytes(b'built')
        inst.run_cmd = run_cmd
        return inst

    def run_build(self, inst):
        with mock.patch.object(install.precheck, 'memory_info', lambda: (64 * G, 40 * G)), \
             mock.patch.object(install, 'reboot_pending', lambda: {'windows': False, 'vs': False}):
            inst.build_step()

    def test_clean_build_then_quick_rebuild_keeps_objects_and_writes_only_changes(self):
        inst = self.inst(['ok' if False else None])
        self.run_build(inst)                                             # (no reuse: the 1.1.6 clean build)
        self.assertEqual(self.calls[0]['objs'], [])
        self.assertIn('game/src/recomp/gen/recomp_0000.c', self.calls[0]['src']); self.assertNotIn('game/bringup/gen.py', self.calls[0]['src'])
        self.assertIn('game/vendor/%s/bin/avcodec-63.dll' % install.FFMPEG, self.calls[0]['src'])
        src = inst.work / 'src'
        old = time.time() - 3600
        for p in src.rglob('*'):
            if p.is_file(): os.utime(p, (old, old))
        (self.kit / 'source/runtime/src/a.c').write_text('int a2;')                  # a one-line runtime change
        (self.kit / 'source/game/src/b.c').write_text('int b;')                      # a new file
        (self.kit / 'source/game/CMakeLists.txt').unlink()                             # a removed file
        inst2 = self.inst([None]); inst2.state.update(reuse='fast', reuse_root=str(inst2.work), reuse_preseed={}, preseed={})
        self.run_build(inst2)
        self.assertEqual(self.calls[1]['objs'], ['old.obj'])                          # the kept objects: Ninja recompiles only what changed
        self.assertTrue(any('source: 2 file(s) changed (game/src/b.c, runtime/src/a.c), 1 removed' in l for l in self.lines), self.lines)
        self.assertAlmostEqual((src / 'game/src/a.c').stat().st_mtime, old, delta=1)
        self.assertGreater((src / 'runtime/src/a.c').stat().st_mtime, old + 100)
        self.assertFalse((src / 'game/CMakeLists.txt').exists())
        self.assertTrue(inst2.state['fast_tried'])

    def test_quick_rebuild_failure_builds_again_from_the_start(self):
        for fail in (install.Stop('CMake + Ninja failed', 'compile_error'), RuntimeError('anything'), install.Stop('x', 'out_of_memory')):
            with self.subTest(repr(fail)):
                self.calls = []; self.lines.clear()
                shutil.rmtree(self.tmp / 'game', ignore_errors=True)
                inst = self.inst([fail, None]); inst.state.update(reuse='fast', reuse_root=str(inst.work))
                (inst.work / 'build').mkdir(parents=True); (inst.work / 'build' / 'build.ninja').write_text(''); (inst.work / 'build' / 'kept.obj').write_bytes(b'')
                status = []; inst.on_status = status.append
                self.run_build(inst)
                self.assertEqual(len(self.calls), 2); self.assertEqual(self.calls[0]['objs'], ['kept.obj']); self.assertEqual(self.calls[1]['objs'], [])
                self.assertTrue(any(l.startswith('  RECOVERY: the quick rebuild did not work') for l in self.lines), self.lines)
                self.assertEqual(status, ['taking longer: building again from the start'])

    def test_cancel_during_the_quick_rebuild_is_not_retried(self):
        inst = self.inst([install.Stop('Cancelled.', 'cancelled')]); inst.state.update(reuse='fast', reuse_root=str(inst.work))
        (inst.work / 'build').mkdir(); (inst.work / 'build' / 'build.ninja').write_text('')
        with self.assertRaises(install.Stop) as cm: self.run_build(inst)
        self.assertEqual(cm.exception.kind, 'cancelled'); self.assertEqual(len(self.calls), 1)

    def test_a_resume_after_a_stopped_quick_rebuild_builds_clean(self):
        inst = self.inst([None]); inst.state.update(reuse='fast', reuse_root=str(inst.work), fast_tried=True)
        (inst.work / 'build').mkdir(); (inst.work / 'build' / 'build.ninja').write_text(''); (inst.work / 'build' / 'kept.obj').write_bytes(b'')
        self.run_build(inst)
        self.assertEqual(self.calls[0]['objs'], [])

    def test_another_build_folder_or_no_build_ninja_is_not_reused(self):
        for setup in ('root', 'ninja'):
            self.calls = []; shutil.rmtree(self.tmp / 'game', ignore_errors=True)
            inst = self.inst([None]); inst.state.update(reuse='fast', reuse_root=str(inst.work) if setup == 'ninja' else 'C:/other')
            (inst.work / 'build').mkdir(); (inst.work / 'build' / 'kept.obj').write_bytes(b'')
            if setup == 'root': (inst.work / 'build' / 'build.ninja').write_text('')
            self.run_build(inst)
            self.assertEqual(len(self.calls), 1); self.assertEqual(self.calls[0]['objs'], [], setup)

    def test_fault_sync_throwing_builds_clean(self):
        inst = self.inst([None]); inst.state.update(reuse='fast', reuse_root=str(inst.work))
        (inst.work / 'build').mkdir(); (inst.work / 'build' / 'build.ninja').write_text(''); (inst.work / 'build' / 'kept.obj').write_bytes(b'')
        with mock.patch.object(install, 'sync_tree', boom):
            self.run_build(inst)
        self.assertEqual(len(self.calls), 1); self.assertEqual(self.calls[0]['objs'], [])

    def test_preseed_is_recorded_and_reused_unchanged(self):
        inst = self.inst([None])
        with mock.patch.object(install.precheck, 'installed_memory', lambda: 16 * G), \
             mock.patch.object(install.precheck, 'memory_info', lambda: (16 * G, 10 * G)), \
             mock.patch.object(install, 'reboot_pending', lambda: {'windows': False, 'vs': False}):
            inst.build_step()
        self.assertEqual(inst.state['preseed']['kind'], 'heavy')
        inst2 = self.inst([None]); inst2.fp = {'lowmem': 'heavy'}
        inst2.state.update(reuse='fast', reuse_root=str(inst2.work), reuse_preseed={'src/recomp/gen/recomp_0076.c': ['/cgthreads1']})
        self.run_build(inst2)
        self.assertEqual(inst2.preseed, {'src/recomp/gen/recomp_0076.c': ['/cgthreads1']})
        self.assertIn('recomp_0076.c', (inst2.work / install.LOWMEM_FILE).read_text())


class RealNinja(Base):
    """With this PC's real CMake + Ninja + MSVC (skipped without Build Tools): after a sync that changed one of three files, Ninja
    compiles exactly that one file again (proof that unchanged files' times are kept and changed ones look newer)."""
    def test_only_the_changed_file_is_recompiled(self):
        try:
            vcvars, cmake, ninja, vs = install.find_visual_studio(None)
        except Exception as e:
            self.skipTest('no Build Tools: %s' % e)
        kit = self.tmp / 'kitsrc'; (kit / 'src').mkdir(parents=True)
        for n in ('a.c', 'b.c', 'c.c'): (kit / 'src' / n).write_text('int %s_(void){return 1;}\n' % n[0], encoding='utf-8')
        (kit / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(t C)\nfile(GLOB S CONFIGURE_DEPENDS src/*.c)\n'
                                            'add_library(t STATIC ${S})\n', encoding='utf-8')
        src, b = self.tmp / 'src', self.tmp / 'b'
        def build():
            install.sync_tree(install.tree_files(kit), src)
            bat = self.tmp / 'b.cmd'
            bat.write_text('@echo off\r\ncall "%s" >nul || exit /b 1\r\n"%s" -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%s" >nul || exit /b 1\r\n'
                           '"%s" -C "%s" || exit /b 1\r\n' % (vcvars, cmake, src, b, Path(ninja).as_posix(), ninja, b), encoding='utf-8')
            r = subprocess.run(['cmd.exe', '/d', '/c', str(bat)], capture_output=True, text=True, creationflags=install.NO_CONSOLE)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return re.findall(r'Building C object \S*?([abc])\.c\.obj', r.stdout)
        self.assertEqual(sorted(build()), ['a', 'b', 'c'])
        time.sleep(1.1)
        self.assertEqual(build(), [])                                            # nothing changed: nothing compiled
        (kit / 'src' / 'b.c').write_text('int b_(void){return 2;}\n', encoding='utf-8')
        self.assertEqual(build(), ['b'])                                         # one change: exactly that file


# ---------------------------------------------------------------------------- finish: keep the trimmed work folder

class Keep(Base):
    def populate(self, inst):
        self.complete_cache(inst)
        for rel in ('toolkit/tools/x.py', 'toolkit/MechAssault2/game_files/ma2_analysis.json', 'src/game/a.c', 'build/x.obj', 'build.cmd',
                    'install.log', 'enhancer/big.bin', 'deps/%s/bin/x.dll' % install.FFMPEG, 'deps/realesrgan/x.exe', 'state.finished.json'):
            (inst.work / rel).parent.mkdir(parents=True, exist_ok=True); (inst.work / rel).write_text('x')
        inst.logfile = open(inst.work / 'install.log', 'a')

    def test_keeps_only_what_a_rebuild_needs(self):
        inst = self.installer(); self.populate(inst)
        inst.finish()
        have = sorted(p.relative_to(inst.work).as_posix() for p in inst.work.rglob('*') if p.is_file())
        self.assertIn('cache.json', have); self.assertIn('build/x.obj', have); self.assertIn('src/game/a.c', have); self.assertIn('build.cmd', have)
        self.assertIn('deps/%s/bin/x.dll' % install.FFMPEG, have); self.assertIn(install.GEN_REL + '/recomp_0000.c', have)
        for gone in ('install.log', 'enhancer/big.bin', 'toolkit/tools/x.py', 'toolkit/MechAssault2/game_files/ma2_analysis.json', 'deps/realesrgan/x.exe'):
            self.assertNotIn(gone, have)
        self.assertTrue(any(l.startswith('  kept for the next update: ') for l in self.lines), self.lines)
        self.assertIsNone(precheck.read_state(inst.dest))

    def test_low_disk_no_keep_switch_or_unfinished_record_remove_it_as_1_1_6(self):
        for case in ('disk', 'flag', 'env', 'incomplete', 'nocache', 'fault'):
            with self.subTest(case):
                env = {'MA2_KEEP_BUILD': '0'} if case == 'env' else {}
                with mock.patch.dict(os.environ, env):
                    inst = self.installer(case, keep_build=(case != 'flag'))
                self.populate(inst)
                if case == 'incomplete': install.write_cache(inst.work, {'complete': False})
                if case == 'nocache': (inst.work / 'cache.json').unlink()
                du = shutil.disk_usage
                fake = (lambda p: du(p)._replace(free=int(9.9 * G))) if case == 'disk' else du
                with mock.patch.object(install.shutil, 'disk_usage', fake), \
                     mock.patch.object(install.Installer, 'keep_kept_build', boom) if case == 'fault' else mock.patch.object(install, 'KIT', install.KIT):
                    inst.finish()
                self.assertFalse(inst.work.exists(), case)

    def test_fault_record_cache_or_check_manifest_replaced_by_a_throwing_method(self):
        inst = self.installer(); self.populate(inst); install.write_cache(inst.work, {'complete': False})
        inst.state['done'] = ['build']; inst.fp = {'x': 1}
        with mock.patch.object(install.Installer, 'record_cache', boom):
            inst.finish()
        self.assertFalse(inst.work.exists())
        self.assertTrue(any('(the build record was not written: injected fault)' in l for l in self.lines), self.lines)
        inst2 = self.installer('g2')
        with mock.patch.object(install.Installer, 'check_manifest', boom), mock.patch.object(install.Installer, 'plan_reuse', lambda self: None),              mock.patch.object(install.precheck, 'smart_app_control', lambda: 'off'):
            inst2.game_ok = True; inst2.check()
        self.assertTrue(any('(the installed files were not checked: injected fault)' in l for l in self.lines))

    def test_a_plain_build_folder_is_kept_and_trimmed_and_uninstall_removes_it(self):
        inst = self.installer('Game'); self.populate(inst)
        root = self.tmp / ('%sabcd1234' % install.BUILD_PREFIX); (root / 'build').mkdir(parents=True); (root / 'build' / 'x.obj').write_text('o')
        (root / 'junk.txt').write_text('j'); (root / 'owner.txt').write_text(str(inst.dest))
        install.write_cache(inst.work, dict(install.read_cache(inst.work), root=str(root)))
        with mock.patch.object(inst, 'build_root', lambda: root):
            inst.finish()
        self.assertTrue((root / 'build' / 'x.obj').is_file()); self.assertFalse((root / 'junk.txt').exists())
        self.assertFalse((inst.work / 'build').exists())                     # (the work folder keeps no build of its own then)
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (inst.dest / f).parent.mkdir(parents=True, exist_ok=True); (inst.dest / f).write_text('x')
        plan = install.uninstall_plan(inst.dest)
        self.assertEqual(plan['build'], root)
        res = install.uninstall(inst.dest, log=lambda s: None)
        self.assertFalse(root.exists()); self.assertFalse(inst.dest.exists()); self.assertEqual(res['failed'], [])

    def test_uninstall_leaves_a_build_folder_that_is_not_this_installs(self):
        d = self.tmp / 'g'
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text('x')
        root = self.tmp / ('%sffff0000' % install.BUILD_PREFIX); root.mkdir(); (root / 'owner.txt').write_text(str(self.tmp / 'other'))
        install.write_cache(d / '_work', {'complete': True, 'root': str(root)})
        self.assertIsNone(install.uninstall_plan(d)['build'])
        install.uninstall(d, log=lambda s: None); self.assertTrue(root.exists())


# ---------------------------------------------------------------------------- manifest, copy only what differs, Repair

class Manifest(Base):
    def install_tree(self, d, kit='k1'):
        for f, t in (('MechAssault 2.exe', 'exe'), ('avcodec-63.dll', 'dll'), ('MA2 Report.exe', 'rep'), ('README.txt', 'r'),
                     ('Docs/Troubleshooting.txt', 't'), ('Licenses/x.txt', 'l'), ('pc_assets/common.mgf', 'm'), ('pc_settings.ini', 'ini'),
                     ('game_files/ma2.xbe', 'xbe'), ('notes.txt', 'mine')):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text(t)
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': kit}))

    def test_manifest_lists_installer_files_only_and_verify_finds_changes(self):
        d = self.tmp / 'g'; self.install_tree(d)
        m = install.write_manifest(d, 'k1')
        self.assertEqual(sorted(m['files']), ['Docs/Troubleshooting.txt', 'Licenses/x.txt', 'MA2 Report.exe', 'MechAssault 2.exe', 'README.txt',
                                               'avcodec-63.dll', 'pc_assets/common.mgf', 'pc_settings.ini'])
        self.assertEqual(m['files']['README.txt'], [1, install.sha256(d / 'README.txt')])
        self.assertEqual(install.verify_manifest(d), {'changed': [], 'missing': []})
        (d / 'avcodec-63.dll').write_text('DLL'); (d / 'Docs/Troubleshooting.txt').unlink(); (d / 'pc_settings.ini').write_text('mine')
        self.assertEqual(install.verify_manifest(d), {'changed': ['avcodec-63.dll'], 'missing': ['Docs/Troubleshooting.txt']})
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': 'other'}))
        self.assertIsNone(install.verify_manifest(d))                           # a manifest of another version is not used

    def test_place_copies_only_what_differs_and_keeps_previous_only_for_those(self):
        inst = self.installer(); inst.state = {'update': True, 'done': []}
        a = self.tmp / 'new'; a.mkdir()
        (a / 'same.dll').write_text('same'); (a / 'diff.dll').write_text('new')
        (inst.dest / 'same.dll').write_text('same'); (inst.dest / 'diff.dll').write_text('old')
        self.assertFalse(inst.place(a / 'same.dll', inst.dest / 'same.dll')); self.assertTrue(inst.place(a / 'diff.dll', inst.dest / 'diff.dll'))
        self.assertTrue(inst.place(a / 'diff.dll', inst.dest / 'missing.dll'))
        self.assertEqual([Path(b).name for _, b in inst.state['previous']], ['diff.dll.previous'])
        self.assertEqual((inst.dest / 'diff.dll').read_text(), 'new')
        with mock.patch.object(install, 'same_bytes', boom):                   # fault: copied as 1.1.6 does
            self.assertTrue(inst.place(a / 'same.dll', inst.dest / 'same.dll'))

    def test_repair_check_logs_and_notes(self):
        inst = self.installer(); self.install_tree(inst.dest, install.KIT_ID); install.write_manifest(inst.dest, install.KIT_ID)
        (inst.dest / 'MechAssault 2.exe').write_text('damaged')
        inst.check_manifest()
        self.assertEqual(inst.repair_found, ['MechAssault 2.exe'])
        self.assertTrue(any('installed files that changed or are missing (1): MechAssault 2.exe' in l for l in self.lines))
        with mock.patch.object(install, 'verify_manifest', boom):
            inst2 = self.installer('g2'); inst2.check_manifest()
        self.assertIsNone(inst2.repair_found)


# ---------------------------------------------------------------------------- the earlier version: _previous, good launch, rollback

class Previous(Base):
    def updated(self):
        """An update just finished: the new files in place, the replaced ones recorded as .previous."""
        inst = self.installer(); d = inst.dest
        for f, t in (('MechAssault 2.exe', 'old-exe'), ('avcodec-63.dll', 'old-dll'), ('pc_assets/common.mgf', 'old-mgf'), ('game_files/ma2.xbe', 'x')):
            (d / f).parent.mkdir(parents=True, exist_ok=True); (d / f).write_text(t)
        inst.old_stamp = {'kit_id': 'oldkit', 'public_version': '1.1.6', 'version': '2026.11.21'}
        (d / precheck.STAMP).write_text(json.dumps(inst.old_stamp))
        inst.state = {'update': True, 'done': []}
        for f in ('MechAssault 2.exe', 'avcodec-63.dll', 'pc_assets'):
            inst.keep_previous(d / f)
        (d / 'MechAssault 2.exe').write_text('new-exe'); (d / 'avcodec-63.dll').write_text('new-dll')
        (d / 'pc_assets').mkdir(); (d / 'pc_assets' / 'common.mgf').write_text('new-mgf')
        (d / precheck.STAMP).write_text(json.dumps({'kit_id': install.KIT_ID, 'public_version': install.PUBLIC_VERSION}))
        return inst, d

    def commit(self, inst):
        """settle() + finish(): the replaced files go to _previous, the state file is renamed (a finished install)."""
        inst.commit_previous()
        (inst.work / 'state.json').replace(inst.work / 'state.finished.json')

    def test_update_keeps_the_replaced_files_in_previous(self):
        inst, d = self.updated()
        inst.commit_previous()
        self.assertFalse(list(d.glob('*.previous')))
        info = install.read_previous(d)
        self.assertEqual(sorted(info['files']), ['MechAssault 2.exe', 'avcodec-63.dll', 'pc_assets'])
        self.assertEqual(info['version'], '1.1.6'); self.assertEqual(info['new_kit'], install.KIT_ID); self.assertEqual(info['stamp']['kit_id'], 'oldkit')
        self.assertEqual((d / '_previous/files/MechAssault 2.exe').read_text(), 'old-exe')
        self.assertEqual(inst.state['previous'], [])

    def test_fault_keeping_falls_back_to_deleting_as_1_1_6(self):
        inst, d = self.updated()
        with mock.patch.object(install.Installer, 'keep_previous_version', boom):
            inst.commit_previous()
        self.assertFalse(list(d.glob('*.previous'))); self.assertFalse((d / '_previous').exists())

    def test_good_launch_deletes_previous_at_the_next_run(self):
        inst, d = self.updated(); self.commit(inst)
        logs = precheck.logs_dir(); logs.mkdir(parents=True, exist_ok=True)
        for f in logs.glob('*'): f.unlink()
        inst.old_stamp = precheck.read_stamp(d)
        start = time.localtime(time.time() - 30)
        name = time.strftime('%Y%m%d-%H%M%S', start) + '-1234.log'
        line = '[LAUNCH] build version=%s exe_version=%s install=x kit=%s exe_bytes=1 exe_time=x\n' % (install.PUBLIC_VERSION, install.PUBLIC_VERSION, install.KIT_ID)
        (logs / name).write_text(line)                                            # 30 s, no normal end: not yet
        self.assertFalse(inst.prune_previous()); self.assertTrue((d / '_previous').exists())
        (logs / name).write_text(line + '[CRASH] x\n[EXIT] phase=terminate source=x\n')        # a crash is never a good launch
        self.assertFalse(inst.prune_previous())
        (logs / name).write_text(line.replace(install.KIT_ID, 'otherkit') + '[EXIT] phase=terminate source=x\n')   # another version
        self.assertFalse(inst.prune_previous())
        (logs / name).write_text(line + '[EXIT] phase=terminate source=fb_wndproc reason=WM_CLOSE\n')
        self.assertTrue(inst.prune_previous()); self.assertFalse((d / '_previous').exists())

    def test_sixty_seconds_count_as_a_good_launch(self):
        logs = self.tmp / 'logs'; logs.mkdir()
        start = time.time() - 200
        f = logs / (time.strftime('%Y%m%d-%H%M%S', time.localtime(start)) + '-1.log')
        f.write_text('[LAUNCH] build version=1 exe_version=1 install=x kit=K exe_bytes=1 exe_time=x\n')
        os.utime(f, (start + 61, start + 61))
        self.assertTrue(install._launch_ok(f, 'K', 0)); self.assertFalse(install._launch_ok(f, 'K', start + 100))
        os.utime(f, (start + 59, start + 59)); self.assertFalse(install._launch_ok(f, 'K', 0))

    def test_rollback_puts_the_earlier_version_back(self):
        inst, d = self.updated(); self.commit(inst); install.write_manifest(d)
        shown = install.rollback(d, log=self.lines.append)
        self.assertEqual(shown, '1.1.6')
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'old-exe'); self.assertEqual((d / 'avcodec-63.dll').read_text(), 'old-dll')
        self.assertEqual((d / 'pc_assets/common.mgf').read_text(), 'old-mgf')
        self.assertEqual(precheck.read_stamp(d)['kit_id'], 'oldkit'); self.assertFalse((d / install.MANIFEST).exists())
        self.assertFalse((d / '_previous').exists()); self.assertFalse(list(d.rglob('*.rollback-new')))
        self.assertEqual(install.install_relation(d), 'older')                    # the window offers the update again
        with self.assertRaises(install.Stop): install.rollback(d, log=self.lines.append)     # nothing kept any more

    def test_rollback_is_all_or_nothing(self):
        inst, d = self.updated(); self.commit(inst)
        real = Path.rename; n = [0]
        def flaky(self_, target):
            n[0] += 1
            if n[0] == 4: raise PermissionError(5, 'Access is denied')
            return real(self_, target)
        with mock.patch.object(Path, 'rename', flaky), mock.patch.object(install.time, 'sleep', lambda s: None), \
             mock.patch.object(install, 'retry_io', lambda fn, *a, **k: fn()), self.assertRaises(install.Stop) as cm:
            install.rollback(d, log=self.lines.append)
        self.assertIn('Nothing was changed', str(cm.exception))
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'new-exe'); self.assertEqual((d / 'avcodec-63.dll').read_text(), 'new-dll')
        self.assertEqual(precheck.read_stamp(d)['kit_id'], install.KIT_ID); self.assertTrue(install.read_previous(d))

    def test_rollback_refuses_an_unfinished_install(self):
        inst, d = self.updated(); inst.commit_previous(); inst.save()
        with self.assertRaises(install.Stop) as cm: install.rollback(d)
        self.assertIn('not finished', str(cm.exception))

    def test_a_later_update_replaces_previous_and_a_repair_keeps_it(self):
        inst, d = self.updated(); inst.commit_previous()
        inst.state = {'update': True, 'done': []}; inst.commit_previous()      # a Repair that replaced nothing: kept
        self.assertTrue(install.read_previous(d))
        info = install.read_previous(d); info['new_kit'] = 'olderkit'; (d / '_previous/previous.json').write_text(json.dumps(info))
        inst.commit_previous()                                                  # an update to another version that replaced nothing
        self.assertFalse((d / '_previous').exists())

    def test_cli_rollback(self):
        inst, d = self.updated(); self.commit(inst)
        r = subprocess.run([sys.executable, '-B', str(REAL_KIT / 'installer' / 'install.py'), '--rollback', '--dest', str(d)],
                           capture_output=True, text=True, env=dict(os.environ), creationflags=install.NO_CONSOLE)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertIn('Went back to 1.1.6', r.stdout)
        r = subprocess.run([sys.executable, '-B', str(REAL_KIT / 'installer' / 'install.py'), '--rollback', '--dest', str(d)],
                           capture_output=True, text=True, env=dict(os.environ), creationflags=install.NO_CONSOLE)
        self.assertEqual(r.returncode, 1); self.assertIn('ROLLBACK STOPPED: There is no earlier version kept', r.stderr)


# ---------------------------------------------------------------------------- a whole update twice (Installer.run, the steps faked)

class Runs(Base):
    """Installer.run() on an update, three times: the first update has no kept build (everything again, as 1.1.6), the second with a
    one-line source change takes the quick path, the third after a patch change takes the 1.1.6 path again. The translation and the
    compiler are faked; the source sync, cache, keep, manifest and _previous are the real code."""
    def setUp(self):
        super().setUp()
        if not XBE: self.skipTest('set MA2_TEST_XBE to your own ma2.xbe')
        self.kit = self.tmp / 'kit'
        shutil.copytree(REAL_KIT, self.kit, ignore=shutil.ignore_patterns('__pycache__', 'vendor', 'editor', 'report', '*.pack'))
        self.ff = self.tmp / 'ffmpeg'; (self.ff / 'bin').mkdir(parents=True)
        for n in install.FFMPEG_DLLS: (self.ff / 'bin' / n).write_text('dll ' + n)
        (self.ff / 'README.txt').write_text('ff')
        self.d = self.tmp / 'Game'
        for f, t in (('MechAssault 2.exe', 'old-exe'), ('avcodec-63.dll', 'old-dll'), ('pc_assets/common.mgf', 'old-assets'), ('pc_settings.ini', '[Display]\n')):
            (self.d / f).parent.mkdir(parents=True, exist_ok=True); (self.d / f).write_text(t)
        (self.d / 'game_files').mkdir(); shutil.copy2(XBE, self.d / 'game_files' / 'ma2.xbe')
        (self.d / precheck.STAMP).write_text(json.dumps({'kit_id': 'oldkit', 'version': '2026.11.21', 'public_version': '1.1.6'}))
        self.vs = self.fake_vs()
        self.ran = []

    def update(self, **kw):
        install._TREE_MEMO.clear()
        inst = install.Installer(None, self.d, update=True, log=self.lines.append, **kw)
        test = self
        def step(name):
            def f(*a):
                test.ran.append(name)
                if name == 'patch':
                    gen = inst.project() / 'src' / 'recomp' / 'gen'; gen.mkdir(parents=True, exist_ok=True)
                    (gen / 'recomp_0000.c').write_text('int g;')
                    spec = json.loads((install.KIT / 'patches' / 'fastcode' / 'fastcode.json').read_text(encoding='utf-8'))
                    for x in spec['final']: (gen / x).write_text('fast ' + x)
                if name == 'assets':
                    inst.keep_previous(inst.dest / 'pc_assets'); (inst.dest / 'pc_assets').mkdir(); (inst.dest / 'pc_assets' / 'common.mgf').write_text('assets')
            return f
        for n in ('analyze', 'recompile', 'generate', 'patch', 'assets'): setattr(inst, n, step(n))
        inst.verify = step('verify')
        def run_cmd(args, cwd=None, env=None, **k):
            test.ran.append('compile')
            b = Path(cwd) / 'build'; b.mkdir(exist_ok=True); (b / 'build.ninja').write_text('')
            src = Path(cwd) / 'src'
            (b / 'mechassault2_recomp.exe').write_text('exe ' + install.tree_hash(src / 'runtime', memo=False)[:12])
        inst.run_cmd = run_cmd
        inst._ffmpeg = self.ff
        return inst

    def go(self, inst):
        spec = json.loads((install.KIT / 'patches' / 'fastcode' / 'fastcode.json').read_text(encoding='utf-8'))
        real = install.sha256
        fake_sha = lambda p: spec['final'].get(Path(p).name, real(p)) if Path(p).parent.name == 'gen' else real(p)
        with mock.patch.object(install, 'KIT', self.kit), \
             mock.patch.object(install, 'find_visual_studio', lambda choose=None: (self.vs / 'vcvars64.bat', self.vs / 'cmake.exe', self.vs / 'ninja.exe', self.vs)), \
             mock.patch.object(install, 'sha256', fake_sha), \
             mock.patch.object(install.precheck, 'smart_app_control', lambda: 'off'), \
             mock.patch.object(install, 'reboot_pending', lambda: {'windows': False, 'vs': False}), \
             mock.patch.object(install, 'clear_resume_after_restart', lambda: False):
            inst.run()

    def test_first_update_slow_second_quick_then_patch_change_slow(self):
        inst = self.update(); self.go(inst)
        self.assertEqual([r for r in self.ran if r in ('analyze', 'patch', 'compile')], ['analyze', 'patch', 'compile'])
        c = install.read_cache(self.d / '_work'); self.assertTrue(c['complete']); self.assertIsNone(c['reuse'])
        self.assertTrue((self.d / '_work' / install.GEN_REL / 'recomp_0000.c').is_file())
        self.assertTrue((self.d / '_work' / 'src' / 'runtime').is_dir()); self.assertFalse((self.d / '_work' / 'install.log').exists())
        self.assertEqual(install.read_previous(self.d)['version'], '1.1.6')
        self.assertEqual(install.verify_manifest(self.d), {'changed': [], 'missing': []})
        exe1 = (self.d / 'MechAssault 2.exe').read_text()
        # 2: a one-line runtime change -> quick path
        self.ran.clear(); self.lines.clear()
        f = self.kit / 'source' / 'runtime' / 'CMakeLists.txt'; f.write_text(f.read_text(encoding='utf-8') + '\n# one line\n', encoding='utf-8')
        inst = self.update(); self.go(inst)
        self.assertEqual(self.ran, ['verify', 'compile', 'assets'], self.ran)
        self.assertTrue(any('source: 1 file(s) changed (runtime/CMakeLists.txt)' in l for l in self.lines), self.lines)
        self.assertEqual(install.read_cache(self.d / '_work')['reuse'], 'fast')
        self.assertNotEqual((self.d / 'MechAssault 2.exe').read_text(), exe1)
        self.assertEqual(sorted(install.read_previous(self.d)['files']), ['MechAssault 2.exe', 'pc_assets'])     # the DLLs were the same: not replaced
        # 3: nothing changed (a Repair): quick, the exe stays as it is
        self.ran.clear(); inst = self.update(); self.go(inst)
        self.assertEqual(self.ran, ['verify', 'compile', 'assets'])
        # 4: a patch change -> everything again
        self.ran.clear()
        (self.kit / 'patches' / 'manifest.json').write_text((self.kit / 'patches' / 'manifest.json').read_text(encoding='utf-8') + ' ', encoding='utf-8')
        inst = self.update(); self.go(inst)
        self.assertEqual([r for r in self.ran if r in ('analyze', 'patch', 'compile')], ['analyze', 'patch', 'compile'])
        self.assertTrue(any('kept build: not used, everything is made again (changed: patches)' in l for l in self.lines))
        # 5: --clean-build: everything again
        self.ran.clear(); inst = self.update(clean_build=True); self.go(inst)
        self.assertIn('analyze', self.ran)

    def test_killed_quick_rebuild_resumes_with_a_clean_build(self):
        self.go(self.update())
        self.ran.clear()
        inst = self.update()
        def killed(args, cwd=None, env=None, **k):
            raise KeyboardInterrupt                                            # (the process ends: no stop handling runs)
        inst.run_cmd = killed
        with mock.patch.object(install.Installer, 'clear_in_progress', lambda self: None), self.assertRaises(KeyboardInterrupt):
            self.go(inst)                                                      # (a killed process clears nothing)
        st = precheck.read_state(self.d); self.assertTrue(st['fast_tried']); self.assertEqual(st['in_progress'], 'build')
        self.assertFalse(install.read_cache(self.d / '_work')['complete'])
        self.ran.clear(); self.lines.clear()
        inst = self.update(); self.go(inst)                                     # the Resume
        self.assertEqual(self.ran, ['compile', 'assets'])
        self.assertTrue(any('copying the source tree' in l for l in self.lines))   # the clean build (1.1.6)
        self.assertTrue(install.read_cache(self.d / '_work')['complete'])


# ---------------------------------------------------------------------------- the window's estimate

class Window(Base):
    def test_progress_model_quick_build(self):
        m = progress.ProgressModel(['check', 'verify', 'analyze', 'recompile', 'generate', 'patch', 'build', 'assets', 'install', 'finish'])
        m.set_quick(True); self.assertEqual(m.seconds['build'], progress.QUICK_BUILD_SECONDS)
        self.assertEqual(progress.STEP_SECONDS['build'], 540)                  # (the module numbers stay)
        m.set_quick(False); self.assertEqual(m.seconds['build'], 540)

    def test_reuse_preview_never_raises(self):
        self.assertIsNone(install.reuse_preview(self.tmp / 'nothing'))
        with mock.patch.object(install, 'read_cache', boom):
            self.assertIsNone(install.reuse_preview(self.tmp))

    def test_texts_are_plain(self):
        try:
            import gui
        except ImportError as e:
            self.skipTest(str(e))
        for t in (gui.QUICK_NOTE, gui.REBUILD_TITLE, gui.REBUILD_SUB):
            self.assertNotRegex(t.lower(), r'cache|ninja|cmake|fingerprint|incremental|reddit')


if __name__ == '__main__':
    unittest.main(verbosity=2)

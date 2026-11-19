"""1.1.5: the game build finishes on low-memory PCs. Only after the build ran out of memory with one parallel compile (where 1.1.4
stopped): one code-generation thread for the files not compiled yet, then lighter optimisation for the file that still runs short
(/O1, /O1 /Ob0, /Od); if even that runs short, the build WAITS for free memory (a calm state in the window, no failure screen) and
starts again by itself; a Resume (after Cancel or closing) keeps the compiled files; plain advice when an out-of-memory stop happens
anyway. Every failure inside the new code ends exactly as 1.1.4 did (its out-of-memory stop).
Run with the kit's Python:  python -B test_v115.py
"""
import json, os, re, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
import precheck, install                                                     # noqa: E402

G = precheck.GB
GEN = ['recomp_0000.c', 'recomp_0001.c', 'recomp_0002.c', 'recomp_0057.c', 'recomp_0101.c']
OBJDIR = 'CMakeFiles/mechassault2_recomp.dir/'
MANIFEST = '<assembly/>\n'


def oom_tail(unit=None):
    """The end of a ninja + cl run that ran out of memory (in `unit`, or in the link)."""
    if unit is None: return ['LINK : fatal error LNK1102: out of memory', 'ninja: build stopped: subcommand failed.']
    return ['FAILED: [code=2] %ssrc/recomp/gen/%s.obj ' % (OBJDIR, unit),
            'C:\\PROGRA~1\\cl.exe /nologo /O2 /bigobj /Fo%ssrc/recomp/gen/%s.obj /c C:\\b\\src\\game\\src\\recomp\\gen\\%s' % (OBJDIR, unit, unit),
            unit, 'C:\\b\\src\\game\\src\\recomp\\gen\\%s(123456): fatal error C1060: compiler is out of heap space' % unit,
            'ninja: build stopped: subcommand failed.']


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-115-', dir=str(WORK / 'tmp')))
        self.lines = []
        self._sleep = install.time.sleep
        install.time.sleep = lambda s: None
    def tearDown(self):
        install.time.sleep = self._sleep
        shutil.rmtree(self.tmp, ignore_errors=True)

    def installer(self, name='i'):
        d = self.tmp / name; d.mkdir(parents=True, exist_ok=True)
        inst = install.Installer(None, d, log=self.lines.append)
        inst.state = {'done': []}; inst.work.mkdir(parents=True, exist_ok=True)
        inst.LOWMEM_WAIT = 0; inst.LOWMEM_POLL = 0                    # (no real waiting in tests)
        self.waits = []; inst.on_wait = self.waits.append
        return inst

    def fake_build(self, inst, outcomes, jobs=8, mem=None):
        """build() with the kit copy and the toolchain faked. run_cmd follows `outcomes`: 'ok', or (kind, tail, finished) where
        `finished` = the gen files compiled before the failure (their objects are written), or an exception instance to raise."""
        inst.vcvars, inst.cmake, inst.ninja, inst.vs = Path('vcvars64.bat'), Path('cmake.exe'), Path('ninja.exe'), self.tmp / 'vs'
        inst.jobs = jobs
        inst.project = lambda: self.tmp / 'proj'
        def copytree(s, d, *a, **k):
            d = Path(d); d.mkdir(parents=True, exist_ok=True)
            if d.name == 'game': (d / 'src').mkdir(); (d / 'src' / 'app.manifest').write_text(MANIFEST, encoding='utf-8')
            if d.name == 'gen':
                for n in GEN: (d / n).write_text('int x;\n', encoding='utf-8')
        self.calls = getattr(self, 'calls', [])
        def run_cmd(args, cwd=None, env=None, **k):
            script = Path(args[-1]).read_text(encoding='utf-8')
            jobs = int(script.split(' -j ')[1].split()[0])
            build = Path(cwd) / 'build'; build.mkdir(exist_ok=True)
            objs = sorted(p.name for p in build.rglob('*.obj'))
            inc = Path(cwd) / install.LOWMEM_FILE
            self.calls.append(dict(root=Path(cwd), jobs=jobs, objs=objs, script=script,
                                   include=inc.read_text(encoding='utf-8') if 'CMAKE_PROJECT_mechassault2_recomp_INCLUDE' in script else None))
            out = outcomes.pop(0)
            if isinstance(out, BaseException): raise out
            if out == 'ok':
                (build / 'mechassault2_recomp.exe').write_bytes(b'built'); (build / 'build.ninja').write_text('', encoding='utf-8'); return
            kind, tail, finished = out
            (build / 'build.ninja').write_text('', encoding='utf-8')
            for n in finished:
                o = build / (OBJDIR + 'src/recomp/gen/' + n + '.obj'); o.parent.mkdir(parents=True, exist_ok=True); o.write_bytes(b'')
            e = install.Stop('CMake + Ninja failed', kind); e.tail = tail; raise e
        inst.run_cmd = run_cmd
        with mock.patch.object(install.shutil, 'copytree', copytree), \
             mock.patch.object(install.precheck, 'memory_info', mem or (lambda: (8 * G, 3 * G))), \
             mock.patch.object(install, 'reboot_pending', lambda: {'windows': False, 'vs': False}):
            inst.build()


class Parsing(Base):
    def test_failed_unit_from_ninja_and_from_the_compiler(self):
        game = self.tmp / 'b' / 'src' / 'game'
        self.assertEqual(install.lowmem_failed_units(oom_tail('recomp_0101.c'), game), ['src/recomp/gen/recomp_0101.c'])
        old_ninja = ['FAILED: CMakeFiles/mechassault2_recomp.dir/vendor/imgui-1.92.5/imgui.cpp.obj']
        self.assertEqual(install.lowmem_failed_units(old_ninja, game), ['vendor/imgui-1.92.5/imgui.cpp'])
        cl_only = ['%s(9): fatal error C1002: compiler is out of heap space in pass 2' % (game / 'src' / 'recomp' / 'gen' / 'recomp_0002.c')]
        self.assertEqual(install.lowmem_failed_units(cl_only, game), ['src/recomp/gen/recomp_0002.c'])

    def test_link_runtime_and_other_lines_name_no_unit(self):
        game = self.tmp / 'g'
        self.assertEqual(install.lowmem_failed_units(oom_tail(None), game), [])
        self.assertEqual(install.lowmem_failed_units(['FAILED: xboxrecomp/src/CMakeFiles/xbox_kernel.dir/k.c.obj'], game), [])
        self.assertEqual(install.lowmem_failed_units(['C:\\elsewhere\\x.c(1): fatal error C1060: out of heap'], game), [])
        self.assertEqual(install.lowmem_failed_units(None, game), [])

    def test_unbuilt_lists_only_files_without_an_object(self):
        game, build = self.tmp / 'game', self.tmp / 'build'
        for n in GEN: (game / 'src/recomp/gen').mkdir(parents=True, exist_ok=True); (game / 'src/recomp/gen' / n).write_text('')
        (game / 'src/main.c').write_text(''); (game / 'src/app.rc').write_text('')
        (game / 'vendor/imgui-1.92.5/backends').mkdir(parents=True); (game / 'vendor/imgui-1.92.5/backends/imgui_impl_dx11.cpp').write_text('')
        (game / 'vendor/other').mkdir(); (game / 'vendor/other/x.c').write_text('')
        for rel in ('src/main.c', 'src/recomp/gen/recomp_0000.c'):
            o = install.lowmem_obj(build, rel); o.parent.mkdir(parents=True, exist_ok=True); o.write_bytes(b'')
        self.assertEqual(sorted(install.lowmem_unbuilt(game, build)),
                         ['src/recomp/gen/' + n for n in GEN[1:]] + ['vendor/imgui-1.92.5/backends/imgui_impl_dx11.cpp'])

    def test_steps_and_include_file(self):
        self.assertEqual(install.lowmem_step_of(['/cgthreads1']), -1)
        self.assertEqual([install.lowmem_step_of(install.LOWMEM_ALL + s) for s in install.LOWMEM_STEPS], [0, 1, 2])
        f = self.tmp / 'lowmem.cmake'
        install.write_lowmem_cmake(f, {'src/recomp/gen/recomp_0001.c': ['/cgthreads1'], 'src/recomp/gen/recomp_0057.c': ['/cgthreads1', '/O1'],
                                       'bad"name.c': ['/O1'], 'src/x.c': ['/O1; message(boom)']})
        text = f.read_text(encoding='utf-8')
        self.assertIn('set_property(SOURCE "src/recomp/gen/recomp_0057.c" PROPERTY COMPILE_OPTIONS /cgthreads1 /O1 ${_o})', text)
        self.assertIn('set_property(SOURCE "src/recomp/gen/recomp_0001.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o})', text)
        self.assertNotIn('bad', text); self.assertNotIn('boom', text)            # nothing but plain paths and plain flags
        self.assertIn('cmake_language(DEFER CALL _ma2_lowmem_apply)', text)


class Ladder(Base):
    def test_a_normal_build_is_exactly_1_1_4(self):
        inst = self.installer()
        self.fake_build(inst, ['ok'])
        c = self.calls[0]
        self.assertIsNone(c['include']); self.assertNotIn('CMAKE_PROJECT', c['script'])
        b = c['root']
        expect = '"cmake.exe" -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DXBOXRECOMP_DIR=%s" "-DCMAKE_MAKE_PROGRAM=ninja.exe" || exit /b 1' % (
            b / 'src' / 'game', b / 'build', b / 'src' / 'runtime')
        self.assertIn(expect, c['script'].splitlines())
        self.assertNotIn('lowmem', inst.state); self.assertFalse((b / install.LOWMEM_FILE).exists())
        self.assertIsNone(inst.lowmem_record())

    def test_out_of_memory_with_more_jobs_is_1_1_4(self):
        inst = self.installer()
        self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), ['recomp_0000.c']), 'ok'], jobs=2)
        self.assertEqual([c['jobs'] for c in self.calls], [2, 1]); self.assertIsNone(self.calls[1]['include'])
        self.assertNotIn('lowmem', inst.state)

    def test_full_ladder_then_waits_for_memory_and_finishes(self):
        inst = self.installer(); statuses = []; inst.on_status = statuses.append
        u = 'recomp_0101.c'
        self.fake_build(inst, [('out_of_memory', oom_tail(u), ['recomp_0000.c']),
                               ('out_of_memory', oom_tail(u), ['recomp_0001.c']),
                               ('out_of_memory', oom_tail(u), []), ('out_of_memory', oom_tail(u), []),
                               ('out_of_memory', oom_tail(u), []), ('out_of_memory', oom_tail(u), []), 'ok'], jobs=1)
        self.assertEqual(len(self.calls), 7)                     # 1.1.4 run + cgthreads + 3 lighter steps + 2 waits, then it finishes
        self.assertIsNone(self.calls[0]['include'])
        inc1 = self.calls[1]['include']
        self.assertNotIn('recomp_0000.c', inc1)                                       # already compiled: untouched, not rebuilt
        self.assertIn('"src/recomp/gen/recomp_0101.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o}', inc1)
        self.assertIn('"src/recomp/gen/recomp_0001.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o}', inc1)
        self.assertIn('recomp_0000.c.obj', self.calls[1]['objs'])                     # objects kept
        self.assertIn('"src/recomp/gen/recomp_0101.c" PROPERTY COMPILE_OPTIONS /cgthreads1 /O1 ${_o}', self.calls[2]['include'])
        self.assertIn('"src/recomp/gen/recomp_0001.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o}', self.calls[2]['include'])  # same: not rebuilt
        self.assertIn('/cgthreads1 /O1 /Ob0 ${_o}', self.calls[3]['include'])
        for c in self.calls[4:]:
            self.assertIn('"src/recomp/gen/recomp_0101.c" PROPERTY COMPILE_OPTIONS /cgthreads1 /Od ${_o}', c['include'])
        self.assertEqual(self.waits, [{'free_gb': 3.0}, None, {'free_gb': 3.0}, None])        # the window: waiting, then carrying on
        self.assertEqual(sum('WAITING' in l for l in self.lines), 2)                         # each automatic retry logged once
        self.assertTrue(any('RECOVERY' in l and '/cgthreads1' in l for l in self.lines), self.lines)
        self.assertTrue(any('RECOVERY' in l and 'recomp_0101.c /Od' in l for l in self.lines))
        self.assertTrue(statuses and 'lighter settings' in statuses[-1])
        self.assertTrue(any('waiting for memory' in e for e in inst.state['events']))
        self.assertNotIn('lowmem_keep', inst.state); self.assertNotIn('lowmem_waits', inst.state)
        self.assertEqual(inst.lowmem_record()['lighter_optimisation'], {'src/recomp/gen/recomp_0101.c': '/Od'})

    def test_waiting_retries_at_once_when_memory_frees_up(self):
        inst = self.installer(); inst.LOWMEM_WAIT = 10 ** 6                          # (only a rise in free memory ends the wait)
        free = iter([4 * G, 2 * G, 2.5 * G, 3.2 * G] + [3.2 * G] * 10)     # (the build's start, the wait's start, then two checks)
        self.fake_build(inst, [('out_of_memory', oom_tail(None), GEN), 'ok'], jobs=1, mem=lambda: (8 * G, next(free)))
        self.assertEqual(len(self.calls), 2); self.assertEqual(self.waits, [{'free_gb': 2.0}, None])
        self.assertTrue(any('building again (3.2 GB free)' in l for l in self.lines), self.lines)

    def test_cancel_while_waiting_then_resume_carries_on(self):
        inst = self.installer()
        inst.on_wait = lambda info: setattr(inst, 'cancelled', True) if info else None
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), ['recomp_0000.c'])] * 5, jobs=1)
        self.assertEqual(cm.exception.kind, 'cancelled'); self.assertTrue(inst.state['lowmem_keep'])
        n = len(self.calls); inst.cancelled = False; inst.on_wait = self.waits.append
        self.fake_build(inst, ['ok'], jobs=1)                                          # Resume (the window opened again)
        self.assertIn('recomp_0000.c.obj', self.calls[n]['objs'])
        self.assertIn('/cgthreads1 /Od', self.calls[n]['include'])                    # the level reached: nothing compiled again
        self.assertTrue(any('carrying on with the' in l for l in self.lines))
        self.assertNotIn('lowmem_keep', inst.state)

    def test_ladder_finishes_and_is_recorded(self):
        inst = self.installer()
        self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), ['recomp_0000.c']),
                               ('out_of_memory', oom_tail('recomp_0101.c'), ['recomp_0001.c', 'recomp_0002.c']), 'ok'], jobs=1)
        self.assertEqual(len(self.calls), 3)
        rec = inst.lowmem_record()
        self.assertEqual(rec, {'one_codegen_thread': 4, 'lighter_optimisation': {'src/recomp/gen/recomp_0101.c': '/O1'}})
        self.assertNotIn('lowmem_keep', inst.state)
        self.assertTrue(any('built in low-memory mode: 4 file(s)' in l and 'recomp_0101.c (/O1)' in l for l in self.lines), self.lines)
        # recomp-install.json carries it; diagnostics name it
        inst.state['done'] = ['build']; (inst.dest / 'MechAssault 2.exe').write_bytes(b'x')
        with mock.patch.object(inst, 'game_installed', lambda: False):
            inst.write_stamp(inst.dest / 'none.ini')
        stamp = json.loads((inst.dest / precheck.STAMP).read_text(encoding='utf-8'))
        self.assertEqual(stamp['low_memory_build'], rec)
        facts = precheck.diagnostics_facts(inst.dest, inst.state) if 'state' in precheck.diagnostics_facts.__code__.co_varnames else None
        if facts is not None:
            self.assertTrue(any(l.startswith('Low-memory build: 4 file(s)') and 'recomp_0101.c /O1' in l for l in facts), facts)

    def test_a_normal_stamp_has_no_low_memory_key(self):
        inst = self.installer(); inst.state['done'] = ['build']; (inst.dest / 'MechAssault 2.exe').write_bytes(b'x')
        with mock.patch.object(inst, 'game_installed', lambda: False):
            inst.write_stamp(inst.dest / 'none.ini')
        self.assertNotIn('low_memory_build', json.loads((inst.dest / precheck.STAMP).read_text(encoding='utf-8')))

    def test_link_out_of_memory_after_everything_compiled_waits_without_new_flags(self):
        inst = self.installer()
        self.fake_build(inst, [('out_of_memory', oom_tail(None), GEN), 'ok'], jobs=1)
        self.assertEqual(len(self.calls), 2); self.assertNotIn('lowmem', inst.state)
        self.assertIsNone(self.calls[1]['include']); self.assertEqual(len(self.calls[1]['objs']), len(GEN))    # same commands, files kept
        self.assertEqual(self.waits, [{'free_gb': 3.0}, None])

    def test_a_new_failing_file_starts_at_the_first_lighter_step(self):
        inst = self.installer()
        self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), []), ('out_of_memory', oom_tail('recomp_0101.c'), []),
                               ('out_of_memory', oom_tail('recomp_0057.c'), ['recomp_0101.c']), 'ok'], jobs=1)
        units = inst.state['lowmem']['units']
        self.assertEqual(units['src/recomp/gen/recomp_0101.c'], ['/cgthreads1', '/O1'])
        self.assertEqual(units['src/recomp/gen/recomp_0057.c'], ['/cgthreads1', '/O1'])


class Resume(Base):
    def stopped_while_waiting(self, inst):
        """The ladder up to /Od, then the player closes the window while it waits (the install stops as 'cancelled')."""
        inst.on_wait = lambda info: setattr(inst, 'cancelled', True) if info else None
        with self.assertRaises(install.Stop):
            self.fake_build(inst, [('out_of_memory', oom_tail(None), [])] + [('out_of_memory', oom_tail('recomp_0101.c'), ['recomp_0000.c'])] * 5, jobs=1)
        inst.cancelled = False; inst.on_wait = self.waits.append
        return len(self.calls)

    def test_resume_after_a_low_memory_stop_keeps_the_compiled_files(self):
        inst = self.installer(); n = self.stopped_while_waiting(inst)
        self.fake_build(inst, ['ok'], jobs=1)
        self.assertIn('recomp_0000.c.obj', self.calls[n]['objs'])
        self.assertIn('/cgthreads1 /Od', self.calls[n]['include'])
        self.assertNotIn('lowmem_keep', inst.state)

    def test_kept_files_that_fail_otherwise_are_rebuilt_clean_as_1_1_4(self):
        inst = self.installer(); n = self.stopped_while_waiting(inst)
        self.fake_build(inst, [('compile_error', ['error LNK2019: unresolved external'], []), 'ok'], jobs=1)
        self.assertEqual(len(self.calls), n + 2)
        self.assertEqual(self.calls[n + 1]['objs'], []); self.assertIsNone(self.calls[n + 1]['include'])
        self.assertNotIn('lowmem', inst.state)
        self.assertTrue(any('RECOVERY' in l and 'from the start' in l for l in self.lines))

    def test_a_new_run_without_a_low_memory_stop_starts_clean(self):
        inst = self.installer()
        self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), ['recomp_0000.c']), 'ok'], jobs=1)
        self.assertIn('lowmem', inst.state)
        self.fake_build(inst, ['ok'], jobs=1)                                          # e.g. the build step runs again later
        self.assertEqual(self.calls[-1]['objs'], []); self.assertIsNone(self.calls[-1]['include'])
        self.assertNotIn('lowmem', inst.state)

    def test_other_errors_still_show_the_stop(self):
        inst = self.installer()
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), []), ('compile_error', ['error C2065: x'], [])], jobs=1)
        self.assertEqual(cm.exception.kind, 'compile_error'); self.assertEqual(self.waits, [])


class Faults(Base):
    """A failure inside the new code: the build ends exactly as 1.1.4's (its out-of-memory stop)."""
    def stops_as_1_1_4(self, patch, outcomes=None):
        inst = self.installer()
        with mock.patch.object(*patch), self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, outcomes or [('out_of_memory', oom_tail('recomp_0101.c'), [])], jobs=1)
        self.assertEqual(cm.exception.kind, 'out_of_memory')
        self.assertNotIn('lowmem', inst.state); self.assertNotIn('lowmem_keep', inst.state)
        return inst

    def test_fault_listing_files(self):
        self.stops_as_1_1_4((install, 'lowmem_unbuilt', mock.Mock(side_effect=OSError('boom'))))
        self.assertEqual(len(self.calls), 1)
        self.assertTrue(any('low-memory build is not available' in l for l in self.lines))

    def test_fault_writing_the_include_file(self):
        self.stops_as_1_1_4((install, 'write_lowmem_cmake', mock.Mock(side_effect=PermissionError('denied'))))
        self.assertEqual(len(self.calls), 1)

    def test_fault_saving_state(self):
        inst = self.installer()
        real = inst.save; n = {'c': 0}
        def save():
            if 'lowmem' in inst.state and not n['c']: n['c'] = 1; raise OSError('disk')
            real()
        inst.save = save
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), [])], jobs=1)
        self.assertEqual(cm.exception.kind, 'out_of_memory'); self.assertEqual(len(self.calls), 1)

    def test_fault_the_retry_itself_failing_unexpectedly(self):
        inst = self.installer()
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), []), RuntimeError('tool start failed')], jobs=1)
        self.assertEqual(cm.exception.kind, 'out_of_memory'); self.assertEqual(len(self.calls), 2)
        self.assertNotIn('lowmem', inst.state)
        self.assertTrue(any('low-memory build could not run' in l for l in self.lines))

    def test_fault_parsing_the_failure(self):
        inst = self.installer()
        with mock.patch.object(install, 'lowmem_failed_units', mock.Mock(side_effect=ValueError('x'))), self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail('recomp_0101.c'), []), ('out_of_memory', oom_tail('recomp_0101.c'), [])], jobs=1)
        self.assertEqual(cm.exception.kind, 'out_of_memory'); self.assertEqual(len(self.calls), 2)
        self.assertNotIn('lowmem', inst.state)

    def test_fault_checking_a_kept_build(self):
        inst = self.installer(); n = Resume.stopped_while_waiting(self, inst)
        with mock.patch.object(inst, 'event', mock.Mock(side_effect=OSError('unreadable'))):
            self.fake_build(inst, ['ok'], jobs=1)
        self.assertEqual(self.calls[n]['objs'], []); self.assertIsNone(self.calls[n]['include'])   # clean, as 1.1.4

    def test_fault_waiting_for_memory(self):
        """The wait helper failing (free memory unreadable): the 1.1.4 out-of-memory stop, no waiting state left behind."""
        inst = self.installer(); calls = {'n': 0}
        def mem():
            calls['n'] += 1
            if calls['n'] > 1: raise OSError('GlobalMemoryStatusEx failed')
            return (8 * G, 3 * G)
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail(None), GEN)], jobs=1, mem=mem)
        self.assertEqual(cm.exception.kind, 'out_of_memory'); self.assertEqual(len(self.calls), 1)
        self.assertNotIn('lowmem_keep', inst.state); self.assertEqual(self.waits, [])
        self.assertTrue(any('waiting for memory is not available' in l for l in self.lines))

    def test_fault_the_window_hook(self):
        inst = self.installer(); inst.on_wait = mock.Mock(side_effect=RuntimeError('tk gone'))
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', oom_tail(None), GEN)], jobs=1)
        self.assertEqual(cm.exception.kind, 'out_of_memory'); self.assertEqual(len(self.calls), 1)

    def test_fault_every_new_method_on_a_normal_build(self):
        """Each 1.1.5 method replaced by one that throws (as the real-exe fault run does): a normal build is exactly 1.1.4's."""
        inst = self.installer(); boom = mock.Mock(side_effect=RuntimeError('injected'))
        with mock.patch.multiple(install.Installer, lowmem_keep=boom, lowmem_include=boom, lowmem_summary=boom, lowmem_record=boom):
            self.fake_build(inst, ['ok'])
            inst.state['done'] = ['build']; (inst.dest / 'MechAssault 2.exe').write_bytes(b'x')
            with mock.patch.object(inst, 'game_installed', lambda: False):
                inst.write_stamp(inst.dest / 'none.ini')
        self.assertNotIn('CMAKE_PROJECT', self.calls[0]['script']); self.assertGreaterEqual(boom.call_count, 4)
        self.assertNotIn('low_memory_build', json.loads((inst.dest / precheck.STAMP).read_text(encoding='utf-8')))

    def test_fault_the_ladder_or_the_wait_method_throwing(self):
        for name in ('lowmem_next', 'lowmem_wait'):
            inst = self.installer(name); self.calls = []
            with mock.patch.object(install.Installer, name, mock.Mock(side_effect=RuntimeError('injected'))), self.assertRaises(install.Stop) as cm:
                self.fake_build(inst, [('out_of_memory', oom_tail(None), GEN)], jobs=1)      # (nothing lighter left: the wait is next)
            self.assertEqual(cm.exception.kind, 'out_of_memory', name); self.assertEqual(len(self.calls), 1, name)

    def test_fault_stamp_record(self):
        inst = self.installer(); inst.state['done'] = ['build']; (inst.dest / 'MechAssault 2.exe').write_bytes(b'x')
        with mock.patch.object(inst, 'game_installed', lambda: False), mock.patch.object(inst, 'lowmem_record', mock.Mock(side_effect=KeyError('x'))):
            inst.write_stamp(inst.dest / 'none.ini')
        self.assertIn('kit_id', json.loads((inst.dest / precheck.STAMP).read_text(encoding='utf-8')))


class Advice(unittest.TestCase):
    def test_both_windows_say_what_to_do(self):
        for mod, key in (('gui', 'FAILED'), ('gui_legacy', 'FAILURES')):
            try:
                head, body = getattr(__import__(mod), key)['out_of_memory']
            except ImportError as e:                                    # (no Tk in this Python)
                self.skipTest(str(e))
            for word in ('web browsers', 'Discord', 'page file', 'automatically', 'Virtual memory', 'C:', 'finished files are kept'):
                self.assertIn(word.lower(), body.lower(), (mod, word))
            self.assertIn('memory', head)
            self.assertNotIn('{', body)
        import gui, gui_legacy
        for text in (gui.MEMWAIT_NOTE + ' ' + gui.MEMWAIT_HINT, gui_legacy.MEMORY_WAIT):
            for word in ('browsers', 'Discord', 'by itself', 'page file', 'automatically', 'Virtual memory', 'C:'):
                self.assertIn(word, text)


class RealCMake(Base):
    """The include file with the real CMake + Ninja of this PC (skipped without Build Tools): flags reach only the listed files,
    after /O2 and before a file's own options, and files not listed keep their command lines."""
    def test_flags_reach_only_the_listed_files(self):
        try:
            vcvars, cmake, ninja, vs = install.find_visual_studio(None)
        except Exception as e:
            self.skipTest('no Build Tools: %s' % e)
        src = self.tmp / 'p'; (src / 'src').mkdir(parents=True)
        for n in ('a.c', 'b.c', 'c.c'): (src / 'src' / n).write_text('int %s_(void){return 1;}\n' % n[0], encoding='utf-8')
        (src / 'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(mechassault2_recomp C)\n'
                                            'add_library(mechassault2_recomp STATIC src/a.c src/b.c src/c.c)\n'
                                            'set_source_files_properties(src/c.c PROPERTIES COMPILE_OPTIONS /Ob0)\n', encoding='utf-8')
        inc = self.tmp / install.LOWMEM_FILE
        install.write_lowmem_cmake(inc, {'src/b.c': ['/cgthreads1'], 'src/c.c': ['/cgthreads1', '/O1']})
        b0, b1 = self.tmp / 'b0', self.tmp / 'b1'
        def configure(b, extra):
            bat = self.tmp / ('cfg-%s.cmd' % b.name)
            bat.write_text('@echo off\r\ncall "%s" >nul || exit /b 1\r\n"%s" -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%s"%s || exit /b 1\r\n'
                           % (vcvars, cmake, src, b, Path(ninja).as_posix(), extra), encoding='utf-8')
            r = subprocess.run(['cmd.exe', '/d', '/c', str(bat)], capture_output=True, text=True, creationflags=install.NO_CONSOLE)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return (b / 'build.ninja').read_text(encoding='utf-8')
        def flags(text, n):
            m = re.search(r'build CMakeFiles[\\/]mechassault2_recomp\.dir[\\/]src[\\/]%s\.obj:.*?\n((?:  .*\n)+)' % n, text)
            return re.search(r'  FLAGS = (.*)', m.group(1)).group(1).strip()
        n0 = configure(b0, '')
        n1 = configure(b1, ' "-DCMAKE_PROJECT_mechassault2_recomp_INCLUDE=%s"' % inc.as_posix())
        self.assertEqual(flags(n0, 'a.c'), flags(n1, 'a.c'))
        self.assertEqual(flags(n1, 'b.c'), flags(n0, 'b.c') + ' /cgthreads1')
        f = flags(n1, 'c.c')
        self.assertTrue(f.endswith('/cgthreads1 /O1 /Ob0'), f); self.assertLess(f.index('/O2'), f.index('/O1'))


if __name__ == '__main__':
    unittest.main(verbosity=2)

"""1.1.3 reliability fixes: rollback integrity, Build Tools bootstrapper, toolchain discovery, the stub's own log, diagnostics,
retries on file locks, location checks, pip isolation, the Play button, done markers, numeric error codes.
Run with the kit's Python:  python -B test_reliability.py
"""
import inspect, json, os, shutil, sys, tempfile, time, unittest, zipfile
from pathlib import Path
from unittest import mock
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
import precheck, install                                                     # noqa: E402

G = precheck.GB
SHARING = OSError(None, 'The process cannot access the file because it is being used by another process', None, 32)


REAL_SLEEP = time.sleep        # setUp replaces time.sleep itself (install.time is the time module): restore this one


class Reliability(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-rel-', dir=str(WORK / 'tmp')))
        install.time.sleep = lambda s: None if s < 1 else None                # retries without waiting
    def tearDown(self):
        install.time.sleep = REAL_SLEEP
        shutil.rmtree(self.tmp, ignore_errors=True)

    def update_install(self):
        d = self.tmp / 'i'; d.mkdir()
        (d / 'MechAssault 2.exe').write_text('old'); (d / 'avutil-61.dll').write_text('old-dll')
        inst = install.Installer(None, d, log=self.lines.append); inst.state = {'update': True, 'done': ['analyze', 'assets', 'install']}
        inst.work.mkdir()
        return d, inst

    # 1. rollback integrity
    def test_backup_pair_recorded_before_the_rename(self):
        self.lines = []; d, inst = self.update_install()
        seen = []
        real = Path.rename
        def rename(p, target):
            seen.append(json.loads((inst.work / 'state.json').read_text())['previous'])
            return real(p, target)
        with mock.patch.object(Path, 'rename', rename):
            inst.keep_previous(d / 'MechAssault 2.exe')
        self.assertEqual(seen[0], [[str(d / 'MechAssault 2.exe'), str(d / 'MechAssault 2.exe.previous')]])
        # a failed rename takes its record back out, and says what to close
        with mock.patch.object(Path, 'rename', lambda p, t: (_ for _ in ()).throw(PermissionError(13, 'Access is denied'))):
            with self.assertRaises(install.Stop) as cm: inst.keep_previous(d / 'avutil-61.dll')
        self.assertEqual(cm.exception.kind, 'access_denied')
        self.assertEqual(len(inst.state['previous']), 1)

    def test_restore_with_a_sharing_failure_keeps_the_pair_and_says_partly(self):
        self.lines = []; d, inst = self.update_install()
        inst.keep_previous(d / 'MechAssault 2.exe'); inst.keep_previous(d / 'avutil-61.dll')
        (d / 'MechAssault 2.exe').write_text('new'); (d / 'avutil-61.dll').write_text('new-dll')
        real = Path.unlink
        def unlink(p, missing_ok=False):                                   # the game exe is held open (a scanner, the game itself)
            if p.name == 'MechAssault 2.exe': raise SHARING
            return real(p, missing_ok=missing_ok)
        with mock.patch.object(Path, 'unlink', unlink):
            inst.restore_previous()
        self.assertEqual((d / 'avutil-61.dll').read_text(), 'old-dll')                     # the one that could be restored was
        self.assertEqual((d / 'MechAssault 2.exe.previous').read_text(), 'old')           # the failed one's backup is kept
        self.assertEqual(inst.state['previous'], [[str(d / 'MechAssault 2.exe'), str(d / 'MechAssault 2.exe.previous')]])
        saved = json.loads((inst.work / 'state.json').read_text())
        self.assertEqual(saved['previous'], inst.state['previous'])
        self.assertTrue(any('only partly restored' in l for l in self.lines), self.lines)
        self.assertFalse(any('the previous version was put back' in l for l in self.lines), self.lines)
        self.assertTrue(any('WinError 32' in l or 'busy' in l for l in self.lines), self.lines)        # the retries were logged
        inst.restore_previous()                                                              # the lock is gone: the next run finishes it
        self.assertEqual((d / 'MechAssault 2.exe').read_text(), 'old'); self.assertEqual(inst.state['previous'], [])
        self.assertTrue(any('the previous version was put back' in l for l in self.lines))

    def test_settle_writes_stamp_and_core_done_before_deleting_backups(self):
        self.lines = []; d, inst = self.update_install()
        inst.keep_previous(d / 'MechAssault 2.exe'); (d / 'MechAssault 2.exe').write_text('new')
        order = []
        real_commit, real_stamp = inst.commit_previous, inst.write_stamp
        inst.write_stamp = lambda ini: (order.append('stamp'), real_stamp(ini))
        def commit():
            order.append('commit')
            self.assertTrue((d / precheck.STAMP).is_file())
            self.assertTrue(json.loads((inst.work / 'state.json').read_text()).get('core_done'))
            real_commit()
        inst.commit_previous = commit
        inst.finalize_core()
        self.assertEqual(order, ['stamp', 'commit']); self.assertFalse((d / 'MechAssault 2.exe.previous').exists())

    # 3. Build Tools bootstrapper
    def test_build_tools_channel_and_exit_codes(self):
        self.assertEqual(install.build_tools_urls(19045), ['https://aka.ms/vs/17/release/vs_buildtools.exe'])   # Windows 10: VS 2022 only
        self.assertEqual(install.build_tools_urls(26100)[0], 'https://aka.ms/vs/18/stable/vs_buildtools.exe')
        for code in (1602, 1618, 740, 5, 5003, 5004):
            self.assertIn(code, install.BUILD_TOOLS_EXIT)
        self.assertEqual(install.BUILD_TOOLS_EXIT[5003][1], 'network'); self.assertEqual(install.BUILD_TOOLS_EXIT[1602][1], 'cancelled')
        src = inspect.getsource(install.install_build_tools) + inspect.getsource(install.run_elevated)   # 1.2.0: the runas part is shared
        self.assertNotIn('powershell', src.lower())
        for need in ('microsoft_signed(boot)', 'get_last_error()', '1223', "'reboot'", 'SetThreadExecutionState', 'GetExitCodeProcess(info.hProcess'):
            self.assertIn(need, src)
        self.assertIn("e.code != 'reboot'", inspect.getsource(install.Installer._run))     # a restart keeps the work folder for Resume

    def test_signature_check_without_powershell(self):
        vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        self.assertFalse(install.microsoft_signed(sys.executable))                       # signed, but not by Microsoft (or unsigned)
        odd = self.tmp / "O'Brien temp"; odd.mkdir(); (odd / 'x.exe').write_bytes(b'MZ not signed')
        self.assertFalse(install.microsoft_signed(odd / 'x.exe'))                        # an apostrophe in the path is fine
        if vswhere.is_file():
            shutil.copy2(vswhere, odd / 'vs_buildtools.exe')
            self.assertTrue(install.microsoft_signed(odd / 'vs_buildtools.exe'))

    # 4. toolchain discovery
    def fake_vs(self, name, cl=True, cmake=True, vcvars=True):
        vs = self.tmp / name
        if vcvars: (vs / 'VC/Auxiliary/Build').mkdir(parents=True); (vs / 'VC/Auxiliary/Build/vcvars64.bat').write_text('@echo off')
        if cl: (vs / 'VC/Tools/MSVC/14.50.1/bin/Hostx64/x64').mkdir(parents=True); (vs / 'VC/Tools/MSVC/14.50.1/bin/Hostx64/x64/cl.exe').write_bytes(b'')
        if cmake:
            for f in ('CMake/bin/cmake.exe', 'Ninja/ninja.exe'):
                p = vs / 'Common7/IDE/CommonExtensions/Microsoft/CMake' / f; p.parent.mkdir(parents=True); p.write_bytes(b'')
        return vs

    def test_newest_complete_stable_instance_wins(self):
        good, newer_broken, preview = self.fake_vs('good'), self.fake_vs('newer', cl=False), self.fake_vs('preview')
        insts = [{'installationPath': str(good), 'isComplete': True, 'installationVersion': '17.14.1'},
                 {'installationPath': str(newer_broken), 'isComplete': True, 'installationVersion': '18.1.0'},
                 {'installationPath': str(preview), 'isComplete': True, 'isPrerelease': True, 'installationVersion': '18.2.0'},
                 {'installationPath': str(self.fake_vs('partial')), 'isComplete': False, 'installationVersion': '18.3.0'}]
        with mock.patch.object(install, 'vs_instances', lambda: insts), mock.patch.object(install, 'windows_sdk', lambda: Path('w.h')), \
             mock.patch.object(install.shutil, 'which', lambda n: None):
            self.assertEqual(install.find_visual_studio()[3], good)
            insts[:] = [i for i in insts if i['installationPath'] != str(good)]
            self.assertEqual(install.find_visual_studio()[3], preview)                   # a preview only when no stable one works
        with mock.patch.object(install, 'vs_instances', lambda: [{'installationPath': str(self.fake_vs('nocmake', cmake=False))}]), \
             mock.patch.object(install, 'windows_sdk', lambda: None), mock.patch.object(install.shutil, 'which', lambda n: None):
            st = install.compiler_state()
        self.assertEqual(st['state'], 'incomplete'); self.assertEqual(st['missing'], ['cmake', 'sdk'])
        self.assertEqual(st['add'], [install.VS_PARTS['cmake'][1], install.VS_PARTS['sdk'][1]])
        self.assertIn('Windows SDK', st['detail']); self.assertTrue(st['vs'].endswith('nocmake'))
        with mock.patch.object(install, 'vs_instances', lambda: []):
            self.assertEqual(install.compiler_state()['state'], 'missing')
        src = inspect.getsource(install.vs_instances)
        for need in ("'-all'", "'-prerelease'", "'json'", "'-utf8'", 'VC_X64'): self.assertIn(need, src)

    def test_windows_sdk_needs_x64_libs_and_rc(self):
        base = self.tmp / 'pf86' / 'Windows Kits' / '10'
        (base / 'Include/10.0.1.0/um').mkdir(parents=True); (base / 'Include/10.0.1.0/um/windows.h').write_text('')
        with mock.patch.dict(os.environ, {'ProgramFiles(x86)': str(self.tmp / 'pf86')}):
            self.assertIsNone(install.windows_sdk())                                     # headers only: not enough to link
            (base / 'Lib/10.0.1.0/um/x64').mkdir(parents=True); (base / 'Lib/10.0.1.0/um/x64/kernel32.lib').write_text('')
            (base / 'bin/10.0.1.0/x64').mkdir(parents=True); (base / 'bin/10.0.1.0/x64/rc.exe').write_text('')
            self.assertEqual(install.windows_sdk(), base / 'Include/10.0.1.0/um/windows.h')

    # 6. the stub's own log
    def test_stub_log_and_kit_checks(self):
        src = (WORK.parent / 'installer-exe' / 'stub.cpp').read_text(encoding='utf-8', errors='replace')
        for need in ('stub-%04d%02d%02d-%02d%02d%02d.log', 'L"MechAssault2" / L"Logs"', 'python313.dll', '_tkinter.pyd', 'tcl86t.dll', 'tk86t.dll',
                     'init.tcl', 'capstone.dll', 'GetTempPathW(0, nullptr)', 'it ended with exit code', '%USERPROFILE%', 'Win32 error'):
            self.assertIn(need, src)
        self.assertNotIn('wchar_t temp[MAX_PATH]', src)
        tail = src[src.index('Leave nothing behind'):]
        self.assertLess(tail.index('L".unpacked"'), tail.index('fs::remove_all(kit'))       # the marker goes before the kit

    # 7. diagnostics
    def test_diagnostics_stub_log_step_toolchain_and_redaction(self):
        local = os.environ['LOCALAPPDATA']; os.environ['LOCALAPPDATA'] = str(self.tmp / 'la')
        try:
            logs = precheck.logs_dir(); logs.mkdir(parents=True)
            (logs / 'stub-20261118-100000.log').write_text('starting\n', encoding='utf-8')
            (logs / 'install-20261118-100001.log').write_text(
                'copy \\\\fileserver\\games\\disc.iso\nGET https://example.com/f.zip?sig=abc&se=1\nproxy http://bob:hunter2@proxy:8080/\npassword=hunter2\n', encoding='utf-8')
            dest = self.tmp / 'game'; (dest / '_work').mkdir(parents=True)
            (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check', 'disc'], 'last_title': 'Compiling the game',
                                                                   'toolchain': r'C:\VS\BuildTools'}), encoding='utf-8')
            res = precheck.collect_diagnostics(dest, self.tmp / 'Desktop', gpus=[])
            self.assertIn('logs/stub-20261118-100000.log', res['members'])
            with zipfile.ZipFile(res['path']) as z:
                system = z.read('system.txt').decode('utf-8'); log = z.read('logs/install-20261118-100001.log').decode('utf-8')
            self.assertIn('Failed stage: Compiling the game', system); self.assertIn(r'C++ tools: C:\VS\BuildTools', system)
            for secret in ('fileserver', 'sig=abc', 'hunter2'): self.assertNotIn(secret, log)
            self.assertIn('<host>', log); self.assertIn('?<query>', log)
        finally:
            os.environ['LOCALAPPDATA'] = local
        src = inspect.getsource(install.Installer.run_steps)
        self.assertIn("self.state['last_title'], self.state['last_step'] = title, name", src)
        self.assertIn("self.state['toolchain'] = str(self.vs)", inspect.getsource(install.Installer.check))

    # 8. retries on file locks
    def test_retry_io(self):
        log, calls = [], []
        def flaky():
            calls.append(1)
            if len(calls) < 3: raise SHARING
            return 'ok'
        self.assertEqual(install.retry_io(flaky, 'copy x', log.append), 'ok'); self.assertEqual(len(calls), 3)
        self.assertTrue(all('WinError 32' in l for l in log), log)
        calls.clear()
        def always(): calls.append(1); raise SHARING
        with self.assertRaises(OSError): install.retry_io(always, 'copy x', log.append)
        self.assertEqual(len(calls), 7)                                                   # 0.2 s doubling to 5 s, then the error
        for code in (225, 4551):
            calls.clear()
            def blocked(c=code): calls.append(1); raise OSError(None, 'blocked', None, c)
            with self.assertRaises(install.Stop) as cm: install.retry_io(blocked, 'copy x', log.append)
            self.assertEqual(cm.exception.kind, 'access_denied'); self.assertEqual(len(calls), 1)          # never retried
        calls.clear()
        def missing(): calls.append(1); raise FileNotFoundError(2, 'gone')
        with self.assertRaises(FileNotFoundError): install.retry_io(missing, 'copy x')
        self.assertEqual(len(calls), 1)
        self.assertIn('self.copy_in(build', inspect.getsource(install.Installer.install))

    # 9. location and space
    def test_location_checks(self):
        codes = lambda t: [f.code for f in precheck.check_folder(t).findings]
        self.assertEqual(codes(r'\\server\share\MechAssault 2'), ['unc'])
        with mock.patch.object(precheck, 'volume_fs', lambda p: 'FAT32'):
            self.assertIn('fat', codes(str(self.tmp / 'x')))
        with mock.patch.object(precheck, 'volume_fs', lambda p: 'exFAT'):
            self.assertIn('exfat', codes(str(self.tmp / 'x')))
        home = Path(os.environ.get('USERPROFILE') or Path.home())
        self.assertIn('userfolder', codes(str(home / 'Downloads' / 'MA2')))
        long = 'C:\\Games\\' + 'x' * 160
        with mock.patch.object(precheck, 'long_paths_enabled', lambda: False): self.assertIn('long', codes(long))
        with mock.patch.object(precheck, 'long_paths_enabled', lambda: True): self.assertNotIn('long', codes(long))

    def test_extras_never_stop_the_core_install(self):
        d = self.tmp / 'core'
        lines = []
        inst = install.Installer(None, d, textures=['4x'], videos=True, log=lines.append); inst.state = {'done': []}
        inst.work.mkdir(parents=True)
        def no_python(): raise install.Stop('The enhanced content needs Python 3.11, 3.12 or 3.13 (64-bit) for PyTorch.')
        with mock.patch.object(install, 'enhancer_python', no_python), mock.patch.object(install, 'find_visual_studio', lambda c=None: ('v', 'c', 'n', Path('vs'))), \
             mock.patch.object(inst, 'need_disc', lambda: False):
            inst.check()
        self.assertEqual(inst.opts, {'textures': [], 'videos': False}); self.assertTrue(any('will not be made' in n for n in inst.notes), inst.notes)
        self.assertFalse(inst.wanted('textures-4x')); self.assertFalse(inst.wanted('videos')); self.assertTrue(inst.wanted('build'))
        # space: enough for the game but not for the extras -> the extras are unticked, the game goes on
        inst = install.Installer(None, d, textures=['4x'], log=lines.append); inst.state = {'done': []}
        free = install.needed_space({'textures': [], 'videos': False}, set()) + 1 * G
        with mock.patch.object(install, 'enhancer_python', lambda: sys.executable), mock.patch.object(install, 'nvidia_gpu', lambda: 'RTX'), \
             mock.patch.object(install, 'find_visual_studio', lambda c=None: ('v', 'c', 'n', Path('vs'))), mock.patch.object(inst, 'need_disc', lambda: False), \
             mock.patch.object(install.shutil, 'disk_usage', lambda p: mock.Mock(free=free)):
            inst.check()
        self.assertEqual(inst.opts['textures'], [])
        # an add-ons-only run stops instead (the extras are all it does)
        inst = install.Installer(None, d, videos=True, log=lines.append); inst.state = {'done': [], 'addons_only': True}
        with mock.patch.object(install, 'enhancer_python', no_python), self.assertRaises(install.Stop): inst.check()

    # 10. pip and the models
    def test_enhancer_setup_isolation(self):
        text = (KIT / 'enhancer' / 'setup.py').read_text(encoding='utf-8')
        for need in ('"--isolated"', '"--no-cache-dir"', '"--disable-pip-version-check"', '"--timeout", "120"', '"--retries", "10"',
                     '"PIP_", "CONDA_", "PYENV_"', 'PIP_CACHE_DIR', 'system_site_packages=False', '*PIP, "--version"', 'expected or recorded.get(relative'):
            self.assertIn(need, text)
        self.assertNotIn('"-m", "pip", "install"', text)
        overlay = WORK.parent / 'enhancer-overlay' / 'setup.py'                     # (maintainer tree: the kit chain rebuilds from it)
        if overlay.is_file(): self.assertEqual(text, overlay.read_text(encoding='utf-8'))
        sys.path.insert(0, str(KIT / 'enhancer'))
        import importlib; setup = importlib.import_module('setup')
        with mock.patch.dict(os.environ, {'PIP_INDEX_URL': 'http://x', 'CONDA_PREFIX': 'c', 'PYENV_VERSION': 'p'}):
            env = setup.child_env()
        self.assertFalse([k for k in env if k.upper().startswith(('PIP_INDEX', 'CONDA_', 'PYENV_'))])
        self.assertTrue(env['TEMP'].startswith(str(setup.ROOT)))
        shutil.rmtree(setup.ROOT / 'tmp', ignore_errors=True)

    # 11. Play button
    def test_launch_advice(self):
        self.assertIn('Smart App Control', install.launch_advice('x/MechAssault 2.exe', OSError(None, 'x', None, 4551)))
        self.assertIn('4551', install.launch_advice('x/MechAssault 2.exe', OSError(None, 'x', None, 4551)))
        for code in (225, 2):
            self.assertIn('quarantined or removed', install.launch_advice('x/MechAssault 2.exe', OSError(None, 'x', None, code)))

    # 12. done markers
    def test_done_step_whose_output_is_gone_runs_again(self):
        d = self.tmp / 'stale'; (d / 'game_files').mkdir(parents=True)
        inst = install.Installer(None, d, log=lambda s: None); inst.work.mkdir()
        inst.state = {'done': ['check', 'disc', 'extract', 'analyze', 'build']}
        self.assertEqual(inst.stale_done(), ['extract', 'build'])                         # no ma2.xbe, no built exe
        self.assertEqual(inst.state['done'], ['check', 'disc', 'analyze'])
        (d / 'game_files' / install.RELEASE['xbe']).write_text('x')
        exe = inst.build_root() / 'build' / 'mechassault2_recomp.exe'; exe.parent.mkdir(parents=True); exe.write_text('x')
        inst.state = {'done': ['extract', 'build']}
        self.assertEqual(inst.stale_done(), [])
        self.assertIn('self.stale_done()', inspect.getsource(install.Installer.run_steps))

    # 13. numeric codes first
    def test_numeric_classification(self):
        for text, kind in (('Fehler [WinError 5] Zugriff verweigert', 'access_denied'), ('[WinError 32] ...', 'access_denied'),
                           ('Erreur [WinError 112]', 'disk_full'), ('[Errno 28] ...', 'disk_full'), ('fatal error C1060: ...', 'out_of_memory'),
                           ('[WinError 1455]', 'out_of_memory'), ('[WinError 11001] getaddrinfo', 'network'), ('LINK : fatal error LNK1168', 'access_denied'),
                           ('[WinError 4551]', 'access_denied')):
            self.assertEqual(install.classify([text]), kind, text)
        self.assertEqual(install.classify(['[WinError 10060]'], 'CMake + Ninja'), 'build_failed')
        self.assertEqual(install.classify(['error LNK1117: syntax error'], 'CMake + Ninja'), 'compile_error')


if __name__ == '__main__':
    unittest.main(verbosity=2)

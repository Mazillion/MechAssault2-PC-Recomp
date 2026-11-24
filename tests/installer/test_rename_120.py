"""1.2.0 rename (replaced in place): the installer is "MechAssault 2 Expanded Installer.exe" and new installs go to a
"MechAssault 2 Expanded" folder. Existing installs keep their folder; an installer copy under the earlier name is replaced
on install, update and repair; the Start menu's Update MechAssault 2 follows the new copy; the game's "Update now" starts
the new copy, else the earlier one. Nothing here touches the real %LOCALAPPDATA%, Start menu or C:\\Games: every path is
in tests/tmp (SystemDrive and LOCALAPPDATA point there).
Run with the kit's Python:  python -B test_rename_120.py
"""
import json, os, re, shutil, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
REPO = WORK.parent
sys.path.insert(0, str(REPO / 'kit' / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ren-local-', dir=str(WORK / 'tmp'))
for v in ('MA2_SELF_UPDATE', 'MA2_SELF_UPDATE_TEST', 'MA2_INSTALLER_EXE', 'MA2_SELF_UPDATE_DEST', 'MA2_HANDOFF_WAIT'):
    os.environ.pop(v, None)
import selfupdate, install, precheck                         # noqa: E402

NEW, OLD = selfupdate.INSTALLER_COPY, selfupdate.OLD_INSTALLER_COPIES[0]


def fake_install(dest, shortcuts=()):
    for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
        p = Path(dest) / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'x')
    (Path(dest) / precheck.STAMP).write_text(json.dumps({'kit_id': 'k', 'public_version': '1.2.0', 'shortcuts': list(shortcuts)}))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ren-', dir=str(WORK / 'tmp')))
        self.drive = self.tmp / 'C'; self.drive.mkdir()
        self.local = self.tmp / 'local'; self.local.mkdir()
        self.env = mock.patch.dict(os.environ, {'SystemDrive': str(self.drive), 'LOCALAPPDATA': str(self.local),
                                                'MA2_SHORTCUT_DIR': str(self.tmp / 'lnk')})
        self.env.start()
        os.environ.pop('MA2_INSTALLER_EXE', None)
        selfupdate._switched_off = False
        self.lines = []

    def tearDown(self):
        mock.patch.stopall()
        selfupdate._switched_off = False
        shutil.rmtree(self.tmp, ignore_errors=True)


class Names(Base):
    def test_the_constants(self):
        self.assertEqual(NEW, 'MechAssault 2 Expanded Installer.exe')
        self.assertEqual(OLD, 'MechAssault 2 Recomp Installer.exe')
        self.assertEqual(install.INSTALLER_COPY, NEW)
        self.assertEqual(tuple(install.OLD_INSTALLER_COPIES), (OLD,))
        self.assertEqual(precheck.FOLDER_NAME, 'MechAssault 2 Expanded')

    def test_window_titles_and_stub(self):
        import gui, gui_legacy
        self.assertEqual(gui.TITLE, 'MechAssault 2 Expanded Installer')
        self.assertEqual(gui_legacy.TITLE, 'MechAssault 2 Expanded Installer')
        stub = (REPO / 'installer-exe' / 'stub.cpp').read_text(encoding='utf-8')
        self.assertIn('static const wchar_t *TITLE = L"MechAssault 2 Expanded Installer";', stub)
        self.assertNotIn('Recomp Installer"', stub)
        self.assertIn('L"Local\\\\MechAssault2RecompInstaller"', stub, 'the mutex keeps its name: an old and a new installer never run together')
        rc = (REPO / 'installer-exe' / 'stub.rc').read_text(encoding='utf-8')
        self.assertIn('VALUE "OriginalFilename", "%s"' % NEW, rc)
        self.assertIn('VALUE "FileDescription", "MechAssault 2 Expanded Installer"', rc)
        self.assertIn('<description>MechAssault 2 Expanded Installer</description>',
                      (REPO / 'installer-exe' / 'stub.manifest').read_text(encoding='utf-8'))

    def test_build_outputs_use_the_constant(self):
        sys.path.insert(0, str(REPO / 'tools')); sys.path.insert(0, str(REPO / 'installer-exe'))
        try:
            import build_release, pack_installer
        finally:
            sys.path.pop(0); sys.path.pop(0)
        self.assertEqual(build_release.INSTALLER_EXE, NEW)
        if not os.environ.get('MA2_PACK_OUT'): self.assertEqual(pack_installer.OUT.name, NEW)
        # SHA256SUMS.txt lists the names GitHub publishes (sha256sum -c on the downloaded files)
        self.assertEqual(build_release.published_name(NEW), 'MechAssault.2.Expanded.Installer.exe')
        self.assertEqual(build_release.published_name(NEW), selfupdate.github_asset_name(NEW))
        zipname = build_release.launcher_zip_name('1.2.0')
        self.assertEqual(build_release.published_name(zipname), zipname)
        src = (REPO / 'tools' / 'build_release.py').read_text(encoding='utf-8')
        self.assertIn("published_name(p.name)) for p in (out, zpath)]", src)

    def test_no_old_name_left_in_player_text(self):
        for rel in ('README.md', 'kit/defaults/README.txt', 'kit/defaults/Docs/Install guide.txt', 'kit/installer/gui.py',
                    'kit/installer/gui_legacy.py', 'kit/installer/install.py', 'kit/installer/precheck.py', 'recomplauncher.json'):
            self.assertNotIn('Recomp Installer.exe', (REPO / rel).read_text(encoding='utf-8'), rel)
        log = (REPO / 'kit' / 'defaults' / 'Docs' / 'Changelog.txt').read_text(encoding='utf-8')
        self.assertIn('(look for "MechAssault 2: Lone Wolf Expanded")', ' '.join(log.split()))
        self.assertNotIn('Expanded (PC Recomp)', ' '.join(log.split()))


class DefaultFolder(Base):
    def test_new_install_goes_to_the_expanded_folder(self):
        self.assertEqual(precheck.default_install_folder(), str(self.drive / 'Games' / 'MechAssault 2 Expanded'))

    def test_fallback_when_games_cannot_be_made(self):
        (self.drive / 'Games').write_text('a file, not a folder')
        self.assertEqual(precheck.default_install_folder(), str(self.local / 'Programs' / 'MechAssault 2 Expanded'))

    def test_an_install_in_the_old_default_folder_is_found_without_installer_json(self):
        old = self.drive / 'Games' / 'MechAssault 2'; fake_install(old)
        self.assertIsNone(precheck.remembered_install())                     # installer.json is gone
        self.assertEqual(precheck.old_default_install(), str(old))
        self.assertEqual(precheck.default_install_folder(), str(old), 'opens on the existing install: it is never moved')
        self.assertEqual(precheck.default_install_folder(keep_old=False), str(self.drive / 'Games' / 'MechAssault 2 Expanded'))

    def test_an_unfinished_install_in_the_old_programs_folder_is_found(self):
        old = self.local / 'Programs' / 'MechAssault 2'; (old / '_work').mkdir(parents=True)
        (old / '_work' / 'state.json').write_text('{"done": []}')
        self.assertEqual(precheck.default_install_folder(), str(old))

    def test_an_old_named_folder_that_is_not_an_install_is_ignored(self):
        (self.drive / 'Games' / 'MechAssault 2').mkdir(parents=True)
        (self.drive / 'Games' / 'MechAssault 2' / 'notes.txt').write_text('mine')
        self.assertIsNone(precheck.old_default_install())
        self.assertEqual(precheck.default_install_folder(), str(self.drive / 'Games' / 'MechAssault 2 Expanded'))

    def test_remembered_install_still_wins(self):
        mine = self.tmp / 'D' / 'Games' / 'MechAssault 2'; fake_install(mine)
        (self.local / 'MechAssault2').mkdir(parents=True)
        (self.local / 'MechAssault2' / 'installer.json').write_text(json.dumps({'dest': str(mine)}))
        self.assertEqual(precheck.remembered_install(), str(mine))
        self.assertTrue(precheck.is_install(mine), 'an existing install in a folder named "MechAssault 2" is still an install')

    def test_suggestions_use_the_new_name(self):
        full = self.tmp / 'full'; full.mkdir(); (full / 'x.txt').write_text('x')
        r = precheck.check_folder(str(full), precheck.GB, False, install.KIT)
        self.assertEqual(r.suggest, str(full / 'MechAssault 2 Expanded'))
        r = precheck.check_folder('MechAssault 2', precheck.GB, False, install.KIT)
        self.assertIn('C:\\Games\\MechAssault 2 Expanded', ' '.join(f.text for f in r.findings))

    def test_install_somewhere_else_skips_the_old_install(self):
        src = (REPO / 'kit' / 'installer' / 'gui_legacy.py').read_text(encoding='utf-8')
        self.assertIn("('Install somewhere else', lambda: self.choose(precheck.default_install_folder(keep_old=False), 'install')", src)
        src = (REPO / 'kit' / 'installer' / 'gui.py').read_text(encoding='utf-8')
        self.assertIn("self.dest = precheck.default_install_folder(keep_old=False)", src)
        self.assertEqual(src.count("'MechAssault 2')"), 0, 'the picker uses precheck.FOLDER_NAME')


class OldCopy(Base):
    def inst(self, dest=None, shortcuts=('start',), stamp_shortcuts=()):
        dest = dest or self.drive / 'Games' / 'MechAssault 2'; fake_install(dest, stamp_shortcuts)
        i = install.Installer(None, dest, shortcuts=list(shortcuts), log=self.lines.append)
        i.state = {'done': []}
        return i

    def runner(self, data=b'MZ new installer'):
        exe = self.tmp / 'dl' / NEW; exe.parent.mkdir(parents=True, exist_ok=True); exe.write_bytes(data)
        os.environ['MA2_INSTALLER_EXE'] = str(exe)
        return exe

    def test_update_replaces_the_old_named_copy(self):
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ old installer'); self.runner()
        self.assertTrue(i.copy_installer())
        self.assertEqual(i.retire_old_copies(), [OLD])
        self.assertFalse((i.dest / OLD).exists())
        self.assertEqual((i.dest / NEW).read_bytes(), b'MZ new installer')
        self.assertTrue(any('%s removed' % OLD in l for l in self.lines), self.lines)

    def test_old_copy_stays_when_the_new_one_could_not_be_written(self):
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ old'); self.runner()
        with mock.patch.object(install.shutil, 'copy2', lambda *a, **k: (_ for _ in ()).throw(OSError(5, 'denied'))):
            with self.assertRaises(OSError): i.copy_installer()
        self.assertEqual(i.retire_old_copies(), [])
        self.assertTrue((i.dest / OLD).is_file(), 'still the one copy "Update now" can start')

    def test_old_copy_stays_when_self_update_is_off(self):
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ old'); self.runner()
        with mock.patch.dict(os.environ, {'MA2_SELF_UPDATE': '0'}):
            self.assertFalse(i.copy_installer()); self.assertEqual(i.retire_old_copies(), [])
        self.assertTrue((i.dest / OLD).is_file())

    def test_running_old_copy_is_not_deleted(self):
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ old'); (i.dest / NEW).write_bytes(b'MZ new')
        os.environ['MA2_INSTALLER_EXE'] = str(i.dest / OLD)
        self.assertEqual(i.retire_old_copies(), [])
        self.assertTrue((i.dest / OLD).is_file())

    def test_wired_after_the_copy_and_fails_safe(self):
        src = (REPO / 'kit' / 'installer' / 'install.py').read_text(encoding='utf-8')
        a, b = src.index('try: self.copy_installer()'), src.index('try: self.retire_old_copies()')
        self.assertLess(a, b); self.assertLess(b, src.index('self.make_shortcuts()', b))
        self.assertIn("except Exception as e: self.log('  (an older installer copy was not removed: %s)' % e)", src)

    def test_update_shortcut_follows_the_new_copy(self):
        made = []
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ old'); self.runner()
        i.copy_installer(); i.retire_old_copies()
        with mock.patch.object(install, 'create_shortcut', lambda lnk, target, *a, **k: made.append((Path(lnk).name, Path(target)))):
            i.make_shortcuts()
        self.assertIn((install.UPDATE_LNK, i.dest / NEW), made)

    def test_update_shortcut_follows_even_without_the_start_tick(self):
        lnk = self.tmp / 'lnk' / 'start' / install.UPDATE_LNK; lnk.parent.mkdir(parents=True); lnk.write_bytes(b'old lnk')
        made = []
        i = self.inst(shortcuts=(), stamp_shortcuts=[str(lnk)]); (i.dest / OLD).write_bytes(b'MZ old'); self.runner()
        i.copy_installer(); i.retire_old_copies()
        with mock.patch.object(install, 'create_shortcut', lambda l, target, *a, **k: made.append((Path(l), Path(target)))):
            i.make_shortcuts()
        self.assertEqual(made, [(lnk, i.dest / NEW)])
        self.assertIn(str(lnk), i.state['shortcuts'])

    def test_manifest_and_repair_leave_both_copies_out(self):
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ old'); (i.dest / NEW).write_bytes(b'MZ new')
        files = install.manifest_files(i.dest)
        self.assertNotIn(OLD, files); self.assertNotIn(NEW, files); self.assertIn('MechAssault 2.exe', files)

    def test_uninstall_from_an_old_named_running_copy(self):
        i = self.inst(); (i.dest / OLD).write_bytes(b'MZ')
        os.environ['MA2_INSTALLER_EXE'] = str(i.dest / OLD)
        self.assertEqual(install.running_copy(i.dest), (i.dest / OLD).resolve())


class Launcher(unittest.TestCase):
    def test_manifest(self):
        m = json.loads((REPO / 'recomplauncher.json').read_text(encoding='utf-8'))
        self.assertEqual(m['prepare']['program'], [NEW])
        self.assertEqual(m['start']['program'], ['MechAssault 2 Expanded/MechAssault 2.exe', 'MechAssault 2/MechAssault 2.exe', NEW])
        self.assertEqual(m['start']['env']['MA2_RECOMPLAUNCHER_DEST'], '{install}/MechAssault 2 Expanded')
        self.assertEqual(m['folders']['game'], [{'path': '{install}/MechAssault 2 Expanded'}])
        self.assertNotIn('{install}/MechAssault 2/', json.dumps(m))

    def test_check_launcher_zip(self):
        import zipfile
        sys.path.insert(0, str(REPO / 'tools'))
        try:
            import build_release
        finally:
            sys.path.pop(0)
        tmp = Path(tempfile.mkdtemp(prefix='ma2ren-zip-', dir=str(WORK / 'tmp')))
        try:
            exe = tmp / NEW; exe.write_bytes(b'MZ' + os.urandom(64))
            prof = (REPO / 'recomplauncher.json').read_text(encoding='utf-8')
            def zipped(name, text):
                z = tmp / name
                with zipfile.ZipFile(z, 'w') as zf:
                    zf.writestr('recomplauncher.json', text); zf.write(exe, NEW)
                return z
            self.assertEqual(build_release.check_launcher_zip(zipped('good.zip', prof), exe), [])
            bad = prof.replace('"MA2_RECOMPLAUNCHER_DEST": "{install}/MechAssault 2 Expanded"', '"MA2_RECOMPLAUNCHER_DEST": "{install}/MechAssault 2"')
            self.assertIn('MA2_RECOMPLAUNCHER_DEST', ' '.join(build_release.check_launcher_zip(zipped('bad1.zip', bad), exe)))
            bad = prof.replace('["MechAssault 2 Expanded/MechAssault 2.exe", ', '[')
            self.assertIn('start.program must start with', ' '.join(build_release.check_launcher_zip(zipped('bad2.zip', bad), exe)))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class Game(unittest.TestCase):
    def test_update_now_falls_back_to_the_old_copy_name(self):
        h = (REPO / 'kit' / 'source' / 'game' / 'src' / 'pc_messages.h').read_text(encoding='utf-8')
        names = re.findall(r'L"([^"]+Installer\.exe)"', h[h.index('MA2_INSTALLER_COPIES['):])
        self.assertEqual(names, [NEW, OLD], 'the game tries the new name first, then the earlier one (same as selfupdate.py)')
        cpp = (REPO / 'kit' / 'source' / 'game' / 'src' / 'pc_native_settings.cpp').read_text(encoding='utf-8')
        self.assertNotIn('Installer.exe', cpp, 'no literal: installer_copy_path() picks the name')
        self.assertIn('!installer_copy_path().empty()', cpp[cpp.index('static int update_action_locked'):])
        upd = cpp[cpp.index('static void update_now_locked'):]
        self.assertIn('exe=installer_copy_path()', upd[:upd.index('CreateProcessW')])
        self.assertLess(upd.index('if(exe.empty())'), upd.index('CreateProcessW'))
        # the C unit test of ma2_pick_installer_copy runs in test_replies_120.py (report_replies_test.c)
        t = (REPO / 'tests' / 'game' / 'report_replies_test.c').read_text(encoding='utf-8')
        self.assertIn('ma2_pick_installer_copy(only_old) == 1', t)


if __name__ == '__main__':
    unittest.main(verbosity=2)

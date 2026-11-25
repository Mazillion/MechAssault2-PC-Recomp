"""1.1.7: the game's update line (kit/source/game/src/pc_update_check.h, pc_native_settings.cpp) and its setting.

    py -3.13 tests/installer/test_update_notice.py

Checks the compiled-in relay address against kit/installer/data/report.json (one address for every client), the
[Update] CheckForUpdates default and the installer's add-only settings merge, the docs, the player-facing text, and
builds + runs the C unit test tests/game/update_check_test.c when the MSVC x64 tools are found (else that test is skipped).
Nothing is sent anywhere: no test here opens a network connection.
"""
import json, os, re, subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
KIT = REPO / 'kit'
SRC = KIT / 'source' / 'game' / 'src'
sys.path.insert(0, str(KIT / 'installer'))
import precheck                                                         # noqa: E402

NO_WINDOW = 0x08000000
LINE = 'Version %s is available. Run the installer to update.'
OLD_DEFAULTS_116 = '[Display]\nShowFPS=0\n\n[Audio]\nEnabled=1\nVolume=100\n\n[Bots]\nBotRecorder=0\n'


def header():
    return (SRC / 'pc_update_check.h').read_text(encoding='utf-8')


def vcvars():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if not vswhere.exists():
        return None
    try:
        out = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                              '-property', 'installationPath'], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=60).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    bat = Path(out.splitlines()[0]) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat' if out else None
    return bat if bat and bat.exists() else None


class UpdateNotice(unittest.TestCase):
    def test_relay_address_is_the_report_address(self):
        m = re.search(r'#define MA2_UPDATE_RELAY "([^"]*)"', header())
        self.assertIsNotNone(m)
        relay = json.loads((KIT / 'installer' / 'data' / 'report.json').read_text(encoding='utf-8')).get('relay_url') or ''
        self.assertEqual(m.group(1), relay.rstrip('/'), 'pc_update_check.h MA2_UPDATE_RELAY must equal report.json relay_url')
        self.assertTrue(m.group(1).startswith('https://'))

    def test_request_carries_only_the_version(self):
        h = header()
        self.assertIn('L"MechAssault2-Recomp/%hs", version', h)
        self.assertIn('L"Accept: application/json\\r\\n"', h)
        self.assertIn('WINHTTP_NO_REQUEST_DATA, 0, 0', h)                     # a GET with no body
        self.assertIn('L"GET"', h)
        self.assertNotIn('X-MA2-', h)                                         # none of the report headers
        self.assertIn('WINHTTP_OPTION_REDIRECT_POLICY_NEVER', h)
        self.assertIn('WINHTTP_DISABLE_COOKIES', h)
        self.assertIn('#define MA2_UPDATE_TIMEOUT_MS 5000', h)

    def test_off_switches_and_test_runs(self):
        h = header()
        self.assertIn('pc_switch_ex("RECOMP_UPDATE_CHECK", "Update", "CheckForUpdates", 1, &source)', h)
        # off = no thread: the switch is read before CreateThread
        self.assertLess(h.index('"CheckForUpdates"'), h.index('CreateThread('))
        # harness runs never ask the compiled-in relay
        self.assertRegex(h, r'else if \(getenv\("RECOMP_SAVE_DIR"\)\)')

    def test_game_starts_the_check_once_and_shows_one_line(self):
        cpp = (SRC / 'pc_native_settings.cpp').read_text(encoding='utf-8')
        self.assertEqual(cpp.count('ma2_update_check_start('), 1)
        # 1.2.0: the line is replaced by Messages (an update message "Version %s is available" with Update now), main menu only
        self.assertNotIn('draw_update_line(', cpp)
        self.assertIn('msg_text_update_title="Version %s is available"', cpp)
        self.assertIn('menu_kind==0x481B70', cpp[cpp.index('static bool envelope_visible'):])

    def test_default_on_in_the_defaults_ini(self):
        keys, order = precheck._ini_keys((KIT / 'defaults' / 'pc_settings.ini').read_text(encoding='utf-8'))
        self.assertIn('Update', order)
        self.assertEqual(keys['Update']['checkforupdates'].strip(), 'CheckForUpdates=1')

    def test_update_merge_adds_the_key_and_keeps_a_players_choice(self):
        defaults = (KIT / 'defaults' / 'pc_settings.ini').read_text(encoding='utf-8')
        cut = defaults.index('\n[Update]')
        old = defaults[:cut] + '\n'                                             # the 1.1.6 defaults: no [Update]
        self.assertNotIn('CheckForUpdates', old)
        new, added = precheck.merge_ini(defaults, old)
        self.assertEqual(added, ['Update.checkforupdates'])
        self.assertEqual(new, old + '[Update]\nCheckForUpdates=1\n')           # appended, nothing else touched
        new, added = precheck.merge_ini(defaults, old + '\n[Update]\nCheckForUpdates=0\n')
        self.assertEqual(added, [])
        self.assertIn('CheckForUpdates=0', new)
        self.assertNotIn('CheckForUpdates=1', new)

    def test_docs_name_the_switch_and_what_is_sent(self):
        first = (KIT / 'defaults' / 'Docs' / 'First run and settings.txt').read_text(encoding='utf-8')
        self.assertIn('[Update] CheckForUpdates=1', first)
        readme = (KIT / 'defaults' / 'README.txt').read_text(encoding='utf-8')
        self.assertIn("It sends only the game's own version number", readme)
        self.assertIn('[Update] CheckForUpdates=0', readme)
        for p in (KIT / 'defaults' / 'pc_settings.ini', KIT / 'defaults' / 'README.txt',
                  KIT / 'defaults' / 'Docs' / 'First run and settings.txt', SRC / 'pc_update_check.h'):
            p.read_bytes().decode('ascii')                                     # plain ASCII like the rest

    def test_c_unit_test(self):
        bat = vcvars()
        if not bat:
            self.skipTest('MSVC x64 tools not found')
        with tempfile.TemporaryDirectory() as out:
            exe = Path(out) / 'update_check_test.exe'
            script = Path(out) / 'build.cmd'
            script.write_text('@echo off\r\ncall "%s" >nul || exit /b 9\r\n'
                              'cl /nologo /W3 /WX /D_CRT_SECURE_NO_WARNINGS tests\\game\\update_check_test.c /Fo"%s\\\\" /Fe"%s"\r\n'
                              % (bat, out, exe), encoding='ascii')
            r = subprocess.run(['cmd', '/d', '/c', str(script)], cwd=REPO, capture_output=True, text=True, creationflags=NO_WINDOW,
                               timeout=600)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
            r = subprocess.run([str(exe)], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertRegex(r.stdout, r'update_check_test: \d+ checks, 0 failures')


if __name__ == '__main__':
    unittest.main(verbosity=2)

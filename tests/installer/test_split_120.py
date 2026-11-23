"""1.2.0 split screen: on by default with a hidden off switch, quick join, the guest's pad routing, the bot room next to the
humans, F6 (camera slot restore guard) in the patch set, and the Add Guest prompt kept as the game's own text.

Builds and runs the C unit test tests/game/split_slots_test.c when the MSVC x64 tools are found (else that test is skipped).
"""
import json, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
KIT = REPO / 'kit'
SRC = KIT / 'source' / 'game' / 'src'
RT = KIT / 'source' / 'runtime' / 'src'
sys.path.insert(0, str(HERE))
from test_update_notice import vcvars, NO_WINDOW                         # noqa: E402


def edits(name):
    ops = json.loads((KIT / 'patches' / 'diff' / name).read_text(encoding='utf-8'))
    return ''.join(l for op in ops for l in (op[2] if len(op) > 2 and isinstance(op[2], list) else []) if isinstance(l, str))


class SplitScreen(unittest.TestCase):
    def test_on_by_default_with_off_switch(self):
        src = (SRC / 'main.c').read_text(encoding='utf-8')
        self.assertIn('pc_setting(ini, "Debug", "SplitScreen", "RECOMP_SPLITSCREEN", 1, 0, 1, TRUE);', src)
        self.assertNotIn('SplitScreenKeyboard', src)                       # P1 = keyboard and mouse is the rule, not an option
        rt = (RT / 'input' / 'xinput_device.c').read_text(encoding='utf-8')
        self.assertIn('getenv("RECOMP_SPLITSCREEN")', rt)

    def test_no_settings_menu_toggle(self):
        for name in ('pc_native_settings.cpp', 'pc_settings.cpp'):
            p = SRC / name
            if p.is_file():
                self.assertNotIn('SplitScreen', p.read_text(encoding='utf-8', errors='replace'), name)

    def test_pause_add_guest_stays_off(self):
        ui = (SRC / 'bots' / 'ma2_bots_ui.c').read_text(encoding='utf-8')
        self.assertIn('600, FEATURE_MIDMATCH_GUEST, "pause Add Guest"', ui)
        self.assertIn('int ma2_menu_policy_splitscreen(void) { return !feature_off(FEATURE_MIDMATCH_GUEST); }', ui)

    def test_quick_join_uses_the_game_path(self):
        ui = (SRC / 'bots' / 'ma2_bots_ui.c').read_text(encoding='utf-8')
        self.assertIn('RECOMP_ABI_CALL(0x00224753u, sub_00224753)', ui)    # what the Add Guest dialog completes into
        self.assertIn('#define SPLIT_PLACE_GUEST 0', ui)                     # the guest keeps the game's own team

    def test_add_guest_prompt_is_retail_text(self):
        src = (SRC / 'pc_prompts.cpp').read_text(encoding='utf-8')
        i = src.index('new player\'s controller')
        self.assertIn("return original;", src[i:i + 500])

    def test_f6_patch_and_switch(self):
        text = edits('recomp_0037.c.json')
        self.assertIn('if (ma2_cam_slot_restore_ok(edi)) {', text)
        self.assertIn('eax = ma2_cam_slot_uid(eax);', text)
        src = (SRC / 'recomp_manual.c').read_text(encoding='utf-8')
        self.assertIn('dot_env("RECOMP_FIX_CAM_SLOT_RESTORE", 1)', src)

    def test_c_unit_test(self):
        bat = vcvars()
        if not bat:
            self.skipTest('MSVC x64 tools not found')
        with tempfile.TemporaryDirectory() as out:
            exe = Path(out) / 'split_slots_test.exe'
            script = Path(out) / 'build.cmd'
            script.write_text('@echo off\r\ncall "%s" >nul || exit /b 9\r\n'
                              'cl /nologo /W3 /WX tests\\game\\split_slots_test.c /Fo"%s\\\\" /Fe"%s"\r\n' % (bat, out, exe),
                              encoding='ascii')
            r = subprocess.run(['cmd', '/d', '/c', str(script)], cwd=REPO, capture_output=True, text=True, creationflags=NO_WINDOW,
                               timeout=600)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
            r = subprocess.run([str(exe)], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertRegex(r.stdout, r'split_slots_test: \d+ checks, 0 failed')


if __name__ == '__main__':
    unittest.main(verbosity=2)

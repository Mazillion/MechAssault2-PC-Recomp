"""1.1.8 DotCarry (BUG-001): continuous damage as at 30 fps at any frame rate.

Checks the patch set calls the helpers on all three paths of the hit resolver sub_000DC60D, the off switch, and builds + runs
the C unit test tests/game/dot_carry_test.c when the MSVC x64 tools are found (else that test is skipped).
"""
import json, re, subprocess, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
KIT = REPO / 'kit'
SRC = KIT / 'source' / 'game' / 'src'
sys.path.insert(0, str(HERE))
from test_update_notice import vcvars, NO_WINDOW                         # noqa: E402


class DotCarry(unittest.TestCase):
    def edits(self):
        ops = json.loads((KIT / 'patches' / 'diff' / 'recomp_0022.c.json').read_text(encoding='utf-8'))
        return ''.join(l for op in ops for l in (op[2] if len(op) > 2 and isinstance(op[2], list) else []))

    def test_all_three_paths_patched(self):
        text = self.edits()
        self.assertIn('ma2_dot_rate((int32_t)eax, edi, MEM32(ebp + -4), 0x1B8u)', text)       # collapsing buildings
        self.assertIn('ma2_dot_rate((int32_t)eax, edi, MEM32(ebp + -4), 0x1B0u)', text)       # destroyed props
        self.assertEqual(text.count('ma2_dot_beam('), 2 + 1, text)                             # beam call, record, declaration

    def test_switch_and_helpers(self):
        src = (SRC / 'recomp_manual.c').read_text(encoding='utf-8')
        self.assertIn('dot_env("RECOMP_DOT_CARRY", 1)', src)
        for name in ('int32_t ma2_dot_rate(', 'int32_t ma2_dot_beam('):
            self.assertIn(name, src)
        (SRC / 'ma2_dot_carry.h').read_bytes().decode('ascii')

    def test_c_unit_test(self):
        bat = vcvars()
        if not bat:
            self.skipTest('MSVC x64 tools not found')
        with tempfile.TemporaryDirectory() as out:
            exe = Path(out) / 'dot_carry_test.exe'
            script = Path(out) / 'build.cmd'
            script.write_text('@echo off\r\ncall "%s" >nul || exit /b 9\r\n'
                              'cl /nologo /W3 /WX tests\\game\\dot_carry_test.c /Fo"%s\\\\" /Fe"%s"\r\n' % (bat, out, exe),
                              encoding='ascii')
            r = subprocess.run(['cmd', '/d', '/c', str(script)], cwd=REPO, capture_output=True, text=True, creationflags=NO_WINDOW,
                               timeout=600)
            self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
            r = subprocess.run([str(exe)], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout)
            self.assertRegex(r.stdout, r'dot_carry_test: \d+ checks, 0 failed')


if __name__ == '__main__':
    unittest.main(verbosity=2)

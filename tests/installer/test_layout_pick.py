"""linux-proton-121: the Wine/Proton-only free-base search of the memory layout (runtime/src/kernel/xbox_layout_pick.h).

Checks that xbox_memory_layout.c tries the search only under Wine and only before the 256 MB fixed base, and builds + runs
tests/game/layout_pick_test.c when the MSVC x64 tools are found (else that test is skipped).
"""
import re, sys, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
KERNEL = REPO / 'kit' / 'source' / 'runtime' / 'src' / 'kernel'
sys.path.insert(0, str(HERE))
from test_power_118 import PowerThrottle                                   # noqa: E402  (its c_test builds and runs a tests/game C test)


class LayoutPick(unittest.TestCase):
    def test_search_is_wine_only_and_after_the_fixed_low_bases(self):
        src = (KERNEL / 'xbox_memory_layout.c').read_text(encoding='utf-8')
        self.assertIn('#include "xbox_layout_pick.h"', src)
        m = re.search(r'if \(try_bases\[i\] == 0x10000000 && xbox_layout_under_wine\(\)\)', src)
        self.assertTrue(m, 'the search must be gated on Wine and placed at the 256 MB entry')
        bases = re.search(r'try_bases\[\] = \{(.*?)\};', src, re.S).group(1)
        self.assertLess(bases.index('XBOX_BASE_ADDRESS'), bases.index('0x10000000'))     # the fixed low bases still come first
        hdr = (KERNEL / 'xbox_layout_pick.h').read_text(encoding='utf-8')
        self.assertIn('"wine_get_version"', hdr)

    def test_isinf_rewrite_is_wine_only(self):
        """Proton's vkd3d-shader stops on isinf(); under Wine only, the generated fog test is rewritten to the same bit test."""
        src = (KERNEL / 'nv2a_shader_cache.c').read_text(encoding='utf-8')
        self.assertIn('if(!shader_under_wine() || length<sizeof(from))return NULL;', src)
        self.assertIn('"isinf(distance)"', src)
        self.assertIn('((asuint(distance)&0x7FFFFFFFu)==0x7F800000u)', src)
        self.assertIn('shader_path(source,length,entry,profile,path)', src)        # the cache key stays the original source
        vsh = (KERNEL / 'nv2a_gpu_vsh.c').read_text(encoding='utf-8')
        self.assertEqual(vsh.count('isinf('), 1, 'a new isinf() use needs the Wine rewrite too')
        self.assertIn('isinf(distance)', vsh)

    def test_c_unit_test(self):
        PowerThrottle.c_test(self, 'layout_pick_test')


if __name__ == '__main__':
    unittest.main(verbosity=2)

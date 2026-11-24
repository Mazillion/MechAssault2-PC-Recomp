/* linux-proton-121 unit test: the free-base search used under Wine/Proton (kit/source/runtime/src/kernel/xbox_layout_pick.h).
 * Build from the repo root: cl /nologo /W3 /WX tests\game\layout_pick_test.c /Fe:layout_pick_test.exe
 * Address-space shapes seen under Wine 9 and Proton 11, a crowded one with no answer, and the real process it runs in
 * (on Windows xbox_layout_under_wine() must be false, so the game never uses the search there). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include "../../kit/source/runtime/src/kernel/xbox_layout_pick.h"

static int checks, failures;
static void check(int ok, const char *what) { checks++; if (!ok) { failures++; printf("FAIL %s\n", what); } }

#define MB (1024ull * 1024ull)
#define RAM (64ull * MB)

static int windows_free(const XboxBusyRange *busy, size_t n, uint64_t base)
{
    XboxBusyRange w[6];
    size_t nw = xbox_layout_windows(RAM, 28, w);
    for (size_t i = 0; i < nw; i++)
        if (xbox_layout_overlaps(busy, n, base + w[i].lo, base + w[i].hi, NULL)) return 0;
    return 1;
}

int main(void)
{
    const XboxBusyRange teb = {0x7FDE0000ull, 0x7FFE1000ull};
    {   /* nothing mapped: the Xbox's own base */
        check(xbox_layout_pick_base(NULL, 0, RAM, 28) == 0x10000, "empty: 0x10000");
    }
    {   /* Wine 9 shape: low memory free, Wine's TEB/PEB reservation: still 0x10000, like the fixed list */
        XboxBusyRange b[] = {teb};
        check(xbox_layout_pick_base(b, 1, RAM, 28) == 0x10000, "wine 9: 0x10000");
    }
    {   /* Proton 11 shape: low memory taken up to 96 MB, TEB/PEB, Wine 11's block at 4 GB */
        XboxBusyRange b[] = {{0x10000, 0x6000000}, teb, {0x100000000ull, 0x100100000ull}};
        uint64_t base = xbox_layout_pick_base(b, 3, RAM, 28);
        check(base == 0x6000000, "proton 11: first free base after the low use");
        check(windows_free(b, 3, base), "proton 11: every window free");
        check(base + RAM * 29 <= teb.lo, "proton 11: mirrors end below the TEB area");
        check(!windows_free(b, 3, 0x10000000), "proton 11: the old 256 MB fallback collides (the bug)");
    }
    {   /* a busy page in the middle of where the mirrors would go: the base moves past it */
        XboxBusyRange b[] = {{0x10000, 0x200000}, {0x1000000, 0x1010000}, teb};
        uint64_t base = xbox_layout_pick_base(b, 3, RAM, 28);
        check(base != 0 && windows_free(b, 3, base), "hole: a base with every window free");
        check(base == 0x1010000, "hole: the base moves just past the busy page");
    }
    {   /* low memory taken up to 192 MB: the mirrors cannot fit below the TEB area: no base (caller goes on as before) */
        XboxBusyRange b[] = {{0x10000, 0xC000000}, teb};
        check(xbox_layout_pick_base(b, 2, RAM, 28) == 0, "crowded: no base");
    }
    {   /* a larger map (devkit 128 MB): mirrors are cut at the contiguous window, so a base is still found */
        check(xbox_layout_pick_base(NULL, 0, 128ull * MB, 28) == 0x10000, "128 MB map: 0x10000");
    }
    {   /* this process */
        int wine = xbox_layout_under_wine();
        uintptr_t real = xbox_layout_scan_base(RAM, 28);
        printf("this process: wine=%d scan base=%p\n", wine, (void *)real);
        check(real == 0 || (real % XBOX_PICK_ALIGN) == 0, "real: aligned or none");
        if (!getenv("LAYOUT_PICK_EXPECT_WINE")) check(!wine, "real: not Wine (run on Windows)");
        else check(wine, "real: Wine");
    }
    printf("layout_pick_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

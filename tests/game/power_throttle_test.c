/* 1.1.8 unit test: the EcoQoS opt-out's Windows 10 fallback (kit/source/game/src/pc_power_throttle.h).
 * Build from the repo root: cl /nologo /W3 /WX tests\game\power_throttle_test.c /Fe:power_throttle_test.exe
 * Also checks the real call on this PC: it must not fail (Windows 11: both controls; Windows 10: speed only). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../kit/source/game/src/pc_power_throttle.h"

static int checks, failures;
static void check(int ok, const char *what) { checks++; if (!ok) { failures++; printf("FAIL %s\n", what); } }

/* a fake Windows: accepts the masks in ok[], records the masks tried */
typedef struct { unsigned long ok[4]; int n_ok; unsigned long tried[4]; int n; } fake;
static int fake_set(unsigned long mask, void *ctx, unsigned long *err)
{
    fake *f = (fake *)ctx;
    f->tried[f->n++] = mask;
    for (int i = 0; i < f->n_ok; i++) if (f->ok[i] == mask) return 1;
    *err = 87;
    return 0;
}

static int real_set(unsigned long mask, void *ctx, unsigned long *err)
{
    PROCESS_POWER_THROTTLING_STATE t;
    (void)ctx;
    memset(&t, 0, sizeof t);
    t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    t.ControlMask = mask;
    t.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &t, sizeof t)) return 1;
    *err = GetLastError();
    return 0;
}

int main(void)
{
    unsigned long e1, e2;
    {   /* Windows 11: both at once, one call */
        fake f = {{PC_PT_EXECUTION_SPEED | PC_PT_IGNORE_TIMER_RESOLUTION, PC_PT_EXECUTION_SPEED}, 2};
        int r = pc_power_optout(fake_set, &f, &e1, &e2);
        check(r == 2 && f.n == 1 && f.tried[0] == (PC_PT_EXECUTION_SPEED | PC_PT_IGNORE_TIMER_RESOLUTION) && !e1, "Windows 11: both, one call");
        check(!strcmp(pc_power_optout_text(r), "speed and timer resolution"), "Windows 11 text");
    }
    {   /* Windows 10: the pair refused (87), then speed only */
        fake f = {{PC_PT_EXECUTION_SPEED}, 1};
        int r = pc_power_optout(fake_set, &f, &e1, &e2);
        check(r == 1 && f.n == 2 && f.tried[1] == PC_PT_EXECUTION_SPEED && e1 == 87 && !e2, "Windows 10: speed only after 87");
        check(!strcmp(pc_power_optout_text(r), "speed only (Windows 10)"), "Windows 10 text");
    }
    {   /* neither: both errors kept */
        fake f = {{0}, 0};
        int r = pc_power_optout(fake_set, &f, &e1, &e2);
        check(r == 0 && f.n == 2 && e1 == 87 && e2 == 87, "neither: failed with both errors");
    }
    {   /* this PC */
        int r = pc_power_optout(real_set, NULL, &e1, &e2);
        printf("this PC: %s\n", pc_power_optout_text(r));
        check(r != 0, "the real call does not fail");
    }
    printf("power_throttle_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}

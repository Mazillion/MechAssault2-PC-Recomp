/* 1.1.8 unit test: the power state on [POWER] / [HEALTH] (kit/source/runtime/src/platform/pc_power_state.h).
 * Build from the repo root: cl /nologo /W3 /WX tests\game\power_state_test.c /Fe:power_state_test.exe
 * Names for the known GUIDs, a custom plan as its GUID only, every call failing = "unknown", and a real read on this PC. */
#include "../../kit/source/runtime/src/platform/pc_power_state.h"

static int checks, failures;
static void check(int ok, const char *what) { checks++; if (!ok) { failures++; printf("FAIL %s\n", what); } }

int main(void)
{
    char t[64], line[160];
    {
        GUID zero = {0}, eff = {0x961CC777, 0x2547, 0x4F9D, {0x81, 0x74, 0x7D, 0x86, 0x18, 0x1B, 0x8A, 0x7A}};
        GUID best = {0xDED574B5, 0x45A0, 0x4F42, {0x87, 0x37, 0x46, 0x34, 0x5C, 0x09, 0xC2, 0x38}}, other = {1, 2, 3, {4}};
        check(!strcmp(pc_power_mode_name(&zero), "balanced"), "mode: zero GUID = balanced");
        check(!strcmp(pc_power_mode_name(&eff), "best-efficiency"), "mode: best efficiency");
        check(!strcmp(pc_power_mode_name(&best), "best-performance"), "mode: best performance");
        check(!strcmp(pc_power_mode_name(&other), "unknown"), "mode: other GUID = unknown");
    }
    {
        GUID bal = {0x381B4222, 0xF694, 0x41F0, {0x96, 0x85, 0xFF, 0x5B, 0xB2, 0x60, 0xDF, 0x2E}};
        GUID hp = {0x8C5E7FDA, 0xE8BF, 0x4A96, {0x9A, 0x85, 0xA6, 0xE2, 0x3A, 0x8C, 0x63, 0x5C}};
        GUID ult = {0xE9A42B02, 0xD5DF, 0x448D, {0xAA, 0x00, 0x03, 0xF1, 0x47, 0x49, 0xEB, 0x61}};
        GUID sav = {0xA1841308, 0x3541, 0x4FAB, {0xBC, 0x81, 0xF7, 0x15, 0x56, 0xF2, 0x0B, 0x4A}};
        GUID oem = {0x12345678, 0x9ABC, 0xDEF0, {1, 2, 3, 4, 5, 6, 7, 8}};
        pc_power_plan_name(&bal, t, sizeof t); check(!strcmp(t, "balanced"), "plan: balanced");
        pc_power_plan_name(&hp, t, sizeof t); check(!strcmp(t, "high-performance"), "plan: high performance");
        pc_power_plan_name(&ult, t, sizeof t); check(!strcmp(t, "ultimate"), "plan: ultimate");
        pc_power_plan_name(&sav, t, sizeof t); check(!strcmp(t, "power-saver"), "plan: power saver");
        pc_power_plan_name(&oem, t, sizeof t); check(!strcmp(t, "custom-12345678-9ABC-DEF0-0102-030405060708"), "plan: custom = its GUID only");
    }
    {   /* every Windows call fails: unknown, nothing waits */
        pc_power_state s;
        pc_power_state_test(1);
        pc_power_state_read(&s); pc_power_state_text(&s, line, sizeof line);
        check(!strcmp(line, "pmode=unknown pplan=unknown mhz=unknown"), "failure: all unknown");
        pc_power_state_test(0);
    }
    {   /* this PC */
        pc_power_state s;
        pc_power_state_read(&s); pc_power_state_text(&s, line, sizeof line);
        printf("this PC: %s cpus=%u\n", line, s.ncpu);
        check(strcmp(s.plan, "unknown") != 0 && s.ncpu > 0 && s.mhz_max >= s.mhz_min && s.mhz_avg >= s.mhz_min, "this PC: plan and clocks read");
    }
    printf("power_state_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}

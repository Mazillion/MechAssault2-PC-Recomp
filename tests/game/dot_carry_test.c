/* 1.1.8 unit test: DotCarry (kit/source/game/src/ma2_dot_carry.h, BUG-001). Damage per second of every retail continuous
 * damage rate at 30, 45, 60, 120, 144 and 240 fps, before (the game's own _ftol(dt * dps) every frame) and after, with even
 * frames and with +-15% frame time jitter. After = the 30 fps game's damage at every frame rate, never more.
 * Build from the repo root: cl /nologo /W3 /WX tests\game\dot_carry_test.c /Fe:dot_carry_test.exe
 * (tests/installer/test_dot_carry.py builds and runs it when the MSVC tools are found). Prints the table with -v. */
#include <stdio.h>
#include <string.h>
#include "../../kit/source/game/src/ma2_dot_carry.h"

static int checks, failures;
static void check(int ok, const char *what) { checks++; if (!ok) { failures++; printf("FAIL %s\n", what); } }

/* the game's x87 path: double dt * float dps, _ftol truncates toward zero */
static int32_t game_ftol(double dt, double dps) { return (int32_t)(dt * (double)(float)dps); }

static unsigned rng = 12345u;
static double jitter(void) { rng = rng * 1103515245u + 12345u; return 0.85 + 0.30 * ((rng >> 8) & 0xFFFF) / 65535.0; }

/* seconds of continuous contact at fps; returns damage per second before (carry off) and after (carry on) */
static double g_elapsed, g_sum1;
static void run(double fps, double dps, int jit, double secs, double *before, double *after)
{
    static ma2_dot_slot tab[MA2_DOT_SLOTS];
    memset(tab, 0, sizeof tab);
    double t = 100.0, sum0 = 0, sum1 = 0, end = t + secs;
    while (t < end) {
        double dt = (1.0 / fps) * (jit ? jitter() : 1.0);
        t += dt;
        int32_t orig = game_ftol(dt, dps);
        sum0 += orig;
        sum1 += ma2_dot_carry_step(tab, 0x1234560u, 0x2345670u, t, dt, dps, orig);
    }
    g_elapsed = t - 100.0; g_sum1 = sum1;
    *before = sum0 / secs; *after = sum1 / secs;
}

int main(int argc, char **argv)
{
    const double rates[] = {20, 25, 44, 100, 150, 250, 300, 350, 500, 800, 2000};   /* every retail DoT rate (e2-dot-audit) */
    const double fpss[] = {30, 45, 60, 120, 144, 240};
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    char what[160];
    if (verbose) printf("dps    fps   before  after  (30 fps game: floor(dps/30)*30)\n");
    for (unsigned i = 0; i < sizeof rates / sizeof *rates; i++) {
        double want = (double)(int)(rates[i] / 30.0 + 1e-9) * 30.0;      /* the 30 fps game */
        for (unsigned j = 0; j < sizeof fpss / sizeof *fpss; j++) {
            for (int jit = 0; jit < 2; jit++) {
                double b, a;
                run(fpss[j], rates[i], jit, 60.0, &b, &a);
                if (verbose && !jit) printf("%-6.0f %-5.0f %-7.1f %-6.1f (%.0f)\n", rates[i], fpss[j], b, a, want);
                /* the 30 fps game over the same contact time: one tick at once, then one every 1/30 s; never more */
                double per = want / 30.0, ticks = g_sum1 / (per > 0 ? per : 1), most = 1.0 + floor(g_elapsed * 30.0 + 1e-6);
                snprintf(what, sizeof what, "dps %.0f at %.0f fps%s: %.2f per second (%.0f ticks in %.3f s), 30 fps game %.0f",
                         rates[i], fpss[j], jit ? " (jitter)" : "", a, ticks, g_elapsed, want);
                check(per > 0 ? (ticks <= most && ticks >= most - 1.0) : g_sum1 == 0, what);
            }
        }
    }
    {   /* the final boss EyeBeam (44) did nothing at 60 and 144 fps before */
        double b, a;
        run(60, 44, 0, 10, &b, &a);   check(b == 0 && a >= 29.9 && a <= 30.2, "EyeBeam 60 fps: 0 before, 30 after");
        run(144, 44, 0, 10, &b, &a);  check(b == 0 && a >= 29.9 && a <= 30.2, "EyeBeam 144 fps: 0 before, 30 after");
        run(30, 44, 0, 10, &b, &a);   check(b >= 29.9 && b <= 30.2 && a >= 29.9 && a <= 30.2, "EyeBeam 30 fps: 30 before and after");
    }
    {   /* a second hit of the same pair in the same frame: the same damage, no second tick */
        ma2_dot_slot tab[MA2_DOT_SLOTS]; memset(tab, 0, sizeof tab);
        int32_t a = ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 10.0, 1.0 / 30, 44, 1);
        int32_t b = ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 10.0, 1.0 / 30, 44, 1);
        int32_t c = ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 10.0 + 1.0 / 30, 1.0 / 30, 44, 1);
        check(a == 1 && b == 1 && c == 1, "same frame twice: same damage; next frame: one tick");
        /* other pairs keep their own count */
        int32_t d = ma2_dot_carry_step(tab, 1u << 16, 3u << 16, 10.0 + 1.0 / 30, 1.0 / 30, 44, 1);
        check(d == 1, "a new victim: a tick at once");
    }
    {   /* bad inputs: the game's own value */
        ma2_dot_slot tab[MA2_DOT_SLOTS]; memset(tab, 0, sizeof tab);
        check(ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 1.0, 0.0, 44, 7) == 7, "dt 0: game value");
        check(ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 1.0, 1.0, 44, 7) == 7, "dt 1 s: game value");
        check(ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 1.0, 0.01, -5, 7) == 7, "negative rate: game value");
        check(ma2_dot_carry_step(tab, 0, 2u << 16, 1.0, 0.01, 44, 7) == 7, "no source: game value");
    }
    {   /* a stall (0.4 s frame): at most 8 ticks, never more than 30 fps would do over that time (12) */
        ma2_dot_slot tab[MA2_DOT_SLOTS]; memset(tab, 0, sizeof tab);
        ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 5.0, 1.0 / 144, 500, 3);
        int32_t r = ma2_dot_carry_step(tab, 1u << 16, 2u << 16, 5.4, 0.4, 500, 200);
        check(r <= 8 * 16 && r > 0, "stall: capped ticks");
    }
    {   /* many pairs at once (more than the table): no crash, every pair still damaged at the 30 fps rate overall */
        static ma2_dot_slot tab[MA2_DOT_SLOTS]; memset(tab, 0, sizeof tab);
        double t = 1.0, sum = 0; int frames = 144 * 10;
        for (int f = 0; f < frames; f++) {
            t += 1.0 / 144;
            for (uint32_t v = 0; v < 64; v++) sum += ma2_dot_carry_step(tab, 0x100000u, 0x200000u + v * 0x400u, t, 1.0 / 144, 100, 0);
        }
        snprintf(what, sizeof what, "64 victims of one source at 144 fps: %.1f per victim per second (30 fps game 90)", sum / 64 / 10);
        check(sum / 64 / 10 >= 89.0 && sum / 64 / 10 <= 91.0, what);
    }
    printf("dot_carry_test: %d checks, %d failed\n", checks, failures);
    return failures != 0;
}

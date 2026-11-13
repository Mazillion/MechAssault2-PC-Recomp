/* T1-3/T1-4 FastCode tier switch (pipeline Track 1), read by every
 * recomp_fast_*.c (tools/fastcode.py). Build-time only:
 *   0 = tier 1 (plain guest pointer + flat ICALL probe; the T1-2 code),
 *   1 = tier 2: guest GPRs as locals, published/reloaded at every sync line.
 * The runtime fallback stays [Cpu] FastCode=0 / RECOMP_FASTCODE=0 (originals). */
#ifndef RECOMP_FAST_REGS
#define RECOMP_FAST_REGS 1
#endif

/* C7 diagnostics: one shared per-match counter table for every bots-batch piece.
 *
 *   c7_on(piece, env, ini_key, def)  the piece's switch: env RECOMP_* overrides
 *                                    pc_settings.ini [Bots] <ini_key> (ini_key may be NULL);
 *                                    logged once per match as "[C7] <piece> switch=on|off".
 *   c7_add(piece, counter, n)        adds n to a per-match counter.
 *   c7_max(piece, counter, v)        keeps the largest v seen this match.
 *   t0 = c7_t0(); ...; c7_ms(piece, counter, t0)
 *                                    QPC timer: adds the elapsed ms to this frame's total;
 *                                    the dump prints <counter>_ms (average per frame over
 *                                    the frames the bots ran) and <counter>_max (worst frame).
 *
 * At match end (the bots' level is freed, or the process exits) one line per piece:
 *   [C7] <piece> t=<match time> frames=<n> <counter>=<value> ...
 * Core calls c7_match_begin / c7_frame / c7_match_end; pieces only call the four above.
 * Game thread only (no locking). Piece and counter names should be string literals. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int    c7_on(const char *piece, const char *env, const char *ini_key, int def);
/* W-09b: 1 when bot diagnostics are printed ([C7], [CAM-WATCH], [BOT-NOBOARD], [BOT-TRACE], [BOT-ROOFS]):
 * RECOMP_BOT_DIAG=1 or [Bots] BotLog=1. Counters and the recorder stay live either way. */
int    c7_log_on(void);
void   c7_add(const char *piece, const char *counter, double n);
void   c7_max(const char *piece, const char *counter, double v);
double c7_t0(void);
void   c7_ms(const char *piece, const char *counter, double t0);

void   c7_match_begin(double t);    /* core: bots created for a new match */
void   c7_frame(double t);          /* core: once per bot step (before the bots run) */
void   c7_match_end(void);          /* core: level freed; dumps and clears (no-op when empty) */

#ifdef __cplusplus
}
#endif

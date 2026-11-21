/* MA2 Report.exe: sends a problem report for MechAssault 2 (1.1.6).

   The game's crash and freeze boxes, the in-game "Report a problem" page and the Start menu entry
   "Report a problem (MechAssault 2)" all use this one small program. It runs when the game cannot:
   after a crash, while the game is frozen, or when it does not start at all.

   What it does: collects the same files as the installer's report (kit/installer/precheck.py
   collect_diagnostics with the report settings, kit/installer/report.py make_and_send): system.txt,
   recomp-install.json, pc_settings.ini, the newest three game logs, the newest installer logs, the
   newest crash or freeze dump (15 MB at most) and the newest bot recording (its first block and the newest blocks that fit under the 20 MB report limit, after the other members and 1 MB headroom: about the last 2-3 minutes of a busy match). Every
   text file has the Windows user name, machine name, profile folders and e-mail addresses removed
   (precheck.redact); the dump and the bot recording have them overwritten with the same number of
   'x' bytes (precheck.scrub_bytes), so the files keep their layout. It writes one zip to
   %LOCALAPPDATA%\MechAssault2\Diagnostics first (the player's own copy), then, on the player's click,
   sends it to the project's relay with the wire protocol of kit/installer/report.py. Nothing is ever
   sent without that click (or, for the in-game page, the page's Send).

   Command line:
     (none)                         the "Report a problem" window
     --crash <dump> <log> [--sig S] the game crashed: the window with the crash text
     --hang <dump> <log>            the game froze: Send report, Close the game (exit 10), Keep waiting (exit 11)
     --game --text-file F --result F [--log L] [--ini I] [--kind K] [--sig S]
                                    the in-game page: no window; collect, save and send, then write the
                                    outcome to the result file (UTF-8 key=value lines); exit 0 sent, 1 saved only
     --can-send                     exit 0 when this build has a relay address, 3 when it only saves
   Optional with any mode: --log <game log> --ini <pc_settings.ini> (the game passes its own).

   The relay address is compiled in (relay.h, written by build_report.cmd from MA2_RELAY_URL, which
   tools/build_release.py takes from kit/installer/data/report.json); empty = save only.
   For tests only: MA2_REPORT_RELAY (https, or http on this PC), MA2_REPORT_DATA_DIR (instead of
   %LOCALAPPDATA%\MechAssault2), MA2_REPORT_INSTALL_DIR (instead of this exe's folder), MA2_REPORT_HIDDEN=1
   (the window is created but never shown, for a test driver), MA2_REPORT_FAULT=collect|zip|send (fails at that
   step), MA2_REPORT_TIMEOUT_MS (network timeout). Each also reads as RECOMP_REPORT_* (the game passes RECOMP_* variables through).

   System libraries only: kernel32, user32, gdi32, comctl32, shell32, winhttp, advapi32 (and dxgi /
   cabinet loaded at run time). */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <dxgi.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "version.h"
#include "relay.h"

#ifndef MA2_RELAY_URL
#define MA2_RELAY_URL ""
#endif
#define ISSUES_URL "https://github.com/Mazillion/MechAssault2-PC-Recomp/issues"
#define MAX_TEXT 2000
#define MAX_SUMMARY (16 * 1024)
#define MAX_ZIP (20u * 1024 * 1024)
#define DUMP_LIMIT (15u * 1024 * 1024)
#define BOTREC_HEADROOM (1u * 1024 * 1024)   /* 1.1.6: the recording gets what is left under MAX_ZIP minus this (zip overhead, system.txt, text) */
#define LOG_LIMIT (6u * 1024 * 1024)       /* the newest part of a very long log */
#define NET_TIMEOUT_MS 60000

/* ============================================================== small buffers */
typedef struct { char *p; size_t n, cap; } Buf;
static void buf_reserve(Buf *b, size_t extra) {
    if (b->n + extra + 1 <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + extra + 1) c *= 2;
    char *q = (char *)realloc(b->p, c);
    if (!q) { fprintf(stderr, "out of memory\n"); ExitProcess(2); }
    b->p = q; b->cap = c;
}
static void buf_add(Buf *b, const void *d, size_t n) { buf_reserve(b, n); memcpy(b->p + b->n, d, n); b->n += n; b->p[b->n] = 0; }
static void buf_adds(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_addc(Buf *b, char c) { buf_add(b, &c, 1); }
static void buf_addf(Buf *b, const char *fmt, ...) {
    char tmp[2048]; va_list ap; va_start(ap, fmt); int n = _vsnprintf(tmp, sizeof tmp - 1, fmt, ap); va_end(ap);
    if (n < 0) n = (int)sizeof tmp - 1;
    tmp[n] = 0; buf_add(b, tmp, (size_t)n);
}
static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static void w2u(const wchar_t *w, char *out, int size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, size, NULL, NULL)) out[0] = 0;
}
static void u2w(const char *u, wchar_t *out, int size) {
    if (!MultiByteToWideChar(CP_UTF8, 0, u, -1, out, size)) out[0] = 0;
}

/* Test and support switches: MA2_REPORT_<name>, else RECOMP_REPORT_<name> (the game passes RECOMP_* through). */
static int env_get(const char *name, wchar_t *out, DWORD size) {
    wchar_t key[96]; out[0] = 0;
    swprintf(key, 96, L"MA2_REPORT_%hs", name);
    DWORD n = GetEnvironmentVariableW(key, out, size);
    if (!n || n >= size) { swprintf(key, 96, L"RECOMP_REPORT_%hs", name); n = GetEnvironmentVariableW(key, out, size); }
    if (!n || n >= size) { out[0] = 0; return 0; }
    return 1;
}
static int fault_at(const char *step) {
    wchar_t v[64]; char u[64];
    if (!env_get("FAULT", v, 64)) return 0;
    w2u(v, u, sizeof u);
    return strstr(u, step) != NULL;
}

/* ============================================================== privacy */
/* The values a report must not carry: the profile folders, the user, machine and domain names
   (precheck.private_names). Paths match as paths, names only as whole words. */
typedef struct { char u8[MAX_PATH * 3]; wchar_t w[MAX_PATH]; char ansi[MAX_PATH * 2]; const char *label; int path; } Secret;
static Secret g_secret[6]; static int g_nsecret;
static void secrets_init(void) {
    static const char *vars[6] = { "LOCALAPPDATA", "APPDATA", "USERPROFILE", "USERNAME", "COMPUTERNAME", "USERDOMAIN" };
    static const char *labels[6] = { "%LOCALAPPDATA%", "%APPDATA%", "%USERPROFILE%", "%USERNAME%", "%COMPUTERNAME%", "%USERDOMAIN%" };
    g_nsecret = 0;
    for (int i = 0; i < 6; i++) {
        wchar_t v[MAX_PATH]; wchar_t wv[64];
        swprintf(wv, 64, L"%hs", vars[i]);
        DWORD n = GetEnvironmentVariableW(wv, v, MAX_PATH);
        if (!n || n >= MAX_PATH) continue;
        while (n && (v[n - 1] == L'\\' || v[n - 1] == L'/')) v[--n] = 0;
        if ((int)n <= (i < 3 ? 3 : 2)) continue;
        int dup = 0;
        for (int j = 0; j < g_nsecret; j++) if (!_wcsicmp(g_secret[j].w, v)) dup = 1;
        if (dup) continue;
        Secret *s = &g_secret[g_nsecret++];
        wcscpy(s->w, v); w2u(v, s->u8, sizeof s->u8);
        if (!WideCharToMultiByte(CP_ACP, 0, v, -1, s->ansi, sizeof s->ansi, NULL, NULL)) s->ansi[0] = 0;
        s->label = labels[i]; s->path = i < 3;
    }
    /* longest first, so a profile folder goes before the user name inside it */
    for (int i = 0; i < g_nsecret; i++) for (int j = i + 1; j < g_nsecret; j++)
        if (strlen(g_secret[j].u8) > strlen(g_secret[i].u8)) { Secret t = g_secret[i]; g_secret[i] = g_secret[j]; g_secret[j] = t; }
}

static int lower_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int word_byte(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80; }
static int alnum_byte(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
/* case-insensitive (ASCII letters; other bytes exact) compare of n bytes */
static int ci_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (lower_ascii((unsigned char)a[i]) != lower_ascii((unsigned char)b[i])) return 0;
    return 1;
}

/* One pass: every match of `pat` (case-insensitive) that `accept` allows is replaced by `rep`. */
typedef int (*Accept)(const char *s, size_t n, size_t at, size_t len);
static void replace_all(Buf *b, const char *pat, const char *rep, Accept accept) {
    size_t pl = strlen(pat);
    if (!pl || b->n < pl) return;
    Buf o = { 0 };
    size_t i = 0, last = 0;
    while (i + pl <= b->n) {
        if (lower_ascii((unsigned char)b->p[i]) == lower_ascii((unsigned char)pat[0]) && ci_eq(b->p + i, pat, pl) && (!accept || accept(b->p, b->n, i, pl))) {
            buf_add(&o, b->p + last, i - last); buf_adds(&o, rep);
            i += pl; last = i;
        } else i++;
    }
    if (!o.p) return;
    buf_add(&o, b->p + last, b->n - last);
    buf_free(b); *b = o;
}
static int accept_path(const char *s, size_t n, size_t at, size_t len) {
    (void)at;
    if (at + len == n) return 1;
    char c = s[at + len];
    return c == '\\' || c == '/' || c == '"' || c == '\'' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static int accept_word(const char *s, size_t n, size_t at, size_t len) {
    if (at > 0 && (word_byte((unsigned char)s[at - 1]) || s[at - 1] == '%')) return 0;
    if (at + len < n && (word_byte((unsigned char)s[at + len]) || s[at + len] == '%')) return 0;
    return 1;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
/* e-mail at the '@' at index at: returns start/end of the address, or 0 */
static int email_at(const char *s, size_t n, size_t at, size_t *a0, size_t *a1) {
    size_t a = at, e = at + 1;
    while (a > 0) {
        unsigned char c = (unsigned char)s[a - 1];
        if (alnum_byte(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-') a--; else break;
    }
    if (a == at) return 0;
    size_t d = e;
    while (d < n && (alnum_byte((unsigned char)s[d]) || s[d] == '-')) d++;
    if (d == e || d >= n || s[d] != '.') return 0;
    size_t t = d;
    while (t < n && (alnum_byte((unsigned char)s[t]) || s[t] == '.' || s[t] == '-')) t++;
    /* must end in at least two letters */
    size_t k = t, letters = 0;
    while (k > d && ((s[k - 1] >= 'A' && s[k - 1] <= 'Z') || (s[k - 1] >= 'a' && s[k - 1] <= 'z'))) { k--; letters++; }
    if (letters < 2) {
        /* backtrack to the last run of >= 2 letters */
        while (t > d + 1 && !(((s[t - 1] | 32) >= 'a' && (s[t - 1] | 32) <= 'z') && ((s[t - 2] | 32) >= 'a' && (s[t - 2] | 32) <= 'z'))) t--;
        if (t <= d + 1) return 0;
    }
    *a0 = a; *a1 = t; return 1;
}

/* Text redaction (precheck.redact): disc key lines, profile folders, any X:\Users\<name>, the user /
   machine / domain names as whole words, network host names, URL user info and queries, credential-
   looking values, e-mail addresses. */
static void redact(Buf *b) {
    if (!b->p) return;
    /* 1. disc certificate key lines and the studio build path (older installer logs) */
    {
        static const char *keys[] = { "LAN Key", "Signature Key", "Alternate Signature Key", "Debug Path" };
        Buf o = { 0 }; size_t i = 0; int changed = 0;
        while (i < b->n) {
            size_t e = i; while (e < b->n && b->p[e] != '\n') e++;
            size_t k = i; while (k < e && (b->p[k] == ' ' || b->p[k] == '\t')) k++;
            int drop = 0;
            for (int j = 0; j < 4 && !drop; j++) {
                size_t kl = strlen(keys[j]);
                if (k + kl <= e && ci_eq(b->p + k, keys[j], kl)) {
                    size_t c = k + kl; if (c < e && (b->p[c] == 's' || b->p[c] == 'S')) c++;
                    while (c < e && (b->p[c] == ' ' || b->p[c] == '\t')) c++;
                    drop = c < e && b->p[c] == ':';
                }
            }
            if (drop) { buf_adds(&o, "  [line removed]"); changed = 1; } else buf_add(&o, b->p + i, e - i);
            if (e < b->n) buf_addc(&o, '\n');
            i = e + 1;
        }
        if (changed) { buf_free(b); *b = o; } else buf_free(&o);
    }
    /* 2. the profile folders (longest first) */
    for (int i = 0; i < g_nsecret; i++) if (g_secret[i].path) replace_all(b, g_secret[i].u8, g_secret[i].label, accept_path);
    /* 3. any other X:\Users\<name> (another account, a moved folder): keep the drive, drop the name */
    {
        Buf o = { 0 }; size_t i = 0, last = 0; int changed = 0;
        while (i + 9 <= b->n) {
            const char *p = b->p + i;
            int start_ok = i == 0 || !word_byte((unsigned char)p[-1]);
            if (start_ok && ((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && (p[2] == '\\' || p[2] == '/') &&
                ci_eq(p + 3, "users", 5) && (p[8] == '\\' || p[8] == '/')) {
                size_t s = i + 9, e = s;
                while (e < b->n && b->p[e] != '\\' && b->p[e] != '/' && b->p[e] != '"' && b->p[e] != '\r' && b->p[e] != '\n') e++;
                int skip = e == s;
                if (!skip && e - s >= 6 && ci_eq(b->p + s, "public", 6) && (s + 6 == e)) skip = 1;
                if (!skip && b->p[s] == '<') skip = 1;
                if (!skip && b->p[s] == '%') skip = 1;
                if (!skip) {
                    buf_add(&o, b->p + last, s - last); buf_adds(&o, "<user>");
                    last = i = e; changed = 1; continue;
                }
            }
            i++;
        }
        if (changed) { buf_add(&o, b->p + last, b->n - last); buf_free(b); *b = o; } else buf_free(&o);
    }
    /* 4. the user, machine and domain names as whole words */
    for (int i = 0; i < g_nsecret; i++) if (!g_secret[i].path) replace_all(b, g_secret[i].u8, g_secret[i].label, accept_word);
    /* 5. \\server\share -> \\<host>\share; URL user info and queries; credentials; e-mail addresses */
    {
        static const char *cred[] = { "password", "passwd", "pwd", "token", "secret", "api_key", "api-key", "apikey", "access_key",
                                      "access-key", "accesskey", "client_secret", "client-secret", "clientsecret", "sig", "authorization" };
        Buf o = { 0 }; size_t i = 0, last = 0; int changed = 0;
        while (i < b->n) {
            const char *p = b->p + i; size_t left = b->n - i;
            /* UNC host */
            if (left > 3 && p[0] == '\\' && p[1] == '\\' && !((p[2] == '?' || p[2] == '.') && p[3] == '\\')) {
                size_t e = i + 2;
                while (e < b->n && !strchr("\\/ \t\r\n\"'<>|", b->p[e])) e++;
                if (e > i + 2 && e < b->n && b->p[e] == '\\' && !(e - (i + 2) == 6 && ci_eq(b->p + i + 2, "<host>", 6))) {
                    buf_add(&o, b->p + last, i + 2 - last); buf_adds(&o, "<host>"); last = i = e; changed = 1; continue;
                }
            }
            /* URL: user:password@ and ?query */
            if (left > 8 && (ci_eq(p, "http://", 7) || ci_eq(p, "https://", 8) || ci_eq(p, "ftp://", 6)) && (i == 0 || !word_byte((unsigned char)p[-1]))) {
                size_t s = i + (ci_eq(p, "https://", 8) ? 8 : ci_eq(p, "http://", 7) ? 7 : 6), e = s, at = 0, colon = 0;
                while (e < b->n && !strchr(" \t\r\n\"'#", b->p[e])) {
                    if (b->p[e] == '@' && !at) at = e;
                    if (b->p[e] == ':' && !colon && !at) colon = e;
                    if (b->p[e] == '/' && !at) break;
                    e++;
                }
                if (at && colon && colon > s) {
                    buf_add(&o, b->p + last, s - last); buf_adds(&o, "<hidden>"); last = at; changed = 1;
                }
                size_t q = at ? at : s;
                while (q < b->n && !strchr(" \t\r\n\"'#?", b->p[q])) q++;
                if (q < b->n && b->p[q] == '?') {
                    size_t qe = q + 1;
                    while (qe < b->n && !strchr(" \t\r\n\"'#", b->p[qe])) qe++;
                    if (!(qe - q == 8 && ci_eq(b->p + q, "?<query>", 8))) {
                        buf_add(&o, b->p + last, q + 1 - last); buf_adds(&o, "<query>"); last = i = qe; changed = 1; continue;
                    }
                }
                i = q; continue;
            }
            /* key=value credentials */
            if (i == 0 || !word_byte((unsigned char)p[-1])) {
                int hit = 0;
                for (int k = 0; k < (int)(sizeof cred / sizeof *cred) && !hit; k++) {
                    size_t kl = strlen(cred[k]);
                    if (left > kl && ci_eq(p, cred[k], kl)) {
                        size_t c = i + kl;
                        while (c < b->n && (b->p[c] == ' ' || b->p[c] == '\t')) c++;
                        if (c < b->n && (b->p[c] == '=' || b->p[c] == ':')) {
                            size_t v = c + 1;
                            while (v < b->n && (b->p[v] == ' ' || b->p[v] == '\t')) v++;
                            size_t ve = v;
                            while (ve < b->n && !strchr(" \t\r\n&;,\"'", b->p[ve])) ve++;
                            if (ve > v && !(ve - v == 8 && ci_eq(b->p + v, "<hidden>", 8)) && strncmp(b->p + v, "%", 1)) {
                                buf_add(&o, b->p + last, i + kl - last); buf_adds(&o, "=<hidden>"); last = i = ve; changed = hit = 1;
                            }
                        }
                    }
                }
                if (hit) continue;
            }
            /* e-mail */
            if (p[0] == '@') {
                size_t a0, a1;
                if (email_at(b->p, b->n, i, &a0, &a1) && a0 >= last) {
                    buf_add(&o, b->p + last, a0 - last); buf_adds(&o, "<email>"); last = i = a1; changed = 1; continue;
                }
            }
            i++;
        }
        if (changed) { buf_add(&o, b->p + last, b->n - last); buf_free(b); *b = o; } else buf_free(&o);
    }
}

/* ---- binary scrub (precheck.scrub_bytes): same length, offsets stay valid ---- */
static int g_scrubbed;
static void x_fill(unsigned char *p, size_t n, int wide) {
    for (size_t i = 0; i < n; i++) p[i] = wide ? ((i & 1) ? 0 : 'x') : 'x';
}
static int wlower(int c) { wchar_t w = (wchar_t)c; CharLowerBuffW(&w, 1); return w; }
/* 8-bit: ASCII-insensitive; whole-word check for names */
static void scrub_bytes8(unsigned char *d, size_t n, const char *val, int bounded) {
    size_t l = strlen(val);
    if (!l || n < l) return;
    for (size_t i = 0; i + l <= n; i++) {
        if (lower_ascii(d[i]) != lower_ascii((unsigned char)val[0]) || !ci_eq((const char *)d + i, val, l)) continue;
        if (bounded && ((i > 0 && alnum_byte(d[i - 1])) || (i + l < n && alnum_byte(d[i + l])))) continue;
        x_fill(d + i, l, 0); g_scrubbed++; i += l - 1;
    }
}
static void scrub_bytes16(unsigned char *d, size_t n, const wchar_t *val, int bounded) {
    size_t l = wcslen(val);
    if (!l || n < l * 2) return;
    for (size_t i = 0; i + l * 2 <= n; i++) {          /* any byte offset, like the Python regex */
        size_t k = 0;
        for (; k < l; k++) {
            int c = d[i + 2 * k] | (d[i + 2 * k + 1] << 8);
            if (c != val[k] && wlower(c) != wlower(val[k])) break;
        }
        if (k < l) continue;
        if (bounded) {
            if (i >= 2 && d[i - 1] == 0 && alnum_byte(d[i - 2])) continue;
            if (i + l * 2 + 1 < n && d[i + l * 2 + 1] == 0 && alnum_byte(d[i + l * 2])) continue;
        }
        x_fill(d + i, l * 2, 1); g_scrubbed++; i += l * 2 - 1;
    }
}
static int users_skip(const char *name, size_t n) {
    static const char *skip[] = { "public", "default", "all users", "xxx" };
    for (int k = 0; k < 4; k++) if (strlen(skip[k]) == n && ci_eq(name, skip[k], n)) return 1;
    for (size_t i = 0; i < n; i++) if (name[i] != 'x' && name[i] != 'X' && name[i] != '%') return 0;
    return 1;
}
static void scrub_users8(unsigned char *d, size_t n) {
    for (size_t i = 0; i + 10 <= n; i++) {
        unsigned char *p = d + i;
        if (!(((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && (p[2] == '\\' || p[2] == '/') && ci_eq((char *)p + 3, "users", 5) && (p[8] == '\\' || p[8] == '/'))) continue;
        size_t s = i + 9, e = s;
        while (e < n && e - s <= 64 && d[e] != '\\' && d[e] != '/' && d[e] != 0 && d[e] != '"' && !strchr("<>|*?\r\n", d[e])) e++;
        if (e == s || e - s > 64 || e >= n || !(d[e] == '\\' || d[e] == '/' || d[e] == 0 || d[e] == '"')) continue;
        if (users_skip((char *)d + s, e - s)) continue;
        d[s] = '%'; x_fill(d + s + 1, e - s - 1, 0); g_scrubbed++; i = e;
    }
}
static void scrub_users16(unsigned char *d, size_t n) {
    static const char pre[] = "users";
    for (size_t i = 0; i + 20 <= n; i++) {
        unsigned char *p = d + i;
        if (p[1] || p[3] || p[5]) continue;
        if (!(((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[2] == ':' && (p[4] == '\\' || p[4] == '/'))) continue;
        int ok = 1;
        for (int k = 0; k < 5 && ok; k++) ok = lower_ascii(p[6 + 2 * k]) == pre[k] && p[7 + 2 * k] == 0;
        if (!ok || !((p[16] == '\\' || p[16] == '/') && p[17] == 0)) continue;
        size_t s = i + 18, e = s;
        char name[70]; size_t nl = 0;
        while (e + 1 < n && (e - s) / 2 <= 64) {
            int c = d[e] | (d[e + 1] << 8);
            if (c == '\\' || c == '/' || c == '"' || c == 0) break;
            if (nl < sizeof name - 1) name[nl++] = c < 128 ? (char)c : '?';
            e += 2;
        }
        if (e == s || (e - s) / 2 > 64 || e + 1 >= n) continue;
        name[nl] = 0;
        if (users_skip(name, nl)) continue;
        d[s] = '%'; d[s + 1] = 0; x_fill(d + s + 2, e - s - 2, 1); g_scrubbed++; i = e;
    }
}
static void scrub_email8(unsigned char *d, size_t n) {
    for (size_t i = 1; i + 4 < n; i++) {
        if (d[i] != '@') continue;
        size_t a0, a1;
        if (email_at((const char *)d, n, i, &a0, &a1)) { x_fill(d + a0, a1 - a0, 0); g_scrubbed++; i = a1; }
    }
}
static void scrub_email16(unsigned char *d, size_t n) {
    /* collapse to 8-bit per even/odd alignment, find, then fill the wide range */
    for (int align = 0; align < 2; align++) {
        size_t m = (n - align) / 2;
        if (m < 6) continue;
        unsigned char *t = (unsigned char *)malloc(m);
        if (!t) return;
        for (size_t k = 0; k < m; k++) {
            unsigned char lo = d[align + 2 * k], hi = d[align + 2 * k + 1];
            t[k] = hi ? 1 : lo;
        }
        for (size_t k = 1; k + 4 < m; k++) {
            if (t[k] != '@') continue;
            size_t a0, a1;
            if (email_at((const char *)t, m, k, &a0, &a1)) { x_fill(d + align + 2 * a0, 2 * (a1 - a0), 1); memset(t + a0, 'x', a1 - a0); g_scrubbed++; k = a1; }
        }
        free(t);
    }
}
static void scrub(unsigned char *d, size_t n) {
    for (int i = 0; i < g_nsecret; i++) {
        int bounded = !g_secret[i].path;
        scrub_bytes16(d, n, g_secret[i].w, bounded);
        scrub_bytes8(d, n, g_secret[i].u8, bounded);
        if (g_secret[i].ansi[0] && strcmp(g_secret[i].ansi, g_secret[i].u8)) scrub_bytes8(d, n, g_secret[i].ansi, bounded);
    }
    scrub_users16(d, n); scrub_users8(d, n);
    scrub_email16(d, n); scrub_email8(d, n);
}

/* ============================================================== files */
static int read_file(const wchar_t *path, Buf *out, size_t tail_limit, int *cut) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size; GetFileSizeEx(h, &size);
    uint64_t want = (uint64_t)size.QuadPart, from = 0;
    if (cut) *cut = 0;
    if (tail_limit && want > tail_limit) { from = want - tail_limit; want = tail_limit; if (cut) *cut = 1; }
    if (want > 256u * 1024 * 1024) { CloseHandle(h); return 0; }
    LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)from; SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    buf_reserve(out, (size_t)want);
    DWORD got = 0; size_t total = 0;
    while (total < want && ReadFile(h, out->p + out->n + total, (DWORD)min(want - total, 1u << 24), &got, NULL) && got) total += got;
    out->n += total; out->p[out->n] = 0;
    CloseHandle(h);
    return 1;
}
static int write_file(const wchar_t *path, const void *d, size_t n) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD w = 0; BOOL ok = WriteFile(h, d, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(h);
    return ok;
}
static void make_dirs(const wchar_t *dir) {
    wchar_t t[MAX_PATH]; wcsncpy(t, dir, MAX_PATH - 1); t[MAX_PATH - 1] = 0;
    for (wchar_t *p = t + 3; *p; p++) if (*p == L'\\') { *p = 0; CreateDirectoryW(t, NULL); *p = L'\\'; }
    CreateDirectoryW(t, NULL);
}
static const wchar_t *base_name(const wchar_t *p) { const wchar_t *s = wcsrchr(p, L'\\'); const wchar_t *t = wcsrchr(p, L'/'); if (t > s) s = t; return s ? s + 1 : p; }

typedef struct { wchar_t path[MAX_PATH]; FILETIME t; uint64_t size; } Found;
static int found_newer(const void *a, const void *b) { return CompareFileTime(&((const Found *)b)->t, &((const Found *)a)->t); }
/* files matching dir\pattern, newest first */
static int find_newest(const wchar_t *dir, const wchar_t *pattern, Found *out, int max) {
    wchar_t q[MAX_PATH]; swprintf(q, MAX_PATH, L"%s\\%s", dir, pattern);
    WIN32_FIND_DATAW f; HANDLE h = FindFirstFileW(q, &f);
    Found *all = NULL; int n = 0, cap = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (n == cap) { cap = cap ? cap * 2 : 32; Found *g = (Found *)realloc(all, cap * sizeof *all); if (!g) break; all = g; }
            swprintf(all[n].path, MAX_PATH, L"%s\\%s", dir, f.cFileName);
            all[n].t = f.ftLastWriteTime; all[n].size = ((uint64_t)f.nFileSizeHigh << 32) | f.nFileSizeLow; n++;
        } while (FindNextFileW(h, &f));
        FindClose(h);
    }
    if (n) qsort(all, n, sizeof *all, found_newer);
    int k = n < max ? n : max;
    if (k) memcpy(out, all, k * sizeof *all);
    free(all);
    return k;
}

/* ============================================================== zip (stored) */
static uint32_t g_crc_table[256];
static uint32_t crc32_of(const unsigned char *d, size_t n) {
    if (!g_crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; g_crc_table[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = g_crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
typedef struct { char name[160]; Buf data; int binary; } Member;
typedef struct { Member m[40]; int n; Buf notes; } Bundle;
static Member *bundle_add(Bundle *b, const char *name) {
    for (int i = 0; i < b->n; i++) if (!strcmp(b->m[i].name, name)) return &b->m[i];
    if (b->n >= 40) return NULL;
    Member *m = &b->m[b->n++]; memset(m, 0, sizeof *m);
    strncpy(m->name, name, sizeof m->name - 1);
    return m;
}
static void bundle_drop(Bundle *b, const char *prefix) {
    for (int i = 0; i < b->n; i++) if (!strncmp(b->m[i].name, prefix, strlen(prefix))) { buf_free(&b->m[i].data); b->m[i] = b->m[--b->n]; i--; }
}
static int member_cmp(const void *a, const void *b) { return strcmp(((const Member *)a)->name, ((const Member *)b)->name); }
static void put16(Buf *b, unsigned v) { unsigned char t[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; buf_add(b, t, 2); }
static void put32(Buf *b, uint32_t v) { unsigned char t[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) }; buf_add(b, t, 4); }
static void zip_build(Bundle *b, Buf *z) {
    SYSTEMTIME st; GetLocalTime(&st);
    unsigned dtime = (st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2), ddate = ((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay;
    qsort(b->m, b->n, sizeof *b->m, member_cmp);
    uint32_t offs[40], crcs[40];
    for (int i = 0; i < b->n; i++) {
        Member *m = &b->m[i]; size_t nl = strlen(m->name);
        offs[i] = (uint32_t)z->n; crcs[i] = crc32_of((unsigned char *)m->data.p, m->data.n);
        put32(z, 0x04034b50); put16(z, 20); put16(z, 0x0800); put16(z, 0); put16(z, dtime); put16(z, ddate);
        put32(z, crcs[i]); put32(z, (uint32_t)m->data.n); put32(z, (uint32_t)m->data.n); put16(z, (unsigned)nl); put16(z, 0);
        buf_add(z, m->name, nl); if (m->data.n) buf_add(z, m->data.p, m->data.n);
    }
    uint32_t cd = (uint32_t)z->n;
    for (int i = 0; i < b->n; i++) {
        Member *m = &b->m[i]; size_t nl = strlen(m->name);
        put32(z, 0x02014b50); put16(z, 20); put16(z, 20); put16(z, 0x0800); put16(z, 0); put16(z, dtime); put16(z, ddate);
        put32(z, crcs[i]); put32(z, (uint32_t)m->data.n); put32(z, (uint32_t)m->data.n); put16(z, (unsigned)nl); put16(z, 0); put16(z, 0);
        put16(z, 0); put16(z, 0); put32(z, 0); put32(z, offs[i]); buf_add(z, m->name, nl);
    }
    uint32_t cdn = (uint32_t)z->n - cd;
    put32(z, 0x06054b50); put16(z, 0); put16(z, 0); put16(z, (unsigned)b->n); put16(z, (unsigned)b->n); put32(z, cdn); put32(z, cd); put16(z, 0);
}

/* ============================================================== bot recordings */
/* A .botrec is "MA2BREC1" + blocks {"BRB1", raw length, stored length, algorithm} + data, each block
   packed alone with the Windows Compression API (bots/ma2_bot_rec.c). Unpack every block, scrub it,
   pack it again (MSZIP). Too large: keep the first block (the recording's header and string table)
   and the newest blocks that fit; the decoder (tools/botrec.py) reads any run of whole blocks. */
typedef BOOL (WINAPI *CreateCompFn)(DWORD, void *, void **);
typedef BOOL (WINAPI *CompFn)(void *, const void *, SIZE_T, void *, SIZE_T, SIZE_T *);
typedef BOOL (WINAPI *CloseFn)(void *);
static int botrec_scrub(const Buf *in, Buf *out, char *note, size_t note_n, size_t limit) {
    note[0] = 0;
    if (in->n < 8 || memcmp(in->p, "MA2BREC1", 8)) { snprintf(note, note_n, "not a bot recording"); return 0; }
    HMODULE cab = LoadLibraryW(L"cabinet.dll");
    CreateCompFn cc = cab ? (CreateCompFn)(void *)GetProcAddress(cab, "CreateCompressor") : NULL;
    CreateCompFn cd = cab ? (CreateCompFn)(void *)GetProcAddress(cab, "CreateDecompressor") : NULL;
    CompFn comp = cab ? (CompFn)(void *)GetProcAddress(cab, "Compress") : NULL;
    CompFn decomp = cab ? (CompFn)(void *)GetProcAddress(cab, "Decompress") : NULL;
    CloseFn ccl = cab ? (CloseFn)(void *)GetProcAddress(cab, "CloseCompressor") : NULL;
    CloseFn dcl = cab ? (CloseFn)(void *)GetProcAddress(cab, "CloseDecompressor") : NULL;
    if (!cc || !cd || !comp || !decomp) { snprintf(note, note_n, "the Windows Compression API is missing"); return 0; }
    void *dec[8] = { 0 }, *enc = NULL;
    if (!cc(2, NULL, &enc)) { snprintf(note, note_n, "no MSZIP compressor"); return 0; }
    typedef struct { size_t off, n; } Blk;
    Blk *blocks = NULL; int nb = 0, cap = 0;
    Buf packed = { 0 }; int ok = 1;
    size_t pos = 8;
    while (pos + 16 <= in->n) {
        uint32_t hdr[4]; memcpy(hdr, in->p + pos, 16);
        if (hdr[0] != 0x31425242u || pos + 16 + hdr[2] > in->n) break;      /* cut short: the game was still writing */
        const unsigned char *data = (const unsigned char *)in->p + pos + 16;
        unsigned char *raw = (unsigned char *)malloc(hdr[1] ? hdr[1] : 1);
        if (!raw) { ok = 0; break; }
        if (hdr[3]) {
            if (hdr[3] >= 8) { free(raw); ok = 0; snprintf(note, note_n, "unknown block packing %u", hdr[3]); break; }
            if (!dec[hdr[3]] && !cd(hdr[3], NULL, &dec[hdr[3]])) { free(raw); ok = 0; snprintf(note, note_n, "no decompressor %u", hdr[3]); break; }
            SIZE_T got = 0;
            if (!decomp(dec[hdr[3]], data, hdr[2], raw, hdr[1], &got) || got != hdr[1]) { free(raw); ok = 0; snprintf(note, note_n, "a block could not be unpacked"); break; }
        } else memcpy(raw, data, hdr[1]);
        scrub(raw, hdr[1]);
        size_t need = hdr[1] + hdr[1] / 8 + 65536;
        unsigned char *o = (unsigned char *)malloc(need); SIZE_T got = 0;
        uint32_t nh[4] = { 0x31425242u, hdr[1], hdr[1], 0 };
        const unsigned char *body = raw;
        if (o && comp(enc, raw, hdr[1], o, need, &got) && got > 0 && got < hdr[1]) { nh[2] = (uint32_t)got; nh[3] = 2; body = o; }
        if (nb == cap) { cap = cap ? cap * 2 : 64; Blk *g = (Blk *)realloc(blocks, cap * sizeof *g); if (!g) { free(raw); free(o); ok = 0; break; } blocks = g; }
        blocks[nb].off = packed.n; blocks[nb].n = 16 + nh[2]; nb++;
        buf_add(&packed, nh, 16); buf_add(&packed, body, nh[2]);
        free(raw); free(o);
        pos += 16 + hdr[2];
    }
    for (int i = 0; i < 8; i++) if (dec[i] && dcl) dcl(dec[i]);
    if (enc && ccl) ccl(enc);
    if (!ok || !nb) { if (!note[0]) snprintf(note, note_n, "no complete block"); buf_free(&packed); free(blocks); return 0; }
    if (8 + packed.n > limit && (limit < 8 + blocks[0].n || nb < 2 || limit - 8 - blocks[0].n < blocks[nb - 1].n)) {
        snprintf(note, note_n, "no room left under the report size limit");          /* (a crash or hang dump keeps priority) */
        buf_free(&packed); free(blocks); return 0;
    }
    buf_add(out, "MA2BREC1", 8);
    if (8 + packed.n <= limit) { buf_add(out, packed.p, packed.n); snprintf(note, note_n, "%d blocks", nb); }
    else {
        /* the first block, then the newest blocks that fit */
        size_t room = limit - 8 - blocks[0].n; int first = nb;
        while (first > 1 && blocks[first - 1].n <= room) { room -= blocks[first - 1].n; first--; }
        buf_add(out, packed.p + blocks[0].off, blocks[0].n);
        for (int i = first; i < nb; i++) buf_add(out, packed.p + blocks[i].off, blocks[i].n);
        snprintf(note, note_n, "trimmed to the first block and the newest %d of %d blocks (about %d s of the end of the match)", nb - first, nb, nb - first);
    }
    buf_free(&packed); free(blocks);
    return 1;
}

/* ============================================================== the report */
typedef struct {
    wchar_t install[MAX_PATH], data[MAX_PATH], logs[MAX_PATH], out_dir[MAX_PATH];
    wchar_t log[MAX_PATH], ini[MAX_PATH], dump[MAX_PATH];
    char kind[16], sig[128], version[32], build[80];
    char failure[200];
} Ctx;
static Ctx g;
static char g_text[MAX_TEXT * 4 + 8];       /* the player's words, UTF-8 */
static char g_relay[512];

static void json_str(const char *json, const char *key, char *out, size_t n) {
    char pat[64]; out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return;
    p++;
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < n) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                char hex[5] = { p[1], p[2], p[3], p[4], 0 }; unsigned v = (unsigned)strtoul(hex, NULL, 16); p += 5;
                wchar_t w[2] = { (wchar_t)v, 0 }; char u[8]; w2u(w, u, sizeof u);
                for (char *c = u; *c && k + 1 < n; c++) out[k++] = *c;
                continue;
            }
            out[k++] = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p; p++;
            continue;
        }
        out[k++] = *p++;
    }
    out[k] = 0;
}
static int json_true(const char *json, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':' || *p == '\r' || *p == '\n') p++;
    return !strncmp(p, "true", 4);
}

static void ctx_init(void) {
    wchar_t v[MAX_PATH];
    if (env_get("INSTALL_DIR", v, MAX_PATH)) wcscpy(g.install, v);
    else { GetModuleFileNameW(NULL, g.install, MAX_PATH); wchar_t *s = wcsrchr(g.install, L'\\'); if (s) *s = 0; }
    if (env_get("DATA_DIR", v, MAX_PATH)) wcscpy(g.data, v);
    else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", g.data, MAX_PATH);
        if (!n || n + 16 >= MAX_PATH) wcscpy(g.data, g.install);
        else wcscat(g.data, L"\\MechAssault2");
    }
    swprintf(g.logs, MAX_PATH, L"%s\\Logs", g.data);
    swprintf(g.out_dir, MAX_PATH, L"%s\\Diagnostics", g.data);
    if (!g.ini[0]) swprintf(g.ini, MAX_PATH, L"%s\\pc_settings.ini", g.data);
    strcpy(g.version, MA2_VER_STR);
    /* the install's own record: its version and kit id */
    wchar_t stamp[MAX_PATH]; swprintf(stamp, MAX_PATH, L"%s\\recomp-install.json", g.install);
    Buf j = { 0 };
    if (read_file(stamp, &j, 65536, NULL)) {
        char pv[32], kit[80];
        json_str(j.p, "public_version", pv, sizeof pv); json_str(j.p, "kit_id", kit, sizeof kit);
        if (pv[0]) strncpy(g.version, pv, sizeof g.version - 1);
        if (kit[0]) strncpy(g.build, kit, sizeof g.build - 1);
    }
    buf_free(&j);
    for (char *c = g.version; *c; c++) if (!strchr("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.+-", *c)) *c = '-';
    for (char *c = g.build; *c; c++) if (!strchr("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.+-", *c)) *c = '-';
    /* relay: compiled in; MA2_REPORT_RELAY for tests and support (https, or plain http on this PC) */
    g_relay[0] = 0;
    if (env_get("RELAY", v, MAX_PATH)) {
        char u[512]; w2u(v, u, sizeof u);
        int ok = !_strnicmp(u, "https://", 8) && u[8];
        if (!_strnicmp(u, "http://127.0.0.1", 16) || !_strnicmp(u, "http://localhost", 16) || !_strnicmp(u, "http://[::1]", 12)) ok = 1;
        if (ok) strcpy(g_relay, u);
    }
    if (!g_relay[0] && !_strnicmp(MA2_RELAY_URL, "https://", 8)) strncpy(g_relay, MA2_RELAY_URL, sizeof g_relay - 1);
    size_t rl = strlen(g_relay); while (rl && g_relay[rl - 1] == '/') g_relay[--rl] = 0;
}

static void line_of(const char *text, size_t n, const char *tag, char *out, size_t on) {
    out[0] = 0;
    const char *best = NULL; size_t bl = 0;
    size_t tl = strlen(tag);
    for (size_t i = 0; i + tl <= n; i++) {
        if ((i == 0 || text[i - 1] == '\n') && !strncmp(text + i, tag, tl)) {
            size_t e = i; while (e < n && text[e] != '\n' && text[e] != '\r') e++;
            /* not the start-up notice "[HANG] monitor on (...)": only a real report line */
            if (!(e - i > tl + 11 && !strncmp(text + i + tl, " monitor on", 11))) { best = text + i; bl = e - i; }
        }
    }
    if (best) { if (bl > on - 1) bl = on - 1; if (bl > 240) bl = 240; memcpy(out, best, bl); out[bl] = 0; }
}

static int own_log(const wchar_t *name) {
    static const wchar_t *own[] = { L"install-", L"installer-ui-", L"buildtools-", L"stub-", L"downloads" };
    for (int i = 0; i < 5; i++) if (!_wcsnicmp(name, own[i], wcslen(own[i]))) return 1;
    return 0;
}

static void cpu_brand(char *out, size_t n) {
    int r[4]; char b[49] = { 0 };
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) for (int i = 0; i < 3; i++) { __cpuid(r, (int)(0x80000002u + i)); memcpy(b + 16 * i, r, 16); }
    char *s = b; while (*s == ' ') s++;
    size_t l = strlen(s); while (l && s[l - 1] == ' ') s[--l] = 0;
    snprintf(out, n, "%s", *s ? s : "unknown");
}
typedef HRESULT (WINAPI *CreateFactoryFn)(REFIID, void **);
static void gpu_line(Buf *out) {
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    CreateFactoryFn f = m ? (CreateFactoryFn)(void *)GetProcAddress(m, "CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *fac = NULL; int any = 0;
    static const GUID iid = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
    if (f && SUCCEEDED(f(&iid, (void **)&fac)) && fac) {
        IDXGIAdapter1 *a = NULL;
        for (UINT i = 0; i < 8 && IDXGIFactory1_EnumAdapters1(fac, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (a && SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                char name[256]; w2u(d.Description, name, sizeof name);
                buf_addf(out, "%s%s %.0f GB (vendor %04X device %04X)", any ? "; " : "", name, d.DedicatedVideoMemory / 1073741824.0, d.VendorId, d.DeviceId);
                any = 1;
            }
            if (a) IDXGIAdapter1_Release(a);
            a = NULL;
        }
        IDXGIFactory1_Release(fac);
    }
    if (!any) buf_adds(out, "-");
}

/* The summary (the issue text, about 15 lines) and system.txt; every line is redacted by the caller. */
static void summary_lines(Buf *s, const char *newest_log_name, const char *health, const char *crash, const char *hang) {
    buf_addf(s, "MechAssault 2 Recomp %s (build %s) | report: %s | sent by MA2 Report %s\n", g.version, g.build[0] ? g.build : "-", g.kind, MA2_VER_STR);
    {
        wchar_t stamp[MAX_PATH]; swprintf(stamp, MAX_PATH, L"%s\\recomp-install.json", g.install);
        Buf j = { 0 };
        if (read_file(stamp, &j, 65536, NULL)) {
            char pv[32], kit[80]; json_str(j.p, "public_version", pv, sizeof pv); json_str(j.p, "kit_id", kit, sizeof kit);
            buf_addf(s, "Installed: %s (kit %s)\n", pv[0] ? pv : "?", kit[0] ? kit : "?");
            const char *low = strstr(j.p, "\"low_memory_build\"");
            if (low) { const char *p = strstr(low, "\"one_codegen_thread\""); long cg = 0; if (p && (p = strchr(p, ':'))) cg = strtol(p + 1, NULL, 10); buf_addf(s, "Low-memory build: %ld file(s) with /cgthreads1\n", cg); }
            else buf_adds(s, "Low-memory build: no\n");
        } else buf_adds(s, "Installed: no recomp-install.json in the game folder\n");
        buf_free(&j);
    }
    {
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn gv = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW os; memset(&os, 0, sizeof os); os.dwOSVersionInfoSize = sizeof os;
        SYSTEM_INFO si; GetNativeSystemInfo(&si);
        const char *arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" : si.wProcessorArchitecture == 12 ? "ARM64" : "?";
        if (gv && gv(&os) == 0) buf_addf(s, "Windows build %lu (%s)\n", os.dwBuildNumber, arch);
        char cpu[64]; cpu_brand(cpu, sizeof cpu);
        buf_addf(s, "CPU: %s, %lu threads\n", cpu, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
        if (GlobalMemoryStatusEx(&ms)) buf_addf(s, "Memory: %.1f GB, %.1f GB free; commit limit %.1f GB, %.1f GB free\n", ms.ullTotalPhys / 1073741824.0,
                                               ms.ullAvailPhys / 1073741824.0, ms.ullTotalPageFile / 1073741824.0, ms.ullAvailPageFile / 1073741824.0);
        SYSTEM_POWER_STATUS ps;
        if (GetSystemPowerStatus(&ps)) buf_addf(s, "Power: mains %s, battery %s%s\n", ps.ACLineStatus == 1 ? "on" : ps.ACLineStatus == 0 ? "off" : "unknown",
                                               ps.BatteryFlag == 128 ? "none" : ps.BatteryFlag == 255 ? "unknown" : "present", ps.SystemStatusFlag ? ", saver on" : "");
        buf_adds(s, "GPU: "); gpu_line(s); buf_addc(s, '\n');
    }
    {
        static const char *keys[][2] = { { "Display", "WindowWidth" }, { "Display", "WindowHeight" }, { "Display", "Fullscreen" }, { "Display", "VSync" },
                                         { "Display", "FrameLimit" }, { "Display", "TextureQuality" }, { "Display", "FMVQuality" }, { "Display", "PreloadShaders" },
                                         { "Audio", "Enabled" }, { "Audio", "Threads" }, { "Bots", "BotRecorder" } };
        char last[16] = ""; int any = 0;
        if (GetFileAttributesW(g.ini) == INVALID_FILE_ATTRIBUTES) buf_adds(s, "Settings: none yet (the game has not been started)\n");
        else {
            for (int i = 0; i < (int)(sizeof keys / sizeof *keys); i++) {
                wchar_t sec[32], key[32], val[48];
                swprintf(sec, 32, L"%hs", keys[i][0]); swprintf(key, 32, L"%hs", keys[i][1]);
                if (!GetPrivateProfileStringW(sec, key, L"", val, 48, g.ini)) continue;
                char u[160]; w2u(val, u, sizeof u);
                if (strcmp(last, keys[i][0])) { buf_addf(s, "%s[%s]", any ? "\n" : "", keys[i][0]); strcpy(last, keys[i][0]); }
                buf_addf(s, " %s=%s", keys[i][1], u); any = 1;
            }
            if (any) buf_addc(s, '\n');
        }
    }
    if (g.failure[0]) buf_addf(s, "Failure: %s\n", g.failure);
    buf_addf(s, "Game log: %s\n", newest_log_name && *newest_log_name ? newest_log_name : "none");
    if (health && *health) buf_addf(s, "%s\n", health);
    if (crash && *crash) buf_addf(s, "%s\n", crash);
    if (hang && *hang) buf_addf(s, "%s\n", hang);
}

static void add_text_file(Bundle *b, const char *arc, const wchar_t *path, size_t tail) {
    Member *m = bundle_add(b, arc);
    if (!m) return;
    int cut = 0;
    buf_free(&m->data);
    if (!read_file(path, &m->data, tail, &cut)) { bundle_drop(b, arc); buf_addf(&b->notes, "%s could not be read\n", arc); return; }
    if (cut) buf_addf(&b->notes, "%s: only its newest %u MB (the log is longer)\n", arc, (unsigned)(tail >> 20));
    redact(&m->data);
}

/* Collects the bundle. Returns 0 on success, -1 on a fault (the caller saves nothing). */
static int collect(Bundle *b, Buf *summary) {
    memset(b, 0, sizeof *b);
    if (fault_at("collect")) return -1;
    char arc[200], name8[MAX_PATH * 3];
    /* recomp-install.json, pc_settings.ini */
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\recomp-install.json", g.install);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) add_text_file(b, "recomp-install.json", p, 0);
    else buf_adds(&b->notes, "recomp-install.json was not found in the game folder\n");
    if (GetFileAttributesW(g.ini) != INVALID_FILE_ATTRIBUTES) add_text_file(b, "pc_settings.ini", g.ini, 0);
    else buf_adds(&b->notes, "no pc_settings.ini yet (the game has not been started)\n");
    /* game logs: this run's (when the game passed it) and the newest others, three in all */
    Found f[64]; int nf = find_newest(g.logs, L"*.log", f, 64), taken = 0;
    char newest_name[MAX_PATH * 3] = "";
    Buf newest = { 0 };
    if (g.log[0] && GetFileAttributesW(g.log) != INVALID_FILE_ATTRIBUTES) {
        w2u(base_name(g.log), name8, sizeof name8);
        snprintf(arc, sizeof arc, "logs/%s", name8);
        add_text_file(b, arc, g.log, LOG_LIMIT);
        strcpy(newest_name, name8); taken++;
        read_file(g.log, &newest, 4u << 20, NULL);
    }
    for (int i = 0; i < nf && taken < 3; i++) {
        const wchar_t *bn = base_name(f[i].path);
        if (own_log(bn) || (g.log[0] && !_wcsicmp(f[i].path, g.log))) continue;
        w2u(bn, name8, sizeof name8);
        snprintf(arc, sizeof arc, "logs/%s", name8);
        add_text_file(b, arc, f[i].path, LOG_LIMIT);
        if (!newest_name[0]) { strcpy(newest_name, name8); read_file(f[i].path, &newest, 4u << 20, NULL); }
        taken++;
    }
    if (!taken) buf_adds(&b->notes, "no game logs found\n");
    /* the newest installer logs */
    static const wchar_t *pats[] = { L"install-*.log", L"installer-ui-*.log", L"buildtools-*.log", L"stub-*.log" };
    for (int k = 0; k < 4; k++) {
        Found one;
        if (find_newest(g.logs, pats[k], &one, 1)) { w2u(base_name(one.path), name8, sizeof name8); snprintf(arc, sizeof arc, "logs/%s", name8); add_text_file(b, arc, one.path, LOG_LIMIT); }
    }
    /* the dump: the one the game named, else the newest crash/hang dump */
    {
        wchar_t dump[MAX_PATH] = L"";
        if (g.dump[0] && GetFileAttributesW(g.dump) != INVALID_FILE_ATTRIBUTES) wcscpy(dump, g.dump);
        else {
            Found c, h; int nc = find_newest(g.logs, L"crash-*.dmp", &c, 1), nh = find_newest(g.logs, L"hang-*.dmp", &h, 1);
            if (nc && (!nh || CompareFileTime(&c.t, &h.t) >= 0)) wcscpy(dump, c.path); else if (nh) wcscpy(dump, h.path);
        }
        if (dump[0]) {
            Buf d = { 0 }; w2u(base_name(dump), name8, sizeof name8);
            if (!read_file(dump, &d, 0, NULL)) buf_addf(&b->notes, "%s could not be read\n", name8);
            else if (d.n > DUMP_LIMIT) buf_addf(&b->notes, "%s skipped: larger than %u MB\n", name8, DUMP_LIMIT >> 20);
            else {
                int before = g_scrubbed; scrub((unsigned char *)d.p, d.n);
                snprintf(arc, sizeof arc, "dumps/%s", name8);
                Member *m = bundle_add(b, arc);
                if (m) { m->data = d; m->binary = 1; d.p = NULL; }
                buf_addf(&b->notes, "%s: %d private name(s) or folder(s) overwritten (same length)\n", name8, g_scrubbed - before);
            }
            buf_free(&d);
        }
    }
    /* the newest bot recording, scrubbed and packed again */
    {
        Found r;
        if (find_newest(g.logs, L"*.botrec", &r, 1)) {
            Buf raw = { 0 }, out = { 0 }; char note[200];
            w2u(base_name(r.path), name8, sizeof name8);
            if (r.size > 400u * 1024 * 1024) buf_addf(&b->notes, "%s skipped: larger than 400 MB\n", name8);
            else if (!read_file(r.path, &raw, 0, NULL)) buf_addf(&b->notes, "%s could not be read\n", name8);
            else {
                int before = g_scrubbed;
                /* 1.1.6: the recording's budget is what is left under the report limit after every member so far (logs,
                   settings, the dump), less a headroom for the zip overhead, system.txt and the player's words */
                Buf z = { 0 }; zip_build(b, &z);
                size_t limit = z.n + BOTREC_HEADROOM < MAX_ZIP ? MAX_ZIP - BOTREC_HEADROOM - z.n : 0;
                buf_free(&z);
                if (botrec_scrub(&raw, &out, note, sizeof note, limit)) {
                    snprintf(arc, sizeof arc, "botrec/%s", name8);
                    Member *m = bundle_add(b, arc);
                    if (m) { m->data = out; m->binary = 1; out.p = NULL; }
                    buf_addf(&b->notes, "%s: %s; %d private name(s) or folder(s) overwritten (same length)\n", name8, note, g_scrubbed - before);
                } else buf_addf(&b->notes, "%s left out: it could not be checked for private data (%s)\n", name8, note);
            }
            buf_free(&raw); buf_free(&out);
        }
    }
    /* the player's words */
    if (g_text[0]) {
        Member *m = bundle_add(b, "what-happened.txt");
        if (m) { buf_free(&m->data); buf_adds(&m->data, g_text); buf_addc(&m->data, '\n'); redact(&m->data); }
    }
    /* summary + system.txt */
    char health[256], crash[256], hang[256];
    line_of(newest.p, newest.n, "[HEALTH]", health, sizeof health);
    line_of(newest.p, newest.n, "[CRASH]", crash, sizeof crash);
    line_of(newest.p, newest.n, "[HANG]", hang, sizeof hang);
    buf_free(&newest);
    summary_lines(summary, newest_name, health, crash, hang);
    redact(summary);
    if (summary->n > MAX_SUMMARY) { summary->n = MAX_SUMMARY; summary->p[summary->n] = 0; }
    return 0;
}

static void system_txt(Bundle *b, const Buf *summary) {
    Member *m = bundle_add(b, "system.txt");
    if (!m) return;
    buf_free(&m->data);
    buf_add(&m->data, summary->p, summary->n);
    buf_addf(&m->data, "Screen: %dx%d\n", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    char inst[MAX_PATH * 3]; w2u(g.install, inst, sizeof inst);
    buf_addf(&m->data, "Install folder: %s\n", inst);
    buf_adds(&m->data, "Included: ");
    for (int i = 0; i < b->n; i++) if (strcmp(b->m[i].name, "system.txt")) buf_addf(&m->data, "%s, ", b->m[i].name);
    buf_adds(&m->data, "system.txt\n");
    for (size_t i = 0; i < b->notes.n;) {
        size_t e = i; while (e < b->notes.n && b->notes.p[e] != '\n') e++;
        buf_adds(&m->data, "Note: "); buf_add(&m->data, b->notes.p + i, e - i); buf_addc(&m->data, '\n');
        i = e + 1;
    }
    redact(&m->data);
}

/* Writes the zip (the player's copy), under the size limit. zip_path gets its path. */
static int save_zip(Bundle *b, const Buf *summary, wchar_t *zip_path, Buf *zip) {
    if (fault_at("zip")) return 0;
    const char *drop[] = { "botrec/", "dumps/" };
    for (int round = 0; round < 3; round++) {
        system_txt(b, summary);
        buf_free(zip); zip_build(b, zip);
        if (zip->n <= MAX_ZIP || round == 2) break;
        for (int i = 0; i < b->n; i++) if (!strncmp(b->m[i].name, drop[round], strlen(drop[round])))
            buf_addf(&b->notes, "%s left out: the report would be larger than %u MB\n", b->m[i].name, MAX_ZIP >> 20);
        bundle_drop(b, drop[round]);
    }
    make_dirs(g.out_dir);
    SYSTEMTIME st; GetLocalTime(&st);
    swprintf(zip_path, MAX_PATH, L"%s\\MA2-report-%04u%02u%02u-%02u%02u%02u.zip", g.out_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    for (int k = 2; GetFileAttributesW(zip_path) != INVALID_FILE_ATTRIBUTES && k < 50; k++)
        swprintf(zip_path, MAX_PATH, L"%s\\MA2-report-%04u%02u%02u-%02u%02u%02u-%d.zip", g.out_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, k);
    return write_file(zip_path, zip->p, zip->n);
}

/* ============================================================== send (report.py send()) */
typedef struct { int ok; char cause[16]; char id[16]; char message[320]; int http; } Sent;
static const char *cause_text(const char *c) {
    static const char *t[][2] = { { "sent", "sent" }, { "unset", "sending is not set up in this version" }, { "too_large", "the report is larger than 20 MB" },
        { "rate_limited", "the report service is busy (too many reports right now)" }, { "refused", "the report service did not take the report" },
        { "server", "the report service did not answer properly" }, { "offline", "no internet connection" }, { "timeout", "the connection timed out" },
        { "tls", "Windows couldn't verify the secure connection to the report service" }, { "blocked", "the connection was refused or blocked (a firewall or filter)" },
        { "error", "the report could not be sent" } };
    for (int i = 0; i < (int)(sizeof t / sizeof *t); i++) if (!strcmp(c, t[i][0])) return t[i][1];
    return t[10][1];
}
static void clean_message(const char *in, char *out, size_t n) {
    size_t k = 0; int sp = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k + 4 < n; p++) {
        if (*p < 0x20 || *p == 0x7F) { if (!sp && k) { out[k++] = ' '; sp = 1; } continue; }
        out[k++] = (char)*p; sp = 0;
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = 0;
    if (k > 300) { k = 297; while (k && ((unsigned char)out[k] & 0xC0) == 0x80) k--; strcpy(out + k, "..."); }
}
static int valid_id(const char *s) {
    if (strlen(s) != 10 || strncmp(s, "MA2-", 4)) return 0;
    for (int i = 4; i < 10; i++) if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '2' && s[i] <= '7'))) return 0;
    return 1;
}
static void sig_clean(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) if (strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ._:/+-", *p) && o - s < 120) *o++ = *p;
    *o = 0;
}
static void field(Buf *body, const char *boundary, const char *name, const char *value) {
    buf_addf(body, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n", boundary, name);
    buf_adds(body, value); buf_adds(body, "\r\n");
}
static int g_timeout = NET_TIMEOUT_MS;
static void send_report(const Buf *zip, const char *summary, Sent *r) {
    memset(r, 0, sizeof *r); strcpy(r->cause, "error");
    if (!g_relay[0]) { strcpy(r->cause, "unset"); return; }
    if (zip->n > MAX_ZIP) { strcpy(r->cause, "too_large"); return; }
    if (zip->n < 4 || memcmp(zip->p, "PK\x03\x04", 4)) return;
    if (fault_at("send")) return;
    char boundary[64]; LARGE_INTEGER q; QueryPerformanceCounter(&q);
    snprintf(boundary, sizeof boundary, "ma2-%08lx%08lx%08lx", (unsigned long)GetTickCount(), (unsigned long)q.LowPart, (unsigned long)GetCurrentProcessId());
    const char *client = !strcmp(g.kind, "manual") && GetEnvironmentVariableW(L"MA2_REPORT_FROM_GAME", NULL, 0) ? "game" : "helper";
    char text[MAX_TEXT * 4 + 8]; Buf t = { 0 };
    buf_adds(&t, g_text); redact(&t);
    {   /* at most 2,000 characters */
        int chars = 0; size_t k = 0;
        while (k < t.n && chars < MAX_TEXT) { unsigned char c = (unsigned char)t.p[k]; k += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; chars++; }
        if (k > t.n) k = t.n;
        memcpy(text, t.p, k); text[k] = 0;
    }
    buf_free(&t);
    char sig[128]; strncpy(sig, g.sig, sizeof sig - 1); sig[sizeof sig - 1] = 0; sig_clean(sig);
    Buf body = { 0 };
    field(&body, boundary, "client", client); field(&body, boundary, "version", g.version); field(&body, boundary, "build", g.build);
    field(&body, boundary, "kind", g.kind); field(&body, boundary, "signature", sig); field(&body, boundary, "text", text);
    field(&body, boundary, "summary", summary);
    buf_addf(&body, "--%s\r\nContent-Disposition: form-data; name=\"zip\"; filename=\"report.zip\"\r\nContent-Type: application/zip\r\n\r\n", boundary);
    buf_add(&body, zip->p, zip->n); buf_adds(&body, "\r\n");
    buf_addf(&body, "--%s--\r\n", boundary);

    wchar_t url[600]; u2w(g_relay, url, 600); wcscat(url, L"/v1/report");
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[512]; uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) { buf_free(&body); return; }
    wchar_t ua[128]; swprintf(ua, 128, L"MechAssault2-Recomp-Report/%hs (%hs)", g.version, client);
    HINTERNET s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = NULL, rq = NULL;
    DWORD err = 0;
    if (s) {
        wchar_t tv[16]; int to = env_get("TIMEOUT_MS", tv, 16) ? _wtoi(tv) : 0;     /* tests only */
        if (to <= 0) to = NET_TIMEOUT_MS;
        WinHttpSetTimeouts(s, to, to, to, to);
        g_timeout = to;
        c = WinHttpConnect(s, host, uc.nPort, 0);
    }
    if (c) rq = WinHttpOpenRequest(c, L"POST", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (rq) {
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, rto = (DWORD)g_timeout;
        WinHttpSetOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
        WinHttpSetOption(rq, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &rto, sizeof rto);
        wchar_t hdr[2048], wsig[256], wver[64], wbuild[128], wkind[32], wclient[16], wb[80];
        u2w(sig, wsig, 256); u2w(g.version, wver, 64); u2w(g.build, wbuild, 128); u2w(g.kind, wkind, 32); u2w(client, wclient, 16); u2w(boundary, wb, 80);
        swprintf(hdr, 2048, L"Content-Type: multipart/form-data; boundary=%s\r\nX-MA2-Client: %s\r\nX-MA2-Version: %s\r\nX-MA2-Build: %s\r\nX-MA2-Kind: %s\r\nX-MA2-Signature: %s\r\nAccept: application/json\r\n",
                 wb, wclient, wver, wbuild, wkind, wsig);
        if (WinHttpSendRequest(rq, hdr, (DWORD)-1L, body.p, (DWORD)body.n, (DWORD)body.n, 0) && WinHttpReceiveResponse(rq, NULL)) {
            DWORD status = 0, sl = sizeof status;
            WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sl, WINHTTP_NO_HEADER_INDEX);
            r->http = (int)status;
            Buf ans = { 0 }; DWORD avail = 0;
            while (ans.n < 65536 && WinHttpQueryDataAvailable(rq, &avail) && avail) {
                buf_reserve(&ans, avail); DWORD got = 0;
                if (!WinHttpReadData(rq, ans.p + ans.n, avail, &got) || !got) break;
                ans.n += got; ans.p[ans.n] = 0;
            }
            char id[64] = "", msg[1024] = "";
            int is_json = ans.p && strchr(ans.p, '{');
            if (is_json) { json_str(ans.p, "id", id, sizeof id); json_str(ans.p, "message", msg, sizeof msg); }
            clean_message(msg, r->message, sizeof r->message);
            if (status == 200 && is_json && json_true(ans.p, "ok") && valid_id(id)) { r->ok = 1; strcpy(r->cause, "sent"); strcpy(r->id, id); }
            else if (status == 429) strcpy(r->cause, "rate_limited");
            else if (status == 413) strcpy(r->cause, "too_large");
            else if (status >= 400 && status < 500) strcpy(r->cause, "refused");
            else if (status == 200 && is_json && strstr(ans.p, "\"ok\"") && !json_true(ans.p, "ok")) strcpy(r->cause, "refused");
            else strcpy(r->cause, "server");
            buf_free(&ans);
        } else err = GetLastError();
    } else err = GetLastError();
    if (err) {
        strcpy(r->cause, err == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "offline" : err == ERROR_WINHTTP_TIMEOUT ? "timeout" :
                         err == ERROR_WINHTTP_SECURE_FAILURE ? "tls" : err == ERROR_WINHTTP_CANNOT_CONNECT ? "blocked" : "server");
    }
    if (rq) WinHttpCloseHandle(rq);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    buf_free(&body);
}

/* The whole job: collect, save, send. On a fault in collecting or saving nothing is sent (the caller
   says so); the zip is always on disk before anything is sent. */
typedef struct { int saved; wchar_t zip[MAX_PATH]; Sent sent; int sent_tried; Buf members; } Outcome;
static void run_report(int send_it, Outcome *o) {
    memset(o, 0, sizeof *o); strcpy(o->sent.cause, "error");
    Bundle b; Buf summary = { 0 }, zip = { 0 };
    if (collect(&b, &summary) == 0 && save_zip(&b, &summary, o->zip, &zip)) {
        o->saved = 1;
        for (int i = 0; i < b.n; i++) buf_addf(&o->members, "%s  (%u KB)\n", b.m[i].name, (unsigned)((b.m[i].data.n + 1023) / 1024));
        if (send_it) { o->sent_tried = 1; send_report(&zip, summary.p ? summary.p : "", &o->sent); }
        else strcpy(o->sent.cause, "unset");
    }
    for (int i = 0; i < b.n; i++) buf_free(&b.m[i].data);
    buf_free(&b.notes); buf_free(&summary); buf_free(&zip);
}

/* ============================================================== window */
enum { ID_HEAD = 100, ID_BODY, ID_LABEL, ID_EDIT, ID_PRIVACY, ID_NOTE, ID_LINKS, ID_STATUS, ID_PROGRESS, ID_SEND, ID_CLOSE, ID_WAIT, ID_COPY, ID_OPEN, ID_RESULT, ID_HINT };
enum { MODE_MANUAL, MODE_CRASH, MODE_HANG };
#define WM_SENT (WM_APP + 1)
static int g_mode; static HWND g_wnd; static HFONT g_font, g_bold; static int g_dpi = 96; static int g_exit; static int g_busy;
static Outcome g_out;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static HWND item(int id) { return GetDlgItem(g_wnd, id); }
static void set_text8(int id, const char *u) { wchar_t w[2048]; u2w(u, w, 2048); SetWindowTextW(item(id), w); }

static int g_send_it;
static DWORD WINAPI send_thread(LPVOID p) {
    (void)p;
    run_report(g_send_it, &g_out);
    PostMessageW(g_wnd, WM_SENT, 0, 0);
    return 0;
}
static void folder_of(const wchar_t *path, char *out, size_t n) {
    wchar_t d[MAX_PATH]; wcscpy(d, path); wchar_t *s = wcsrchr(d, L'\\'); if (s) *s = 0;
    char u[MAX_PATH * 3]; w2u(d, u, sizeof u);
    Buf b = { 0 }; buf_adds(&b, u); redact(&b);
    snprintf(out, n, "%s", b.p ? b.p : ""); buf_free(&b);
}
static void show_result(void) {
    char head[400], body[1200], where[MAX_PATH * 3];
    ShowWindow(item(ID_PROGRESS), SW_HIDE);
    for (int id = ID_LABEL; id <= ID_STATUS; id++) ShowWindow(item(id), SW_HIDE);
    if (!g_out.saved) {
        snprintf(head, sizeof head, "Could not make the report.");
        snprintf(body, sizeof body, "Something went wrong while collecting the files. You can still report the problem on the issues page: %s", ISSUES_URL);
        ShowWindow(item(ID_COPY), SW_HIDE); ShowWindow(item(ID_OPEN), SW_HIDE);
    } else if (g_out.sent.ok) {
        snprintf(head, sizeof head, "Report sent. Your report number is %s.", g_out.sent.id);
        snprintf(body, sizeof body, "%s%sQuote this number in a Reddit post or a GitHub comment so the developer can find your report.", g_out.sent.message, g_out.sent.message[0] ? " " : "");
        ShowWindow(item(ID_COPY), SW_SHOW); ShowWindow(item(ID_OPEN), SW_HIDE);
    } else {
        folder_of(g_out.zip, where, sizeof where);
        if (!strcmp(g_out.sent.cause, "unset")) snprintf(head, sizeof head, "Report saved.");
        else snprintf(head, sizeof head, "Could not send (%s).", cause_text(g_out.sent.cause));
        snprintf(body, sizeof body, "%s%sThe report was saved to %s. Attach it to a problem report on %s", g_out.sent.message, g_out.sent.message[0] ? " " : "", where, ISSUES_URL);
        ShowWindow(item(ID_COPY), SW_HIDE); ShowWindow(item(ID_OPEN), SW_SHOW);
    }
    set_text8(ID_HEAD, head); set_text8(ID_RESULT, body); ShowWindow(item(ID_RESULT), SW_SHOW); ShowWindow(item(ID_BODY), SW_HIDE);
    ShowWindow(item(ID_SEND), SW_HIDE); ShowWindow(item(ID_HINT), SW_HIDE);
    EnableWindow(item(ID_CLOSE), TRUE); EnableWindow(item(ID_WAIT), TRUE);
    if (g_mode != MODE_HANG) SetWindowTextW(item(ID_CLOSE), L"Close");
    SetFocus(item(g_out.sent.ok ? ID_COPY : ID_CLOSE));
}
static void copy_id(void) {
    if (!g_out.sent.ok || !OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, 32 * sizeof(wchar_t));
    if (h) { wchar_t *w = (wchar_t *)GlobalLock(h); u2w(g_out.sent.id, w, 32); GlobalUnlock(h); if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h); }
    CloseClipboard();
    SetWindowTextW(item(ID_COPY), L"Copied");
}
static void whats_included(void) {
    /* the real member list, from a bundle made without saving or sending */
    Bundle b; Buf summary = { 0 };
    Buf t = { 0 };
    buf_adds(&t, "A report contains:\n\n");
    if (collect(&b, &summary) == 0) {
        system_txt(&b, &summary);
        for (int i = 0; i < b.n; i++) buf_addf(&t, "%s  (%u KB)\n", b.m[i].name, (unsigned)((b.m[i].data.n + 1023) / 1024));
        for (int i = 0; i < b.n; i++) buf_free(&b.m[i].data);
        buf_free(&b.notes);
    } else buf_adds(&t, "(the list could not be made)\n");
    buf_adds(&t, "\nYour Windows user name, PC name, personal folders and e-mail addresses are removed from every file. "
                 "Nothing else on your PC is read. A copy is saved in %LOCALAPPDATA%\\MechAssault2\\Diagnostics.");
    buf_free(&summary);
    wchar_t *w = (wchar_t *)malloc((t.n + 1) * sizeof(wchar_t) * 2);
    if (w) { MultiByteToWideChar(CP_UTF8, 0, t.p, -1, w, (int)t.n * 2 + 1); MessageBoxW(g_wnd, w, L"What's included", MB_OK | MB_ICONINFORMATION); free(w); }
    buf_free(&t);
}
static void start_send(int send_it) {
    if (g_busy) return;
    wchar_t w[MAX_TEXT + 8]; GetWindowTextW(item(ID_EDIT), w, MAX_TEXT + 1);
    w2u(w, g_text, sizeof g_text);
    { char *s = g_text; while (*s == ' ' || *s == '\r' || *s == '\n' || *s == '\t') s++; memmove(g_text, s, strlen(s) + 1); size_t l = strlen(g_text); while (l && strchr(" \r\n\t", g_text[l - 1])) g_text[--l] = 0; }
    g_busy = 1;
    EnableWindow(item(ID_SEND), FALSE); EnableWindow(item(ID_EDIT), FALSE);
    if (g_mode != MODE_HANG) EnableWindow(item(ID_CLOSE), FALSE);
    set_text8(ID_STATUS, send_it && g_relay[0] ? "Sending..." : "Saving...");
    ShowWindow(item(ID_STATUS), SW_SHOW); ShowWindow(item(ID_PROGRESS), SW_SHOW);
    SendMessageW(item(ID_PROGRESS), PBM_SETMARQUEE, TRUE, 30);
    g_send_it = send_it && g_relay[0];
    HANDLE h = CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    if (h) CloseHandle(h); else { g_busy = 0; run_report(g_send_it, &g_out); show_result(); }
}

static void layout(HWND w) {
    RECT r; GetClientRect(w, &r);
    int W = r.right, m = S(16), y = S(14), cw = W - 2 * m;
    MoveWindow(item(ID_HEAD), m, y, cw, S(24), TRUE); y += S(30);
    MoveWindow(item(ID_BODY), m, y, cw, S(36), TRUE);
    MoveWindow(item(ID_RESULT), m, y, cw, S(110), TRUE); y += S(44);
    MoveWindow(item(ID_LABEL), m, y, cw, S(18), TRUE); y += S(20);
    MoveWindow(item(ID_EDIT), m, y, cw, S(104), TRUE); y += S(108);
    MoveWindow(item(ID_PRIVACY), m, y, cw, S(18), TRUE); y += S(24);
    MoveWindow(item(ID_NOTE), m, y, cw, S(34), TRUE); y += S(36);
    if (g_mode == MODE_MANUAL) { MoveWindow(item(ID_HINT), m, y, cw, S(48), TRUE); y += S(50); }
    MoveWindow(item(ID_LINKS), m, y, cw, S(20), TRUE); y += S(26);
    MoveWindow(item(ID_STATUS), m, y, S(120), S(20), TRUE);
    MoveWindow(item(ID_PROGRESS), m + S(124), y + S(3), cw - S(124), S(14), TRUE);
    int by = r.bottom - S(44), bw = S(118), bh = S(30), x = W - m - bw;
    MoveWindow(item(ID_CLOSE), x, by, bw, bh, TRUE);
    if (g_mode == MODE_HANG) { x -= bw + S(8); MoveWindow(item(ID_WAIT), x, by, bw, bh, TRUE); }
    x -= bw + S(8);
    MoveWindow(item(ID_SEND), x, by, bw, bh, TRUE);
    MoveWindow(item(ID_COPY), x, by, bw, bh, TRUE);
    MoveWindow(item(ID_OPEN), x, by, bw, bh, TRUE);
}
static HFONT make_font(int pt, int weight) {
    NONCLIENTMETRICSW nm; nm.cbSize = sizeof nm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
    LOGFONTW lf = nm.lfMessageFont;
    lf.lfHeight = -MulDiv(pt, g_dpi, 72); lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}
static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = w;
        HINSTANCE hi = GetModuleHandleW(NULL);
        static const struct { int id; const wchar_t *cls; DWORD style; const wchar_t *text; } items[] = {
            { ID_HEAD, L"STATIC", SS_LEFT | SS_NOPREFIX, L"" }, { ID_BODY, L"STATIC", SS_LEFT | SS_NOPREFIX, L"" },
            { ID_RESULT, L"EDIT", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, L"" },
            { ID_LABEL, L"STATIC", SS_LEFT, L"What happened? (optional)" },
            { ID_EDIT, L"EDIT", WS_BORDER | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, L"" },
            { ID_PRIVACY, L"STATIC", SS_LEFT, L"Please don't include personal details." },
            { ID_NOTE, L"STATIC", SS_LEFT, L"Sends logs, settings and PC details to the developer. No names or personal files." },
            { ID_LINKS, L"SysLink", WS_TABSTOP, L"<a id=\"what\">What's included?</a>     <a id=\"save\">Save a copy instead</a>" },
            { ID_STATUS, L"STATIC", SS_LEFT, L"" }, { ID_PROGRESS, PROGRESS_CLASSW, PBS_MARQUEE, L"" },
            { ID_SEND, L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, L"Send report" }, { ID_CLOSE, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, L"Close" },
            { ID_WAIT, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, L"Keep waiting" }, { ID_COPY, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, L"Copy number" },
            { ID_OPEN, L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, L"Open folder" },
            /* 1.1.6 (owner): the manual window says when a report is most useful */
            { ID_HINT, L"STATIC", SS_LEFT, L"Best sent right after the problem: the logs cover your last 3 game sessions, and a bot match "
              L"recording only its last 2-3 minutes. If the game is still open, use Report a Problem in its pause menu instead." } };
        for (int i = 0; i < (int)(sizeof items / sizeof *items); i++) {
            HWND c = CreateWindowExW(0, items[i].cls, items[i].text, WS_CHILD | items[i].style, 0, 0, 10, 10, w, (HMENU)(INT_PTR)items[i].id, hi, NULL);
            SendMessageW(c, WM_SETFONT, (WPARAM)(items[i].id == ID_HEAD ? g_bold : g_font), TRUE);
            int hidden = items[i].id == ID_STATUS || items[i].id == ID_PROGRESS || items[i].id == ID_COPY || items[i].id == ID_OPEN || items[i].id == ID_RESULT ||
                         (items[i].id == ID_WAIT && g_mode != MODE_HANG) || (items[i].id == ID_HINT && g_mode != MODE_MANUAL);
            if (!hidden) ShowWindow(c, SW_SHOW);
        }
        SendMessageW(item(ID_EDIT), EM_LIMITTEXT, MAX_TEXT, 0);
        if (g_mode == MODE_CRASH) {
            SetWindowTextW(item(ID_HEAD), L"MechAssault 2 has stopped unexpectedly.");
            SetWindowTextW(item(ID_BODY), L"A crash report was saved. Send it so the developer can find and fix the problem.");
        } else if (g_mode == MODE_HANG) {
            SetWindowTextW(item(ID_HEAD), L"MechAssault 2 has stopped responding.");
            SetWindowTextW(item(ID_BODY), L"The game has not drawn a picture for a while. You can send a report, close the game, or keep waiting.");
            SetWindowTextW(item(ID_CLOSE), L"Close the game");
        } else {
            SetWindowTextW(item(ID_HEAD), L"Report a problem");
            SetWindowTextW(item(ID_BODY), L"Tell the developer what went wrong. The game's logs and settings are sent with your words.");
        }
        if (!g_relay[0]) {
            SetWindowTextW(item(ID_SEND), L"Save report");
            SetWindowTextW(item(ID_NOTE), L"Sending is not set up in this version: the report (logs, settings and PC details, no names or personal files) is saved on this PC.");
            SetWindowTextW(item(ID_LINKS), L"<a id=\"what\">What's included?</a>");
        }
        layout(w);
        return 0;
    }
    case WM_SIZE: layout(w); return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; int id = GetDlgCtrlID((HWND)lp);
        if (id == ID_PRIVACY || id == ID_STATUS) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->idFrom == ID_LINKS && (h->code == NM_CLICK || h->code == NM_RETURN)) {
            NMLINK *l = (NMLINK *)lp;
            if (!wcscmp(l->item.szID, L"what")) whats_included();
            else if (!wcscmp(l->item.szID, L"save")) start_send(0);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SEND: start_send(1); return 0;
        case ID_COPY: copy_id(); return 0;
        case ID_OPEN: { wchar_t d[MAX_PATH]; wcscpy(d, g_out.zip); wchar_t *s = wcsrchr(d, L'\\'); if (s) *s = 0; ShellExecuteW(w, L"open", d, NULL, NULL, SW_SHOWNORMAL); return 0; }
        case ID_CLOSE: g_exit = g_mode == MODE_HANG ? 10 : 0; DestroyWindow(w); return 0;
        case ID_WAIT: g_exit = 11; DestroyWindow(w); return 0;
        case IDCANCEL: if (!g_busy || g_mode == MODE_HANG) { g_exit = g_mode == MODE_HANG ? 11 : 0; DestroyWindow(w); } return 0;
        }
        break;
    case WM_SENT: g_busy = 0; show_result(); return 0;
    case WM_CLOSE: if (g_busy && g_mode != MODE_HANG) return 0; g_exit = g_mode == MODE_HANG ? 11 : 0; DestroyWindow(w); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
static int run_window(void) {
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LINK_CLASS | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&ic);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *DpiFn)(void);
    DpiFn gd = u ? (DpiFn)(void *)GetProcAddress(u, "GetDpiForSystem") : NULL;
    g_dpi = gd ? (int)gd() : 96;
    g_font = make_font(9, FW_NORMAL); g_bold = make_font(12, FW_SEMIBOLD);
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MA2ReportWindow";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    RECT r = { 0, 0, S(540), S(430) + (g_mode == MODE_MANUAL ? S(50) : 0) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - wh) / 3;
    wchar_t hidden[8]; int hide = env_get("HIDDEN", hidden, 8) && hidden[0] == L'1';
    HWND w = CreateWindowExW(hide ? 0 : WS_EX_TOPMOST, wc.lpszClassName, L"MechAssault 2 - Report a problem", style, x, y, ww, wh, NULL, NULL, wc.hInstance, NULL);
    if (!w) return 2;
    if (!hide) { ShowWindow(w, SW_SHOWNORMAL); SetForegroundWindow(w); }
    SetFocus(item(ID_EDIT));
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_RETURN && m.hwnd == item(ID_EDIT)) { TranslateMessage(&m); DispatchMessageW(&m); continue; }
        if (IsDialogMessageW(w, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return g_exit;
}

/* ============================================================== in-game mode */
static void result_line(Buf *b, const char *key, const char *value) {
    buf_adds(b, key); buf_addc(b, '=');
    for (const char *p = value; *p; p++) buf_addc(b, *p == '\n' || *p == '\r' ? ' ' : *p);
    buf_addc(b, '\n');
}
static int run_game_mode(const wchar_t *text_file, const wchar_t *result_file) {
    if (text_file[0]) {
        Buf t = { 0 };
        if (read_file(text_file, &t, 0, NULL) && t.p) {
            const char *s = t.p; if (t.n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;
            snprintf(g_text, sizeof g_text, "%s", s);
        }
        buf_free(&t);
    }
    SetEnvironmentVariableW(L"MA2_REPORT_FROM_GAME", L"1");
    Outcome o; run_report(g_relay[0] != 0, &o);
    Buf r = { 0 };
    char where[MAX_PATH * 3] = "";
    if (o.saved) folder_of(o.zip, where, sizeof where);
    result_line(&r, "saved", o.saved ? "1" : "0");
    result_line(&r, "ok", o.sent.ok ? "1" : "0");
    result_line(&r, "id", o.sent.id);
    result_line(&r, "cause", o.saved ? o.sent.cause : "error");
    result_line(&r, "cause_text", o.saved ? cause_text(o.sent.cause) : "the report could not be made");
    result_line(&r, "message", o.sent.message);
    result_line(&r, "folder", where);
    char http[16]; snprintf(http, sizeof http, "%d", o.sent.http); result_line(&r, "http", http);
    result_line(&r, "end", "1");
    if (result_file[0]) {
        wchar_t tmp[MAX_PATH + 8]; swprintf(tmp, MAX_PATH + 8, L"%s.part", result_file);
        if (write_file(tmp, r.p, r.n)) MoveFileExW(tmp, result_file, MOVEFILE_REPLACE_EXISTING);
    }
    buf_free(&r); buf_free(&o.members);
    return o.sent.ok ? 0 : o.saved ? 1 : 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t text_file[MAX_PATH] = L"", result_file[MAX_PATH] = L"";
    int game = 0, can_send = 0;
    strcpy(g.kind, "manual");
    for (int i = 1; argv && i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!wcscmp(a, L"--crash") || !wcscmp(a, L"--hang")) {
            g_mode = !wcscmp(a, L"--crash") ? MODE_CRASH : MODE_HANG;
            strcpy(g.kind, g_mode == MODE_CRASH ? "crash" : "hang");
            if (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2)) wcsncpy(g.dump, argv[++i], MAX_PATH - 1);
            if (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2)) wcsncpy(g.log, argv[++i], MAX_PATH - 1);
        } else if (!wcscmp(a, L"--log") && i + 1 < argc) wcsncpy(g.log, argv[++i], MAX_PATH - 1);
        else if (!wcscmp(a, L"--ini") && i + 1 < argc) wcsncpy(g.ini, argv[++i], MAX_PATH - 1);
        else if (!wcscmp(a, L"--sig") && i + 1 < argc) { w2u(argv[++i], g.sig, sizeof g.sig); sig_clean(g.sig); }
        else if (!wcscmp(a, L"--failure") && i + 1 < argc) w2u(argv[++i], g.failure, sizeof g.failure);
        else if (!wcscmp(a, L"--kind") && i + 1 < argc) { char k[16]; w2u(argv[++i], k, sizeof k); if (!strcmp(k, "crash") || !strcmp(k, "hang") || !strcmp(k, "manual")) strcpy(g.kind, k); }
        else if (!wcscmp(a, L"--game")) game = 1;
        else if (!wcscmp(a, L"--text-file") && i + 1 < argc) wcsncpy(text_file, argv[++i], MAX_PATH - 1);
        else if (!wcscmp(a, L"--result") && i + 1 < argc) wcsncpy(result_file, argv[++i], MAX_PATH - 1);
        else if (!wcscmp(a, L"--can-send")) can_send = 1;
    }
    if (argv) LocalFree(argv);
    secrets_init();
    ctx_init();
    if (g_mode == MODE_CRASH && !g.failure[0]) snprintf(g.failure, sizeof g.failure, "the game crashed%s%s", g.sig[0] ? ": " : "", g.sig);
    if (g_mode == MODE_HANG && !g.failure[0]) snprintf(g.failure, sizeof g.failure, "the game stopped responding");
    if (g_mode == MODE_HANG && !g.sig[0]) strcpy(g.sig, "hang");
    if (can_send) return g_relay[0] ? 0 : 3;
    if (game) return run_game_mode(text_file, result_file);
    return run_window();
}

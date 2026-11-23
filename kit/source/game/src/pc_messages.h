/* pc_messages.h -- 1.2.0: the main menu's Messages (pure helpers; pc_native_settings.cpp has the inbox itself).
 *
 * The inbox keeps its messages in messages.txt next to pc_settings.ini (%LOCALAPPDATA%\MechAssault2), one per line:
 *     kind <TAB> key <TAB> date yyyy-mm-dd <TAB> read 0|1 <TAB> extra <TAB> title <TAB> body
 * kind = update | whatsnew | reply; key = the version, or MA2-XXXXXX/n for a reply; extra = for a reply
 * "sent=yyyy-mm-dd can=0|1 answered=0|1", else "". Text fields are escaped (\\ \n \t), so a line holds no raw tab
 * or line break. Update and What's new messages are kept 6 months (MA2_MSG_KEEP_DAYS), replies 60 days (the life of a
 * report conversation). Nothing in this file is ever sent anywhere.
 * Header-only C; tests/game/report_replies_test.c tests it. */
#pragma once
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MA2_MSG_KEEP_DAYS 183
#define MA2_MSG_REPLY_DAYS 60

/* \ -> \\, new line -> \n, tab -> \t; carriage returns dropped. Returns the length written (cut at out_size - 1). */
static size_t ma2_msg_escape(const char *in, char *out, size_t out_size)
{
    size_t k = 0;
    if (!out_size) return 0;
    for (; in && *in; in++) {
        const char *rep = *in == '\\' ? "\\\\" : *in == '\n' ? "\\n" : *in == '\t' ? "\\t" : NULL;
        size_t n = rep ? 2 : 1;
        if (*in == '\r') continue;
        if (k + n >= out_size) break;
        if (rep) { out[k] = rep[0]; out[k + 1] = rep[1]; } else out[k] = *in;
        k += n;
    }
    out[k] = 0;
    return k;
}

static size_t ma2_msg_unescape(const char *in, char *out, size_t out_size)
{
    size_t k = 0;
    if (!out_size) return 0;
    for (; in && *in && k + 1 < out_size; in++) {
        if (*in == '\\' && (in[1] == 'n' || in[1] == 't' || in[1] == '\\')) { out[k++] = in[1] == 'n' ? '\n' : in[1] == 't' ? '\t' : '\\'; in++; }
        else out[k++] = *in;
    }
    out[k] = 0;
    return k;
}

static int ma2_msg_days(int y, int m, int d)   /* days since 1970-01-01 */
{
    unsigned yoe, doy, doe; int era;
    y -= m <= 2; era = (y >= 0 ? y : y - 399) / 400; yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1); doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

/* "yyyy-mm-dd" -> days, or -1 */
static int ma2_msg_parse_date(const char *s)
{
    int y, m, d;
    if (!s || strlen(s) < 10 || sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return ma2_msg_days(y, m, d);
}

static void ma2_msg_format_date(int days, char out[11])
{
    int z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, y; unsigned doe = (unsigned)(z - era * 146097), yoe, doy, mp, d, m;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; y = (int)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100); mp = (5 * doy + 2) / 153; d = doy - (153 * mp + 2) / 5 + 1; m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    snprintf(out, 11, "%04d-%02u-%02u", y, m, d);
}

/* "Nov 22" from yyyy-mm-dd */
static void ma2_msg_short_date(const char *ymd, char *out, size_t size)
{
    static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int y, m, d;
    if (!ymd || sscanf(ymd, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12) { snprintf(out, size, "%s", ymd ? ymd : ""); return; }
    snprintf(out, size, "%s %d", months[m - 1], d);
}

/* Is the line a version heading ("1.2.0", "1.1.10", optional spaces) with a line of dashes under it? */
static int ma2_changelog_heading(const char *line, size_t len, const char *next, size_t next_len, char *version, size_t vsize)
{
    size_t i = 0, k = 0, dots = 0, dashes = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    while (len && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) len--;
    if (i >= len || line[i] < '0' || line[i] > '9') return 0;
    for (; i < len; i++) {
        if (line[i] == '.') dots++;
        else if (line[i] < '0' || line[i] > '9') return 0;
        if (k + 1 < vsize) version[k++] = line[i];
    }
    version[k] = 0;
    if (!dots || dots > 3) return 0;
    for (i = 0; i < next_len; i++) {
        if (next[i] == '-') dashes++;
        else if (next[i] != ' ' && next[i] != '\t' && next[i] != '\r') return 0;
    }
    return dashes >= 3;
}

/* The section of Changelog.txt for `version`: the lines under its heading up to the next heading, without leading and
 * trailing blank lines (LF line ends). Returns its length; 0 (out = "") when there is none. */
static size_t ma2_changelog_section(const char *text, const char *version, char *out, size_t out_size)
{
    const char *p = text, *start = NULL, *end = NULL;
    char v[32];
    size_t k = 0;
    if (out_size) out[0] = 0;
    if (!text || !version || !out_size) return 0;
    while (*p) {
        const char *e = strchr(p, '\n'), *n = e ? e + 1 : p + strlen(p), *ne = strchr(n, '\n');
        size_t len = (size_t)((e ? e : p + strlen(p)) - p), nlen = (size_t)((ne ? ne : n + strlen(n)) - n);
        if (*n && ma2_changelog_heading(p, len, n, nlen, v, sizeof v)) {
            if (start) { end = p; break; }
            if (!strcmp(v, version)) start = ne ? ne + 1 : n + nlen;
        }
        p = n;
    }
    if (!start) return 0;
    if (!end) end = start + strlen(start);
    while (start < end && (*start == '\n' || *start == '\r' || *start == ' ')) {      /* leading blank lines */
        const char *q = start; while (q < end && (*q == ' ' || *q == '\r')) q++;
        if (q < end && *q != '\n') break;
        start = q < end ? q + 1 : q;
    }
    while (end > start && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) end--;
    for (; start < end && k + 1 < out_size; start++) if (*start != '\r') out[k++] = *start;
    out[k] = 0;
    return k;
}

/* Who updates this install, from recomp-install.json "managed_by" (written by the installer since 1.2.0):
 * "recomplauncher" -> 1, "installer" -> 0; a stamp without the field (older installs) -> env_recomplauncher (the
 * RECOMPLAUNCHER variable RecompLauncher sets when it starts the game) as the fallback. */
static int ma2_managed_by_launcher(const char *stamp_json, int env_recomplauncher)
{
    const char *p = stamp_json ? strstr(stamp_json, "\"managed_by\"") : NULL;
    if (!p) return env_recomplauncher ? 1 : 0;
    p += 12; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    if (!strncmp(p, "\"recomplauncher\"", 16)) return 1;
    if (!strncmp(p, "\"installer\"", 11)) return 0;
    return env_recomplauncher ? 1 : 0;
}

/* What "Update now" does for an update message. */
enum { MA2_UPDATE_HISTORY = 0, MA2_UPDATE_IN_LAUNCHER = 1, MA2_UPDATE_RUN_INSTALLER = 2, MA2_UPDATE_RELEASES_PAGE = 3 };
static int ma2_update_action(int newer_than_installed, int from_recomplauncher, int installer_copy_exists)
{
    if (!newer_than_installed) return MA2_UPDATE_HISTORY;
    if (from_recomplauncher) return MA2_UPDATE_IN_LAUNCHER;
    return installer_copy_exists ? MA2_UPDATE_RUN_INSTALLER : MA2_UPDATE_RELEASES_PAGE;
}

#ifdef __cplusplus
}
#endif

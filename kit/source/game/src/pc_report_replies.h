/* pc_report_replies.h -- 1.2.0: the developer's replies to reports sent from this PC, on the main menu.
 *
 * The game never talks to the relay for this itself and never reads the conversation keys: MA2 Report.exe does
 * (report-helper/report.c --replies / --seen / --answer), and only when %LOCALAPPDATA%\MechAssault2\
 * report-conversations.txt exists (a report was sent from this PC; the helper itself asks only for entries under 60
 * days old). The helper writes one line per reply:
 *     reply=<MA2-XXXXXX> <n> <sent yyyy-mm-dd> <can answer 0|1> <text, "\n" = new line, "\\" = backslash>
 * This header parses those lines and formats the head line; pc_native_settings.cpp draws the box and runs the helper.
 * Header-only (C and C++); tests/game/report_replies_test.c tests it. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MA2_REPLY_MAX_TEXT 4096          /* bytes of UTF-8: 1000 characters at most */

typedef struct {
    char id[11];                         /* MA2-XXXXXX */
    int n;                               /* the reply's number in its conversation */
    int year, month, day;                /* the date the report was sent */
    int can_answer;                      /* 1 = the newest message: the player may answer it */
    char text[MA2_REPLY_MAX_TEXT];       /* UTF-8, real new lines */
} Ma2Reply;

static int ma2_reply_id_ok(const char *s)
{
    if (strlen(s) != 10 || strncmp(s, "MA2-", 4)) return 0;
    for (int i = 4; i < 10; i++) if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '2' && s[i] <= '7'))) return 0;
    return 1;
}

/* One "reply=" line (without the line end). Returns 1 and fills r, or 0 for anything malformed. */
static int ma2_reply_parse_line(const char *line, Ma2Reply *r)
{
    char id[16], date[16];
    int n = 0, can = 0, used = 0;
    const char *p;
    size_t k = 0;
    memset(r, 0, sizeof *r);
    if (strncmp(line, "reply=", 6)) return 0;
    if (sscanf(line + 6, "%15s %d %15s %d %n", id, &n, date, &can, &used) != 4 || used <= 0) return 0;
    if (!ma2_reply_id_ok(id) || n <= 0 || (can != 0 && can != 1)) return 0;
    if (strlen(date) != 10 || sscanf(date, "%4d-%2d-%2d", &r->year, &r->month, &r->day) != 3 || r->month < 1 || r->month > 12 ||
        r->day < 1 || r->day > 31) return 0;
    strcpy(r->id, id); r->n = n; r->can_answer = can;
    for (p = line + 6 + used; *p && k + 1 < sizeof r->text; p++) {
        if (*p == '\r') continue;
        if (*p == '\\' && (p[1] == 'n' || p[1] == '\\')) { r->text[k++] = p[1] == 'n' ? '\n' : '\\'; p++; continue; }
        r->text[k++] = *p;
    }
    while (k && (r->text[k - 1] == ' ' || r->text[k - 1] == '\n')) k--;
    r->text[k] = 0;
    return k > 0;
}

/* All replies in the helper's result text (at most max). Returns the count; *complete = 1 when the "end=1" line is there. */
static int ma2_reply_parse_result(const char *text, Ma2Reply *out, int max, int *complete)
{
    int count = 0;
    const char *p = text;
    char line[MA2_REPLY_MAX_TEXT * 2 + 64];
    if (complete) *complete = 0;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len < sizeof line) {
            memcpy(line, p, len); line[len] = 0;
            if (len && line[len - 1] == '\r') line[len - 1] = 0;
            if (!strcmp(line, "end=1") && complete) *complete = 1;
            else if (count < max && ma2_reply_parse_line(line, &out[count])) count++;
        }
        p = e ? e + 1 : NULL;
    }
    return count;
}

/* "Reply to your report MA2-XXXXXX (Nov 22)" */
static void ma2_reply_head(const Ma2Reply *r, char *out, size_t size)
{
    static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    snprintf(out, size, "Reply to your report %s (%s %d)", r->id, months[(r->month - 1) % 12], r->day);
}

#ifdef __cplusplus
}
#endif

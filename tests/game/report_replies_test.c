/* 1.2.0: the game's parsing of MA2 Report.exe --replies results (kit/source/game/src/pc_report_replies.h).
   Built and run by tests/installer/test_replies_120.py (cl /nologo report_replies_test.c), or by hand. Exit 0 = pass. */
#include <stdio.h>
#include <string.h>
#include "../../kit/source/game/src/pc_report_replies.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    Ma2Reply r, many[4];
    char head[96];
    int complete = 0;
    CHECK(ma2_reply_parse_line("reply=MA2-H3DVVH 1 2026-11-22 1 Thanks!\\nDoes it \\\\ crash?", &r));
    CHECK(!strcmp(r.id, "MA2-H3DVVH") && r.n == 1 && r.can_answer == 1 && r.year == 2026 && r.month == 11 && r.day == 22);
    CHECK(!strcmp(r.text, "Thanks!\nDoes it \\ crash?"));
    ma2_reply_head(&r, head, sizeof head);
    CHECK(!strcmp(head, "Reply to your report MA2-H3DVVH (Nov 22)"));
    CHECK(ma2_reply_parse_line("reply=MA2-AAAAAA 3 2026-01-05 0 caf\xC3\xA9 \\x stays", &r) && r.can_answer == 0 && !strcmp(r.text, "caf\xC3\xA9 \\x stays"));
    ma2_reply_head(&r, head, sizeof head);
    CHECK(!strcmp(head, "Reply to your report MA2-AAAAAA (Jan 5)"));
    /* malformed lines are refused */
    CHECK(!ma2_reply_parse_line("reply=MA2-000000 1 2026-11-22 1 x", &r));       /* 0 is not in the id alphabet */
    CHECK(!ma2_reply_parse_line("reply=MA2-AAAAAA 0 2026-11-22 1 x", &r));
    CHECK(!ma2_reply_parse_line("reply=MA2-AAAAAA 1 2026-13-22 1 x", &r));
    CHECK(!ma2_reply_parse_line("reply=MA2-AAAAAA 1 2026-11-22 2 x", &r));
    CHECK(!ma2_reply_parse_line("reply=MA2-AAAAAA 1 2026-11-22 1 ", &r));        /* no text */
    CHECK(!ma2_reply_parse_line("count=1", &r));
    /* a whole result */
    CHECK(ma2_reply_parse_result("reply=MA2-AAAAAA 1 2026-11-22 1 one\r\nreply=bad\nreply=MA2-BBBBBB 2 2026-11-20 0 two\ncount=2\nend=1\n",
                                 many, 4, &complete) == 2 && complete == 1);
    CHECK(!strcmp(many[0].text, "one") && !strcmp(many[1].id, "MA2-BBBBBB"));
    CHECK(ma2_reply_parse_result("count=0\n", many, 4, &complete) == 0 && complete == 0);
    CHECK(ma2_reply_parse_result("", many, 4, &complete) == 0);
    /* a text at the size limit is cut, never overrun */
    {
        static char big[MA2_REPLY_MAX_TEXT * 2 + 64];
        strcpy(big, "reply=MA2-AAAAAA 1 2026-11-22 1 ");
        memset(big + strlen(big), 'x', MA2_REPLY_MAX_TEXT + 100);
        CHECK(ma2_reply_parse_line(big, &r) && strlen(r.text) == MA2_REPLY_MAX_TEXT - 1);
    }
    printf(fails ? "report_replies_test: %d failed\n" : "report_replies_test: all passed\n", fails);
    return fails ? 1 : 0;
}

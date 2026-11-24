/* 1.2.0: the game's parsing of MA2 Report.exe --replies results (kit/source/game/src/pc_report_replies.h).
   Built and run by tests/installer/test_replies_120.py (cl /nologo report_replies_test.c), or by hand. Exit 0 = pass. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../../kit/source/game/src/pc_report_replies.h"
#include "../../kit/source/game/src/pc_messages.h"
#define MA2_UPDATE_NO_NETWORK
#include "../../kit/source/game/src/pc_update_check.h"

static const char *CHANGELOG =
    "MechAssault 2 PC recompilation - changes\r\n========================================\r\n\r\nYou have version 1.2.0 (build x).\r\n\r\n\r\n"
    "1.2.1\r\n-----\r\n- Fixed the music stopping.\r\n  Second line of it.\r\n\r\n"
    "1.2.0\r\n-----\r\n- Split screen is back.\r\n\r\n1.1.10\r\n------\r\n- Older.\r\n";

static int messages_tests(void)
{
    int fails = 0;
    char out[512], back[512], d[11];
#define MCHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
    MCHECK(ma2_changelog_section(CHANGELOG, "1.2.1", out, sizeof out) && !strcmp(out, "- Fixed the music stopping.\n  Second line of it."));
    MCHECK(ma2_changelog_section(CHANGELOG, "1.2.0", out, sizeof out) && !strcmp(out, "- Split screen is back."));
    MCHECK(ma2_changelog_section(CHANGELOG, "1.1.10", out, sizeof out) && !strcmp(out, "- Older."));
    MCHECK(ma2_changelog_section(CHANGELOG, "1.1.1", out, sizeof out) == 0 && out[0] == 0);
    MCHECK(ma2_changelog_section(CHANGELOG, "9.9.9", out, sizeof out) == 0);
    MCHECK(ma2_changelog_section(CHANGELOG, "1.2.1", out, 10) == 9);                 /* cut, never overrun */
    ma2_msg_escape("a\\b\nc\td\r", out, sizeof out);
    MCHECK(!strcmp(out, "a\\\\b\\nc\\td"));
    ma2_msg_unescape(out, back, sizeof back);
    MCHECK(!strcmp(back, "a\\b\nc\td"));
    MCHECK(ma2_msg_parse_date("2026-11-23") - ma2_msg_parse_date("2026-05-24") == 183);
    ma2_msg_format_date(ma2_msg_parse_date("2024-02-29") + 1, d);
    MCHECK(!strcmp(d, "2024-03-01"));
    MCHECK(ma2_msg_parse_date("2026-13-01") == -1);
    ma2_msg_short_date("2026-11-03", out, sizeof out);
    MCHECK(!strcmp(out, "Nov 3"));
    MCHECK(ma2_update_action(1, 1, 1) == MA2_UPDATE_IN_LAUNCHER);
    MCHECK(ma2_update_action(1, 0, 1) == MA2_UPDATE_RUN_INSTALLER);
    MCHECK(ma2_update_action(1, 0, 0) == MA2_UPDATE_RELEASES_PAGE);
    MCHECK(ma2_update_action(0, 0, 1) == MA2_UPDATE_HISTORY);
    {   /* 1.2.0 rename: "Update now" starts the new-named copy, else the earlier-named one, else none */
        int both[2] = {1, 1}, only_new[2] = {1, 0}, only_old[2] = {0, 1}, none[2] = {0, 0};
        MCHECK(MA2_INSTALLER_COPY_COUNT == 2);
        MCHECK(!wcscmp(MA2_INSTALLER_COPIES[0], L"MechAssault 2 Expanded Installer.exe"));
        MCHECK(!wcscmp(MA2_INSTALLER_COPIES[1], L"MechAssault 2 Recomp Installer.exe"));
        MCHECK(ma2_pick_installer_copy(both) == 0);
        MCHECK(ma2_pick_installer_copy(only_new) == 0);
        MCHECK(ma2_pick_installer_copy(only_old) == 1);
        MCHECK(ma2_pick_installer_copy(none) == -1);
    }
    {   /* reflow: a bullet's continuation lines join; new bullets and blank lines stay */
        char t[256] = "- Split screen is back. In the menus, choose Add Guest or\n  press START on a second controller.\n- Second bullet.\n\nAfter a blank line\n  joined too.";
        ma2_msg_reflow(t);
        MCHECK(!strcmp(t, "- Split screen is back. In the menus, choose Add Guest or press START on a second controller.\n- Second bullet.\n\nAfter a blank line joined too."));
        ma2_msg_reflow(t);
        MCHECK(!strcmp(t, "- Split screen is back. In the menus, choose Add Guest or press START on a second controller.\n- Second bullet.\n\nAfter a blank line joined too."));
    }
    /* who updates: the stamp decides; RECOMPLAUNCHER only when the stamp says nothing */
    MCHECK(ma2_managed_by_launcher("{\"version\":\"1\",\"managed_by\": \"recomplauncher\"}", 0) == 1);   /* launcher stamp, no env */
    MCHECK(ma2_managed_by_launcher("{\"managed_by\":\"installer\"}", 1) == 0);                          /* standalone + stray env */
    MCHECK(ma2_managed_by_launcher("{\"version\":\"1.1.8\"}", 1) == 1);                                 /* old stamp: the env */
    MCHECK(ma2_managed_by_launcher("{\"version\":\"1.1.8\"}", 0) == 0);
    MCHECK(ma2_managed_by_launcher(NULL, 0) == 0);
    /* the relay's notes in /v1/latest */
    MCHECK(ma2_update_parse_notes("{\"ok\":true,\"latest\":\"1.2.1\",\"notes\":\"- A \\\"fix\\\"\\n- caf\\u00e9 \\ud83d\\ude00\\tx\\u0007\"}", out, sizeof out)
           && !strcmp(out, "- A \"fix\"\n- caf\xC3\xA9 \xF0\x9F\x98\x80 x"));
    MCHECK(!ma2_update_parse_notes("{\"ok\":true,\"latest\":\"1.2.1\"}", out, sizeof out) && out[0] == 0);
    MCHECK(ma2_update_parse_notes("{\"notes\":\"abcdef\"}", out, 4) && !strcmp(out, "abc"));
    return fails;
}

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
    fails += messages_tests();
    printf(fails ? "report_replies_test: %d failed\n" : "report_replies_test: all passed\n", fails);
    return fails ? 1 : 0;
}

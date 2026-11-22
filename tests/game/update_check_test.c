/* 1.1.7 unit test: the update line's version parsing and comparing, the relay answer parser and the
 * override address check (kit/source/game/src/pc_update_check.h, without its network part).
 * Build from the repo root: cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS tests\game\update_check_test.c /Fe:update_check_test.exe
 * (tests/installer/test_update_notice.py builds and runs it when the MSVC tools are found). */
#define MA2_UPDATE_NO_NETWORK
#include "../../kit/source/game/src/pc_update_check.h"

static int checks, failures;
static void check(int ok, const char *what) { checks++; if (!ok) { failures++; printf("FAIL %s\n", what); } }

int main(void)
{
    char out[16];
    int p[4], pre;

    /* versions */
    check(ma2_update_parse_version("1.1.7", p, &pre) == 3 && p[0] == 1 && p[1] == 1 && p[2] == 7 && !pre, "parse 1.1.7");
    check(ma2_update_parse_version("1.1.7-preview.2", p, &pre) == 3 && pre, "parse preview");
    check(ma2_update_parse_version("2.0", p, &pre) == 2, "parse 2.0");
    check(ma2_update_parse_version("1.2.3.4", p, &pre) == 4, "parse four parts");
    check(!ma2_update_parse_version("1.2.3.4.5", p, &pre), "five parts refused");
    check(!ma2_update_parse_version("", p, &pre) && !ma2_update_parse_version(NULL, p, &pre), "empty refused");
    check(!ma2_update_parse_version("v1.1.7", p, &pre), "leading v refused");
    check(!ma2_update_parse_version("1..7", p, &pre) && !ma2_update_parse_version("1.1.", p, &pre), "empty part refused");
    check(!ma2_update_parse_version("1.1.7 ", p, &pre) && !ma2_update_parse_version("1.1.7-", p, &pre), "trailing junk refused");
    check(!ma2_update_parse_version("1.1.7-<b>", p, &pre), "odd suffix refused");
    check(!ma2_update_parse_version("12345.1", p, &pre), "five-digit part refused");

    /* newer */
    check(ma2_update_is_newer("1.1.8", "1.1.7"), "1.1.8 > 1.1.7");
    check(ma2_update_is_newer("1.2", "1.1.7"), "1.2 > 1.1.7");
    check(ma2_update_is_newer("2.0.0", "1.1.7"), "2.0.0 > 1.1.7");
    check(ma2_update_is_newer("1.1.10", "1.1.9"), "numeric, not text: 1.1.10 > 1.1.9");
    check(!ma2_update_is_newer("1.1.7", "1.1.7"), "same: not newer");
    check(!ma2_update_is_newer("1.1.7.0", "1.1.7"), "1.1.7.0 == 1.1.7");
    check(!ma2_update_is_newer("1.1.6", "1.1.7"), "older: not newer");
    check(!ma2_update_is_newer("1.0.99", "1.1.7"), "older minor: not newer");
    check(ma2_update_is_newer("1.1.7", "1.1.7-preview.3"), "release above its preview");
    check(!ma2_update_is_newer("1.1.8-preview.1", "1.1.7"), "a preview is never announced");
    check(!ma2_update_is_newer("1", "0.9"), "one-part latest refused");
    check(!ma2_update_is_newer("1.1.8", "dev"), "own version unknown: nothing");
    check(!ma2_update_is_newer(NULL, "1.1.7") && !ma2_update_is_newer("1.1.8", NULL), "NULL safe");

    /* the relay's answer */
    check(ma2_update_parse_reply("{\"ok\":true,\"latest\":\"1.1.8\"}", out, sizeof out) && !strcmp(out, "1.1.8"), "reply plain");
    check(ma2_update_parse_reply(" {\n \"ok\" : true , \"latest\" : \"1.1.8\", \"notice\": \"x\"}", out, sizeof out) && !strcmp(out, "1.1.8"),
          "reply spaced, notice ignored");
    /* 1.1.7 self-update: the relay also gives the installer's sha256, size and asset name; the game reads only latest */
    check(ma2_update_parse_reply("{\"ok\":true,\"latest\":\"1.1.8\",\"notice\":\"Please update.\",\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"size\":23107422,\"asset\":\"MechAssault 2 Recomp Installer.exe\"}", out, sizeof out) && !strcmp(out, "1.1.8"),
          "reply with the installer fields");
    check(!ma2_update_parse_reply("{\"ok\":true,\"latest\":null}", out, sizeof out) && !out[0], "latest null");
    check(!ma2_update_parse_reply("{\"ok\":false,\"latest\":\"1.1.8\"}", out, sizeof out), "ok false");
    check(!ma2_update_parse_reply("{\"latest\":\"1.1.8\"}", out, sizeof out), "ok missing");
    check(!ma2_update_parse_reply("<html>{\"ok\":true,\"latest\":\"1.1.8\"}</html>", out, sizeof out), "html portal");
    check(!ma2_update_parse_reply("{\"ok\":true,\"latest\":\"1.1.8-preview.1\"}", out, sizeof out), "preview latest");
    check(!ma2_update_parse_reply("{\"ok\":true,\"latest\":\"Version 9 is out! Click here\"}", out, sizeof out), "text latest");
    check(!ma2_update_parse_reply("{\"ok\":true,\"latest\":\"1.1.8.0.0.0.0.0.0\"}", out, sizeof out), "long latest");
    check(!ma2_update_parse_reply("{\"ok\":true,\"latest\":1.1}", out, sizeof out), "number latest");
    check(!ma2_update_parse_reply("", out, sizeof out) && !ma2_update_parse_reply(NULL, out, sizeof out), "empty body");
    check(!ma2_update_parse_reply("\x01\x02garbage\xff", out, sizeof out), "garbage");
    check(!ma2_update_parse_reply("{\"ok\":true,\"latest\":\"1.1.8\"}", out, 4), "small buffer");

    /* override addresses */
    check(ma2_update_relay_ok("https://relay.example"), "https ok");
    check(ma2_update_relay_ok("http://127.0.0.1:8799") && ma2_update_relay_ok("http://localhost:1") && ma2_update_relay_ok("http://[::1]:5"),
          "loopback http ok");
    check(!ma2_update_relay_ok("http://relay.example"), "plain http elsewhere refused");
    check(!ma2_update_relay_ok("http://localhost.example") && !ma2_update_relay_ok("http://127.0.0.1.example.com"),
          "look-alike loopback names refused");
    check(!ma2_update_relay_ok("https://") && !ma2_update_relay_ok("https:///x") && !ma2_update_relay_ok("") && !ma2_update_relay_ok(NULL),
          "empty host refused");
    check(!ma2_update_relay_ok("ftp://x") && !ma2_update_relay_ok("file:///c:/x"), "other schemes refused");
    check(ma2_update_relay_ok(MA2_UPDATE_RELAY) && !strncmp(MA2_UPDATE_RELAY, "https://", 8), "compiled-in address is https");

    printf("update_check_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

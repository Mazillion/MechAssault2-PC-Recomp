/* pc_update_check.h -- 1.1.7: one quiet "new version" line on the main menu.
 *
 * Once per launch a background thread asks the project's relay GET <relay>/v1/latest, which answers
 * {"ok": true, "latest": "1.1.8" | null, ...}. When "latest" is a plain release number above this
 * install's version, the main menu shows one line (pc_native_settings.cpp, render()). It is never a
 * prompt, never shown in a match, and any failure (no network, a timeout, a refused or odd answer)
 * shows nothing: the game is then exactly as in 1.1.6.
 *
 * What is sent: one GET with the User-Agent "MechAssault2-Recomp/<version>" and Accept: application/json.
 * No ids, no machine data, no settings, no cookies. Redirects are not followed; the answer is read up
 * to 4 KB. The relay route keeps nothing (relay/src/worker.js handleLatest).
 *
 * Off switch: [Update] CheckForUpdates=0 in pc_settings.ini, or RECOMP_UPDATE_CHECK=0 (env wins). Off
 * means no thread and no request at all.
 * The relay address is compiled in (MA2_UPDATE_RELAY, the same address as kit/installer/data/report.json;
 * tests/installer/test_update_notice.py checks they match). Tests: RECOMP_UPDATE_RELAY, else
 * MA2_REPORT_RELAY / RECOMP_REPORT_RELAY (the report helper's override): https, or http on this PC
 * only. Harness runs (RECOMP_SAVE_DIR set) never ask the compiled-in relay: they check only with an
 * override, so test runs never reach the real relay.
 *
 * Header-only (C and C++), included once by pc_native_settings.cpp; tests/update_check_test.c tests the
 * parsing and comparing. WinHTTP is loaded at run time: a PC without it simply never shows the line. */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MA2_UPDATE_RELAY
#define MA2_UPDATE_RELAY "https://ma2-report-relay.ma2-relay.workers.dev"
#endif
#define MA2_UPDATE_TIMEOUT_MS 5000
#define MA2_UPDATE_MAX_BODY 4096

#ifdef __cplusplus
extern "C" {
#endif

/* "1.2.3" (1 to 4 numbers, each 0..9999) with an optional "-suffix" (a preview). Returns the count of
 * numbers (0 = not a version); *pre = 1 when a suffix follows. Plain digits and dots only before it. */
static int ma2_update_parse_version(const char *s, int parts[4], int *pre)
{
    int n = 0;
    if (pre) *pre = 0;
    parts[0] = parts[1] = parts[2] = parts[3] = 0;
    if (!s) return 0;
    for (;;) {
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { if (++digits > 4) return 0; v = v * 10 + (*s++ - '0'); }
        if (!digits || n == 4) return 0;
        parts[n++] = v;
        if (*s == '.') { s++; continue; }
        if (*s == 0) return n;
        if (*s == '-' && s[1]) {
            for (s++; *s; s++)
                if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '.')) return 0;
            if (pre) *pre = 1;
            return n;
        }
        return 0;
    }
}

/* 1 when latest is a plain release (no suffix, at least two numbers) above own. own may carry a
 * preview suffix: 1.1.7-preview.2 is below 1.1.7. An own version that does not parse gives 0. */
static int ma2_update_is_newer(const char *latest, const char *own)
{
    int a[4], b[4], apre, bpre, an = ma2_update_parse_version(latest, a, &apre), bn = ma2_update_parse_version(own, b, &bpre);
    if (an < 2 || apre || bn < 1) return 0;
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] > b[i];
    return bpre;   /* same numbers: newer only for a preview build */
}

/* The relay's answer: needs "ok": true and "latest": "<plain release>". Copies latest (at most 15
 * characters) into out; 0 on anything else (null, missing, odd JSON, an HTML portal page). */
static int ma2_update_parse_reply(const char *body, char *out, size_t out_size)
{
    const char *p, *q;
    int parts[4], pre;
    char v[16];
    if (out && out_size) out[0] = 0;
    if (!body || !out || out_size < 2) return 0;
    while (*body == ' ' || *body == '\t' || *body == '\r' || *body == '\n') body++;
    if (*body != '{') return 0;
    p = strstr(body, "\"ok\"");
    if (!p) return 0;
    p += 4; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    if (strncmp(p, "true", 4)) return 0;
    p = strstr(body, "\"latest\"");
    if (!p) return 0;
    p += 8; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    if (*p != '"') return 0;
    q = strchr(++p, '"');
    if (!q || q - p < 3 || (size_t)(q - p) >= sizeof v) return 0;
    memcpy(v, p, (size_t)(q - p)); v[q - p] = 0;
    if (ma2_update_parse_version(v, parts, &pre) < 2 || pre) return 0;
    if (strlen(v) >= out_size) return 0;
    strcpy(out, v);
    return 1;
}

/* An override address: https://<host>..., or plain http to this PC only (127.0.0.1, localhost or [::1],
 * followed by a port, a path or the end: "http://localhost.example" is not this PC). */
static int ma2_update_relay_ok(const char *u)
{
    static const char *const local[] = { "http://127.0.0.1", "http://localhost", "http://[::1]" };
    if (!u || !*u) return 0;
    if (!_strnicmp(u, "https://", 8)) return u[8] && u[8] != '/' && u[8] != ':';
    for (int i = 0; i < 3; i++) {
        size_t n = strlen(local[i]);
        if (!_strnicmp(u, local[i], n) && (u[n] == 0 || u[n] == ':' || u[n] == '/')) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ the check (game only) */
#ifndef MA2_UPDATE_NO_NETWORK
#include <winhttp.h>
#include "platform/pc_switch.h"

/* 0 idle / off, 1 asking, 2 done (nothing to show), 3 a newer version: show the line. */
static volatile LONG g_update_state;
static char g_update_latest[16];
static char g_update_url[512];
static char g_update_own[32];

/* This install's version: recomp-install.json "public_version" beside the exe, else the build's own. */
static void ma2_update_own_version(char *out, size_t size, const char *fallback)
{
    wchar_t path[MAX_PATH + 32], *slash;
    char buf[8192], *p, *q;
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH), got = 0;
    HANDLE h;
    snprintf(out, size, "%s", fallback);
    if (!n || n >= MAX_PATH || !(slash = wcsrchr(path, L'\\'))) return;
    wcscpy(slash + 1, L"recomp-install.json");
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof buf - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    if (!(p = strstr(buf, "\"public_version\""))) return;
    p += 16; while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"' || !(q = strchr(++p, '"')) || q == p || (size_t)(q - p) >= size || q - p > 31) return;
    {
        char v[32]; int parts[4], pre;
        memcpy(v, p, (size_t)(q - p)); v[q - p] = 0;
        if (ma2_update_parse_version(v, parts, &pre)) snprintf(out, size, "%s", v);
    }
}

typedef HINTERNET (WINAPI *ma2_WinHttpOpen_t)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *ma2_WinHttpConnect_t)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *ma2_WinHttpOpenRequest_t)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *ma2_WinHttpSendRequest_t)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *ma2_WinHttpReceiveResponse_t)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *ma2_WinHttpQueryHeaders_t)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *ma2_WinHttpReadData_t)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *ma2_WinHttpSetTimeouts_t)(HINTERNET, int, int, int, int);
typedef BOOL (WINAPI *ma2_WinHttpSetOption_t)(HINTERNET, DWORD, LPVOID, DWORD);
typedef BOOL (WINAPI *ma2_WinHttpCrackUrl_t)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
typedef BOOL (WINAPI *ma2_WinHttpCloseHandle_t)(HINTERNET);

/* GET <base>/v1/latest. Returns the HTTP status (0 = no answer) and the body (NUL-terminated, cut at
 * cap - 1 bytes); *cause names a failure for the log. */
static int ma2_update_get(const char *base, const char *version, char *body, size_t cap, const char **cause)
{
    HMODULE w = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ma2_WinHttpOpen_t pOpen; ma2_WinHttpConnect_t pConnect; ma2_WinHttpOpenRequest_t pOpenRequest;
    ma2_WinHttpSendRequest_t pSend; ma2_WinHttpReceiveResponse_t pReceive; ma2_WinHttpQueryHeaders_t pQuery;
    ma2_WinHttpReadData_t pRead; ma2_WinHttpSetTimeouts_t pTimeouts; ma2_WinHttpSetOption_t pOption;
    ma2_WinHttpCrackUrl_t pCrack; ma2_WinHttpCloseHandle_t pClose;
    wchar_t url[600], host[256], path[512], ua[64];
    URL_COMPONENTS uc;
    HINTERNET s = NULL, c = NULL, rq = NULL;
    int status = 0;
    size_t used = 0;
    body[0] = 0;
    *cause = "no_winhttp";
    if (!w) return 0;
    pOpen = (ma2_WinHttpOpen_t)(void *)GetProcAddress(w, "WinHttpOpen");
    pConnect = (ma2_WinHttpConnect_t)(void *)GetProcAddress(w, "WinHttpConnect");
    pOpenRequest = (ma2_WinHttpOpenRequest_t)(void *)GetProcAddress(w, "WinHttpOpenRequest");
    pSend = (ma2_WinHttpSendRequest_t)(void *)GetProcAddress(w, "WinHttpSendRequest");
    pReceive = (ma2_WinHttpReceiveResponse_t)(void *)GetProcAddress(w, "WinHttpReceiveResponse");
    pQuery = (ma2_WinHttpQueryHeaders_t)(void *)GetProcAddress(w, "WinHttpQueryHeaders");
    pRead = (ma2_WinHttpReadData_t)(void *)GetProcAddress(w, "WinHttpReadData");
    pTimeouts = (ma2_WinHttpSetTimeouts_t)(void *)GetProcAddress(w, "WinHttpSetTimeouts");
    pOption = (ma2_WinHttpSetOption_t)(void *)GetProcAddress(w, "WinHttpSetOption");
    pCrack = (ma2_WinHttpCrackUrl_t)(void *)GetProcAddress(w, "WinHttpCrackUrl");
    pClose = (ma2_WinHttpCloseHandle_t)(void *)GetProcAddress(w, "WinHttpCloseHandle");
    if (!pOpen || !pConnect || !pOpenRequest || !pSend || !pReceive || !pQuery || !pRead || !pTimeouts || !pOption || !pCrack || !pClose)
        return 0;
    *cause = "bad_address";
    if (MultiByteToWideChar(CP_UTF8, 0, base, -1, url, 580) <= 0) return 0;
    wcscat(url, L"/v1/latest");
    memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 512;
    if (!pCrack(url, 0, 0, &uc)) return 0;
    swprintf(ua, 64, L"MechAssault2-Recomp/%hs", version);
    s = pOpen(ua, 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, Windows 8.1+ */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = pOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    *cause = "no_session";
    if (s) {
        pTimeouts(s, MA2_UPDATE_TIMEOUT_MS, MA2_UPDATE_TIMEOUT_MS, MA2_UPDATE_TIMEOUT_MS, MA2_UPDATE_TIMEOUT_MS);
        c = pConnect(s, host, uc.nPort, 0);
    }
    if (c) rq = pOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (rq) {
        DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, rto = MA2_UPDATE_TIMEOUT_MS;
        DWORD nocookies = WINHTTP_DISABLE_COOKIES;
        pOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
        pOption(rq, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &rto, sizeof rto);
        pOption(rq, WINHTTP_OPTION_DISABLE_FEATURE, &nocookies, sizeof nocookies);
        if (pSend(rq, L"Accept: application/json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && pReceive(rq, NULL)) {
            DWORD st = 0, sl = sizeof st, got = 0;
            pQuery(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sl, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            while (used + 1 < cap && pRead(rq, body + used, (DWORD)(cap - 1 - used), &got) && got) used += got;
            body[used] = 0;
            *cause = "answer";
        } else {
            DWORD e = GetLastError();
            *cause = e == ERROR_WINHTTP_NAME_NOT_RESOLVED ? "offline" : e == ERROR_WINHTTP_TIMEOUT ? "timeout" :
                     e == ERROR_WINHTTP_CANNOT_CONNECT ? "blocked" : e == ERROR_WINHTTP_SECURE_FAILURE ? "tls" : "no_answer";
        }
    }
    if (rq) pClose(rq);
    if (c) pClose(c);
    if (s) pClose(s);
    return status;
}

static DWORD WINAPI ma2_update_thread(LPVOID unused)
{
    char body[MA2_UPDATE_MAX_BODY + 1], latest[16];
    const char *cause = "";
    DWORD t0 = GetTickCount();
    int status;
    (void)unused;
    status = ma2_update_get(g_update_url, g_update_own, body, sizeof body, &cause);
    if (status == 200 && ma2_update_parse_reply(body, latest, sizeof latest)) {
        if (ma2_update_is_newer(latest, g_update_own)) {
            strcpy(g_update_latest, latest);
            fprintf(stderr, "[UPDATE] latest=%s own=%s: the main menu shows the update line (%lu ms)\n", latest, g_update_own,
                    (unsigned long)(GetTickCount() - t0));
            InterlockedExchange(&g_update_state, 3);
            return 0;
        }
        fprintf(stderr, "[UPDATE] latest=%s own=%s: up to date (%lu ms)\n", latest, g_update_own, (unsigned long)(GetTickCount() - t0));
    } else if (status == 200) {
        fprintf(stderr, "[UPDATE] no version in the answer: nothing shown (%lu ms)\n", (unsigned long)(GetTickCount() - t0));
    } else if (status) {
        fprintf(stderr, "[UPDATE] answer %d: nothing shown (%lu ms)\n", status, (unsigned long)(GetTickCount() - t0));
    } else {
        fprintf(stderr, "[UPDATE] no answer (%s): nothing shown (%lu ms)\n", cause, (unsigned long)(GetTickCount() - t0));
    }
    InterlockedExchange(&g_update_state, 2);
    return 0;
}

/* Called once at start-up (after the settings file is known). Never blocks; any failure = no line. */
static void ma2_update_check_start(const char *build_version)
{
    int source = 0;
    const char *over = NULL, *names[] = { "RECOMP_UPDATE_RELAY", "MA2_REPORT_RELAY", "RECOMP_REPORT_RELAY" };
    HANDLE t;
    if (InterlockedCompareExchange(&g_update_state, 1, 0) != 0) return;
    if (!pc_switch_ex("RECOMP_UPDATE_CHECK", "Update", "CheckForUpdates", 1, &source)) {
        fprintf(stderr, "[UPDATE] check off (%s)\n", pc_switch_source_name(source));
        InterlockedExchange(&g_update_state, 2);
        return;
    }
    for (int i = 0; i < 3 && !over; i++) { const char *e = getenv(names[i]); if (e && *e) over = e; }
    if (over && ma2_update_relay_ok(over) && strlen(over) < sizeof g_update_url) strcpy(g_update_url, over);
    else if (getenv("RECOMP_SAVE_DIR")) {
        fprintf(stderr, "[UPDATE] check skipped (test run without a test relay)\n");
        InterlockedExchange(&g_update_state, 2);
        return;
    } else strcpy(g_update_url, MA2_UPDATE_RELAY);
    { size_t n = strlen(g_update_url); while (n && g_update_url[n - 1] == '/') g_update_url[--n] = 0; }
    ma2_update_own_version(g_update_own, sizeof g_update_own, build_version);
    t = CreateThread(NULL, 64 * 1024, ma2_update_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { InterlockedExchange(&g_update_state, 2); return; }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    fprintf(stderr, "[UPDATE] check on (%s), version %s\n", pc_switch_source_name(source), g_update_own);
}

/* The newer version to show, or NULL. */
static const char *ma2_update_available(void)
{
    return InterlockedCompareExchange(&g_update_state, 3, 3) == 3 ? g_update_latest : NULL;
}
#endif /* MA2_UPDATE_NO_NETWORK */

#ifdef __cplusplus
}
#endif

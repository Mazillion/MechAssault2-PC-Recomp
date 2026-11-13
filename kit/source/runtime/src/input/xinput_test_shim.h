#ifndef XINPUT_TEST_SHIM_H
#define XINPUT_TEST_SHIM_H
/* TEST-ONLY fake XInput. Included by xinput_device.c (Windows backend).
 *
 * It lets a test put scripted pads in any of the four host slots and connect and
 * disconnect them, with no driver and no hardware (a virtual-pad driver cannot
 * be installed unattended). It is OFF unless one of these environment variables
 * is set to something non-empty, so a normal run never touches it:
 *
 *   RECOMP_TEST_XINPUT_SCRIPT   inline script, applied once, times relative to the first XInput call
 *   RECOMP_TEST_XINPUT_FILE     path of a live command file, re-read whenever its bytes change
 *                               (times relative to the moment the change was seen). Write the file
 *                               to send commands; add a "#n" comment to resend the same commands.
 *
 * When active, all four host slots start disconnected and the real XInput is not
 * called at all, so a pad that is really plugged in cannot confuse a test.
 *
 * Script grammar: entries separated by ';' or newlines ('#' starts a comment to the
 * end of the line). Entry = AT_MS:SLOT:CMD[,CMD...]
 *   on / off            connect / disconnect the slot
 *   btn=<hex or dec>    XInput wButtons mask (0x1000 = A, 0x0010 = START, ...)
 *   lt= rt=             trigger 0-255
 *   lx= ly= rx= ry=     stick axes -32768..32767
 * Example: "0:2:on;1500:2:btn=0x1000;1700:2:btn=0;4000:2:off"
 *
 * Every applied command is logged as "[TEST-XINPUT] ..." so a run proves what the
 * game was fed. The shim also logs once, loudly, when it activates. */

#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { XS_ON = 1, XS_OFF, XS_BTN, XS_LT, XS_RT, XS_LX, XS_LY, XS_RX, XS_RY };
typedef struct { ULONGLONG at; int slot, op, value; } XsEvent;
#define XS_MAX_EVENTS 256

typedef struct {
    int connected;
    XINPUT_STATE state;
    XINPUT_VIBRATION vibration;
} XsSlot;

static SRWLOCK xs_lock = SRWLOCK_INIT;
static volatile LONG xs_mode = -1;          /* -1 undecided, 0 off, 1 on */
static XsSlot xs_slots[4];
static XsEvent xs_events[XS_MAX_EVENTS];
static int xs_event_count, xs_event_next;
static ULONGLONG xs_base;
static const char *xs_file_path;
static ULONGLONG xs_file_check;
static char *xs_file_text;
static unsigned xs_log_lines;
static ULONGLONG xs_real_clock(void) { return GetTickCount64(); }
static ULONGLONG (*xs_clock)(void) = xs_real_clock;   /* a unit test substitutes a manual clock */

static void xs_log(const char *what, int slot, long value)
{
    if (xs_log_lines >= 400) return;
    xs_log_lines++;
    fprintf(stderr, "[TEST-XINPUT] t=%llu slot=%d %s %ld\n",
            (unsigned long long)(xs_clock() - xs_base), slot, what, value);
}

/* Parse "AT:SLOT:cmd,cmd;..." into events at base+AT. Returns the number added. */
static int xs_parse(const char *text, ULONGLONG base)
{
    int added = 0;
    const char *p = text;
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ';') p++;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (!*p) break;
        char *end;
        unsigned long at = strtoul(p, &end, 10);
        if (end == p || *end != ':') { fprintf(stderr, "[TEST-XINPUT] bad script near: %.24s\n", p); break; }
        p = end + 1;
        long slot = strtol(p, &end, 10);
        if (end == p || *end != ':' || slot < 0 || slot > 3) { fprintf(stderr, "[TEST-XINPUT] bad slot near: %.24s\n", p); break; }
        p = end + 1;
        for (;;) {
            int op = 0; long value = 0;
            if (!strncmp(p, "on", 2)) { op = XS_ON; p += 2; }
            else if (!strncmp(p, "off", 3)) { op = XS_OFF; p += 3; }
            else {
                static const struct { const char *name; int op; } names[] = {
                    {"btn=", XS_BTN}, {"lt=", XS_LT}, {"rt=", XS_RT},
                    {"lx=", XS_LX}, {"ly=", XS_LY}, {"rx=", XS_RX}, {"ry=", XS_RY} };
                for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
                    size_t n = strlen(names[k].name);
                    if (!strncmp(p, names[k].name, n)) {
                        op = names[k].op; value = strtol(p + n, &end, 0); p = end; break;
                    }
                }
            }
            if (!op) { fprintf(stderr, "[TEST-XINPUT] bad command near: %.24s\n", p); return added; }
            if (xs_event_count < XS_MAX_EVENTS) {
                XsEvent *e = &xs_events[xs_event_count++];
                e->at = base + at; e->slot = (int)slot; e->op = op; e->value = (int)value;
                added++;
            }
            if (*p == ',') { p++; continue; }
            break;
        }
    }
    /* Keep time order (stable insertion sort; tiny arrays). */
    for (int i = xs_event_next + 1; i < xs_event_count; i++) {
        XsEvent e = xs_events[i]; int j = i - 1;
        while (j >= xs_event_next && xs_events[j].at > e.at) { xs_events[j + 1] = xs_events[j]; j--; }
        xs_events[j + 1] = e;
    }
    return added;
}

static void xs_apply(const XsEvent *e)
{
    XsSlot *s = &xs_slots[e->slot];
    switch (e->op) {
    case XS_ON:  if (!s->connected) { memset(s, 0, sizeof *s); s->connected = 1; s->state.dwPacketNumber = 1; } xs_log("on", e->slot, 0); return;
    case XS_OFF: s->connected = 0; xs_log("off", e->slot, 0); return;
    case XS_BTN: s->state.Gamepad.wButtons = (WORD)e->value; xs_log("btn", e->slot, e->value); break;
    case XS_LT:  s->state.Gamepad.bLeftTrigger = (BYTE)e->value; xs_log("lt", e->slot, e->value); break;
    case XS_RT:  s->state.Gamepad.bRightTrigger = (BYTE)e->value; xs_log("rt", e->slot, e->value); break;
    case XS_LX:  s->state.Gamepad.sThumbLX = (SHORT)e->value; xs_log("lx", e->slot, e->value); break;
    case XS_LY:  s->state.Gamepad.sThumbLY = (SHORT)e->value; xs_log("ly", e->slot, e->value); break;
    case XS_RX:  s->state.Gamepad.sThumbRX = (SHORT)e->value; xs_log("rx", e->slot, e->value); break;
    case XS_RY:  s->state.Gamepad.sThumbRY = (SHORT)e->value; xs_log("ry", e->slot, e->value); break;
    }
    s->state.dwPacketNumber++;
}

/* Turn the shim on with an explicit script (the unit test, or the env decision below). */
static void xs_enable(const char *script)
{
    xs_base = xs_clock();
    if (script && *script) xs_parse(script, xs_base);
    InterlockedExchange(&xs_mode, 1);
}

static int xs_enabled(void)
{
    LONG mode = InterlockedCompareExchange(&xs_mode, -1, -1);
    if (mode >= 0) return mode > 0;
    const char *script = getenv("RECOMP_TEST_XINPUT_SCRIPT");
    const char *file = getenv("RECOMP_TEST_XINPUT_FILE");
    int on = (script && *script) || (file && *file);
    AcquireSRWLockExclusive(&xs_lock);
    if (InterlockedCompareExchange(&xs_mode, -1, -1) < 0) {
        if (on) {
            xs_file_path = (file && *file) ? file : NULL;
            xs_enable(script);
            fprintf(stderr, "[TEST-XINPUT] *** FAKE XINPUT SHIM ACTIVE (test only; real pads ignored) script=%s file=%s ***\n",
                    script && *script ? script : "-", xs_file_path ? xs_file_path : "-");
        } else InterlockedExchange(&xs_mode, 0);
    }
    ReleaseSRWLockExclusive(&xs_lock);
    return InterlockedCompareExchange(&xs_mode, -1, -1) > 0;
}

/* Apply due events; poll the live file. Caller holds xs_lock. */
static void xs_pump_locked(void)
{
    ULONGLONG now = xs_clock();
    if (xs_file_path && now - xs_file_check >= 100) {
        xs_file_check = now;
        FILE *f = fopen(xs_file_path, "rb");
        if (f) {
            char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
            buf[n] = 0;
            if (!xs_file_text || strcmp(xs_file_text, buf)) {
                free(xs_file_text); xs_file_text = _strdup(buf);
                xs_parse(buf, now);
            }
        }
    }
    while (xs_event_next < xs_event_count && xs_events[xs_event_next].at <= now)
        xs_apply(&xs_events[xs_event_next++]);
}

static DWORD xs_get_state(DWORD slot, XINPUT_STATE *out)
{
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (slot > 3) return result;
    AcquireSRWLockExclusive(&xs_lock);
    xs_pump_locked();
    if (xs_slots[slot].connected) { *out = xs_slots[slot].state; result = ERROR_SUCCESS; }
    ReleaseSRWLockExclusive(&xs_lock);
    return result;
}

static DWORD xs_set_state(DWORD slot, XINPUT_VIBRATION *v)
{
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (slot > 3) return result;
    AcquireSRWLockExclusive(&xs_lock);
    if (xs_slots[slot].connected) {
        if (xs_slots[slot].vibration.wLeftMotorSpeed != v->wLeftMotorSpeed ||
            xs_slots[slot].vibration.wRightMotorSpeed != v->wRightMotorSpeed)
            xs_log("rumble", (int)slot, (long)v->wLeftMotorSpeed * 65536L + v->wRightMotorSpeed);
        xs_slots[slot].vibration = *v;
        result = ERROR_SUCCESS;
    }
    ReleaseSRWLockExclusive(&xs_lock);
    return result;
}

static DWORD xs_get_caps(DWORD slot, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    (void)flags;
    if (slot > 3) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&xs_lock);
    DWORD result = xs_slots[slot].connected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    if (result == ERROR_SUCCESS) {
        memset(caps, 0, sizeof *caps);
        caps->Type = XINPUT_DEVTYPE_GAMEPAD;
        caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    }
    ReleaseSRWLockExclusive(&xs_lock);
    return result;
}

#endif

#ifndef OHCI_TICK_TIMER_H
#define OHCI_TICK_TIMER_H
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

/* A controller frame is a millisecond of elapsed time, including scheduler
 * delays. Keep the fractional QPC remainder rather than losing it each pass. */
typedef struct {
    HANDLE timer;
    int initialized;
    int64_t frequency, last, remainder, deadline;
} OhciTickTimer;

static unsigned ohci_tick_elapsed(OhciTickTimer *clock, int64_t now)
{
    int64_t ticks = now - clock->last;
    int64_t scaled;
    clock->last = now;
    if (ticks <= 0) return 0;
    scaled = ticks * 1000 + clock->remainder;
    clock->remainder = scaled % clock->frequency;
    return (unsigned)(scaled / clock->frequency);
}

static void ohci_tick_timer_close(OhciTickTimer *clock)
{
    if (clock->timer) CloseHandle(clock->timer);
    clock->timer = NULL;
    clock->initialized = 0;
}

static unsigned ohci_tick_timer_wait(OhciTickTimer *clock, unsigned interval_ms)
{
    LARGE_INTEGER now;
    int64_t period;
    QueryPerformanceCounter(&now);
    if (!clock->initialized) {
        LARGE_INTEGER frequency;
        const char *setting = getenv("RECOMP_USB_HIGH_RES_POLLING");
        QueryPerformanceFrequency(&frequency);
        clock->frequency = frequency.QuadPart;
        clock->last = now.QuadPart;
        clock->remainder = 0;
        clock->deadline = now.QuadPart + clock->frequency * interval_ms / 1000;
        clock->initialized = 1;
        if (!setting || atoi(setting) != 0)
            clock->timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    period = clock->frequency * interval_ms / 1000;
    if (now.QuadPart < clock->deadline) {
        int64_t remaining = clock->deadline - now.QuadPart;
        int waited = 0;
        if (clock->timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(remaining * 10000000 / clock->frequency);
            if (!due.QuadPart) due.QuadPart = -1;
            waited = SetWaitableTimer(clock->timer, &due, 0, NULL, NULL, FALSE)
                && WaitForSingleObject(clock->timer, 1000) == WAIT_OBJECT_0;
            if (!waited) {
                CloseHandle(clock->timer);
                clock->timer = NULL;
            }
        }
        if (!waited)
            Sleep((DWORD)((remaining * 1000 + clock->frequency - 1) /
                clock->frequency));
        QueryPerformanceCounter(&now);
    }
    /* Collapse missed polling slots after a delayed ISR. The frame clock
     * catches up, but transfer/interrupt delivery does not burst in a loop. */
    if (now.QuadPart >= clock->deadline)
        clock->deadline += ((now.QuadPart - clock->deadline) / period + 1) * period;
    return ohci_tick_elapsed(clock, now.QuadPart);
}
#endif

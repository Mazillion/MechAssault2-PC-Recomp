#ifndef NV2A_GPU_PACING_H
#define NV2A_GPU_PACING_H
#include <windows.h>

/* One reusable timer per presentation thread. Unsupported Windows versions
 * retain the previous Sleep/yield path; no system-wide timer policy changes. */
typedef struct { HANDLE timer; int initialized; } Nv2aFramePacer;

static void nv2a_frame_pacer_close(Nv2aFramePacer *pacer)
{
    if(pacer->timer)CloseHandle(pacer->timer);
    pacer->timer=NULL;
    pacer->initialized=0;
}

static void nv2a_frame_pacer_wait(Nv2aFramePacer *pacer,LONGLONG frequency,
    LONGLONG target,int high_resolution)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if(now.QuadPart>=target)return;
    if(!pacer->initialized) {
        pacer->initialized=1;
        if(high_resolution)
            pacer->timer=CreateWaitableTimerExW(NULL,NULL,0x00000002,TIMER_ALL_ACCESS);
    }
    while(now.QuadPart<target) {
        double ms=(double)(target-now.QuadPart)*1000.0/frequency;
        if(pacer->timer && ms>0.5) {
            LARGE_INTEGER due;
            /* Leave a short final yielding margin, then check the same QPC
             * deadline again. A relative due time uses units of 100 ns. */
            due.QuadPart=-(LONGLONG)((ms-0.25)*10000.0);
            if(!SetWaitableTimer(pacer->timer,&due,0,NULL,NULL,FALSE) ||
                WaitForSingleObject(pacer->timer,1000)!=WAIT_OBJECT_0) {
                CloseHandle(pacer->timer);
                pacer->timer=NULL;
            }
        }
        else if(!pacer->timer && ms>1.5)Sleep((DWORD)(ms-0.5));
        else SwitchToThread();
        QueryPerformanceCounter(&now);
    }
}
#endif

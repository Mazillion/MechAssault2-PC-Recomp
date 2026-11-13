#include "nv2a_gpu_wait.h"
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

typedef BOOL (WINAPI *WaitAddress)(volatile VOID *,PVOID,SIZE_T,DWORD);
typedef VOID (WINAPI *WakeAddress)(PVOID);
static INIT_ONCE api_once=INIT_ONCE_STATIC_INIT;
static WaitAddress wait_address;
static WakeAddress wake_address;
static volatile LONG waiters;
static int enabled;

static BOOL CALLBACK initialize_api(PINIT_ONCE once,PVOID parameter,PVOID *context)
{
    (void)once;(void)parameter;(void)context;
    const char *option=getenv("RECOMP_GPU_FENCE_WAIT");
    enabled=!option || atoi(option)!=0;
    /* The documented API contract is available from Windows 8. Resolve it
     * dynamically so older systems retain the original yielding fallback.
     * https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress */
    HMODULE api=LoadLibraryW(L"api-ms-win-core-synch-l1-2-0.dll");
    if(api) {
        wait_address=(WaitAddress)GetProcAddress(api,"WaitOnAddress");
        wake_address=(WakeAddress)GetProcAddress(api,"WakeByAddressAll");
        if(!wait_address || !wake_address){wait_address=NULL;wake_address=NULL;}
    }
    return TRUE;
}

void nv2a_gpu_wait_word(volatile uint32_t *word,uint32_t observed)
{
    InitOnceExecuteOnce(&api_once,initialize_api,NULL,NULL);
    if(!enabled)return;
    if((uint32_t)InterlockedCompareExchange((volatile LONG *)word,0,0)!=observed)return;
    if(!wait_address){SwitchToThread();return;}
    /* Register before the API's atomic comparison/wait. A producer publishing
     * before registration is observed by that comparison; publishing after
     * registration signals the address. The bounded timeout also handles an
     * uninstrumented guest writer without changing the fence predicate. */
    InterlockedIncrement(&waiters);
    wait_address(word,&observed,sizeof(observed),2);
    InterlockedDecrement(&waiters);
}

void nv2a_gpu_publish_word(volatile uint32_t *word,uint32_t value)
{
    InterlockedExchange((volatile LONG *)word,(LONG)value);
    if(InterlockedCompareExchange(&waiters,0,0))wake_address((PVOID)word);
}
#else
#include <sched.h>
void nv2a_gpu_wait_word(volatile uint32_t *word,uint32_t observed)
{if(__atomic_load_n(word,__ATOMIC_ACQUIRE)==observed)sched_yield();}
void nv2a_gpu_publish_word(volatile uint32_t *word,uint32_t value)
{__atomic_store_n(word,value,__ATOMIC_RELEASE);}
#endif

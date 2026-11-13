#include "nv2a_gpu_memory.h"
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#include "xbox_memory_layout.h"
#include "kernel.h"
extern ptrdiff_t xbox_GetMemoryOffset(void);
#define PAGES (XBOX_CONTIG_SIZE/4096)
static struct {uint64_t version,epoch;unsigned owned,readonly;} pages[PAGES];
static volatile LONG64 serial=1;
static volatile LONG64 allocation_serial=1;
static SRWLOCK lock=SRWLOCK_INIT;
static PVOID handler;
static INIT_ONCE once=INIT_ONCE_STATIC_INIT;
static int range(uint32_t va,size_t bytes,unsigned *first,unsigned *last)
{
    uint32_t base=va>=XBOX_TILED_BASE?XBOX_TILED_BASE:XBOX_CONTIG_BASE;
    if(!bytes || va<base || va-base>=XBOX_CONTIG_SIZE || bytes>XBOX_CONTIG_SIZE-(va-base))return 0;
    *first=(va-base)/4096;*last=(unsigned)((va-base+bytes-1)/4096);return 1;
}
static void *address(unsigned page,unsigned alias)
{
    uint32_t va=(alias?XBOX_TILED_BASE:XBOX_CONTIG_BASE)+page*4096;
    return (void *)((uintptr_t)xbox_GetMemoryOffset()+va);
}
static void release_page(unsigned i)
{
    for(unsigned a=0;a<2;a++)if(pages[i].owned&(1u<<a)) {
        DWORD old;
        VirtualProtect(address(i,a),4096,PAGE_READWRITE,&old);
    }
    pages[i].owned=pages[i].readonly=0;
}
static LONG CALLBACK write_fault(PEXCEPTION_POINTERS e)
{
    if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION ||
       e->ExceptionRecord->NumberParameters<2 || e->ExceptionRecord->ExceptionInformation[0]!=1)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t native=e->ExceptionRecord->ExceptionInformation[1];
    uintptr_t offset=(uintptr_t)xbox_GetMemoryOffset();
    if(native<offset || native-offset>UINT32_MAX)return EXCEPTION_CONTINUE_SEARCH;
    uint32_t va=(uint32_t)(native-offset);unsigned first,last;
    if(!range(va,1,&first,&last))return EXCEPTION_CONTINUE_SEARCH;
    unsigned alias=va>=XBOX_TILED_BASE,bit=1u<<alias;
    AcquireSRWLockExclusive(&lock);
    if(!(pages[first].owned&bit)) {
        ReleaseSRWLockExclusive(&lock);return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD old;
    BOOL ok=VirtualProtect(address(first,alias),4096,PAGE_READWRITE,&old);
    if(ok) {
        pages[first].owned&=~bit;pages[first].readonly&=~bit;
        pages[first].version=(uint64_t)InterlockedIncrement64(&serial);
    }
    ReleaseSRWLockExclusive(&lock);
    return ok?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;
}
static BOOL CALLBACK install(PINIT_ONCE init,PVOID arg,PVOID *out)
{
    (void)init;(void)arg;(void)out;
    handler=AddVectoredExceptionHandler(1,write_fault);return handler!=NULL;
}
uint64_t nv2a_gpu_memory_serial(void){return (uint64_t)InterlockedCompareExchange64(&serial,0,0);}
uint64_t nv2a_gpu_memory_version(uint32_t va,size_t bytes)
{
    unsigned first,last;uint64_t version=1;
    if(!range(va,bytes,&first,&last))return 0;
    AcquireSRWLockShared(&lock);
    for(unsigned i=first;i<=last;i++) {
        if(pages[i].readonly!=3){version=0;break;}
        if(pages[i].version>version)version=pages[i].version;
    }
    ReleaseSRWLockShared(&lock);return version;
}
uint64_t nv2a_gpu_memory_watch(uint32_t va,size_t bytes)
{
    unsigned first,last;
    if(!range(va,bytes,&first,&last) || !InitOnceExecuteOnce(&once,install,NULL,NULL))return 0;
    AcquireSRWLockExclusive(&lock);
    for(unsigned i=first;i<=last;i++)for(unsigned a=0;a<2;a++) {
        unsigned bit=1u<<a;
        if(pages[i].readonly&bit)continue;
        MEMORY_BASIC_INFORMATION info;DWORD old;
        if(!VirtualQuery(address(i,a),&info,sizeof(info)) || info.State!=MEM_COMMIT)continue;
        if(info.Protect==PAGE_READONLY)pages[i].readonly|=bit;
        else if(info.Protect==PAGE_READWRITE && VirtualProtect(address(i,a),4096,PAGE_READONLY,&old)) {
            pages[i].owned|=bit;pages[i].readonly|=bit;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return nv2a_gpu_memory_version(va,bytes);
}
void nv2a_gpu_memory_changed(void *native,size_t bytes)
{
    uintptr_t at=(uintptr_t)native,offset=(uintptr_t)xbox_GetMemoryOffset();unsigned first,last;
    if(at<offset || at-offset>UINT32_MAX || !range((uint32_t)(at-offset),bytes,&first,&last))return;
    AcquireSRWLockExclusive(&lock);
    uint64_t v=(uint64_t)InterlockedIncrement64(&serial);
    for(unsigned i=first;i<=last;i++){release_page(i);pages[i].version=v;}
    ReleaseSRWLockExclusive(&lock);
}
/* The overwhelmingly common case is writable RAM. Querying Windows page
 * permissions for every DSP transfer costs more than executing the program.
 * Only a deliberate CPU read-only mapping needs the slower protected copy. */
static int dma_copy(void *native,const void *source,size_t bytes)
{
#if defined(_MSC_VER)
    __try { memcpy(native,source,bytes);return 1; }
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?
             EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) { }
#endif
    unsigned char *at=native;const unsigned char *input=source;
    while(bytes) {
        MEMORY_BASIC_INFORMATION info;DWORD previous;
        if(!VirtualQuery(at,&info,sizeof(info)) || info.State!=MEM_COMMIT)return 0;
        size_t part=(uintptr_t)info.BaseAddress+info.RegionSize-(uintptr_t)at;
        if(part>bytes)part=bytes;
        unsigned protection=info.Protect&255;
        int readonly=protection==PAGE_READONLY || protection==PAGE_EXECUTE_READ;
        if(!readonly && protection!=PAGE_READWRITE && protection!=PAGE_EXECUTE_READWRITE)return 0;
        if(readonly && !VirtualProtect(at,part,PAGE_READWRITE,&previous))return 0;
        memcpy(at,input,part);
        if(readonly)VirtualProtect(at,part,previous,&previous);
        bytes-=part;at+=part;input+=part;
    }
    return 1;
}
int nv2a_gpu_memory_dma_write(void *native,const void *source,size_t bytes)
{
    uintptr_t at=(uintptr_t)native,offset=(uintptr_t)xbox_GetMemoryOffset();unsigned first,last;
    if(at<offset || at-offset>UINT32_MAX || !range((uint32_t)(at-offset),bytes,&first,&last))
        return dma_copy(native,source,bytes);
    /* Prevent a renderer from installing a write watch between invalidation
     * and the transfer. Both physical aliases acquire the same page lock. */
    AcquireSRWLockExclusive(&lock);
    uint64_t version=(uint64_t)InterlockedIncrement64(&serial);
    for(unsigned i=first;i<=last;i++){release_page(i);pages[i].version=version;}
    int copied=dma_copy(native,source,bytes);
    ReleaseSRWLockExclusive(&lock);return copied;
}
uint64_t nv2a_gpu_memory_allocation_serial(void)
{
    return (uint64_t)InterlockedCompareExchange64(&allocation_serial,0,0);
}
uint64_t nv2a_gpu_memory_epoch(uint32_t va,size_t bytes)
{
    unsigned first,last;uint64_t epoch=0;
    if(!range(va,bytes,&first,&last))return 0;
    AcquireSRWLockShared(&lock);
    for(unsigned i=first;i<=last;i++)if(pages[i].epoch>epoch)epoch=pages[i].epoch;
    ReleaseSRWLockShared(&lock);return epoch;
}
/* CPU allocation threads publish generations only. D3D resources are
 * discarded by the owning graphics thread when it next observes the range. */
void nv2a_gpu_memory_reallocated(void *native,size_t bytes)
{
    uintptr_t at=(uintptr_t)native,offset=(uintptr_t)xbox_GetMemoryOffset();unsigned first,last;
    if(at<offset || at-offset>UINT32_MAX || !range((uint32_t)(at-offset),bytes,&first,&last))return;
    AcquireSRWLockExclusive(&lock);
    uint64_t epoch=(uint64_t)InterlockedIncrement64(&allocation_serial);
    uint64_t version=(uint64_t)InterlockedIncrement64(&serial);
    for(unsigned i=first;i<=last;i++){release_page(i);pages[i].version=version;pages[i].epoch=epoch;}
    ReleaseSRWLockExclusive(&lock);
}
void nv2a_gpu_memory_shutdown(void)
{
    AcquireSRWLockExclusive(&lock);
    for(unsigned i=0;i<PAGES;i++){release_page(i);pages[i].version=pages[i].epoch=0;}
    InterlockedIncrement64(&serial);
    InterlockedIncrement64(&allocation_serial);
    ReleaseSRWLockExclusive(&lock);
    /* Keep the inert handler installed; INIT_ONCE and a second memory-layout
     * initialization can safely reuse it. It claims only pages we own. */
}
uint32_t nv2a_gpu_memory_protection(void *native,uint32_t actual)
{
    uintptr_t at=(uintptr_t)native,offset=(uintptr_t)xbox_GetMemoryOffset();unsigned first,last;
    if(at<offset || at-offset>UINT32_MAX || !range((uint32_t)(at-offset),1,&first,&last))return actual;
    unsigned alias=(uint32_t)(at-offset)>=XBOX_TILED_BASE;
    AcquireSRWLockShared(&lock);
    if(pages[first].owned&(1u<<alias))actual=PAGE_READWRITE;
    ReleaseSRWLockShared(&lock);return actual;
}
#else
uint64_t nv2a_gpu_memory_serial(void){return 0;}
uint64_t nv2a_gpu_memory_version(uint32_t v,size_t n){(void)v;(void)n;return 0;}
uint64_t nv2a_gpu_memory_watch(uint32_t v,size_t n){(void)v;(void)n;return 0;}
void nv2a_gpu_memory_changed(void *p,size_t n){(void)p;(void)n;}
int nv2a_gpu_memory_dma_write(void *p,const void *s,size_t n){memcpy(p,s,n);return 1;}
void nv2a_gpu_memory_reallocated(void *p,size_t n){(void)p;(void)n;}
uint64_t nv2a_gpu_memory_allocation_serial(void){return 0;}
uint64_t nv2a_gpu_memory_epoch(uint32_t v,size_t n){(void)v;(void)n;return 0;}
void nv2a_gpu_memory_shutdown(void){}
uint32_t nv2a_gpu_memory_protection(void *p,uint32_t a){(void)p;return a;}
#endif

/* Persistent DXBC and background compilation. Workers never access the D3D
 * immediate context or mutable renderer state. The render thread owns shader
 * creation and switches programs only after a complete compilation. */
#if defined(_WIN32)
#define COBJMACROS
#include "nv2a_shader_cache.h"
#include "../platform/pc_cpu_topo.h"   /* T2-0: thread roles */
#include <d3d11shader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);

#define JOB_BUCKETS 1024
struct Nv2aShaderJob {
    uint64_t key;
    unsigned kind;
    char *source;
    size_t length;
    char entry[32],profile[16];
    ID3DBlob *code,*errors;
    HRESULT result;
    volatile LONG done;
    struct Nv2aShaderJob *bucket_next,*queue_next;
    Nv2aShaderKey disk;
};
static struct {
    SRWLOCK lock;
    HANDLE wake;
    Nv2aShaderJob *first,*last,*buckets[JOB_BUCKETS];
    int initialized,enabled;
} compiler;
static INIT_ONCE cache_once=INIT_ONCE_STATIC_INIT;
static WCHAR cache_directory[MAX_PATH];
static int cache_enabled;

/* Seed pack (2026-10-15): pc_assets\shader-seed.pack, compiled shaders shipped with the game in
 * the same keys as the cache files (source hash, source length, stage). A machine compiles each
 * guest shader once, 250-450 ms on an i7-1260P laptop, and until then draws it through the CPU
 * vertex fallback -- the freezes the first time a new effect appears. A shader in the seed is
 * there already: it is validated like a cache file, written into the user's cache, and used.
 * Layout: "NV2ASED1", u32 count, u32 0, then count sorted entries {u64 hash; u32 length; u32 kind
 * (0 vertex, 1 pixel); u32 offset; u32 size}, then the DXBC blobs. RECOMP_SHADER_SEED=0 ignores it.
 * RECOMP_SHADER_CACHE_LIST=<file> appends every cache name used, to build a seed from. */
#pragma pack(push,4)
typedef struct {uint64_t hash;uint32_t length,kind,offset,size;} SeedEntry;
#pragma pack(pop)
static const uint8_t *seed_view;static uint64_t seed_bytes;
static const SeedEntry *seed_index;static uint32_t seed_count;
static void open_seed(void)
{
    const char *enabled=getenv("RECOMP_SHADER_SEED");
    if(enabled && atoi(enabled)==0)return;
    const char *overlay=getenv("RECOMP_GAME_OVERLAY");char path[MAX_PATH];
    snprintf(path,sizeof(path),"%s\\shader-seed.pack",overlay&&*overlay?overlay:"pc_assets");
    HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return;
    LARGE_INTEGER size;HANDLE mapping=NULL;const uint8_t *view=NULL;
    if(GetFileSizeEx(file,&size) && size.QuadPart>=16 && size.QuadPart<(1ll<<31))
        mapping=CreateFileMappingA(file,NULL,PAGE_READONLY,0,0,NULL);
    if(mapping)view=(const uint8_t *)MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    if(mapping)CloseHandle(mapping);
    CloseHandle(file);
    if(!view)return;
    uint32_t count;memcpy(&count,view+8,4);
    if(memcmp(view,"NV2ASED1",8) || 16+(uint64_t)count*sizeof(SeedEntry)>(uint64_t)size.QuadPart) {
        fprintf(stderr,"[GPU-SHADER] seed %s is not a seed pack; ignored\n",path);UnmapViewOfFile(view);return;
    }
    seed_view=view;seed_bytes=(uint64_t)size.QuadPart;seed_index=(const SeedEntry *)(view+16);seed_count=count;
    {   /* T2-3 B2.5 (R8 E2): read the pack ahead (one async hint) instead of a page fault per hit. */
        typedef struct {PVOID VirtualAddress;SIZE_T NumberOfBytes;} RangeEntry;
        typedef BOOL (WINAPI *PrefetchFn)(HANDLE,ULONG_PTR,RangeEntry *,ULONG);
        PrefetchFn prefetch=(PrefetchFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),"PrefetchVirtualMemory");
        RangeEntry r={(PVOID)view,(SIZE_T)size.QuadPart};
        if(prefetch)prefetch(GetCurrentProcess(),1,&r,0);
    }
    {char shown[MAX_PATH*2];fprintf(stderr,"[GPU-SHADER] seed: %u compiled shaders in %s\n",count,xbox_log_path(path,shown,sizeof shown));}
}
static int seed_compare(uint64_t hash,uint32_t length,uint32_t kind,const SeedEntry *e)
{
    if(hash!=e->hash)return hash<e->hash?-1:1;
    if(length!=e->length)return length<e->length?-1:1;
    return kind==e->kind?0:kind<e->kind?-1:1;
}
static const SeedEntry *seed_find(uint64_t hash,uint32_t length,uint32_t kind)
{
    uint32_t lo=0,hi=seed_count;
    while(lo<hi) {
        uint32_t mid=lo+(hi-lo)/2;int c=seed_compare(hash,length,kind,&seed_index[mid]);
        if(!c) {
            const SeedEntry *e=&seed_index[mid];
            return (uint64_t)e->offset+e->size<=seed_bytes && e->size>=32?e:NULL;
        }
        if(c<0)hi=mid;else lo=mid+1;
    }
    return NULL;
}
static CRITICAL_SECTION list_lock;static FILE *list_file;static int list_state;
static void note_used(const WCHAR *path)
{
    if(list_state<0)return;
    if(!list_state) {
        const char *name=getenv("RECOMP_SHADER_CACHE_LIST");
        if(!name || !*name){list_state=-1;return;}
        InitializeCriticalSection(&list_lock);list_file=fopen(name,"a");list_state=list_file?1:-1;
        if(list_state<0)return;
    }
    const WCHAR *base=wcsrchr(path,L'\\');
    EnterCriticalSection(&list_lock);fprintf(list_file,"%ls\n",base?base+1:path);fflush(list_file);LeaveCriticalSection(&list_lock);
}

static uint64_t hash_bytes(uint64_t h,const void *bytes,size_t count)
{
    const uint8_t *data=bytes;
    for(size_t i=0;i<count;i++){h^=data[i];h*=1099511628211ULL;}
    return h;
}
static BOOL CALLBACK initialize_cache(PINIT_ONCE once,PVOID parameter,PVOID *context)
{
    (void)once;(void)parameter;(void)context;
    const char *enabled=getenv("RECOMP_SHADER_CACHE");
    /* T2-3b test hook: RECOMP_SHADER_CACHE_DIR=<dir> uses that folder as the user shader cache
     * (a cold-cache run without touching LOCALAPPDATA; the MCP launcher passes only RECOMP_*). */
    const char *dir=getenv("RECOMP_SHADER_CACHE_DIR");
    if((!enabled || atoi(enabled)!=0) && dir && *dir) {
        int m=MultiByteToWideChar(CP_UTF8,0,dir,-1,cache_directory,MAX_PATH-80);
        if(m>1) {
            cache_enabled=CreateDirectoryW(cache_directory,NULL) || GetLastError()==ERROR_ALREADY_EXISTS;
            {char shown[MAX_PATH*2];fprintf(stderr,"[SHADER-CACHE] dir override %s (%s)\n",xbox_log_path(dir,shown,sizeof shown),cache_enabled?"ok":"failed");}
            if(cache_enabled)open_seed();
            return TRUE;
        }
        cache_directory[0]=0;
    }
    /* RECOMP_TEST_LOCALAPPDATA: a stand-in for %LOCALAPPDATA% in this function only (W-16 move proof) */
    DWORD n=GetEnvironmentVariableW(L"RECOMP_TEST_LOCALAPPDATA",cache_directory,MAX_PATH);
    if(!n)n=GetEnvironmentVariableW(L"LOCALAPPDATA",cache_directory,MAX_PATH);
    if((!enabled || atoi(enabled)!=0) && n && n<MAX_PATH-80) {
        /* W-16: the cache lives with the game's other per-user data, %LOCALAPPDATA%\\MechAssault2\\ShaderCache.
         * A cache left in the old xboxrecomp\\shader-cache folder is moved over once. */
        WCHAR old_directory[MAX_PATH];
        wcscpy_s(old_directory,MAX_PATH,cache_directory);
        wcscat_s(old_directory,MAX_PATH,L"\\xboxrecomp\\shader-cache");
        wcscat_s(cache_directory,MAX_PATH,L"\\MechAssault2");CreateDirectoryW(cache_directory,NULL);
        wcscat_s(cache_directory,MAX_PATH,L"\\ShaderCache");
        if(GetFileAttributesW(cache_directory)==INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW(old_directory)!=INVALID_FILE_ATTRIBUTES &&
           (!getenv("RECOMP_SAVE_DIR") || getenv("RECOMP_TEST_SHADER_MOVE"))) {   /* harness runs never move a real cache */
            if(MoveFileExW(old_directory,cache_directory,0))
                {char shown[MAX_PATH*2];fprintf(stderr,"[SHADER-CACHE] moved from xboxrecomp\\shader-cache to %s\n",xbox_log_pathw(cache_directory,shown,sizeof shown));}
            else fprintf(stderr,"[SHADER-CACHE] could not move the old xboxrecomp\\shader-cache (error %lu); starting a new cache\n",GetLastError());
        }
        cache_enabled=CreateDirectoryW(cache_directory,NULL) || GetLastError()==ERROR_ALREADY_EXISTS;
        {char shown[MAX_PATH*2];fprintf(stderr,"[SHADER-CACHE] folder %s%s\n",xbox_log_pathw(cache_directory,shown,sizeof shown),cache_enabled?"":" (unavailable)");}
    }
    if(cache_enabled)open_seed();
    return TRUE;
}
static int shader_path(const char *source,size_t length,const char *entry,const char *profile,
                        WCHAR path[MAX_PATH])
{
    InitOnceExecuteOnce(&cache_once,initialize_cache,NULL,NULL);
    if(!cache_enabled)return 0;
    uint64_t old=hash_bytes(14695981039346656037ULL,source,length);
    uint64_t hash=hash_bytes(hash_bytes(old,entry,strlen(entry)),profile,strlen(profile));
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\v2-%016llX-%zu-o3.%hs",cache_directory,
        (unsigned long long)hash,length,profile[0]=='p'?"psc":"vsc");
    return 1;
}
static int valid_blob(const char *profile,ID3DBlob **code);
static int read_blob(const WCHAR *path,const char *profile,ID3DBlob **code)
{
    WIN32_FILE_ATTRIBUTE_DATA file;
    if(!GetFileAttributesExW(path,GetFileExInfoStandard,&file) || file.nFileSizeHigh ||
       file.nFileSizeLow<32 || file.nFileSizeLow>16u*1024*1024)return 0;
    if(SUCCEEDED(D3DReadFileToBlob(path,code)))return valid_blob(profile,code);
    return 0;
}
static int valid_blob(const char *profile,ID3DBlob **code)
{
    {
        ID3D11ShaderReflection *reflection=NULL;
        D3D11_SHADER_DESC description={0};
        HRESULT hr=D3DReflect(ID3D10Blob_GetBufferPointer(*code),ID3D10Blob_GetBufferSize(*code),
            &IID_ID3D11ShaderReflection,(void **)&reflection);
        if(SUCCEEDED(hr))hr=reflection->lpVtbl->GetDesc(reflection,&description);
        if(reflection)reflection->lpVtbl->Release(reflection);
        unsigned major=0,minor=0;char stage[3]={0};
        int parsed=sscanf(profile,"%2[^_]_%u_%u",stage,&major,&minor)==3;
        unsigned expected=!strcmp(stage,"vs")?D3D11_SHVER_VERTEX_SHADER:
                          !strcmp(stage,"ps")?D3D11_SHVER_PIXEL_SHADER:UINT_MAX;
        if(SUCCEEDED(hr) && parsed && D3D11_SHVER_GET_TYPE(description.Version)==expected &&
           D3D11_SHVER_GET_MAJOR(description.Version)==major &&
           D3D11_SHVER_GET_MINOR(description.Version)==minor)return 1;
        fprintf(stderr,"[GPU-SHADER] Ignored invalid or incompatible cached %s\n",profile);
        ID3D10Blob_Release(*code);*code=NULL;
    }
    return 0;
}
static int seed_blob(const char *source,size_t length,const char *entry,const char *profile,
                     const WCHAR *path,ID3DBlob **code)
{
    if(!seed_count)return 0;
    uint64_t old=hash_bytes(14695981039346656037ULL,source,length);
    uint64_t hash=hash_bytes(hash_bytes(old,entry,strlen(entry)),profile,strlen(profile));
    const SeedEntry *e=seed_find(hash,(uint32_t)length,profile[0]=='p'?1u:0u);
    if(!e || FAILED(D3DCreateBlob(e->size,code)))return 0;
    memcpy(ID3D10Blob_GetBufferPointer(*code),seed_view+e->offset,e->size);
    if(!valid_blob(profile,code))return 0;
    /* T2-3 B2.4 (R8 E1): the seed is looked up before the user's cache and is not copied into it
     * any more (that copy was a disk write on the render thread for every first-play hit). */
    (void)path;
    static LONG seeded;
    LONG n=InterlockedIncrement(&seeded);
    if(n<=4 || (n&63)==0)fprintf(stderr,"[GPU-SHADER] %ld shader(s) taken from the seed instead of compiled\n",n);
    return 1;
}
static HRESULT cached_shader(const char *source,size_t length,const char *entry,
                              const char *profile,ID3DBlob **code)
{
    WCHAR path[MAX_PATH];
    /* v1 omitted the entry point. Generic and clamped pixel programs share
     * source text and cannot safely reuse that ambiguous cache. */
    if(!shader_path(source,length,entry,profile,path))return S_FALSE;
    note_used(path);
    if(seed_blob(source,length,entry,profile,path,code) || read_blob(path,profile,code))return S_OK;
    return S_FALSE;
}
HRESULT nv2a_shader_compile_blocking(const char *source,size_t length,const char *entry,
                                     const char *profile,ID3DBlob **code,ID3DBlob **errors)
{
    if(cached_shader(source,length,entry,profile,code)==S_OK)return S_OK;
    HRESULT hr=D3DCompile(source,length,"nv2a_guest.hlsl",NULL,NULL,entry,profile,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code,errors);
    WCHAR path[MAX_PATH];
    if(SUCCEEDED(hr) && shader_path(source,length,entry,profile,path)) {
        WCHAR temporary[MAX_PATH];
        _snwprintf_s(temporary,MAX_PATH,_TRUNCATE,L"%s.%lu.%lu.tmp",path,
                     GetCurrentProcessId(),GetCurrentThreadId());
        /* T2-3 B2.3 (R3 E2): a failed write left the .tmp behind. */
        HRESULT w=D3DWriteBlobToFile(*code,temporary,TRUE);
        if(FAILED(w) || !MoveFileExW(temporary,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            DeleteFileW(temporary);
    }
    return hr;
}
static DWORD WINAPI shader_worker(LPVOID parameter)
{
    (void)parameter;
    pc_thread_role_enter(PC_ROLE_SHADER);
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    for(;;) {
        WaitForSingleObject(compiler.wake,INFINITE);
        for(;;) {
            AcquireSRWLockExclusive(&compiler.lock);
            Nv2aShaderJob *job=compiler.first;
            if(job){compiler.first=job->queue_next;if(!compiler.first)compiler.last=NULL;}
            ReleaseSRWLockExclusive(&compiler.lock);
            if(!job)break;
            ULONGLONG start=GetTickCount64();
            job->result=nv2a_shader_compile_blocking(job->source,job->length,job->entry,
                                                    job->profile,&job->code,&job->errors);
            free(job->source);job->source=NULL;
            fprintf(stderr,"[GPU-SHADER] background %s %016llX finished in %llums (%08X)\n",
                job->profile,(unsigned long long)job->key,
                (unsigned long long)(GetTickCount64()-start),(unsigned)job->result);
            InterlockedExchange(&job->done,1);
        }
    }
}
static int initialize_compiler(void)
{
    if(compiler.initialized)return compiler.enabled;
    compiler.initialized=1;
    const char *enabled=getenv("RECOMP_SHADER_ASYNC");
    if(enabled && !atoi(enabled))return 0;
    compiler.wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!compiler.wake)return 0;
    /* 2026-11-18: a first launch compiles a level's vertex programs while its first frames wait
     * (nv2a_texture_pack_hold); a quarter of the logical processors, 2 to 4 compilers. */
    DWORD processors=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    unsigned workers=processors>=16?4:processors>=12?3:2;
    for(unsigned i=0;i<workers;i++) {
        HANDLE thread=CreateThread(NULL,0,shader_worker,NULL,0,NULL);
        if(thread){CloseHandle(thread);compiler.enabled=1;}
    }
    if(!compiler.enabled){CloseHandle(compiler.wake);compiler.wake=NULL;}
    return compiler.enabled;
}
Nv2aShaderJob *nv2a_shader_find(uint64_t key,unsigned kind)
{
    if(!compiler.initialized || !compiler.enabled)return NULL;
    unsigned bucket=(unsigned)(key^(kind*0x9E3779B9u))&(JOB_BUCKETS-1);
    /* Only the render thread inserts/reads bucket links. Workers use the
     * separate locked queue and publish result blobs with an interlocked flag. */
    for(Nv2aShaderJob *job=compiler.buckets[bucket];job;job=job->bucket_next)
        if(job->key==key && job->kind==kind)return job;
    return NULL;
}
HRESULT nv2a_shader_poll(Nv2aShaderJob *job,ID3DBlob **code,ID3DBlob **errors)
{
    if(!InterlockedCompareExchange(&job->done,0,0))return NV2A_SHADER_PENDING;
    if(job->code){ID3D10Blob_AddRef(job->code);*code=job->code;}
    if(job->errors){ID3D10Blob_AddRef(job->errors);*errors=job->errors;}
    return job->result;
}
HRESULT nv2a_shader_compile(const char *source,size_t length,const char *entry,const char *profile,
                            uint64_t key,unsigned kind,ID3DBlob **code,ID3DBlob **errors)
{
    if(!initialize_compiler())return nv2a_shader_compile_blocking(source,length,entry,profile,code,errors);
    Nv2aShaderJob *job=nv2a_shader_find(key,kind);
    if(job)return nv2a_shader_poll(job,code,errors);
    if(cached_shader(source,length,entry,profile,code)==S_OK)return S_OK;
    job=calloc(1,sizeof(*job));
    if(!job)return E_OUTOFMEMORY;
    job->source=malloc(length+1);
    if(!job->source){free(job);return E_OUTOFMEMORY;}
    memcpy(job->source,source,length);job->source[length]=0;
    job->key=key;job->kind=kind;job->length=length;
    strcpy_s(job->entry,sizeof(job->entry),entry);strcpy_s(job->profile,sizeof(job->profile),profile);
    nv2a_shader_key(source,length,entry,profile,&job->disk);
    unsigned bucket=(unsigned)(key^(kind*0x9E3779B9u))&(JOB_BUCKETS-1);
    job->bucket_next=compiler.buckets[bucket];compiler.buckets[bucket]=job;
    AcquireSRWLockExclusive(&compiler.lock);
    if(compiler.last)compiler.last->queue_next=job;else compiler.first=job;
    compiler.last=job;
    ReleaseSRWLockExclusive(&compiler.lock);
    SetEvent(compiler.wake);
    return NV2A_SHADER_PENDING;
}
/* ---- Shader preloading (2026-10-15, Display setting "Pre-create shaders") ----
 * Creating a D3D11 shader object from a cached blob still costs ~5 ms on Intel Iris Xe (0.6 ms
 * on a desktop GPU), and the renderer did it on the frame that first drew with a program: a new
 * area or effect needing dozens of them was a visible hitch. Each level now records the shaders
 * it draws with in %LOCALAPPDATA%\xboxrecomp\shader-cache\levels\<level>.txt (one key per line:
 * hash, source length, kind), whatever the setting. With the setting on, a background thread
 * creates that level's shader objects while it loads; the renderer takes a ready object
 * (nv2a_shader_take_*) instead of creating one. A level is the 8-hex name of the Partition5
 * cache file every level load opens (bots/ma2_bot_mapid.c calls nv2a_shader_level_begin).
 * The first visit to a level only records; later visits pre-create. RECOMP_SHADER_PRELOAD
 * (0/1) sets the default; the Display page changes it at run time. */
#include "../platform/pc_frontend.h"
volatile LONG nv2a_shader_level_epoch=1;
typedef struct {Nv2aShaderKey key;int count;ID3D11DeviceChild *object;ID3D11InputLayout *layout;} KeySlot;
typedef struct {KeySlot *slots;unsigned capacity,used;} KeySet;
static struct {
    SRWLOCK lock;
    ID3D11Device *device;
    KeySet level_keys,resident,ready;   /* ready: created ahead, not yet taken */
    uint32_t level;int level_valid;FILE *list;
    Nv2aShaderKey *queue;unsigned queue_count,next,generation;
    HANDLE wake;int thread_started;
    volatile LONG ready_count;
    unsigned created,skipped,failed,taken;ULONGLONG started;
} pre={SRWLOCK_INIT};

void nv2a_shader_key(const char *source,size_t length,const char *entry,const char *profile,Nv2aShaderKey *key)
{
    uint64_t old=hash_bytes(14695981039346656037ULL,source,length);
    key->hash=hash_bytes(hash_bytes(old,entry,strlen(entry)),profile,strlen(profile));
    key->length=(uint32_t)length;key->kind=profile[0]=='p'?1u:0u;
}
void nv2a_shader_job_key(const Nv2aShaderJob *job,Nv2aShaderKey *key){*key=job->disk;}

static unsigned key_bucket(const Nv2aShaderKey *k,unsigned capacity)
{
    uint64_t h=k->hash^((uint64_t)k->length<<20)^k->kind;
    h^=h>>31;h*=0xBF58476D1CE4E5B9ULL;h^=h>>29;
    return (unsigned)h&(capacity-1);
}
static int key_equal(const Nv2aShaderKey *a,const Nv2aShaderKey *b)
{return a->hash==b->hash && a->length==b->length && a->kind==b->kind;}
/* Open addressing; entries are never removed (a count or object goes to 0/NULL). */
static KeySlot *set_find(KeySet *set,const Nv2aShaderKey *key,int insert)
{
    if(insert && (set->used+1)*10>=set->capacity*7) {
        unsigned capacity=set->capacity?set->capacity*2:1024;
        KeySlot *slots=calloc(capacity,sizeof(KeySlot));
        if(!slots)return NULL;
        for(unsigned i=0;i<set->capacity;i++)if(set->slots[i].key.length) {
            unsigned b=key_bucket(&set->slots[i].key,capacity);
            while(slots[b].key.length)b=(b+1)&(capacity-1);
            slots[b]=set->slots[i];
        }
        free(set->slots);set->slots=slots;set->capacity=capacity;
    }
    if(!set->capacity)return NULL;
    unsigned b=key_bucket(key,set->capacity);
    while(set->slots[b].key.length) {
        if(key_equal(&set->slots[b].key,key))return &set->slots[b];
        b=(b+1)&(set->capacity-1);
    }
    if(!insert)return NULL;
    set->slots[b].key=*key;set->used++;return &set->slots[b];
}
static void release_slot(KeySlot *e)
{
    if(e->object){e->object->lpVtbl->Release(e->object);e->object=NULL;InterlockedDecrement(&pre.ready_count);}
    if(e->layout){ID3D11InputLayout_Release(e->layout);e->layout=NULL;}
}
static void set_clear(KeySet *set)
{
    for(unsigned i=0;i<set->capacity;i++)release_slot(&set->slots[i]);
    free(set->slots);set->slots=NULL;set->capacity=set->used=0;
}

static const D3D11_INPUT_ELEMENT_DESC *attribute_elements(void)
{
    /* Same layout as the renderer's expanded-attribute programs (16 x float4 "ATTR"). */
    static D3D11_INPUT_ELEMENT_DESC elements[16];
    if(!elements[0].SemanticName)for(UINT i=0;i<16;i++) {
        elements[i].SemanticName="ATTR";elements[i].SemanticIndex=i;
        elements[i].Format=DXGI_FORMAT_R32G32B32A32_FLOAT;elements[i].AlignedByteOffset=i*16;
    }
    return elements;
}
static int takes_attributes(ID3DBlob *code)
{
    ID3D11ShaderReflection *reflection=NULL;D3D11_SHADER_DESC description={0};
    D3D11_SIGNATURE_PARAMETER_DESC parameter={0};int attributes=0;
    if(SUCCEEDED(D3DReflect(ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),
                            &IID_ID3D11ShaderReflection,(void **)&reflection)) &&
       SUCCEEDED(reflection->lpVtbl->GetDesc(reflection,&description)))
        for(UINT i=0;i<description.InputParameters && !attributes;i++)
            if(SUCCEEDED(reflection->lpVtbl->GetInputParameterDesc(reflection,i,&parameter)) &&
               parameter.SemanticName && !_stricmp(parameter.SemanticName,"ATTR"))attributes=1;
    if(reflection)reflection->lpVtbl->Release(reflection);
    return attributes;
}
static int key_blob(const Nv2aShaderKey *key,ID3DBlob **code)
{
    const char *profile=key->kind?"ps_5_0":"vs_5_0";
    WCHAR path[MAX_PATH];
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\v2-%016llX-%u-o3.%hs",cache_directory,
        (unsigned long long)key->hash,key->length,key->kind?"psc":"vsc");
    /* T2-3 B2.4: the mapped seed first, the user's cache file second. */
    const SeedEntry *e=seed_find(key->hash,key->length,key->kind);
    if(e && SUCCEEDED(D3DCreateBlob(e->size,code))) {
        memcpy(ID3D10Blob_GetBufferPointer(*code),seed_view+e->offset,e->size);
        if(valid_blob(profile,code))return 1;
    }
    return read_blob(path,profile,code);
}
static DWORD WINAPI preload_worker(LPVOID parameter)
{
    (void)parameter;
    pc_thread_role_enter(PC_ROLE_PRELOAD);
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    for(;;) {
        WaitForSingleObject(pre.wake,INFINITE);
        for(;;) {
            AcquireSRWLockExclusive(&pre.lock);
            if(pre.next>=pre.queue_count) {
                if(pre.queue_count && (pre.created || pre.failed))
                    fprintf(stderr,"[GPU-SHADER] level %08X: pre-created %u shaders in %llu ms (%u already loaded, %u unavailable)\n",
                        pre.level,pre.created,(unsigned long long)(GetTickCount64()-pre.started),pre.skipped,pre.failed);
                pre.queue_count=pre.next=0;
                ReleaseSRWLockExclusive(&pre.lock);break;
            }
            Nv2aShaderKey key=pre.queue[pre.next++];unsigned generation=pre.generation;
            ID3D11Device *device=pre.device;
            KeySlot *resident=set_find(&pre.resident,&key,0),*ready=set_find(&pre.ready,&key,0);
            int skip=!device || (resident && resident->count>0) || (ready && ready->object);
            if(skip)pre.skipped++;else ID3D11Device_AddRef(device);
            ReleaseSRWLockExclusive(&pre.lock);
            if(skip)continue;
            ID3DBlob *code=NULL;ID3D11DeviceChild *object=NULL;ID3D11InputLayout *layout=NULL;HRESULT hr=E_FAIL;
            if(key_blob(&key,&code)) {
                const void *bytes=ID3D10Blob_GetBufferPointer(code);SIZE_T size=ID3D10Blob_GetBufferSize(code);
                if(key.kind)hr=ID3D11Device_CreatePixelShader(device,bytes,size,NULL,(ID3D11PixelShader **)&object);
                else {
                    hr=ID3D11Device_CreateVertexShader(device,bytes,size,NULL,(ID3D11VertexShader **)&object);
                    if(SUCCEEDED(hr) && takes_attributes(code))
                        hr=ID3D11Device_CreateInputLayout(device,attribute_elements(),16,bytes,size,&layout);
                }
                ID3D10Blob_Release(code);
            }
            AcquireSRWLockExclusive(&pre.lock);
            KeySlot *e=SUCCEEDED(hr) && generation==pre.generation && device==pre.device?set_find(&pre.ready,&key,1):NULL;
            if(e && !e->object) {
                e->object=object;e->layout=layout;object=NULL;layout=NULL;
                InterlockedIncrement(&pre.ready_count);pre.created++;
            } else if(FAILED(hr))pre.failed++;
            ReleaseSRWLockExclusive(&pre.lock);
            if(object)object->lpVtbl->Release(object);
            if(layout)ID3D11InputLayout_Release(layout);
            ID3D11Device_Release(device);
        }
    }
}
void nv2a_shader_preload_device(ID3D11Device *device)
{
    AcquireSRWLockExclusive(&pre.lock);
    /* Objects belong to one device; the renderer released its tables with it. */
    set_clear(&pre.ready);set_clear(&pre.resident);pre.generation++;pre.queue_count=pre.next=0;
    if(pre.device)ID3D11Device_Release(pre.device);
    pre.device=device;if(device)ID3D11Device_AddRef(device);
    ReleaseSRWLockExclusive(&pre.lock);
}
/* Lists shipped with the game (pc_assets\shader-levels.txt, "LEVEL HASH LENGTH KIND" per line,
 * from test runs; make_shader_seed.py --levels) so a level's first visit is covered too. */
typedef struct {uint32_t level;Nv2aShaderKey key;} ShippedKey;
static ShippedKey *shipped;static unsigned shipped_count;
static int shipped_compare(const void *a,const void *b)
{uint32_t x=((const ShippedKey *)a)->level,y=((const ShippedKey *)b)->level;return x<y?-1:x>y;}
static void load_shipped(void)
{
    static int loaded;if(loaded)return;loaded=1;
    const char *overlay=getenv("RECOMP_GAME_OVERLAY");char path[MAX_PATH];
    snprintf(path,sizeof(path),"%s\\shader-levels.txt",overlay&&*overlay?overlay:"pc_assets");
    FILE *file=fopen(path,"r");if(!file)return;
    unsigned capacity=0;unsigned level,length,kind;unsigned long long hash;char line[96];
    while(fgets(line,sizeof(line),file) && shipped_count<200000) {
        if(sscanf(line,"%x %llx %u %u",&level,&hash,&length,&kind)!=4)continue;
        if(shipped_count==capacity) {
            ShippedKey *grown=realloc(shipped,(capacity=capacity?capacity*2:4096)*sizeof(ShippedKey));
            if(!grown)break;shipped=grown;
        }
        if(length){ShippedKey k={level,{hash,length,kind?1u:0u}};shipped[shipped_count++]=k;}
    }
    fclose(file);
    if(shipped_count)qsort(shipped,shipped_count,sizeof(ShippedKey),shipped_compare);
    {char shown[MAX_PATH*2];fprintf(stderr,"[GPU-SHADER] %u shipped level shader entries in %s\n",shipped_count,xbox_log_path(path,shown,sizeof shown));}
}
static int preload_enabled(void)
{
    static int fallback=-1;
    if(fallback<0){const char *v=getenv("RECOMP_SHADER_PRELOAD");fallback=v&&*v?atoi(v)!=0:0;}
    return pc_setting_get(PC_SHADER_PRELOAD,fallback)!=0;
}
void nv2a_shader_level_begin(uint32_t level)
{
    InitOnceExecuteOnce(&cache_once,initialize_cache,NULL,NULL);
    if(!cache_enabled)return;
    AcquireSRWLockExclusive(&pre.lock);
    if(pre.level_valid && pre.level==level){ReleaseSRWLockExclusive(&pre.lock);return;}
    InterlockedIncrement(&nv2a_shader_level_epoch);
    if(pre.level_valid && pre.created)
        fprintf(stderr,"[GPU-SHADER] level %08X: %u of %u pre-created shaders were used\n",pre.level,pre.taken,pre.created);
    pre.created=pre.taken=0;
    if(pre.list){fclose(pre.list);pre.list=NULL;}
    set_clear(&pre.level_keys);
    pre.level=level;pre.level_valid=1;pre.generation++;pre.queue_count=pre.next=0;
    WCHAR path[MAX_PATH];
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\levels",cache_directory);CreateDirectoryW(path,NULL);
    _snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\levels\\%08X.txt",cache_directory,level);
    unsigned known=0;
    FILE *file=_wfopen(path,L"r");
    if(file) {
        unsigned long long hash;unsigned length,kind;char line[96];
        while(known<20000 && fgets(line,sizeof(line),file)) {
            if(sscanf(line,"%llx %u %u",&hash,&length,&kind)!=3)continue;   /* a torn line is skipped */
            Nv2aShaderKey key={hash,length,kind?1u:0u};
            KeySlot *e=length?set_find(&pre.level_keys,&key,1):NULL;
            if(e && !e->count){e->count=1;known++;}
        }
        fclose(file);
    }
    load_shipped();
    for(unsigned lo=0,hi=shipped_count;lo<hi;) {   /* first shipped entry of this level */
        unsigned mid=lo+(hi-lo)/2;
        if(shipped[mid].level<level)lo=mid+1;else hi=mid;
        if(lo==hi)for(unsigned i=lo;i<shipped_count && shipped[i].level==level && known<20000;i++) {
            KeySlot *e=set_find(&pre.level_keys,&shipped[i].key,1);
            if(e && !e->count){e->count=1;known++;}
        }
    }
    pre.list=_wfopen(path,L"a");
    /* Shaders made ahead for the previous level that this one does not use go. */
    for(unsigned i=0;i<pre.ready.capacity;i++) {
        KeySlot *e=&pre.ready.slots[i];
        if(e->object && !set_find(&pre.level_keys,&e->key,0))release_slot(e);
    }
    int enabled=preload_enabled();
    if(enabled && known && pre.device) {
        free(pre.queue);pre.queue=malloc(known*sizeof(Nv2aShaderKey));
        for(unsigned i=0;pre.queue && i<pre.level_keys.capacity;i++)
            if(pre.level_keys.slots[i].key.length)pre.queue[pre.queue_count++]=pre.level_keys.slots[i].key;
        pre.skipped=pre.failed=0;pre.started=GetTickCount64();
        if(!pre.thread_started) {
            pre.wake=CreateEventW(NULL,FALSE,FALSE,NULL);
            HANDLE thread=pre.wake?CreateThread(NULL,0,preload_worker,NULL,0,NULL):NULL;
            if(thread){CloseHandle(thread);pre.thread_started=1;}
        }
        if(pre.thread_started)SetEvent(pre.wake);else pre.queue_count=0;
    }
    fprintf(stderr,"[GPU-SHADER] level %08X: %u shaders recorded%s\n",level,known,
        !enabled?" (pre-creating off)":known?", pre-creating":"");
    ReleaseSRWLockExclusive(&pre.lock);
}
void nv2a_shader_note_level(const Nv2aShaderKey *key)
{
    if(!key->length)return;
    AcquireSRWLockExclusive(&pre.lock);
    KeySlot *e=pre.level_valid?set_find(&pre.level_keys,key,1):NULL;
    if(e && !e->count) {
        e->count=1;
        if(pre.list){fprintf(pre.list,"%016llX %u %u\n",(unsigned long long)key->hash,key->length,key->kind);fflush(pre.list);}
    }
    ReleaseSRWLockExclusive(&pre.lock);
}
void nv2a_shader_resident(const Nv2aShaderKey *key,int delta)
{
    if(!key->length)return;
    AcquireSRWLockExclusive(&pre.lock);
    KeySlot *e=set_find(&pre.resident,key,delta>0);
    if(e)e->count+=delta;
    ReleaseSRWLockExclusive(&pre.lock);
}
static ID3D11DeviceChild *take(const Nv2aShaderKey *key,ID3D11InputLayout **layout)
{
    if(!InterlockedCompareExchange(&pre.ready_count,0,0) || !key->length)return NULL;
    ID3D11DeviceChild *object=NULL;
    AcquireSRWLockExclusive(&pre.lock);
    KeySlot *e=set_find(&pre.ready,key,0);
    if(e && e->object) {
        object=e->object;e->object=NULL;InterlockedDecrement(&pre.ready_count);pre.taken++;
        if(layout)*layout=e->layout;else if(e->layout)ID3D11InputLayout_Release(e->layout);
        e->layout=NULL;
    }
    ReleaseSRWLockExclusive(&pre.lock);
    if(object) {
        static LONG taken;LONG n=InterlockedIncrement(&taken);
        if(n<=2 || (n&255)==0)fprintf(stderr,"[GPU-SHADER] %ld pre-created shader(s) used\n",n);
    }
    return object;
}
ID3D11VertexShader *nv2a_shader_take_vertex(const Nv2aShaderKey *key,ID3D11InputLayout **layout)
{*layout=NULL;return (ID3D11VertexShader *)take(key,layout);}
ID3D11PixelShader *nv2a_shader_take_pixel(const Nv2aShaderKey *key){return (ID3D11PixelShader *)take(key,NULL);}
#endif

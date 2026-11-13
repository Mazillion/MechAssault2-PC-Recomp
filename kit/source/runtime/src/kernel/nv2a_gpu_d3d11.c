/* Hardware rasterisation of the same NV2A batches as nv2a_pb_exec.c.
 * Guest framebuffer readback is reserved for CPU consumers. Render targets
 * remain GPU resources when sampled by a later pass. */
#include "nv2a_gpu_d3d11.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);
#if defined(_WIN32)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#define NV_GPU_PERF_TLS __declspec(thread)
#else
#define NV_GPU_PERF_TLS __thread
#endif

/* PFIFO execution/presentation/reporting run on nv2a_ack_thread. Keep new
 * diagnostics thread-local as well: a CPU fixture or another caller cannot
 * race a producer or reset somebody else's interval. Shader workers never
 * write these counters. This intentionally reports only the calling thread. */
static NV_GPU_PERF_TLS int s_cpu_enabled = -1;
static NV_GPU_PERF_TLS struct {
    Nv2aCpuSnapshot interval;
    uint64_t program_serial, program_hash;
#if defined(_WIN32)
    LARGE_INTEGER frequency;
#endif
} s_cpu;

int nv2a_gpu_cpu_enabled(void)
{
    if(s_cpu_enabled<0)s_cpu_enabled=getenv("RECOMP_PERF")!=NULL||getenv("RECOMP_HITCH")!=NULL;
    return s_cpu_enabled;
}

double nv2a_gpu_cpu_begin(void)
{
    if(!nv2a_gpu_cpu_enabled())return 0;
#if defined(_WIN32)
    LARGE_INTEGER now;
    if(!s_cpu.frequency.QuadPart && !QueryPerformanceFrequency(&s_cpu.frequency))return 0;
    if(!QueryPerformanceCounter(&now))return 0;
    return (double)now.QuadPart*1000.0/(double)s_cpu.frequency.QuadPart;
#else
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return 0;
    return (double)now.tv_sec*1000.0+(double)now.tv_nsec/1000000.0;
#endif
}

void nv2a_gpu_cpu_end(enum Nv2aCpuPhase phase,double start,uint64_t bytes)
{
    if(!start || (unsigned)phase>=NV2A_CPU_PHASE_COUNT)return;
    double elapsed=nv2a_gpu_cpu_begin()-start;
    if(elapsed<0)elapsed=0;
    Nv2aCpuCounter *c=&s_cpu.interval.phases[phase];
    c->calls++;c->bytes+=bytes;c->total_ms+=elapsed;
    if(elapsed>c->max_ms)c->max_ms=elapsed;
    if(phase==NV2A_CPU_PRESENT)s_cpu.interval.present_serial++;
}

void nv2a_gpu_cpu_count(enum Nv2aCpuEvent event,uint64_t count)
{
    if(nv2a_gpu_cpu_enabled() && (unsigned)event<NV2A_CPU_EVENT_COUNT)
        s_cpu.interval.events[event]+=count;
}

void nv2a_gpu_cpu_take(Nv2aCpuSnapshot *out)
{
    if(!out)return;
    *out=s_cpu.interval;
#if defined(_WIN32)
    out->owner_thread=GetCurrentThreadId();
#endif
    memset(s_cpu.interval.phases,0,sizeof(s_cpu.interval.phases));
    memset(s_cpu.interval.events,0,sizeof(s_cpu.interval.events));
}

uint64_t nv2a_gpu_cpu_draw_serial(void){return s_cpu.interval.draw_serial;}

int nv2a_gpu_cpu_program_since(uint64_t before_serial,uint64_t *hash)
{
    if(hash)*hash=0;
    if(!nv2a_gpu_cpu_enabled() || s_cpu.interval.draw_serial<=before_serial ||
       s_cpu.program_serial!=s_cpu.interval.draw_serial)return 0;
    if(hash)*hash=s_cpu.program_hash;
    return 1;
}

#if defined(_WIN32)
static void cpu_draw_completed(void)
{
    if(s_cpu_enabled>0){s_cpu.interval.draw_serial++;s_cpu.program_serial=0;}
}
static void cpu_program_completed(uint64_t before_serial,uint64_t hash)
{
    if(s_cpu_enabled>0 && s_cpu.interval.draw_serial>before_serial) {
        s_cpu.program_serial=s_cpu.interval.draw_serial;s_cpu.program_hash=hash;
    }
}
#endif
#if defined(_WIN32)
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_gpu_shader.h"
#include "nv2a_gpu_vsh.h"
#include "nv2a_gpu_memory.h"
#include "nv2a_vertex_layout.h"
#include "nv2a_texture.h"
#include "nv2a_texture_pack.h"
#include "nv2a_shader_cache.h"
#include "../platform/pc_frontend.h"
#include "../platform/pc_switch.h"   /* T2-3b: [Render] TexRowDecode */
#define XXH_INLINE_ALL
#define XXH_NO_XXH3
#include "vendor/xxhash/xxhash.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);
#define GPU_TARGETS 32
#define GPU_TEXTURES 1024
#define GPU_TEXTURE_INDEX 4096
#define GPU_STATES 256
#define GPU_STATE_INDEX 1024
#define GPU_VERTICES 131072
#define GPU_PROGRAMS 512
#define GPU_PROGRAM_INDEX 2048
#define GPU_BUFFERS 512
#define GPU_BUFFER_INDEX 2048
#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p)=NULL; } } while(0)
#include "nv2a_gpu_timing.h"
#include "nv2a_gpu_feedback.h"
#include "nv2a_gpu_pacing.h"
#include "nv2a_gpu_recovery.h"

/* Optional map-call evidence. Existing upload phase boundaries remain intact;
 * output is emitted only after their original timing end. No GPU queries. */
extern unsigned long long xbox_InputTraceState(void);
typedef struct { double start,elapsed;const char *caller;uint64_t bytes;UINT mode;HRESULT result; } UploadTrace;
static NV_GPU_PERF_TLS int upload_trace_enabled=-1;
static volatile LONG upload_trace_records;
static double upload_trace_begin(void)
{
    if(upload_trace_enabled<0) {const char *v=getenv("RECOMP_UPLOAD_TRACE");upload_trace_enabled=v&&!strcmp(v,"1");}
    return upload_trace_enabled && InterlockedCompareExchange(&upload_trace_records,0,0)<32?nv2a_gpu_cpu_begin():0;
}
static void upload_trace_capture(UploadTrace *record,double start,const char *caller,uint64_t bytes,UINT mode,HRESULT result)
{
    if(!start)return;
    record->start=start;record->elapsed=nv2a_gpu_cpu_begin()-start;
    record->caller=caller;record->bytes=bytes;record->mode=mode;record->result=result;
}
static void upload_trace_emit(const UploadTrace *record)
{
    if(!record->start || record->elapsed<10.0)return;
    LONG count=InterlockedIncrement(&upload_trace_records);if(count>32)return;
    unsigned long long input=xbox_InputTraceState();unsigned body=(unsigned)input;
    fprintf(stderr,"[PERF] upload-slow record=%ld caller=%s cpu_map_ms=%.3f cpu_start_ms=%.3f bytes=%llu map=%u hr=%08lX draw_serial=%llu input_epoch=%u action=%X context=%u device=%u captured=%u scope=cpu_wall_map_not_gpu_time\n",
        (long)count,record->caller,record->elapsed,record->start,(unsigned long long)record->bytes,record->mode,(unsigned long)record->result,
        (unsigned long long)nv2a_gpu_cpu_draw_serial(),(unsigned)(input>>32),body&31u,
        (unsigned)pc_input_context(),(unsigned)pc_input_device(),pc_frontend_captured()!=0);
}


typedef struct {
    uint32_t va, width, height, pitch, bpp, format, pixel_width, pixel_height;
    ID3D11Texture2D *texture, *staging;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D *feedback;
    ID3D11ShaderResourceView *feedback_srv;
    /* Bytes beyond a smaller alias remain part of the same allocation. */
    ID3D11Texture2D *scratch_backup;
    ID3D11ShaderResourceView *scratch_backup_srv;
    ID3D11RenderTargetView *scratch_backup_rtv;
    uint32_t backup_width,backup_height,backup_pitch;
    int dirty;
    /* SET_SURFACE_FORMAT type 2: guest bytes are Morton-ordered (RECOMP_RT_SWIZZLE_FIX). */
    int swizzled;
    uint64_t epoch,allocation_serial;
} GpuTarget;
typedef struct {
    uint32_t format, width, height;
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *srv;
    ID3D11RenderTargetView *rtv;
    uint64_t serial, used;
} GpuDepthAlias;
typedef struct {
    uint32_t va, width, height, pitch, format, pixel_width, pixel_height;
    ID3D11Texture2D *texture;
    ID3D11DepthStencilView *dsv;
    ID3D11ShaderResourceView *depth_srv, *stencil_srv;
    /* Four texture units can use different interpretations in one draw. */
    GpuDepthAlias aliases[4];
    uint64_t write_serial;
    uint64_t epoch,allocation_serial;
} GpuDepth;
typedef struct {
    Nv2aGpuTexture key;
    ID3D11Resource *texture;
    ID3D11ShaderResourceView *srv;
    uint8_t *snapshot;
    size_t bytes;
    uint32_t palette[256];
    uint64_t used;
    uint64_t memory_version,checked_serial;
    uint64_t hash;
    Nv2aTexturePackKey replacement;
} GpuTexture;
typedef struct {
    float viewport[4], fog[4];
    uint32_t control[4], final[4];
    float final_c0[4], final_c1[4], misc[4];
    uint32_t programs[8][4];
    float factor0[8][4], factor1[8][4], tex_info[4][4];
    uint32_t texture_control[4];
    float bump_matrix[4][4], bump_luminance[4][4], eye_vector[4];
} GpuConstants;

typedef struct { uint32_t words[11], programs[8][4]; } GpuPixelKey;
typedef struct {
    float values[NV2A_VSH_CONSTANTS][4],fog[4];uint32_t flags[4];
} GpuVertexConstants;
typedef struct {uint32_t streams[16][4];float defaults[16][4];} GpuFetchConstants;
typedef struct {
    uint32_t va,bytes;
    ID3D11Buffer *buffer;ID3D11ShaderResourceView *srv;
    uint8_t *snapshot;
    uint64_t used,hash,memory_version,checked_serial;
} GpuBuffer;

static struct {
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11DeviceContext1 *context1;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps, *ps_clamped;
    ID3D11VertexShader *depth_copy_vs;
    ID3D11PixelShader *depth_copy_ps;
    ID3D11Buffer *depth_copy_constants;
    ID3D11VertexShader *scratch_vs;
    ID3D11PixelShader *scratch_ps;
    ID3D11Buffer *scratch_constants;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vertices, *indices, *constants, *vertex_constants, *fetch_constants;
    struct { Nv2aVshProgram key; ID3D11VertexShader *shader; ID3D11InputLayout *layout; ID3D11GeometryShader *verify_shader; uint64_t hash,used; uint8_t formats[16]; uint32_t inputs,verified; int pulling; Nv2aShaderKey disk; LONG epoch; } programs[GPU_PROGRAMS];
    uint16_t program_index[GPU_PROGRAM_INDEX];
    unsigned program_count, last_program_slot;
    int last_program_valid;
    struct { GpuPixelKey key; ID3D11PixelShader *shader; uint64_t hash,used; Nv2aShaderKey disk; LONG epoch; } pixels[GPU_PROGRAMS];
    uint16_t pixel_index[GPU_PROGRAM_INDEX];
    unsigned pixel_count;
    ID3D11RasterizerState *raster;
    IDXGISwapChain1 *swapchain;
    ID3D11RenderTargetView *backbuffer;
    ID3D11VertexShader *present_vs;
    ID3D11PixelShader *present_ps;
    ID3D11SamplerState *present_sampler;
    HWND window;
    UINT present_width, present_height, present_flags;
    int vsync, preserve_aspect, present_failed, previous_filter, occluded;
    LARGE_INTEGER present_deadline;
    int previous_limit;
    GpuTarget targets[GPU_TARGETS];
    GpuDepth depths[GPU_TARGETS];
    GpuTexture textures[GPU_TEXTURES];
    uint16_t texture_index[GPU_TEXTURE_INDEX];
    GpuBuffer buffers[GPU_BUFFERS];uint16_t buffer_index[GPU_BUFFER_INDEX];
    ID3D11ShaderResourceView *bound_vertex_views[16];
    ID3D11Buffer *verify_output,*verify_readback;
    ID3D11Texture2D *debug_pixel,*debug_depth;
    D3D11_TEXTURE2D_DESC debug_depth_desc;
    UINT verify_index_offset,verify_ids[3];
    uint64_t draw_serial;
    GpuFeedbackBounds feedback_bounds[4];
    unsigned sprite_clamp;
    int sprite_original;
    D3D11_PRIMITIVE_TOPOLOGY verify_topology;
    struct { D3D11_BLEND_DESC key; ID3D11BlendState *state; } blends[GPU_STATES];
    struct { D3D11_DEPTH_STENCIL_DESC key; ID3D11DepthStencilState *state; } zstates[GPU_STATES];
    struct { D3D11_SAMPLER_DESC key; ID3D11SamplerState *state; } samplers[GPU_STATES];
    uint16_t sampler_index[GPU_STATE_INDEX];
    struct { D3D11_RASTERIZER_DESC key; ID3D11RasterizerState *state; } rasters[GPU_STATES];
    unsigned blend_count, z_count, sampler_count, raster_count;
    uint64_t serial;
    UINT vertex_cursor, index_cursor;
    UINT constant_cursor, vertex_constant_cursor, constant_first, vertex_constant_first;
    int constant_ranges, constants_valid, vertex_constants_valid;
    GpuConstants last_constants;
    GpuVertexConstants last_vertex_constants;
    GpuFetchConstants last_fetch_constants;
    UINT fetch_cursor,fetch_first;int fetch_valid;
    uint64_t checked_allocation_serial;
    ID3D11RenderTargetView *bound_rtv;
    ID3D11DepthStencilView *bound_dsv;
    ID3D11ShaderResourceView *bound_views[8];
    ID3D11SamplerState *bound_samplers[4];
    ID3D11VertexShader *bound_vs;
    ID3D11PixelShader *bound_ps;
    ID3D11InputLayout *bound_layout;
    ID3D11RasterizerState *bound_raster;
    ID3D11BlendState *bound_blend;
    ID3D11DepthStencilState *bound_depth;
    D3D11_PRIMITIVE_TOPOLOGY bound_topology;
    D3D11_VIEWPORT bound_viewport;
    D3D11_RECT bound_scissor;
    float bound_blend_color[4];
    UINT bound_stencil_ref,bound_guest_first,bound_vertex_first,bound_fetch_first;
    int pipeline_valid,index_bound,guest_bound,vertex_bound,fetch_bound;
    int init, failed, program_pending;
    Nv2aFramePacer frame_pacer;
} s;
static struct {
    Nv2aGpuRecovery recovery;
    uint64_t generation;
    HRESULT reason;
    int error_posted, fatal_error;
} lifetime;

/* The immediate-context owner calls this only at a frame boundary. Bound
 * pointers borrow the caches; releasing those pointers separately would
 * double-release resources. No guest allocation or simulation state changes. */
static void release_render_device(void)
{
    pc_frontend_device_lost();
    if(s.context)ID3D11DeviceContext_ClearState(s.context);
    if(gpu_timing.active)fprintf(stderr,"[GPU-TIMING] cancelled by device loss\n");
    gpu_timing_release();
    nv2a_texture_pack_device_lost();
    for(unsigned i=0;i<GPU_TARGETS;i++) {
        GpuTarget *t=&s.targets[i];
        RELEASE(t->srv);RELEASE(t->rtv);RELEASE(t->staging);RELEASE(t->texture);
        RELEASE(t->feedback_srv);RELEASE(t->feedback);
        RELEASE(t->scratch_backup_srv);RELEASE(t->scratch_backup_rtv);RELEASE(t->scratch_backup);
        GpuDepth *d=&s.depths[i];
        RELEASE(d->dsv);RELEASE(d->depth_srv);RELEASE(d->stencil_srv);RELEASE(d->texture);
        for(unsigned a=0;a<4;a++) {
            RELEASE(d->aliases[a].srv);RELEASE(d->aliases[a].rtv);RELEASE(d->aliases[a].texture);
        }
    }
    for(unsigned i=0;i<GPU_TEXTURES;i++) {
        RELEASE(s.textures[i].srv);RELEASE(s.textures[i].texture);free(s.textures[i].snapshot);
    }
    for(unsigned i=0;i<GPU_BUFFERS;i++) {
        RELEASE(s.buffers[i].srv);RELEASE(s.buffers[i].buffer);free(s.buffers[i].snapshot);
    }
    nv2a_shader_preload_device(NULL);
    for(unsigned i=0;i<GPU_PROGRAMS;i++) {
        RELEASE(s.programs[i].shader);RELEASE(s.programs[i].layout);RELEASE(s.programs[i].verify_shader);
        RELEASE(s.pixels[i].shader);
    }
    for(unsigned i=0;i<GPU_STATES;i++) {
        RELEASE(s.blends[i].state);RELEASE(s.zstates[i].state);
        RELEASE(s.samplers[i].state);RELEASE(s.rasters[i].state);
    }
    RELEASE(s.vs);RELEASE(s.ps);RELEASE(s.ps_clamped);RELEASE(s.layout);
    RELEASE(s.depth_copy_vs);RELEASE(s.depth_copy_ps);RELEASE(s.depth_copy_constants);
    RELEASE(s.scratch_vs);RELEASE(s.scratch_ps);RELEASE(s.scratch_constants);
    RELEASE(s.vertices);RELEASE(s.indices);RELEASE(s.constants);
    RELEASE(s.vertex_constants);RELEASE(s.fetch_constants);RELEASE(s.raster);
    RELEASE(s.verify_output);RELEASE(s.verify_readback);
    RELEASE(s.debug_pixel);RELEASE(s.debug_depth);
    RELEASE(s.present_vs);RELEASE(s.present_ps);RELEASE(s.present_sampler);
    RELEASE(s.backbuffer);RELEASE(s.swapchain);
    /* Flush deferred destruction before another flip chain claims the HWND. */
    if(s.context)ID3D11DeviceContext_Flush(s.context);
    RELEASE(s.context1);RELEASE(s.context);RELEASE(s.device);
    nv2a_frame_pacer_close(&s.frame_pacer);
    memset(&s,0,sizeof(s));
    s.previous_filter=-1;
}

static void close_frame_pacer(void)
{
    nv2a_frame_pacer_close(&s.frame_pacer);
}

static void invalidate_pipeline(void)
{
    s.pipeline_valid=s.index_bound=s.guest_bound=s.vertex_bound=s.fetch_bound=0;
}

static uint64_t shader_hash(const void *bytes,size_t length)
{
    /* Hot in command translation. XXH64 uses portable integer loads rather
     * than a dependent multiply for every byte of each vertex program. The
     * full program is still compared before a cache entry can be reused. */
    return XXH64(bytes,length,0);
}

static void index_program(unsigned slot)
{
    unsigned bucket=(unsigned)s.programs[slot].hash & (GPU_PROGRAM_INDEX-1);
    while(s.program_index[bucket])bucket=(bucket+1)&(GPU_PROGRAM_INDEX-1);
    s.program_index[bucket]=(uint16_t)(slot+1);
}

static int gpu_option(const char *name, int fallback)
{
    const char *v=getenv(name);
    return v && *v?atoi(v):fallback;
}

/* A colour surface whose SET_SURFACE_FORMAT type (bits 8-11) is 2 is written
 * swizzled by the NV2A, at the log2 size in bits 16-23/24-31; the pitch is
 * ignored. MechAssault 2 renders its 512x512 ground light map (fmt 0x06) that
 * way and samples it as a swizzled texture, so guest memory must hold it in
 * Morton order or every fetch that goes through memory lands on the wrong
 * texel (the game seeds the map in memory, so a target created from it was
 * scrambled too). The bit is honoured when a target is created from guest
 * memory and when it is read back; RECOMP_RT_SWIZZLE_FIX=0 restores the old
 * row-major layout, RECOMP_RT_SWIZZLE_TRACE=1 logs it. */
static int rt_swizzle_fix(void)
{
    static int fix=-1;
    if(fix<0)fix=gpu_option("RECOMP_RT_SWIZZLE_FIX",1);
    return fix;
}
static int rt_swizzle_trace(void)
{
    static int trace=-1;
    if(trace<0)trace=gpu_option("RECOMP_RT_SWIZZLE_TRACE",0);
    return trace;
}
static uint32_t rt_swizzle_seen[8];
static int surface_swizzled(const Nv2aGpuState *g,uint32_t width,uint32_t height)
{
    if(((g->format>>8)&15)!=2)return 0;
    uint32_t lw=(g->format>>16)&0xFF,lh=(g->format>>24)&0xFF;
    if(rt_swizzle_trace()) {
        int known=0;
        for(int i=0;i<8;i++)if(rt_swizzle_seen[i]==g->color_va)known=1;
        for(int i=0;!known && i<8;i++)if(!rt_swizzle_seen[i]){rt_swizzle_seen[i]=g->color_va;break;}
    }
    int ok=lw<13 && lh<13 && (1u<<lw)==width && (1u<<lh)>=height && (g->bpp==4 || g->bpp==2);
    if(rt_swizzle_trace()) {
        static unsigned logged;
        if(logged<64){logged++;
            fprintf(stderr,"[RT-SWZ] swizzled surface %08X fmt %08X %ux%u (log2 %u,%u) pitch %u bpp %u -> %s\n",
                g->color_va,g->format,width,height,lw,lh,g->pitch,g->bpp,!ok?"size mismatch, kept linear":rt_swizzle_fix()?"swizzled":"linear (fix off)");}
    }
    return ok && rt_swizzle_fix()?(int)lh+1:0;
}
/* Byte offset of guest pixel (x,y) in a target's memory. */
static size_t target_offset(const GpuTarget *t,uint32_t x,uint32_t y)
{
    if(t->swizzled)return (size_t)swizzle_offset(x,y,t->width,1u<<(t->swizzled-1))*t->bpp;
    return (size_t)y*t->pitch+(size_t)x*t->bpp;
}

static void index_pixel(unsigned slot)
{
    unsigned bucket=(unsigned)s.pixels[slot].hash & (GPU_PROGRAM_INDEX-1);
    while(s.pixel_index[bucket])bucket=(bucket+1)&(GPU_PROGRAM_INDEX-1);
    s.pixel_index[bucket]=(uint16_t)(slot+1);
}

/* Only instructions belong in the program key. Colors, fog, alpha reference,
 * texture bindings and bump matrices remain constants, so animation and
 * lighting changes do not create a shader for every draw. */
static int gpu_failure(const char *,HRESULT);
static ID3D11PixelShader *pixel_program(const Nv2aGpuState *g)
{
    static int specialize=-1;
    float depth_max=((g->format>>4)&15)==1?65535.0f:16777215.0f;
    /* D3D's output merger already clamps interpolated depth to the viewport's
     * 0..1 range before comparison, including with depth clipping disabled.
     * Only narrower guest bounds require SV_Depth. Avoiding that redundant
     * shader output allows early depth rejection of hidden geometry.
     * https://learn.microsoft.com/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage */
    int shader_depth=g->z_clamp && !(g->clip_min<=0 &&
        (g->clip_max<=0 || g->clip_max>=depth_max));
    ID3D11PixelShader *fallback=shader_depth?s.ps_clamped:s.ps;
    const char *entry=shader_depth?"pixel_clamped":"pixel_main";
    if(specialize<0)specialize=gpu_option("RECOMP_PIXEL_SPECIALIZE",1);
    if(!specialize)return fallback;
    GpuPixelKey key={0};
    key.words[10]=shader_depth;
    key.words[0]=g->use_rc!=0;
    key.words[4]=g->alpha_test!=0;
    key.words[7]=g->alpha_test?g->alpha_func:0;
    if(g->use_rc) {
        key.words[1]=g->rc.control;key.words[2]=g->rc.stage_program;
        key.words[3]=g->clip_plane_mode;key.words[5]=g->rc.final0;key.words[6]=g->rc.final1;
        key.words[9]=g->rc.other_stage_input;
        unsigned stages=g->rc.control&255;if(stages>8)stages=8;
        for(unsigned i=0;i<stages;i++) {
            key.programs[i][0]=g->rc.color_icw[i];key.programs[i][1]=g->rc.alpha_icw[i];
            key.programs[i][2]=g->rc.color_ocw[i];key.programs[i][3]=g->rc.alpha_ocw[i];
        }
    } else key.words[8]=g->textured?(g->legacy?1:2):0;
    uint64_t hash=shader_hash(&key,sizeof(key));
    unsigned bucket=(unsigned)hash & (GPU_PROGRAM_INDEX-1);
    while(s.pixel_index[bucket]) {
        unsigned i=s.pixel_index[bucket]-1;
        if(s.pixels[i].hash==hash && !memcmp(&key,&s.pixels[i].key,sizeof(key))) {
            s.pixels[i].used=++s.serial;
            if(s.pixels[i].epoch!=nv2a_shader_level_epoch){s.pixels[i].epoch=nv2a_shader_level_epoch;nv2a_shader_note_level(&s.pixels[i].disk);}
            return s.pixels[i].shader;
        }
        bucket=(bucket+1)&(GPU_PROGRAM_INDEX-1);
    }
    double miss_start=nv2a_gpu_cpu_begin();
    Nv2aShaderJob *job=nv2a_shader_find(hash,1);
    ID3DBlob *code=NULL,*errors=NULL;ID3D11PixelShader *shader=NULL;Nv2aShaderKey disk={0};
    HRESULT hr=job?nv2a_shader_poll(job,&code,&errors):S_OK;
    if(job)nv2a_shader_job_key(job,&disk);
    if(hr==NV2A_SHADER_PENDING) {nv2a_gpu_cpu_count(NV2A_CPU_PIXEL_PENDING,1);nv2a_gpu_cpu_end(NV2A_CPU_PIXEL_MISS,miss_start,0);return fallback;}
    if(!job) {
    char prefix[4096];
    int n=snprintf(prefix,sizeof(prefix),
        "#define NV2A_SPECIALIZED 1\n#define NV2A_USE_RC %uu\n"
        "static const uint4 control=uint4(0x%Xu,0x%Xu,0x%Xu,0x%Xu);\n"
        "static const uint4 finalWords=uint4(0x%Xu,0x%Xu,0x%Xu,0x%Xu);\n"
        "static const uint4 textureControl=uint4(0x%Xu,0,0,0);\n"
        "static const uint4 programs[8]={\n",
        key.words[0],key.words[1],key.words[2],key.words[3],key.words[4],
        key.words[5],key.words[6],key.words[7],key.words[8],key.words[9]);
    for(unsigned i=0;i<8;i++)n+=snprintf(prefix+n,sizeof(prefix)-n,
        "uint4(0x%Xu,0x%Xu,0x%Xu,0x%Xu)%s\n",
        key.programs[i][0],key.programs[i][1],key.programs[i][2],key.programs[i][3],i==7?"};":",");
    size_t length=(size_t)n+sizeof(nv2a_gpu_shader_source)-1;
    char *source=malloc(length+1);
    if(!source) {nv2a_gpu_cpu_end(NV2A_CPU_PIXEL_MISS,miss_start,0);return fallback;}
    memcpy(source,prefix,n);memcpy(source+n,nv2a_gpu_shader_source,sizeof(nv2a_gpu_shader_source));
    nv2a_shader_key(source,length,entry,"ps_5_0",&disk);
    shader=nv2a_shader_take_pixel(&disk);   /* created during loading (shader preloading) */
    if(!shader)hr=nv2a_shader_compile(source,length,entry,"ps_5_0",hash,1,&code,&errors);
    free(source);
    }
    if(hr==NV2A_SHADER_PENDING) {nv2a_gpu_cpu_count(NV2A_CPU_PIXEL_PENDING,1);nv2a_gpu_cpu_end(NV2A_CPU_PIXEL_MISS,miss_start,0);return fallback;}
    if(SUCCEEDED(hr) && !shader) {
        double create_start=nv2a_gpu_cpu_begin();
        hr=ID3D11Device_CreatePixelShader(s.device,ID3D10Blob_GetBufferPointer(code),
                            ID3D10Blob_GetBufferSize(code),NULL,&shader);
        nv2a_gpu_cpu_end(NV2A_CPU_SHADER_RESOURCE,create_start,0);
    }
    if(FAILED(hr)) {
        HRESULT reason=ID3D11Device_GetDeviceRemovedReason(s.device);
        if(nv2a_gpu_device_loss_status((uint32_t)hr,(uint32_t)reason)) {
            RELEASE(errors);RELEASE(code);RELEASE(shader);
            nv2a_gpu_cpu_end(NV2A_CPU_PIXEL_MISS,miss_start,0);
            gpu_failure("CreatePixelShader guest",hr);
            return fallback;
        }
        fprintf(stderr,"[GPU-D3D11] pixel program %016llX uses generic fallback: %08X\n",
            (unsigned long long)hash,(unsigned)hr);
        if(errors)fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
        shader=fallback;ID3D11PixelShader_AddRef(shader);
    }
    RELEASE(errors);RELEASE(code);
    unsigned slot;
    if(s.pixel_count<GPU_PROGRAMS)slot=s.pixel_count++;
    else {
        slot=0;
        nv2a_gpu_cpu_count(NV2A_CPU_PIXEL_EVICT,1);
        for(unsigned i=1;i<GPU_PROGRAMS;i++)if(s.pixels[i].used<s.pixels[slot].used)slot=i;
        nv2a_shader_resident(&s.pixels[slot].disk,-1);
        memset(s.pixel_index,0,sizeof(s.pixel_index));
        for(unsigned i=0;i<s.pixel_count;i++)if(i!=slot)index_pixel(i);
    }
    RELEASE(s.pixels[slot].shader);s.pixels[slot].key=key;s.pixels[slot].shader=shader;
    s.pixels[slot].hash=hash;s.pixels[slot].used=++s.serial;index_pixel(slot);
    s.pixels[slot].disk=disk;s.pixels[slot].epoch=nv2a_shader_level_epoch;
    nv2a_shader_resident(&disk,1);nv2a_shader_note_level(&disk);
    fprintf(stderr,"[GPU-D3D11] loaded pixel program %u (textures %05X, combiners %u)\n",
        slot+1,key.words[2],key.words[1]&255);
    nv2a_gpu_cpu_end(NV2A_CPU_PIXEL_MISS,miss_start,0);
    return shader;
}

static void gpu_debug_messages(void)
{
    ID3D11InfoQueue *queue = NULL;
    if (!s.device || !gpu_option("RECOMP_GPU_DEBUG",0) ||
        FAILED(ID3D11Device_QueryInterface(s.device,&IID_ID3D11InfoQueue,(void **)&queue)))
        return;
    UINT64 count = ID3D11InfoQueue_GetNumStoredMessagesAllowedByRetrievalFilter(queue);
    for (UINT64 i = count > 32 ? count - 32 : 0; i < count; ++i) {
        SIZE_T bytes = 0;
        ID3D11InfoQueue_GetMessage(queue,i,NULL,&bytes);
        D3D11_MESSAGE *message = (D3D11_MESSAGE *)malloc(bytes);
        if (message && SUCCEEDED(ID3D11InfoQueue_GetMessage(queue,i,message,&bytes)))
            fprintf(stderr,"[GPU-DEBUG] severity=%u id=%u %.*s\n",
                (unsigned)message->Severity,(unsigned)message->ID,
                (int)message->DescriptionByteLength,message->pDescription);
        free(message);
    }
    ID3D11InfoQueue_ClearStoredMessages(queue);
    RELEASE(queue);
}

static int gpu_failure(const char *operation, HRESULT hr)
{
    if(s.failed || lifetime.recovery.phase==NV2A_GPU_STOPPED)return 0;
    fprintf(stderr, "[GPU-D3D11] %s failed: 0x%08X\n", operation, (unsigned)hr);
    HRESULT reason=s.device?ID3D11Device_GetDeviceRemovedReason(s.device):S_OK;
    if(nv2a_gpu_device_loss_status((uint32_t)hr,(uint32_t)reason)) {
        lifetime.reason=FAILED(reason)?reason:hr;
        nv2a_gpu_recovery_lost(&lifetime.recovery,GetTickCount64());
        fprintf(stderr,"[GPU-D3D11] Device removed reason: 0x%08X after draw %llu\n",
            (unsigned)lifetime.reason,(unsigned long long)s.draw_serial);
        gpu_debug_messages();
    } else if(lifetime.recovery.phase==NV2A_GPU_READY ||
              lifetime.recovery.phase==NV2A_GPU_VERIFYING) {
        /* A required resource/operation failed on a healthy device. Dropping
         * out of the hardware path here would silently rasterize subsequent
         * frames on the CPU. Stop at the next presentation boundary instead.
         * Creation failures during RESTORING keep the existing retry budget. */
        lifetime.reason=hr;
        lifetime.fatal_error=1;
        lifetime.recovery.phase=NV2A_GPU_STOPPED;
        lifetime.recovery.healthy_presents=0;
    }
    s.failed=1;
    return 0;
}

static int dynamic_buffer(UINT size, UINT bind, ID3D11Buffer **out)
{
    D3D11_BUFFER_DESC d={0};
    d.ByteWidth=size;d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=bind;
    d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    HRESULT hr=ID3D11Device_CreateBuffer(s.device,&d,NULL,out);
    return SUCCEEDED(hr) ? 1 : gpu_failure("CreateBuffer",hr);
}

/* Shader model 5 is required by the NV2A translator. Do not accidentally
 * create a level 10 device and fail later while compiling the first scene.
 * Explicit level 11.0 also supports older D3D11 runtimes without 11.1. */
static HRESULT create_render_device(IDXGIAdapter1 *adapter,D3D_DRIVER_TYPE driver,
                                   D3D_FEATURE_LEVEL *level,
                                   ID3D11Device **device,ID3D11DeviceContext **context)
{
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if(gpu_option("RECOMP_GPU_DEBUG",0))flags|=D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr=D3D11CreateDevice((IDXGIAdapter *)adapter,driver,NULL,
        flags,levels,2,D3D11_SDK_VERSION,device,level,context);
    if(hr==E_INVALIDARG) {
        RELEASE(*context);RELEASE(*device);
        hr=D3D11CreateDevice((IDXGIAdapter *)adapter,driver,NULL,
            flags,levels+1,1,D3D11_SDK_VERSION,device,level,context);
    }
    return hr;
}

static HRESULT select_render_device(D3D_FEATURE_LEVEL *level)
{
    IDXGIFactory1 *factory=NULL;
    IDXGIFactory6 *preferred=NULL;
    IDXGIAdapter1 *adapter=NULL;
    DXGI_ADAPTER_DESC1 description={0};
    HRESULT hr;
    const char *warp=getenv("RECOMP_GPU_WARP");
    const char *legacy=getenv("RECOMP_GPU_LEGACY_ENUMERATION");
    /* WARP is an explicit diagnostic backend, never a silent slow fallback. */
    if(warp && atoi(warp)) {
        hr=create_render_device(NULL,D3D_DRIVER_TYPE_WARP,level,&s.device,&s.context);
        if(SUCCEEDED(hr))fprintf(stderr,"[GPU-D3D11] Diagnostic WARP adapter; feature level %X\n",*level);
        return hr;
    }
    hr=CreateDXGIFactory1(&IID_IDXGIFactory1,(void **)&factory);
    if(FAILED(hr))return hr;
    if(!legacy || !atoi(legacy))
        IDXGIFactory1_QueryInterface(factory,&IID_IDXGIFactory6,(void **)&preferred);
    HRESULT last_failure=DXGI_ERROR_UNSUPPORTED;
    SIZE_T best_memory=0;
    LARGE_INTEGER driver={0};
    for(UINT i=0;;i++) {
        if(preferred)hr=IDXGIFactory6_EnumAdapterByGpuPreference(preferred,i,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,&IID_IDXGIAdapter1,(void **)&adapter);
        else hr=IDXGIFactory1_EnumAdapters1(factory,i,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(hr)) { last_failure=hr;break; }
        DXGI_ADAPTER_DESC1 d={0};
        hr=IDXGIAdapter1_GetDesc1(adapter,&d);
        if(FAILED(hr) || (d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) ||
           (!preferred && s.device && d.DedicatedVideoMemory<=best_memory)) {
            RELEASE(adapter);continue;
        }
        ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
        D3D_FEATURE_LEVEL candidate_level=0;
        LARGE_INTEGER umd={0};
        if(FAILED(IDXGIAdapter1_CheckInterfaceSupport(adapter,&IID_IDXGIDevice,&umd)))umd.QuadPart=0;
        hr=create_render_device(adapter,D3D_DRIVER_TYPE_UNKNOWN,&candidate_level,&device,&context);
        RELEASE(adapter);
        if(FAILED(hr)) {
            fprintf(stderr,"[GPU-D3D11] adapter %ls unavailable at feature level 11: %08X\n",
                d.Description,(unsigned)hr);
            RELEASE(context);RELEASE(device);last_failure=hr;continue;
        }
        RELEASE(s.context);RELEASE(s.device);
        s.context=context;s.device=device;*level=candidate_level;
        description=d;best_memory=d.DedicatedVideoMemory;driver=umd;
        if(preferred)break;
    }
    const char *ranking=preferred?"Windows high performance":"dedicated memory fallback";
    RELEASE(adapter);RELEASE(preferred);RELEASE(factory);
    if(!s.device) {
        fprintf(stderr,"[GPU-D3D11] No hardware adapter supports Direct3D feature level 11.0.\n");
        return last_failure;
    }
    fprintf(stderr,"[GPU-D3D11] Hardware adapter: %ls; feature level %X; %s; vendor %04X device %04X; "
        "video memory %u MB; driver %u.%u.%u.%u\n",
        description.Description,*level,ranking,description.VendorId,description.DeviceId,
        (unsigned)(description.DedicatedVideoMemory>>20),
        (unsigned)HIWORD(driver.HighPart),(unsigned)LOWORD(driver.HighPart),
        (unsigned)HIWORD(driver.LowPart),(unsigned)LOWORD(driver.LowPart));
    return S_OK;
}

int nv2a_gpu_enabled(void)
{
    if(nv2a_gpu_recovery_discard(&lifetime.recovery) &&
       lifetime.recovery.phase!=NV2A_GPU_RESTORING)return 0;
    if (!s.init) {
        const char *setting=getenv("RECOMP_GPU");
        D3D_FEATURE_LEVEL level;
        ID3DBlob *vs=NULL,*ps=NULL,*errors=NULL;
        HRESULT hr;
        s.init=1;
        s.previous_filter=-1;
        if (!setting || _stricmp(setting,"d3d11")) { s.failed=1; return 0; }
        hr=select_render_device(&level);
        if (FAILED(hr)) return gpu_failure("D3D11CreateDevice (feature level 11)",hr);
        nv2a_shader_preload_device(s.device);
        ID3D11DeviceContext_QueryInterface(s.context,&IID_ID3D11DeviceContext1,(void **)&s.context1);
        D3D11_FEATURE_DATA_D3D11_OPTIONS options={0};
        s.constant_ranges=s.context1 && SUCCEEDED(ID3D11Device_CheckFeatureSupport(s.device,
            D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options))) &&
            options.ConstantBufferOffsetting && options.MapNoOverwriteOnDynamicConstantBuffer;
        const char *streaming=getenv("RECOMP_GPU_CONSTANT_STREAMING");
        if(streaming && !_stricmp(streaming,"discard"))s.constant_ranges=0;
        hr=nv2a_shader_compile_blocking(nv2a_gpu_shader_source,sizeof(nv2a_gpu_shader_source)-1,
                      "vertex_main","vs_5_0",&vs,&errors);
        if (FAILED(hr)) {
            if(errors) fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
            RELEASE(errors);return gpu_failure("vertex shader compile",hr);
        }
        RELEASE(errors);
        hr=ID3D11Device_CreateVertexShader(s.device,ID3D10Blob_GetBufferPointer(vs),
                     ID3D10Blob_GetBufferSize(vs),NULL,&s.vs);
        if(FAILED(hr)) { RELEASE(vs);return gpu_failure("CreateVertexShader",hr); }
        D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"FOG",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,48,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,64,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_FLOAT,0,80,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,96,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_FLOAT,0,112,D3D11_INPUT_PER_VERTEX_DATA,0}
        };
        hr=ID3D11Device_CreateInputLayout(s.device,elements,8,ID3D10Blob_GetBufferPointer(vs),
                                         ID3D10Blob_GetBufferSize(vs),&s.layout);
        RELEASE(vs);
        if(FAILED(hr)) return gpu_failure("CreateInputLayout",hr);
        hr=nv2a_shader_compile_blocking(nv2a_gpu_shader_source,sizeof(nv2a_gpu_shader_source)-1,
                      "pixel_main","ps_5_0",&ps,&errors);
        if(FAILED(hr)) {
            if(errors) fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
            RELEASE(errors);return gpu_failure("pixel shader compile",hr);
        }
        RELEASE(errors);
        hr=ID3D11Device_CreatePixelShader(s.device,ID3D10Blob_GetBufferPointer(ps),
                                         ID3D10Blob_GetBufferSize(ps),NULL,&s.ps);
        RELEASE(ps);
        if(FAILED(hr)) return gpu_failure("CreatePixelShader",hr);
        hr=nv2a_shader_compile_blocking(nv2a_gpu_shader_source,sizeof(nv2a_gpu_shader_source)-1,
                      "pixel_clamped","ps_5_0",&ps,&errors);
        if(FAILED(hr)) {
            if(errors) fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
            RELEASE(errors);return gpu_failure("clamped pixel shader compile",hr);
        }
        RELEASE(errors);
        hr=ID3D11Device_CreatePixelShader(s.device,ID3D10Blob_GetBufferPointer(ps),
                                         ID3D10Blob_GetBufferSize(ps),NULL,&s.ps_clamped);
        RELEASE(ps);
        if(FAILED(hr)) return gpu_failure("CreateClampedPixelShader",hr);
        D3D11_RASTERIZER_DESC raster={0};
        raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;
        raster.ScissorEnable=TRUE;raster.DepthClipEnable=FALSE;
        hr=ID3D11Device_CreateRasterizerState(s.device,&raster,&s.raster);
        if(FAILED(hr)) return gpu_failure("CreateRasterizerState",hr);
        if(!dynamic_buffer(GPU_VERTICES*sizeof(Nv2aGpuRawVertex),D3D11_BIND_VERTEX_BUFFER,&s.vertices) ||
           !dynamic_buffer(GPU_VERTICES*6*sizeof(uint32_t),D3D11_BIND_INDEX_BUFFER,&s.indices) ||
           !dynamic_buffer(s.constant_ranges?65536:sizeof(GpuConstants),D3D11_BIND_CONSTANT_BUFFER,&s.constants) ||
           !dynamic_buffer(s.constant_ranges?65536:sizeof(GpuVertexConstants),D3D11_BIND_CONSTANT_BUFFER,&s.vertex_constants) ||
           !dynamic_buffer(s.constant_ranges?65536:sizeof(GpuFetchConstants),D3D11_BIND_CONSTANT_BUFFER,&s.fetch_constants)) return 0;
        fprintf(stderr,"[GPU-D3D11] constant streaming: %s\n",s.constant_ranges?"D3D11.1 buffer ranges":"discard buffers");
        fprintf(stderr,"[GPU-D3D11] NV2A hardware rasterizer and register combiners ready\n");
        ++lifetime.generation;
    }
    return !s.failed;
}

int nv2a_gpu_discard_frame(void)
{
    return nv2a_gpu_recovery_discard(&lifetime.recovery);
}
uint64_t nv2a_gpu_device_generation(void)
{
    return lifetime.generation;
}
int nv2a_gpu_debug_device_loss(void)
{
    if(!gpu_option("RECOMP_GPU_RECOVERY_TEST",0) || !s.device || s.failed)return 0;
    /* An owned-process lifecycle test; never remove/reset the system adapter. */
    gpu_failure("Diagnostic injected device removal",DXGI_ERROR_DEVICE_REMOVED);
    return nv2a_gpu_discard_frame();
}

static void discard_target(GpuTarget *t,int preserve)
{
    if(rt_swizzle_trace()) {
        static unsigned logged;
        for(int i=0;i<8 && logged<200;i++)if(rt_swizzle_seen[i]==t->va) {
            logged++;
            fprintf(stderr,"[RT-SWZ] target %08X %ux%u discarded (preserve %d, swizzled %d)\n",t->va,t->width,t->height,preserve,t->swizzled!=0);
            break;
        }
    }
    if(preserve)nv2a_gpu_readback(t->va);
    ID3D11ShaderResourceView *none[8]={0};
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    memset(s.bound_views,0,sizeof(s.bound_views));
    if(s.bound_rtv==t->rtv) {
        ID3D11DeviceContext_OMSetRenderTargets(s.context,0,NULL,NULL);
        s.bound_rtv=NULL;s.bound_dsv=NULL;
    }
    RELEASE(t->feedback_srv);RELEASE(t->feedback);
    RELEASE(t->scratch_backup_srv);RELEASE(t->scratch_backup_rtv);RELEASE(t->scratch_backup);
    RELEASE(t->srv);RELEASE(t->rtv);RELEASE(t->staging);RELEASE(t->texture);
    memset(t,0,sizeof(*t));
}
static int target_current(GpuTarget *t)
{
    uint64_t serial=nv2a_gpu_memory_allocation_serial();
    if(t->allocation_serial==serial)return 1;
    size_t bytes=(size_t)t->pitch*t->height,backup=(size_t)t->backup_pitch*t->backup_height;
    uint64_t epoch=nv2a_gpu_memory_epoch(t->va,bytes>backup?bytes:backup);
    if(t->epoch!=epoch) {
        /* The CPU already owns the replacement allocation. Reading the old
         * target back here would overwrite its new contents with stale pixels. */
        discard_target(t,0);return 0;
    }
    t->allocation_serial=serial;return 1;
}
static void discard_depth(GpuDepth *z)
{
    ID3D11ShaderResourceView *none[8]={0};
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    memset(s.bound_views,0,sizeof(s.bound_views));
    if(s.bound_dsv==z->dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(s.context,0,NULL,NULL);
        s.bound_rtv=NULL;s.bound_dsv=NULL;
    }
    for(unsigned i=0;i<4;i++) {
        RELEASE(z->aliases[i].srv);RELEASE(z->aliases[i].rtv);RELEASE(z->aliases[i].texture);
    }
    RELEASE(z->depth_srv);RELEASE(z->stencil_srv);RELEASE(z->dsv);RELEASE(z->texture);
    memset(z,0,sizeof(*z));
}
static void check_allocations(void)
{
    uint64_t serial=nv2a_gpu_memory_allocation_serial();
    if(s.checked_allocation_serial==serial)return;
    for(unsigned i=0;i<GPU_TARGETS;i++) {
        if(s.targets[i].texture)target_current(&s.targets[i]);
        GpuDepth *z=&s.depths[i];
        if(z->texture && z->allocation_serial!=serial) {
            uint64_t epoch=nv2a_gpu_memory_epoch(z->va,(size_t)z->pitch*z->height);
            if(z->epoch!=epoch) {
                discard_depth(z);
            } else z->allocation_serial=serial;
        }
    }
    s.checked_allocation_serial=serial;
}
/* Reinterpret a scratch allocation's byte layout on the GPU. Bloom reuses one
 * allocation at 320x240 and 160x120; a resize is not a CPU read of its pixels. */
static int scratch_pipeline(void)
{
    if(s.scratch_vs && s.scratch_ps && s.scratch_constants)return 1;
    static const char source[]=
        "cbuffer Shape:register(b0){uint4 oldShape;uint4 newShape;}"
        "Texture2DArray<float4> pixels:register(t0);"
        "float4 vs(uint id:SV_VertexID):SV_Position{"
        "return float4(id==1?3:-1,id==2?-3:1,0,1);}"
        "float4 ps(float4 p:SV_Position):SV_Target{"
        "uint address=(uint)p.y*newShape.z+(uint)p.x*4;"
        "uint y=address/oldShape.z,x=(address%oldShape.z)/4;"
        "if(x>=oldShape.x||y>=oldShape.y)discard;"
        "return pixels.Load(int4(x,y,0,0));}";
    ID3DBlob *code=NULL,*errors=NULL;
    ID3D11VertexShader *vertex=NULL;
    ID3D11PixelShader *pixel=NULL;
    ID3D11Buffer *constants=NULL;
    const char *operation="scratch vertex shader compile";
    HRESULT hr=nv2a_shader_compile_blocking(source,sizeof(source)-1,"vs","vs_5_0",&code,&errors);
    if(FAILED(hr))goto failure;
    operation="CreateVertexShader scratch";
    hr=ID3D11Device_CreateVertexShader(s.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&vertex);
    RELEASE(code);RELEASE(errors);if(FAILED(hr))goto failure;
    operation="scratch pixel shader compile";
    hr=nv2a_shader_compile_blocking(source,sizeof(source)-1,"ps","ps_5_0",&code,&errors);
    if(FAILED(hr))goto failure;
    operation="CreatePixelShader scratch";
    hr=ID3D11Device_CreatePixelShader(s.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&pixel);
    RELEASE(code);RELEASE(errors);if(FAILED(hr))goto failure;
    D3D11_BUFFER_DESC d={0};d.ByteWidth=32;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    operation="CreateBuffer scratch constants";
    hr=ID3D11Device_CreateBuffer(s.device,&d,NULL,&constants);
    if(FAILED(hr))goto failure;
    RELEASE(s.scratch_vs);RELEASE(s.scratch_ps);RELEASE(s.scratch_constants);
    s.scratch_vs=vertex;s.scratch_ps=pixel;s.scratch_constants=constants;
    return 1;
failure:
    if(errors)fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
    RELEASE(code);RELEASE(errors);RELEASE(vertex);RELEASE(pixel);RELEASE(constants);
    return gpu_failure(operation,hr);
}
static void scratch_restore(GpuTarget *target,const GpuTarget *old)
{
    uint32_t constants[8]={old->width,old->height,old->pitch,0,target->width,target->height,target->pitch,0};
    ID3D11DeviceContext_UpdateSubresource(s.context,(ID3D11Resource *)s.scratch_constants,0,NULL,constants,0,0);
    ID3D11ShaderResourceView *none[8]={0};
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    ID3D11DeviceContext_OMSetRenderTargets(s.context,1,&target->rtv,NULL);
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,1,&old->srv);
    ID3D11DeviceContext_PSSetConstantBuffers(s.context,0,1,&s.scratch_constants);
    D3D11_VIEWPORT vp={0,0,(float)target->pixel_width,(float)target->pixel_height,0,1};
    D3D11_RECT rect={0,0,(LONG)target->pixel_width,(LONG)target->pixel_height};
    ID3D11DeviceContext_RSSetViewports(s.context,1,&vp);
    ID3D11DeviceContext_RSSetScissorRects(s.context,1,&rect);
    ID3D11DeviceContext_RSSetState(s.context,s.raster);
    ID3D11DeviceContext_OMSetBlendState(s.context,NULL,NULL,~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(s.context,NULL,0);
    ID3D11DeviceContext_IASetInputLayout(s.context,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s.context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s.context,s.scratch_vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(s.context,s.scratch_ps,NULL,0);
    ID3D11DeviceContext_Draw(s.context,3,0);
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    ID3D11DeviceContext_OMSetRenderTargets(s.context,0,NULL,NULL);
    s.bound_rtv=NULL;s.bound_dsv=NULL;memset(s.bound_views,0,sizeof(s.bound_views));invalidate_pipeline();
    target->dirty=old->dirty;
}
static GpuTarget scratch_backing(const GpuTarget *t)
{
    GpuTarget backing={0};backing.va=t->va;backing.width=t->backup_width;backing.height=t->backup_height;
    backing.pitch=t->backup_pitch;backing.bpp=4;backing.format=t->format;backing.dirty=t->dirty;
    backing.pixel_width=backing.width;backing.pixel_height=backing.height;
    backing.texture=t->scratch_backup;backing.srv=t->scratch_backup_srv;backing.rtv=t->scratch_backup_rtv;
    return backing;
}
static GpuTarget *target_for(const Nv2aGpuState *g)
{
    check_allocations();
    static int trace=-1,trace_resize=-1;
    if(trace<0)trace=gpu_option("RECOMP_RENDER_TRACE",0);
    if(trace_resize<0)trace_resize=gpu_option("RECOMP_TARGET_TRACE",0);
    uint32_t width=g->bpp?g->pitch/g->bpp:0, height=g->clip_y+g->clip_h;
    uint32_t pixel_width=width,pixel_height=height;
    GpuTarget *t=NULL,old={0};
    if(!g->color_va || width==0 || width>4096 || height==0 || height>4096) return NULL;
    int swizzled=surface_swizzled(g,width,height);
    /* The scene and its depth buffer follow Resolution. Authored low-resolution
     * bloom/shadow scratch surfaces keep their texel layout and blur radius. */
    int scene=width>=640 || pc_setting_get(PC_RENDER_SCALE,0)>0;
    if(scene)pc_display_render_extent(width,height,&pixel_width,&pixel_height);
    if(trace && width==640 && height>=480) {
        static unsigned reported_width,reported_height;
        if(reported_width!=pixel_width || reported_height!=pixel_height) {
            fprintf(stderr,"[GPU-D3D11] Scene resolution: %ux%u (guest %ux%u)\n",pixel_width,pixel_height,width,height);
            reported_width=pixel_width;reported_height=pixel_height;
        }
    }
    for(int i=0;i<GPU_TARGETS;i++) {
        GpuTarget *v=&s.targets[i];
        if(v->texture && v->va==g->color_va) {
            unsigned previous_width=width,previous_height=v->height;
            if(scene)pc_display_render_extent(width,v->height,&previous_width,&previous_height);
            if(v->width==width && v->height>=height && v->bpp==g->bpp && v->format==(g->format&15) && v->swizzled==swizzled && v->pixel_width==previous_width && v->pixel_height==previous_height)return v;
            /* A larger clip exposes more of an existing allocation. Preserve
             * completed pixels instead of keeping two stale targets at one VA. */
            if(trace_resize)fprintf(stderr,"[TARGET] resize %08X old=%ux%u/%u bpp=%u fmt=%X pixels=%ux%u new=%ux%u/%u bpp=%u fmt=%X pixels=%ux%u\n",v->va,v->width,v->height,v->pitch,v->bpp,v->format,v->pixel_width,v->pixel_height,width,height,g->pitch,g->bpp,g->format&15,pixel_width,pixel_height);
            if(!scene && v->width<640 && v->bpp==4 && g->bpp==4 && v->format==(g->format&15) && !v->swizzled && !swizzled &&
                    v->pixel_width==v->width && v->pixel_height==v->height && scratch_pipeline()) {
                old=*v;
                if(v->scratch_backup) {
                    GpuTarget backing=scratch_backing(v);
                    scratch_restore(&backing,v);
                    if((size_t)backing.pitch*backing.height>(size_t)v->pitch*v->height)old=backing;
                }
                IUnknown_AddRef((IUnknown *)old.texture);IUnknown_AddRef((IUnknown *)old.srv);IUnknown_AddRef((IUnknown *)old.rtv);
                discard_target(v,0);
            } else nv2a_gpu_invalidate(v->va);
        }
        if(!v->texture && !t) t=v;
    }
    if(!t) { fprintf(stderr,"[GPU-D3D11] render target capacity exceeded\n");return NULL; }
    t->epoch=nv2a_gpu_memory_epoch(g->color_va,(size_t)g->pitch*height);
    t->allocation_serial=nv2a_gpu_memory_allocation_serial();
    t->va=g->color_va;t->width=width;t->height=height;t->pitch=g->pitch;t->bpp=g->bpp;
    t->format=g->format&15;t->pixel_width=pixel_width;t->pixel_height=pixel_height;t->swizzled=swizzled;
    D3D11_TEXTURE2D_DESC d={0};
    d.Width=pixel_width;d.Height=pixel_height;d.MipLevels=1;d.ArraySize=1;
    d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;
    d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    uint32_t *pixels=malloc((size_t)d.Width*d.Height*4);
    /* T2-3 B2.7 (R3 E4): the source column per x once, the source row once per y; same pixels. */
    uint32_t *columns=pixels?malloc((size_t)d.Width*sizeof(uint32_t)):NULL;
    if(!columns){free(pixels);gpu_failure("Allocate render target pixels",E_OUTOFMEMORY);goto allocation_failure;}
    for(uint32_t x=0;x<d.Width;x++) columns[x]=(uint32_t)((uint64_t)x*width/pixel_width);
    const uint8_t *mem=(const uint8_t *)xbox_GetMemoryOffset()+g->color_va;
    for(uint32_t y=0;y<d.Height;y++) {
      uint32_t source_y=(uint32_t)((uint64_t)y*height/pixel_height);
      for(uint32_t x=0;x<d.Width;x++) {
        size_t index=(size_t)y*d.Width+x;
        uint32_t source_x=columns[x];
        if(g->bpp==4) pixels[index]=*(const uint32_t *)(mem+target_offset(t,source_x,source_y));
        else { uint16_t p=*(const uint16_t *)(mem+target_offset(t,source_x,source_y));
            if(t->format==0xA)pixels[index]=0xFF000000|p;
            else if(t->format==1 || t->format==2)
                pixels[index]=(t->format==2?0xFF000000:0)|(((p>>10)&31)*255/31<<16)|(((p>>5)&31)*255/31<<8)|((p&31)*255/31);
            else pixels[index]=0xFF000000|((p>>11)*255/31<<16)|(((p>>5)&63)*255/63<<8)|((p&31)*255/31); }
      }
    }
    free(columns);
    D3D11_SUBRESOURCE_DATA initial={pixels,d.Width*4,0};
    HRESULT hr=ID3D11Device_CreateTexture2D(s.device,&d,&initial,&t->texture);
    free(pixels);
    if(FAILED(hr)) { gpu_failure("CreateTexture2D target",hr);goto allocation_failure; }
    hr=ID3D11Device_CreateRenderTargetView(s.device,(ID3D11Resource *)t->texture,NULL,&t->rtv);
    if(FAILED(hr)) {gpu_failure("CreateRenderTargetView",hr);goto allocation_failure;}
    D3D11_SHADER_RESOURCE_VIEW_DESC view={0};
    view.Format=d.Format;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=1;
    hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)t->texture,&view,&t->srv);
    if(FAILED(hr)) {gpu_failure("CreateShaderResourceView target",hr);goto allocation_failure;}
    /* T2-3 B2.6 (R3 E3): the staging copy is created by readback_target on first use; most
     * targets are never read back (about 100-150 MB of VRAM at 3840x1600). */
    if(old.texture) {
        scratch_restore(t,&old);
        if((size_t)old.pitch*old.height>(size_t)t->pitch*t->height) {
            t->scratch_backup=old.texture;t->scratch_backup_srv=old.srv;t->scratch_backup_rtv=old.rtv;
            t->backup_width=old.width;t->backup_height=old.height;t->backup_pitch=old.pitch;
            t->epoch=nv2a_gpu_memory_epoch(t->va,(size_t)old.pitch*old.height);
        } else {RELEASE(old.srv);RELEASE(old.rtv);RELEASE(old.texture);}
    }
    return t;
allocation_failure:
    RELEASE(old.srv);RELEASE(old.rtv);RELEASE(old.texture);
    discard_target(t,0);return NULL;
}

static GpuDepth *depth_for(const Nv2aGpuState *g,GpuTarget *t)
{
    GpuDepth *z=NULL;
    uint32_t format=(g->format>>4)&15;
    uint32_t pitch=g->depth_pitch?g->depth_pitch:t->width*(format==1?2:4);
    for(int i=0;i<GPU_TARGETS;i++) {
        GpuDepth *v=&s.depths[i];
        if(v->texture && v->va==g->depth_va) {
            if(v->width==t->width && v->height>=t->height && v->pitch==pitch && v->format==format && (uint64_t)v->pixel_height*t->height==(uint64_t)t->pixel_height*v->height && v->pixel_width==t->pixel_width)return v;
            discard_depth(v);
        }
        if(!v->texture && !z) z=v;
    }
    if(!z) return NULL;
    /* Publish only a complete depth resource. A failed view creation must not
     * leave a texture-only entry that the next lookup mistakes for success. */
    GpuDepth pending={0},*slot=z;z=&pending;
    z->va=g->depth_va;z->width=t->width;z->height=t->height;z->pitch=pitch;z->format=format;z->pixel_height=t->pixel_height;z->pixel_width=t->pixel_width;
    z->epoch=nv2a_gpu_memory_epoch(z->va,(size_t)z->pitch*z->height);
    z->allocation_serial=nv2a_gpu_memory_allocation_serial();
    D3D11_TEXTURE2D_DESC d={0};
    d.Width=z->pixel_width;d.Height=z->pixel_height;d.ArraySize=1;d.MipLevels=1;
    d.Format=format==1?DXGI_FORMAT_R16_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;
    d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr=ID3D11Device_CreateTexture2D(s.device,&d,NULL,&z->texture);
    if(FAILED(hr)) {gpu_failure("CreateTexture2D depth",hr);goto allocation_failure;}
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv={0};
    dsv.Format=format==1?DXGI_FORMAT_D16_UNORM:DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    hr=ID3D11Device_CreateDepthStencilView(s.device,(ID3D11Resource *)z->texture,&dsv,&z->dsv);
    if(FAILED(hr)) {gpu_failure("CreateDepthStencilView",hr);goto allocation_failure;}
    D3D11_SHADER_RESOURCE_VIEW_DESC view={0};
    view.Format=format==1?DXGI_FORMAT_R16_UNORM:DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;
    hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)z->texture,&view,&z->depth_srv);
    if(FAILED(hr)) {gpu_failure("CreateShaderResourceView depth",hr);goto allocation_failure;}
    if(format!=1) {
        view.Format=DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)z->texture,&view,&z->stencil_srv);
        if(FAILED(hr)) {gpu_failure("CreateShaderResourceView stencil",hr);goto allocation_failure;}
    }
    ID3D11DeviceContext_ClearDepthStencilView(s.context,z->dsv,
        D3D11_CLEAR_DEPTH|(format==1?0:D3D11_CLEAR_STENCIL),1,0);
    z->write_serial=1;
    *slot=pending;
    return slot;
allocation_failure:
    RELEASE(pending.depth_srv);RELEASE(pending.stencil_srv);
    RELEASE(pending.dsv);RELEASE(pending.texture);
    return NULL;
}

/* Xbox Z24S8 stores Z in bits 8..31 and stencil in bits 0..7. D3D11's
 * D24S8 memory layout is different. Repack the two shader-readable aspects
 * on the GPU, then let the ordinary texture sampler filter the guest color
 * channels. Filtering normalized depth before packing would be incorrect.
 * See xemu hw/xbox/nv2a/pgraph/vk/surface-compute.c for the guest layout. */
static ID3D11ShaderResourceView *depth_texture_view(GpuDepth *z,const Nv2aGpuTexture *alias)
{
    uint32_t format=alias->color;
    GpuDepthAlias *sample=NULL,*oldest=NULL;
    for(unsigned i=0;i<4;i++) {
        GpuDepthAlias *v=&z->aliases[i];
        if(v->srv && v->format==format && v->width==alias->width && v->height==alias->height) {
            sample=v;break;
        }
        if(!oldest || !v->texture || (oldest->texture && v->used<oldest->used))oldest=v;
    }
    if(!sample)sample=oldest;
    if(sample->srv && (sample->format!=format || sample->width!=alias->width || sample->height!=alias->height)) {
        ID3D11ShaderResourceView *none[8]={0};
        ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
        memset(s.bound_views,0,sizeof(s.bound_views));
        RELEASE(sample->srv);RELEASE(sample->rtv);RELEASE(sample->texture);
        memset(sample,0,sizeof(*sample));
    }
    sample->used=s.draw_serial+1;
    if(sample->srv && sample->serial==z->write_serial)return sample->srv;
    static const char source[]=
        "Texture2D<float> depthImage:register(t0);Texture2D<uint2> stencilImage:register(t1);"
        "cbuffer Format:register(b0){uint bits,format;uint2 reserved;}"
        "float4 vs(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);"
        "return float4(uv.x*2-1,1-uv.y*2,0,1);}"
        "float4 ps(float4 position:SV_Position):SV_Target {"
        "int3 xy=int3(position.xy,0);uint value=(uint)round(depthImage.Load(xy)*(bits==16?65535.0:16777215.0));"
        "if(bits==24)value=(value<<8)|(stencilImage.Load(xy).y&255);"
        "uint color=value;"
        "if(format==0x1e)color|=0xff000000;"
        "else if(format==0x3f)color=(value&0xff00ff00)|((value&255)<<16)|((value>>16)&255);"
        "else if(format==0x40)color=((value&255)<<24)|(((value>>8)&255)<<16)|(((value>>16)&255)<<8)|(value>>24);"
        "else if(format==0x41)color=((value&255)<<24)|(value>>8);"
        "else if(format==0x11)color=0xff000000|(((value>>11)*255/31)<<16)|((((value>>5)&63)*255/63)<<8)|((value&31)*255/31);"
        "else if(format==0x10 || format==0x1c)color=(format==0x1c || (value&0x8000)?0xff000000:0)|((((value>>10)&31)*255/31)<<16)|((((value>>5)&31)*255/31)<<8)|((value&31)*255/31);"
        "else if(format==0x1d)return float4((value>>8)&15,(value>>4)&15,value&15,(value>>12)&15)/15.0;"
        "else if(format==0x16)color=0xff000000|((value&0xff00)<<8)|(value&255);"
        "else if(format==0x17)color=0xff000000|value;"
        "else if(format==0x20){uint y=value&255;color=((value>>8)<<24)|(y*0x010101);}"
        "return float4((color>>16)&255,(color>>8)&255,color&255,color>>24)/255.0;}";
    HRESULT hr;
    if(!s.depth_copy_vs) {
        ID3DBlob *code=NULL,*errors=NULL;
        hr=nv2a_shader_compile_blocking(source,sizeof(source)-1,"vs","vs_5_0",&code,&errors);
        if(SUCCEEDED(hr))hr=ID3D11Device_CreateVertexShader(s.device,ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code),NULL,&s.depth_copy_vs);
        RELEASE(code);
        if(FAILED(hr)) {
            if(errors)fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
            RELEASE(errors);gpu_failure("depth texture vertex shader",hr);return NULL;
        }
        RELEASE(errors);
    }
    if(!s.depth_copy_ps) {
        ID3DBlob *code=NULL,*errors=NULL;
        hr=nv2a_shader_compile_blocking(source,sizeof(source)-1,"ps","ps_5_0",&code,&errors);
        if(SUCCEEDED(hr))hr=ID3D11Device_CreatePixelShader(s.device,ID3D10Blob_GetBufferPointer(code),
            ID3D10Blob_GetBufferSize(code),NULL,&s.depth_copy_ps);
        RELEASE(code);
        if(FAILED(hr)) {
            if(errors)fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
            RELEASE(errors);gpu_failure("depth texture pixel shader",hr);return NULL;
        }
        RELEASE(errors);
    }
    if(!s.depth_copy_constants && !dynamic_buffer(16,D3D11_BIND_CONSTANT_BUFFER,&s.depth_copy_constants))return NULL;
    if(!sample->texture) {
        D3D11_TEXTURE2D_DESC d={0};
        d.Width=(UINT)((uint64_t)alias->width*z->pixel_width/z->width);d.Height=(UINT)((uint64_t)alias->height*z->pixel_height/z->height);d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        hr=ID3D11Device_CreateTexture2D(s.device,&d,NULL,&sample->texture);
        if(FAILED(hr)){gpu_failure("CreateTexture2D depth alias",hr);return NULL;}
        hr=ID3D11Device_CreateRenderTargetView(s.device,(ID3D11Resource *)sample->texture,NULL,&sample->rtv);
        if(FAILED(hr)){gpu_failure("CreateRenderTargetView depth alias",hr);return NULL;}
        D3D11_SHADER_RESOURCE_VIEW_DESC view={0};
        view.Format=d.Format;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels=view.Texture2DArray.ArraySize=1;
        hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)sample->texture,&view,&sample->srv);
        if(FAILED(hr)){gpu_failure("CreateShaderResourceView depth alias",hr);return NULL;}
        sample->format=format;sample->width=alias->width;sample->height=alias->height;
        fprintf(stderr,"[GPU-D3D11] GPU depth texture alias: %08X %ux%u of %ux%u, %u-bit Z, format %02X, pixels %ux%u\n",
            z->va,alias->width,alias->height,z->width,z->height,z->format==1?16:24,format,d.Width,d.Height);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.depth_copy_constants,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
    if(FAILED(hr)){gpu_failure("Map depth alias constants",hr);return NULL;}
    uint32_t constants[4]={z->format==1?16:24,format,0,0};
    memcpy(mapped.pData,constants,sizeof(constants));
    ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.depth_copy_constants,0);
    ID3D11ShaderResourceView *none[8]={0},*source_views[2]={z->depth_srv,z->stencil_srv};
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    ID3D11DeviceContext_OMSetRenderTargets(s.context,1,&sample->rtv,NULL);
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,2,source_views);
    ID3D11DeviceContext_PSSetConstantBuffers(s.context,0,1,&s.depth_copy_constants);
    D3D11_VIEWPORT vp={0,0,(float)((uint64_t)alias->width*z->pixel_width/z->width),(float)((uint64_t)alias->height*z->pixel_height/z->height),0,1};
    D3D11_RECT scissor={0,0,(LONG)vp.Width,(LONG)vp.Height};
    ID3D11DeviceContext_RSSetViewports(s.context,1,&vp);
    ID3D11DeviceContext_RSSetScissorRects(s.context,1,&scissor);
    ID3D11DeviceContext_RSSetState(s.context,s.raster);
    ID3D11DeviceContext_OMSetBlendState(s.context,NULL,NULL,~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(s.context,NULL,0);
    ID3D11DeviceContext_IASetInputLayout(s.context,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s.context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s.context,s.depth_copy_vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(s.context,s.depth_copy_ps,NULL,0);
    ID3D11DeviceContext_Draw(s.context,3,0);
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    ID3D11DeviceContext_OMSetRenderTargets(s.context,0,NULL,NULL);
    s.bound_rtv=NULL;s.bound_dsv=NULL;memset(s.bound_views,0,sizeof(s.bound_views));
    invalidate_pipeline();
    sample->serial=z->write_serial;
    return sample->srv;
}

static void blend_constant(uint32_t src,uint32_t dst,uint32_t argb,float color[4])
{
    nv2a_rc_unpack(argb,color);
    /* D3D11 BLEND_FACTOR is an RGBA vector. Xbox CONSTANT_ALPHA uses
     * the constant's alpha in every channel, including RGB. This covers
     * both source and destination, and the inverse factors use D3D's
     * existing INV_BLEND_FACTOR. Do not overwrite CONSTANT_COLOR in a
     * mixed color/alpha pair: that needs a separate shader fallback. */
    int alpha=src==0x8003 || src==0x8004 || dst==0x8003 || dst==0x8004;
    int rgb=src==0x8001 || src==0x8002 || dst==0x8001 || dst==0x8002;
    if(alpha && !rgb)color[0]=color[1]=color[2]=color[3];
}
static D3D11_BLEND blend_factor(uint32_t f)
{
    switch(f) {
    case 0:return D3D11_BLEND_ZERO;case 1:return D3D11_BLEND_ONE;
    case 0x300:return D3D11_BLEND_SRC_COLOR;case 0x301:return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:return D3D11_BLEND_SRC_ALPHA;case 0x303:return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:return D3D11_BLEND_DEST_ALPHA;case 0x305:return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:return D3D11_BLEND_DEST_COLOR;case 0x307:return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001:case 0x8003:return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002:case 0x8004:return D3D11_BLEND_INV_BLEND_FACTOR;
    default:return D3D11_BLEND_ONE;
    }
}
static D3D11_BLEND alpha_factor(D3D11_BLEND f)
{
    switch(f) {
    case D3D11_BLEND_SRC_COLOR:return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR:return D3D11_BLEND_INV_DEST_ALPHA;
    default:return f;
    }
}
static ID3D11BlendState *blend_state(const Nv2aGpuState *g)
{
    D3D11_BLEND_DESC d={0};
    D3D11_RENDER_TARGET_BLEND_DESC *b=&d.RenderTarget[0];
    b->BlendEnable=g->blend_enable;
    b->SrcBlend=blend_factor(g->blend_src);b->DestBlend=blend_factor(g->blend_dst);
    b->SrcBlendAlpha=alpha_factor(b->SrcBlend);b->DestBlendAlpha=alpha_factor(b->DestBlend);
    switch(g->blend_equation) {
    case 0x800A:b->BlendOp=D3D11_BLEND_OP_SUBTRACT;break;
    case 0x800B:b->BlendOp=D3D11_BLEND_OP_REV_SUBTRACT;break;
    case 0x8007:b->BlendOp=D3D11_BLEND_OP_MIN;break;
    case 0x8008:b->BlendOp=D3D11_BLEND_OP_MAX;break;
    default:b->BlendOp=D3D11_BLEND_OP_ADD;
    }
    b->BlendOpAlpha=b->BlendOp;
    b->RenderTargetWriteMask=((g->color_mask&0x10000)?1:0)|((g->color_mask&0x100)?2:0)|
                            ((g->color_mask&1)?4:0)|((g->color_mask&0x1000000)?8:0);
    if((g->format&15)==0xA)b->RenderTargetWriteMask&=D3D11_COLOR_WRITE_ENABLE_GREEN|D3D11_COLOR_WRITE_ENABLE_BLUE;
    for(unsigned i=0;i<s.blend_count;i++) if(!memcmp(&d,&s.blends[i].key,sizeof(d))) return s.blends[i].state;
    unsigned i=s.blend_count<GPU_STATES?s.blend_count++:GPU_STATES-1;
    RELEASE(s.blends[i].state);s.blends[i].key=d;
    HRESULT hr=ID3D11Device_CreateBlendState(s.device,&d,&s.blends[i].state);
    if(FAILED(hr)) gpu_failure("CreateBlendState",hr);
    return s.blends[i].state;
}
static D3D11_STENCIL_OP stencil_op(uint32_t op)
{
    switch(op) {
    case 0:return D3D11_STENCIL_OP_ZERO;
    case 0x1E01:return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02:return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03:return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A:return D3D11_STENCIL_OP_INVERT;
    case 0x8507:return D3D11_STENCIL_OP_INCR;
    case 0x8508:return D3D11_STENCIL_OP_DECR;
    default:return D3D11_STENCIL_OP_KEEP;
    }
}
static ID3D11DepthStencilState *depth_state(const Nv2aGpuState *g)
{
    D3D11_DEPTH_STENCIL_DESC d={0};
    /* NV2A ZWRITEENABLE is a mask, not an independent Z-enable. Disabled
     * depth testing also disables depth writes (including depth-only draws). */
    d.DepthEnable=g->depth_test;
    d.DepthWriteMask=g->depth_mask?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc=g->depth_test && g->depth_func>=0x200 && g->depth_func<=0x207
        ?(D3D11_COMPARISON_FUNC)(g->depth_func-0x200+1):D3D11_COMPARISON_ALWAYS;
    d.StencilEnable=g->stencil_test && ((g->format>>4)&15)!=1;
    d.StencilReadMask=(UINT8)g->stencil_read_mask;d.StencilWriteMask=(UINT8)g->stencil_write_mask;
    d.FrontFace.StencilFunc=g->stencil_func>=0x200 && g->stencil_func<=0x207
        ?(D3D11_COMPARISON_FUNC)(g->stencil_func-0x200+1):D3D11_COMPARISON_ALWAYS;
    d.FrontFace.StencilFailOp=stencil_op(g->stencil_fail);
    d.FrontFace.StencilDepthFailOp=stencil_op(g->stencil_zfail);
    d.FrontFace.StencilPassOp=stencil_op(g->stencil_zpass);
    d.BackFace=d.FrontFace;
    for(unsigned i=0;i<s.z_count;i++) if(!memcmp(&d,&s.zstates[i].key,sizeof(d))) return s.zstates[i].state;
    unsigned i=s.z_count<GPU_STATES?s.z_count++:GPU_STATES-1;
    RELEASE(s.zstates[i].state);s.zstates[i].key=d;
    HRESULT hr=ID3D11Device_CreateDepthStencilState(s.device,&d,&s.zstates[i].state);
    if(FAILED(hr)) gpu_failure("CreateDepthStencilState",hr);
    return s.zstates[i].state;
}
static D3D11_TEXTURE_ADDRESS_MODE texture_address(uint32_t mode)
{
    switch(mode) {
    case 1:return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:return D3D11_TEXTURE_ADDRESS_BORDER;
    default:return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}
static void index_sampler(unsigned slot)
{
    unsigned bucket=(unsigned)shader_hash(&s.samplers[slot].key,sizeof(D3D11_SAMPLER_DESC)) & (GPU_STATE_INDEX-1);
    while(s.sampler_index[bucket])bucket=(bucket+1)&(GPU_STATE_INDEX-1);
    s.sampler_index[bucket]=(uint16_t)(slot+1);
}

static ID3D11SamplerState *sampler_state(const Nv2aGpuTexture *t,uint32_t replacement_levels,uint32_t replacement_scale)
{
    D3D11_SAMPLER_DESC d={0};
    uint32_t min=(t->filter>>16)&255,mag=(t->filter>>24)&15;
    int mipmapped=(d3d8_format_is_swizzled(t->color)||d3d8_format_dxt_block_bytes(t->color)) && min>=3 && min<=6;
    int min_linear=min==2 || min==4 || min==6 || min==7,mag_linear=mag==2 || mag==4;
    /* D3D filter bits independently select minification, magnification and
     * mip interpolation. A linear magnifier must not turn nearest min into
     * trilinear filtering or enable mipmaps on an NV2A LOD0 texture. */
    d.Filter=(D3D11_FILTER)((min_linear?0x10:0)|(mag_linear?4:0)|(mipmapped && min>=5?1:0));
    d.AddressU=texture_address(t->addr_u);d.AddressV=texture_address(t->addr_v);
    d.AddressW=texture_address(t->addr_w);
    int bias=(int)(t->filter<<19)>>19;d.MipLODBias=bias/256.0f;
    uint32_t levels=replacement_levels?replacement_levels:(t->levels?t->levels:1);
    unsigned extra=replacement_scale==4?2:replacement_scale==2?1:0;
    d.MaxLOD=mipmapped?(float)(levels-1):0;
    if(t->control0 && mipmapped) {
        uint32_t lo=(t->control0>>18)&4095,hi=(t->control0>>6)&4095;
        if(lo)lo+=extra;
        if(hi)hi+=extra;
        d.MinLOD=(float)(lo<levels?lo:levels-1);
        d.MaxLOD=(float)(hi<levels?hi:levels-1);
        if(d.MinLOD>d.MaxLOD)d.MinLOD=d.MaxLOD;
    }
    nv2a_rc_unpack(t->border_color,d.BorderColor);
    d.ComparisonFunc=D3D11_COMPARISON_NEVER;d.MaxAnisotropy=1u<<((t->control0>>4)&3);
    if(d.MaxAnisotropy>1 && min_linear && mag_linear)d.Filter=D3D11_FILTER_ANISOTROPIC;
    /* Compare the complete canonical descriptor after a hash match. LOD bounds,
     * replacement levels, address modes and border colors all remain distinct.
     * A quarter-full index avoids searching up to 256 descriptors per texture
     * unit while retaining the existing bounded state cache. */
    unsigned bucket=(unsigned)shader_hash(&d,sizeof(d)) & (GPU_STATE_INDEX-1);
    while(s.sampler_index[bucket]) {
        unsigned slot=s.sampler_index[bucket]-1;
        if(!memcmp(&d,&s.samplers[slot].key,sizeof(d)))return s.samplers[slot].state;
        bucket=(bucket+1)&(GPU_STATE_INDEX-1);
    }
    unsigned i=s.sampler_count<GPU_STATES?s.sampler_count++:GPU_STATES-1;
    if(s.samplers[i].state) {
        /* Replacing a slot must remove its old lookup entry without breaking
         * an open-addressed collision chain. This happens only at capacity. */
        memset(s.sampler_index,0,sizeof(s.sampler_index));
        for(unsigned slot=0;slot<s.sampler_count;slot++)
            if(slot!=i && s.samplers[slot].state)index_sampler(slot);
    }
    RELEASE(s.samplers[i].state);s.samplers[i].key=d;
    HRESULT hr=ID3D11Device_CreateSamplerState(s.device,&d,&s.samplers[i].state);
    if(FAILED(hr)) gpu_failure("CreateSamplerState",hr);
    if(s.samplers[i].state)index_sampler(i);
    return s.samplers[i].state;
}

static ID3D11RasterizerState *raster_state(const Nv2aGpuState *g)
{
    D3D11_RASTERIZER_DESC d={0};
    d.FillMode=D3D11_FILL_SOLID;d.ScissorEnable=TRUE;d.DepthClipEnable=!g->z_clamp;
    d.CullMode=!g->cull_enable?D3D11_CULL_NONE:
        g->cull_face==0x404?D3D11_CULL_FRONT:D3D11_CULL_BACK;
    d.FrontCounterClockwise=g->front_face==0x901;
    if(g->polygon_offset_enable) {
        d.SlopeScaledDepthBias=g->polygon_offset_scale;
        float bias=g->polygon_offset_bias;
        d.DepthBias=isfinite(bias)?(INT)fmaxf(-2147483520.0f,fminf(2147483520.0f,roundf(bias))):0;
    }
    for(unsigned i=0;i<s.raster_count;i++)if(!memcmp(&d,&s.rasters[i].key,sizeof(d)))return s.rasters[i].state;
    unsigned i=s.raster_count<GPU_STATES?s.raster_count++:GPU_STATES-1;
    RELEASE(s.rasters[i].state);s.rasters[i].key=d;
    HRESULT hr=ID3D11Device_CreateRasterizerState(s.device,&d,&s.rasters[i].state);
    if(FAILED(hr))gpu_failure("CreateRasterizerState guest",hr);
    return s.rasters[i].state;
}

static uint64_t texture_hash(const Nv2aGpuTexture *t)
{
    uint32_t key[]={t->offset,t->width,t->height,t->depth,t->dimensionality,
                   t->pitch,t->color,(uint32_t)t->cube,t->levels};
    return shader_hash(key,sizeof(key));
}
static int texture_matches(const Nv2aGpuTexture *a,const Nv2aGpuTexture *b)
{
    return a->offset==b->offset && a->width==b->width && a->height==b->height &&
        a->depth==b->depth && a->dimensionality==b->dimensionality &&
        a->pitch==b->pitch && a->color==b->color && a->cube==b->cube && a->levels==b->levels;
}
static void index_texture(unsigned slot)
{
    unsigned bucket=(unsigned)s.textures[slot].hash & (GPU_TEXTURE_INDEX-1);
    while(s.texture_index[bucket])bucket=(bucket+1)&(GPU_TEXTURE_INDEX-1);
    s.texture_index[bucket]=(uint16_t)(slot+1);
}
/* T2-3b H1: RECOMP_TEX_ROW_DECODE / [Render] TexRowDecode (default 1 since the T2-3b flip): 0 = per-texel upload decode,
 * 1 = row decode, 2 = row decode checked texel for texel against decode ([TEX-ROW] every 30 s). */
static volatile LONG tex_row_checked,tex_row_mismatch;
static int tex_row_decode(void)
{
    static int mode=-1;
    if(mode<0) {
        int v=pc_switch("RECOMP_TEX_ROW_DECODE","Render","TexRowDecode",1);
        mode=v<0?0:v>2?2:v;
        fprintf(stderr,"[TEX-ROW] mode=%d (0 per-texel, 1 row, 2 row+verify)\n",mode);
    }
    return mode;
}
static void tex_row_verify(Nv2aGpuTexel decode,const Nv2aGpuTexture *level,uint32_t y,uint32_t slice,
                           const uint32_t *row,uint32_t w)
{
    static DWORD last;
    uint32_t bad=0;
    for(uint32_t x=0;x<w;x++) {
        uint32_t color=0xFFFFFFFF;
        decode(level,0,x,y,slice,&color);
        if(color!=row[x]) {
            if(!bad && tex_row_mismatch<16)fprintf(stderr,"[TEX-ROW] MISMATCH fmt=%02X %ux%u y=%u x=%u row=%08X texel=%08X\n",
                level->color,level->width,level->height,y,x,row[x],color);
            bad++;
        }
    }
    InterlockedExchangeAdd(&tex_row_checked,(LONG)w);
    if(bad)InterlockedExchangeAdd(&tex_row_mismatch,(LONG)bad);
    DWORD now=GetTickCount();
    if(now-last>=30000){last=now;fprintf(stderr,"[TEX-ROW] verified=%ld mismatches=%ld\n",tex_row_checked,tex_row_mismatch);}
}
/* T2-4 E3: [Render] DrawFast / RECOMP_DRAW_FAST, a bit mask of exact per-draw shortcuts (bit 0 =
 * vertex constants compared in place). 0 = today's path. RECOMP_DRAW_FAST_VERIFY=N recomputes
 * the old decision every Nth draw ([DRAW-FAST] verified/mismatches every 30 s). */
static int draw_fast_verify_every;
static unsigned draw_fast_counter;
static LONG draw_fast_checked,draw_fast_mismatch;
static int draw_fast(void)
{
    static int mask=-1;
    if(mask<0) {
        mask=pc_switch("RECOMP_DRAW_FAST","Render","DrawFast",0)&1;
        const char *v=getenv("RECOMP_DRAW_FAST_VERIFY");draw_fast_verify_every=v?atoi(v):0;
        fprintf(stderr,"[DRAW-FAST] mask=%d verify_every=%d\n",mask,draw_fast_verify_every);
    }
    return mask;
}
static int draw_fast_verify_due(void)
{
    return draw_fast_verify_every>0 && (++draw_fast_counter%(unsigned)draw_fast_verify_every)==0;
}
static void draw_fast_verify_result(int same)
{
    static DWORD last;
    draw_fast_checked++;
    if(!same && draw_fast_mismatch++<16)fprintf(stderr,"[DRAW-FAST] MISMATCH constants decision at check %ld\n",draw_fast_checked);
    DWORD now=GetTickCount();
    if(now-last>=30000){last=now;fprintf(stderr,"[DRAW-FAST] verified=%ld mismatches=%ld\n",draw_fast_checked,draw_fast_mismatch);}
}
static int pack_texel(const Nv2aGpuTexture *t,uint32_t face,uint32_t x,uint32_t y,uint32_t z,uint32_t *argb)
{
    return nv2a_texture_decode_canonical((const uint8_t *)xbox_GetMemoryOffset(),t,face,x,y,z,argb);
}
static ID3D11ShaderResourceView *sampled_view(GpuTexture *v,const Nv2aGpuTexture *t,Nv2aGpuTexel decode,uint32_t stride,uint32_t *levels,uint32_t *scale)
{
    (void)decode;
    if(s.sprite_original)return v->srv;
    ID3D11ShaderResourceView *replacement=nv2a_texture_pack_view(s.device,t,pack_texel,stride,&v->replacement,levels,scale);
    return replacement?replacement:v->srv;
}
static ID3D11ShaderResourceView *texture_view(const Nv2aGpuTexture *t,Nv2aGpuTexel decode,GpuTarget *target,uint32_t *replacement_levels,uint32_t *replacement_scale,const GpuFeedbackBounds *bounds)
{
    *replacement_levels=0;*replacement_scale=1;
    if(!t->valid || !t->width || !t->height || t->width>4096 || t->height>4096) return NULL;
    int volume=t->dimensionality==3;
    if(volume && (!t->depth || t->depth>2048 || t->width>2048 || t->height>2048 || t->cube))return NULL;
    for(int i=0;!volume && !t->cube && i<GPU_TARGETS;i++) {
        GpuTarget *rt=&s.targets[i];
        if(rt->texture && rt->va==t->offset && rt->width==t->width && rt->height==t->height) {
            if(rt!=target)return rt->srv;
            /* Snapshot a feedback pass instead of sampling a resource that
             * is simultaneously bound for output, or stale guest RAM. */
            if(!rt->feedback) {
                ID3D11Texture2D *feedback=NULL;
                ID3D11ShaderResourceView *feedback_srv=NULL;
                D3D11_TEXTURE2D_DESC d;
                ID3D11Texture2D_GetDesc(rt->texture,&d);
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                HRESULT hr=ID3D11Device_CreateTexture2D(s.device,&d,NULL,&feedback);
                if(FAILED(hr)){gpu_failure("CreateTexture2D feedback",hr);return NULL;}
                D3D11_SHADER_RESOURCE_VIEW_DESC view={0};
                view.Format=d.Format;view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=1;
                hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)feedback,&view,&feedback_srv);
                if(FAILED(hr)) {
                    RELEASE(feedback_srv);RELEASE(feedback);
                    gpu_failure("CreateShaderResourceView feedback",hr);return NULL;
                }
                rt->feedback=feedback;rt->feedback_srv=feedback_srv;
            }
            D3D11_BOX box;
            int region=feedback_bounds_box(bounds,t,rt->pixel_width,rt->pixel_height,&box);
            Nv2aGpuState copy={0};copy.color_va=rt->va;
            GpuTimedDraw *timing=gpu_timing_begin_draw(s.context,&copy,
                region?box.right-box.left:rt->pixel_width,region?box.bottom-box.top:rt->pixel_height,0,NULL);
            if(region)
                ID3D11DeviceContext_CopySubresourceRegion(s.context,(ID3D11Resource *)rt->feedback,0,
                    box.left,box.top,0,(ID3D11Resource *)rt->texture,0,&box);
            else ID3D11DeviceContext_CopyResource(s.context,(ID3D11Resource *)rt->feedback,(ID3D11Resource *)rt->texture);
            if(timing)ID3D11DeviceContext_End(s.context,(ID3D11Asynchronous *)timing->after);
            return rt->feedback_srv;
        }
    }
    for(int i=0;!volume && !t->cube && i<GPU_TARGETS;i++) {
        GpuDepth *z=&s.depths[i];
        if(!z->texture || z->va!=t->offset || z->width!=t->width || z->height<t->height || z->pitch!=t->pitch)continue;
        /* A linear alias must describe the same bytes per pixel. Swizzled
         * or differently pitched views need their own address conversion. */
        if(z->format==1) {
            switch(t->color) {
            case 0x10:case 0x11:case 0x16:case 0x17:case 0x1C:case 0x1D:case 0x20:
                return depth_texture_view(z,t);
            }
        } else {
            switch(t->color) {
            case 0x12:case 0x1E:case 0x3F:case 0x40:case 0x41:
                return depth_texture_view(z,t);
            }
        }
    }
    if(rt_swizzle_trace()) {
        static unsigned logged;
        for(int i=0;i<8 && logged<200;i++)if(rt_swizzle_seen[i] && rt_swizzle_seen[i]==t->offset) {
            logged++;
            fprintf(stderr,"[RT-SWZ] texture %08X %ux%u fmt %02X sampled from guest memory (no live render target)\n",
                t->offset,t->width,t->height,t->color);
            break;
        }
    }
    uint32_t block=d3d8_format_dxt_block_bytes(t->color);
    int packed=block||d3d8_format_is_swizzled(t->color);
    uint32_t levels=packed && t->levels?t->levels:1;
    if(levels>13) levels=13;
    uint32_t face_bytes=0, offset[13], sizes[13];
    for(uint32_t lv=0;lv<levels;lv++) {
        uint32_t w=t->width>>lv,h=t->height>>lv,z=volume?t->depth>>lv:1;
        if(!w)w=1;if(!h)h=1;if(!z)z=1;
        offset[lv]=face_bytes;
        uint64_t size=block?(uint64_t)((w+3)/4)*((h+3)/4)*block*z:
            packed?(uint64_t)w*h*z*nv2a_texture_texel_bytes(t->color):(uint64_t)t->pitch*h*z;
        if(size+face_bytes>64*1024*1024)return NULL;
        sizes[lv]=(uint32_t)size;
        face_bytes+=sizes[lv];
    }
    uint32_t stride=t->cube?(face_bytes+127)&~127u:face_bytes;
    size_t bytes=(size_t)stride*(t->cube?6:1);
    if(bytes>64*1024*1024) return NULL;
    const uint8_t *raw=(const uint8_t *)xbox_GetMemoryOffset()+t->offset;
    GpuTexture *v=NULL;
    uint64_t hash=texture_hash(t);
    unsigned bucket=(unsigned)hash & (GPU_TEXTURE_INDEX-1);
    while(s.texture_index[bucket]) {
        GpuTexture *c=&s.textures[s.texture_index[bucket]-1];
        if(c->hash==hash && texture_matches(&c->key,t)){v=c;break;}
        bucket=(bucket+1)&(GPU_TEXTURE_INDEX-1);
    }
    uint64_t serial=nv2a_gpu_memory_serial();
    int unchanged=v && v->memory_version && (v->checked_serial==serial ||
        v->memory_version==nv2a_gpu_memory_version(t->offset,bytes));
    uint64_t watch=unchanged?v->memory_version:0;
    if(v && !unchanged && packed)watch=nv2a_gpu_memory_watch(t->offset,bytes);
    if(v && v->bytes==bytes && (unchanged || !memcmp(v->snapshot,raw,bytes)) &&
       (!t->palette || (v->key.palette==t->palette && !memcmp(v->palette,
                        (const uint8_t *)xbox_GetMemoryOffset()+t->palette,1024)))) {
        v->memory_version=watch;v->checked_serial=serial;v->used=++s.serial;
        return sampled_view(v,t,decode,stride,replacement_levels,replacement_scale);
    }
    double prepare_start=nv2a_gpu_cpu_begin();
    int update=v!=NULL && v->bytes==bytes;
    if(!v) {
        unsigned oldest=0;
        for(unsigned i=1;i<GPU_TEXTURES;i++)if(s.textures[i].used<s.textures[oldest].used)oldest=i;
        v=&s.textures[oldest];
        if(v->texture) {
            nv2a_gpu_cpu_count(NV2A_CPU_TEXTURE_EVICT,1);
            memset(s.texture_index,0,sizeof(s.texture_index));
            for(unsigned i=0;i<GPU_TEXTURES;i++)if(i!=oldest && s.textures[i].texture)index_texture(i);
        }
    }
    if(!update) {
        RELEASE(v->srv);RELEASE(v->texture);free(v->snapshot);v->snapshot=malloc(bytes);
    }
    v->key=*t;v->bytes=bytes;v->used=++s.serial;v->hash=hash;
    memset(&v->replacement,0,sizeof(v->replacement));
    if(!v->snapshot){gpu_failure("Allocate sampled texture snapshot",E_OUTOFMEMORY);goto allocation_failure;}
    /* Arm both CPU aliases before copying, so any later CPU store invalidates
     * this upload. Mutable/unwatchable memory retains byte comparisons. */
    v->checked_serial=serial;
    v->memory_version=packed?nv2a_gpu_memory_watch(t->offset,bytes):0;
    memcpy(v->snapshot,raw,bytes);
    if(t->palette)memcpy(v->palette,(const uint8_t *)xbox_GetMemoryOffset()+t->palette,1024);
    D3D11_TEXTURE2D_DESC d={0};
    d.Width=t->width;d.Height=t->height;d.MipLevels=levels;d.ArraySize=t->cube?6:1;
    d.Format=volume?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    int native_bc=!volume && block && t->width>=4 && t->height>=4;
    if(native_bc)d.Format=block==8?DXGI_FORMAT_BC1_UNORM:t->color==0xE?DXGI_FORMAT_BC2_UNORM:DXGI_FORMAT_BC3_UNORM;
    int native_linear=!volume && !packed && (t->color==0x12 || t->color==0x1E);
    if(native_linear && t->color==0x1E)d.Format=DXGI_FORMAT_B8G8R8X8_UNORM;
    D3D11_SUBRESOURCE_DATA data[78]={0};
    uint64_t resource_bytes=0;
    for(uint32_t face=0;face<d.ArraySize;face++) for(uint32_t lv=0;lv<levels;lv++) {
        uint32_t w=t->width>>lv,h=t->height>>lv,z=volume?t->depth>>lv:1;
        if(!w)w=1;if(!h)h=1;if(!z)z=1;
        uint32_t sub=face*levels+lv;
        resource_bytes+=(native_bc || native_linear)?sizes[lv]:(uint64_t)w*h*z*4;
        if(native_bc) {
            data[sub].pSysMem=raw+face*stride+offset[lv];data[sub].SysMemPitch=((w+3)/4)*block;
        } else if(native_linear) {
            data[sub].pSysMem=raw+face*stride+offset[lv];data[sub].SysMemPitch=t->pitch;
        } else {
            uint32_t *pixels=malloc((size_t)w*h*z*4);
            Nv2aGpuTexture level=*t;
            level.width=w;level.height=h;level.depth=z;level.offset+=face*stride+offset[lv];
            if(!pixels) {
                for(uint32_t i=0;i<sub;i++)free((void *)data[i].pSysMem);
                gpu_failure("Allocate sampled texture pixels",E_OUTOFMEMORY);goto allocation_failure;
            }
            int rows=tex_row_decode();
            const uint8_t *memory=(const uint8_t *)xbox_GetMemoryOffset();
            for(uint32_t slice=0;slice<z;slice++)for(uint32_t y=0;y<h;y++) {
                uint32_t *row=pixels+((size_t)slice*h+y)*w;
                /* T2-3b H1: one row decode (masks and row base found once) instead of a
                 * call through decode per texel. decode is the executor's sample_tex_3d,
                 * which for in-range texels is nv2a_texture_decode; a row that fails or
                 * is not handled (-1, 0) is decoded per texel exactly as before. */
                int whole=rows?nv2a_texture_decode_row(memory,&level,0,y,slice,row):-1;
                if(whole==1 && rows==2)tex_row_verify(decode,&level,y,slice,row,w);
                if(whole==1) {
                    if(volume)for(uint32_t x=0;x<w;x++) {
                        uint32_t color=row[x];
                        row[x]=(color&0xFF00FF00u)|((color&255u)<<16)|((color>>16)&255u);
                    }
                    continue;
                }
                for(uint32_t x=0;x<w;x++) {
                    uint32_t color=0xFFFFFFFF;
                    decode(&level,0,x,y,slice,&color);
                    if(volume)color=(color&0xFF00FF00u)|((color&255u)<<16)|((color>>16)&255u);
                    row[x]=color;
                }
            }
            data[sub].pSysMem=pixels;data[sub].SysMemPitch=w*4;data[sub].SysMemSlicePitch=w*h*4;
        }
    }
    nv2a_gpu_cpu_end(NV2A_CPU_TEXTURE_PREPARE,prepare_start,bytes);prepare_start=0;
    double resource_start=nv2a_gpu_cpu_begin();
    HRESULT hr=S_OK;
    if(update) {
        nv2a_gpu_cpu_count(NV2A_CPU_TEXTURE_UPDATE,1);
        for(uint32_t i=0;i<d.ArraySize*levels;i++)
            ID3D11DeviceContext_UpdateSubresource(s.context,(ID3D11Resource *)v->texture,i,NULL,
                data[i].pSysMem,data[i].SysMemPitch,data[i].SysMemSlicePitch);
    } else if(volume) {
        D3D11_TEXTURE3D_DESC box={0};ID3D11Texture3D *resource=NULL;
        box.Width=d.Width;box.Height=d.Height;box.Depth=t->depth;box.MipLevels=levels;
        box.Format=d.Format;box.Usage=d.Usage;box.BindFlags=d.BindFlags;
        hr=ID3D11Device_CreateTexture3D(s.device,&box,data,&resource);
        v->texture=(ID3D11Resource *)resource;
    } else {
        ID3D11Texture2D *resource=NULL;
        hr=ID3D11Device_CreateTexture2D(s.device,&d,data,&resource);
        v->texture=(ID3D11Resource *)resource;
    }
    nv2a_gpu_cpu_end(NV2A_CPU_TEXTURE_RESOURCE,resource_start,resource_bytes);
    if(!native_bc && !native_linear)for(uint32_t i=0;i<d.ArraySize*levels;i++)free((void *)data[i].pSysMem);
    if(FAILED(hr)){gpu_failure(volume?"CreateTexture3D sampled":"CreateTexture2D sampled",hr);goto allocation_failure;}
    if(update)return sampled_view(v,t,decode,stride,replacement_levels,replacement_scale);
    D3D11_SHADER_RESOURCE_VIEW_DESC view={0};
    view.Format=d.Format;
    if(volume) {
        view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE3D;view.Texture3D.MipLevels=levels;
    } else {
        view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels=levels;view.Texture2DArray.ArraySize=d.ArraySize;
    }
    double srv_start=nv2a_gpu_cpu_begin();
    hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)v->texture,&view,&v->srv);
    nv2a_gpu_cpu_end(NV2A_CPU_TEXTURE_SRV,srv_start,0);
    if(FAILED(hr)){gpu_failure("CreateShaderResourceView sampled",hr);goto allocation_failure;}
    index_texture((unsigned)(v-s.textures));
    return sampled_view(v,t,decode,stride,replacement_levels,replacement_scale);
allocation_failure:
    nv2a_gpu_cpu_end(NV2A_CPU_TEXTURE_PREPARE,prepare_start,bytes);
    RELEASE(v->srv);RELEASE(v->texture);free(v->snapshot);memset(v,0,sizeof(*v));
    /* Removing an open-addressed entry must retain other collision chains. */
    memset(s.texture_index,0,sizeof(s.texture_index));
    for(unsigned i=0;i<GPU_TEXTURES;i++)if(s.textures[i].texture)index_texture(i);
    return NULL;
}

static int draw_batch(const Nv2aGpuState *g,const void *vertices,uint32_t stride,uint32_t count,
                      const uint32_t *vertex_indices,uint32_t vertex_count,uint32_t primitive,
                      Nv2aGpuTexel decode,ID3D11VertexShader *shader,ID3D11InputLayout *layout)
{
    if(!nv2a_gpu_enabled() || count>GPU_VERTICES) return 0;
    if(!count)return 1;
    if(g->cull_enable && g->cull_face==0x408 && primitive>=5)return 1;
    GpuTarget *target=target_for(g);
    if(!target)return 0;
    GpuDepth *depth=depth_for(g,target);
    if(!depth)return 0;
    UINT vertex_bytes=vertices?vertex_count*stride:0;
    double upload_start=nv2a_gpu_cpu_begin();
    UploadTrace vertex_trace={0},index_trace={0};double trace_start;
    if(s.vertex_cursor+vertex_bytes>GPU_VERTICES*sizeof(Nv2aGpuRawVertex)) {
        nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_WRAP,1);s.vertex_cursor=0;
    }
    UINT vertex_offset=s.vertex_cursor;
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr=S_OK;
    if(vertex_bytes) {
    if(!vertex_offset)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_DISCARD,1);
    trace_start=upload_trace_begin();
    hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.vertices,0,
                      vertex_offset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&map);
    upload_trace_capture(&vertex_trace,trace_start,"vertex",vertex_bytes,vertex_offset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,hr);
    if(FAILED(hr)) {nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,upload_start,0);upload_trace_emit(&vertex_trace);return gpu_failure("Map vertex buffer",hr);}
    memcpy((uint8_t *)map.pData+vertex_offset,vertices,vertex_bytes);
    s.vertex_cursor+=vertex_bytes;
    ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.vertices,0);
    }
    if(s.index_cursor+count*6>GPU_VERTICES*6) {nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_WRAP,1);s.index_cursor=0;}
    UINT index_offset=s.index_cursor;
    if(!index_offset)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_DISCARD,1);
    trace_start=upload_trace_begin();
    hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.indices,0,
                      index_offset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&map);
    upload_trace_capture(&index_trace,trace_start,"index",(uint64_t)count*6*4,index_offset?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,hr);
    if(FAILED(hr)) {nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,upload_start,vertex_bytes);upload_trace_emit(&index_trace);upload_trace_emit(&vertex_trace);return gpu_failure("Map index buffer",hr);}
    uint32_t *idx=(uint32_t *)map.pData+index_offset,n=0;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
#define INDEX(i) (vertex_indices?vertex_indices[(i)]:(i))
#define TRI(a,b,c) do{idx[n++]=INDEX(a);idx[n++]=INDEX(b);idx[n++]=INDEX(c);}while(0)
    switch(primitive) {
    case 1:topology=D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;for(uint32_t i=0;i<count;i++)idx[n++]=INDEX(i);break;
    case 2:topology=D3D11_PRIMITIVE_TOPOLOGY_LINELIST;for(uint32_t i=0;i+1<count;i+=2){idx[n++]=INDEX(i);idx[n++]=INDEX(i+1);}break;
    case 3:case 4:
        topology=D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        for(uint32_t i=0;i+1<count;i++){idx[n++]=INDEX(i);idx[n++]=INDEX(i+1);}
        if(primitive==3 && count>1){idx[n++]=INDEX(count-1);idx[n++]=INDEX(0);}
        break;
    case 5:for(uint32_t i=0;i+2<count;i+=3)TRI(i,i+1,i+2);break;
    case 6:for(uint32_t i=0;i+2<count;i++){if(i&1)TRI(i+1,i,i+2);else TRI(i,i+1,i+2);}break;
    case 7:case 10:for(uint32_t i=1;i+1<count;i++)TRI(0,i,i+1);break;
    case 8:for(uint32_t i=0;i+3<count;i+=4){TRI(i,i+1,i+2);TRI(i,i+2,i+3);}break;
    case 9:for(uint32_t i=0;i+3<count;i+=2){TRI(i,i+1,i+3);TRI(i,i+3,i+2);}break;
    }
#undef TRI
#undef INDEX
    s.verify_index_offset=index_offset;s.verify_topology=topology;
    if(n>=3)memcpy(s.verify_ids,idx,sizeof(s.verify_ids));
    ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.indices,0);
    nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,upload_start,vertex_bytes+(uint64_t)n*4);
    upload_trace_emit(&vertex_trace);upload_trace_emit(&index_trace);
    if(!n)return primitive>=1 && primitive<=10;
    s.index_cursor+=n;
    GpuConstants c={0};
    c.viewport[0]=(float)target->width;c.viewport[1]=(float)target->height;
    /* Clipping bounds do not change depth-buffer units or clear values. */
    c.viewport[2]=((g->format>>4)&15)==1?65535.0f:16777215.0f;c.viewport[3]=(float)g->use_rc;
    nv2a_rc_unpack(g->fog_color,c.fog);float r=c.fog[0];c.fog[0]=c.fog[2];c.fog[2]=r;
    c.control[0]=g->rc.control;c.control[1]=g->rc.stage_program;c.control[2]=g->clip_plane_mode;c.control[3]=g->alpha_test;
    c.final[0]=g->rc.final0;c.final[1]=g->rc.final1;c.final[2]=g->alpha_func;c.final[3]=g->textured?(g->legacy?1:2):0;
    c.misc[0]=g->alpha_ref/255.0f;
    c.misc[1]=g->clip_min/c.viewport[2];
    c.misc[2]=g->clip_max>0?g->clip_max/c.viewport[2]:1.0f;
    c.misc[3]=c.misc[1]>0.0f || c.misc[2]<1.0f;
    c.texture_control[0]=g->rc.other_stage_input;
    memcpy(c.bump_matrix,g->bump_matrix,sizeof(c.bump_matrix));
    memcpy(c.eye_vector,g->eye_vector,sizeof(g->eye_vector));
    for(int i=0;i<4;i++){c.bump_luminance[i][0]=g->bump_scale[i];c.bump_luminance[i][1]=g->bump_offset[i];}
    nv2a_rc_unpack(g->rc.final_c0,c.final_c0);nv2a_rc_unpack(g->rc.final_c1,c.final_c1);
    for(int i=0;i<8;i++) {
        c.programs[i][0]=g->rc.color_icw[i];c.programs[i][1]=g->rc.alpha_icw[i];
        c.programs[i][2]=g->rc.color_ocw[i];c.programs[i][3]=g->rc.alpha_ocw[i];
        nv2a_rc_unpack(g->rc.factor0[i],c.factor0[i]);nv2a_rc_unpack(g->rc.factor1[i],c.factor1[i]);
    }
    ID3D11ShaderResourceView *views[8]={0};ID3D11SamplerState *samplers[4]={0};
    ID3D11ShaderResourceView *none[8]={0};
    int feedback=0;unsigned sampled=sampled_texture_mask(g);
    for(int i=0;i<4;i++)if((sampled&(1u<<i)) && g->textures[i].valid && g->textures[i].offset==g->color_va)feedback=1;
    if(feedback || s.bound_rtv!=target->rtv || s.bound_dsv!=depth->dsv) {
        ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
        memset(s.bound_views,0,sizeof(s.bound_views));
    }
    if(feedback) {
        ID3D11DeviceContext_OMSetRenderTargets(s.context,0,NULL,NULL);
        s.bound_rtv=NULL;s.bound_dsv=NULL;
    }
    nv2a_texture_pack_draw();
    for(int i=0;i<4;i++) {
        if(!(sampled&(1u<<i)))continue;
        const Nv2aGpuTexture *t=&g->textures[i];
        int slot=t->dimensionality==3?i+4:i;
        uint32_t replacement_levels,replacement_scale;
        /* Sprite mask stages are game-written threshold data, never artwork. */
        s.sprite_original=i>0 && (s.sprite_clamp&1u) && (s.sprite_clamp&(1u<<i));
        views[slot]=texture_view(t,decode,target,&replacement_levels,&replacement_scale,&s.feedback_bounds[i]);
        s.sprite_original=0;
        if(s.sprite_clamp&(1u<<i)) {
            Nv2aGpuTexture clamped=*t;clamped.addr_u=clamped.addr_v=3;
            /* Later sprite mask stages are thresholds: sample them per texel
             * as at native resolution (see program_sprite_clamp). */
            if(i)clamped.filter=(clamped.filter&~0x0FFF0000u)|0x01010000u;
            samplers[i]=sampler_state(&clamped,replacement_levels,replacement_scale);
        } else samplers[i]=sampler_state(t,replacement_levels,replacement_scale);
        int normalized=d3d8_format_is_swizzled(t->color)||d3d8_format_dxt_block_bytes(t->color);
        c.tex_info[i][0]=normalized?1.0f:t->width?1.0f/t->width:1;
        c.tex_info[i][1]=normalized?1.0f:t->height?1.0f/t->height:1;
        c.tex_info[i][2]=views[slot]?1.0f:0.0f;
        c.tex_info[i][3]=t->dimensionality==3?2.0f:t->cube?1.0f:0.0f;
    }
    if(!s.constants_valid || memcmp(&c,&s.last_constants,sizeof(c))) {
        UINT bytes=(sizeof(c)+255)&~255u;
        double constant_start=nv2a_gpu_cpu_begin();
        if(!s.constant_ranges || s.constant_cursor+bytes>65536) {if(s.constant_cursor)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_WRAP,1);s.constant_cursor=0;}
        s.constant_first=s.constant_cursor/16;
        if(!s.constant_cursor)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_DISCARD,1);
        UploadTrace constant_trace={0};double trace_start=upload_trace_begin();
        hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.constants,0,
            s.constant_cursor?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&map);
        upload_trace_capture(&constant_trace,trace_start,"pixel_constants",sizeof(c),s.constant_cursor?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,hr);
        if(FAILED(hr)) {nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,constant_start,0);upload_trace_emit(&constant_trace);return gpu_failure("Map constants",hr);}
        memcpy((uint8_t *)map.pData+s.constant_cursor,&c,sizeof(c));
        ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.constants,0);
        nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,constant_start,sizeof(c));
        upload_trace_emit(&constant_trace);
        s.constant_cursor+=bytes;s.last_constants=c;s.constants_valid=1;
    }
    if(vertices)ID3D11DeviceContext_IASetVertexBuffers(s.context,0,1,&s.vertices,&stride,&vertex_offset);
    if(!s.index_bound) {
        ID3D11DeviceContext_IASetIndexBuffer(s.context,s.indices,DXGI_FORMAT_R32_UINT,0);s.index_bound=1;
    }
    if(!s.pipeline_valid || s.bound_layout!=layout) {
        ID3D11DeviceContext_IASetInputLayout(s.context,layout);s.bound_layout=layout;
    }
    if(!s.pipeline_valid || s.bound_topology!=topology) {
        ID3D11DeviceContext_IASetPrimitiveTopology(s.context,topology);s.bound_topology=topology;
    }
    if(!s.pipeline_valid || s.bound_vs!=shader) {
        ID3D11DeviceContext_VSSetShader(s.context,shader,NULL,0);s.bound_vs=shader;
    }
    ID3D11PixelShader *pixel_shader=pixel_program(g);
    if(!s.pipeline_valid || s.bound_ps!=pixel_shader) {
        ID3D11DeviceContext_PSSetShader(s.context,pixel_shader,NULL,0);s.bound_ps=pixel_shader;
    }
    if(!s.guest_bound || s.bound_guest_first!=s.constant_first) {
        if(s.constant_ranges) {
            UINT num=((sizeof(c)+255)&~255u)/16;
            ID3D11DeviceContext1_VSSetConstantBuffers1(s.context1,0,1,&s.constants,&s.constant_first,&num);
            ID3D11DeviceContext1_PSSetConstantBuffers1(s.context1,0,1,&s.constants,&s.constant_first,&num);
        } else {
            ID3D11DeviceContext_VSSetConstantBuffers(s.context,0,1,&s.constants);
            ID3D11DeviceContext_PSSetConstantBuffers(s.context,0,1,&s.constants);
        }
        s.guest_bound=1;s.bound_guest_first=s.constant_first;
    }
    if(memcmp(s.bound_samplers,samplers,sizeof(samplers))) {
        ID3D11DeviceContext_PSSetSamplers(s.context,0,4,samplers);
        memcpy(s.bound_samplers,samplers,sizeof(samplers));
    }
    ID3D11RasterizerState *raster=raster_state(g);
    if(!s.pipeline_valid || s.bound_raster!=raster) {
        ID3D11DeviceContext_RSSetState(s.context,raster);s.bound_raster=raster;
    }
    D3D11_VIEWPORT vp={0,0,(float)target->pixel_width,(float)target->pixel_height,0,1};
    D3D11_RECT scissor={(LONG)((uint64_t)g->clip_x*target->pixel_width/target->width),(LONG)((uint64_t)g->clip_y*target->pixel_height/target->height),
        (LONG)(((uint64_t)(g->clip_x+g->clip_w)*target->pixel_width+target->width-1)/target->width),(LONG)(((uint64_t)(g->clip_y+g->clip_h)*target->pixel_height+target->height-1)/target->height)};
    if(!s.pipeline_valid || memcmp(&s.bound_viewport,&vp,sizeof(vp))) {
        ID3D11DeviceContext_RSSetViewports(s.context,1,&vp);s.bound_viewport=vp;
    }
    if(!s.pipeline_valid || memcmp(&s.bound_scissor,&scissor,sizeof(scissor))) {
        ID3D11DeviceContext_RSSetScissorRects(s.context,1,&scissor);s.bound_scissor=scissor;
    }
    ID3D11BlendState *blend=blend_state(g);ID3D11DepthStencilState *zs=depth_state(g);
    float blend_color[4];blend_constant(g->blend_src,g->blend_dst,g->blend_color,blend_color);
    if(!s.pipeline_valid || s.bound_blend!=blend || memcmp(s.bound_blend_color,blend_color,sizeof(blend_color))) {
        ID3D11DeviceContext_OMSetBlendState(s.context,blend,blend_color,0xFFFFFFFF);
        s.bound_blend=blend;memcpy(s.bound_blend_color,blend_color,sizeof(blend_color));
    }
    if(!s.pipeline_valid || s.bound_depth!=zs || s.bound_stencil_ref!=(g->stencil_ref&255)) {
        ID3D11DeviceContext_OMSetDepthStencilState(s.context,zs,g->stencil_ref&255);
        s.bound_depth=zs;s.bound_stencil_ref=g->stencil_ref&255;
    }
    if(s.bound_rtv!=target->rtv || s.bound_dsv!=depth->dsv) {
        ID3D11DeviceContext_OMSetRenderTargets(s.context,1,&target->rtv,depth->dsv);
        s.bound_rtv=target->rtv;s.bound_dsv=depth->dsv;
    }
    if(memcmp(s.bound_views,views,sizeof(views))) {
        ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,views);
        memcpy(s.bound_views,views,sizeof(views));
    }
    s.pipeline_valid=1;
    GpuTimedDraw *timing=gpu_timing_begin_draw(s.context,g,target->pixel_width,target->pixel_height,n,pixel_shader);
    ID3D11DeviceContext_DrawIndexed(s.context,n,index_offset,0);
    if(timing)ID3D11DeviceContext_End(s.context,(ID3D11Asynchronous *)timing->after);
    s.draw_serial++;
    cpu_draw_completed();
    if((g->depth_test && g->depth_mask) ||
       (g->stencil_test && (g->stencil_write_mask&255) && depth->format!=1 &&
        (g->stencil_fail!=0x1E00 || g->stencil_zfail!=0x1E00 || g->stencil_zpass!=0x1E00)))
        depth->write_serial++;
    target->dirty=1;
    return !s.failed;
}

int nv2a_gpu_draw(const Nv2aGpuState *g,const Nv2aVshOutput *vertices,uint32_t count,
                  uint32_t primitive,Nv2aGpuTexel decode)
{
    if(!nv2a_gpu_enabled())return 0;
    if(feedback_bounds_begin(g,s.feedback_bounds))
        for(unsigned i=0;i<count;i++)feedback_bounds_add(g,s.feedback_bounds,&vertices[i]);
    return draw_batch(g,vertices,sizeof(*vertices),count,NULL,count,primitive,decode,s.vs,s.layout);
}

static void index_buffer(unsigned slot)
{
    unsigned bucket=(unsigned)s.buffers[slot].hash & (GPU_BUFFER_INDEX-1);
    while(s.buffer_index[bucket])bucket=(bucket+1)&(GPU_BUFFER_INDEX-1);
    s.buffer_index[bucket]=(uint16_t)(slot+1);
}

static ID3D11ShaderResourceView *vertex_buffer(uint32_t va,uint32_t bytes)
{
    uint32_t key[2]={va,bytes};uint64_t hash=shader_hash(key,sizeof(key));
    unsigned bucket=(unsigned)hash & (GPU_BUFFER_INDEX-1);GpuBuffer *v=NULL;
    while(s.buffer_index[bucket]) {
        GpuBuffer *b=&s.buffers[s.buffer_index[bucket]-1];
        if(b->hash==hash && b->va==va && b->bytes==bytes){v=b;break;}
        bucket=(bucket+1)&(GPU_BUFFER_INDEX-1);
    }
    uint64_t serial=nv2a_gpu_memory_serial();
    if(v && v->memory_version && (v->checked_serial==serial ||
        v->memory_version==nv2a_gpu_memory_version(va,bytes))) {
        v->checked_serial=serial;v->used=++s.serial;return v->srv;
    }
    uint64_t watch=nv2a_gpu_memory_watch(va,bytes);
    const uint8_t *raw=(const uint8_t *)xbox_GetMemoryOffset()+va;
    if(v && !memcmp(raw,v->snapshot,bytes)) {
        v->memory_version=watch;v->checked_serial=serial;v->used=++s.serial;return v->srv;
    }
    double fetch_start=nv2a_gpu_cpu_begin();
    int update=v!=NULL;
    if(!v) {
        unsigned oldest=0;
        for(unsigned i=1;i<GPU_BUFFERS;i++)if(s.buffers[i].used<s.buffers[oldest].used)oldest=i;
        v=&s.buffers[oldest];
        if(v->buffer) {
            nv2a_gpu_cpu_count(NV2A_CPU_FETCH_EVICT,1);
            memset(s.buffer_index,0,sizeof(s.buffer_index));
            for(unsigned i=0;i<GPU_BUFFERS;i++)if(i!=oldest && s.buffers[i].buffer)index_buffer(i);
        }
        RELEASE(v->srv);RELEASE(v->buffer);free(v->snapshot);
        v->snapshot=malloc(bytes);
        if(!v->snapshot){gpu_failure("Allocate guest vertex snapshot",E_OUTOFMEMORY);goto allocation_failure;}
    }
    v->va=va;v->bytes=bytes;v->hash=hash;v->memory_version=watch;v->checked_serial=serial;v->used=++s.serial;
    memcpy(v->snapshot,raw,bytes);
    if(update) {
        nv2a_gpu_cpu_count(NV2A_CPU_FETCH_UPDATE,1);
        ID3D11DeviceContext_UpdateSubresource(s.context,(ID3D11Resource *)v->buffer,0,NULL,raw,0,0);
        nv2a_gpu_cpu_end(NV2A_CPU_FETCH_RESOURCE,fetch_start,bytes);
        return v->srv;
    }
    D3D11_BUFFER_DESC d={0};d.ByteWidth=bytes;d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SUBRESOURCE_DATA data={raw,0,0};
    HRESULT hr=ID3D11Device_CreateBuffer(s.device,&d,&data,&v->buffer);
    if(FAILED(hr)){gpu_failure("CreateBuffer guest vertex data",hr);goto allocation_failure;}
    D3D11_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;view.BufferEx.NumElements=bytes/4;
    view.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr=ID3D11Device_CreateShaderResourceView(s.device,(ID3D11Resource *)v->buffer,&view,&v->srv);
    if(FAILED(hr)){gpu_failure("CreateShaderResourceView guest vertex data",hr);goto allocation_failure;}
    index_buffer((unsigned)(v-s.buffers));
    nv2a_gpu_cpu_end(NV2A_CPU_FETCH_RESOURCE,fetch_start,bytes);return v->srv;
allocation_failure:
    nv2a_gpu_cpu_end(NV2A_CPU_FETCH_RESOURCE,fetch_start,bytes);
    RELEASE(v->srv);RELEASE(v->buffer);free(v->snapshot);memset(v,0,sizeof(*v));
    return NULL;
}

static int prepare_vertex_fetch(const Nv2aGpuVertexAttr attrs[16],const float defaults[16][4],
                               const uint32_t *indices,uint32_t count,uint32_t inputs)
{
    uint32_t maximum=0;
    for(uint32_t i=0;i<count;i++)if(indices[i]>maximum)maximum=indices[i];
    GpuFetchConstants c={0};memcpy(c.defaults,defaults,sizeof(c.defaults));
    ID3D11ShaderResourceView *views[16]={0};
    for(unsigned i=0;i<16;i++)if(inputs&(1u<<i)) {
        const Nv2aGpuVertexAttr *a=&attrs[i];
        if(!a->size)continue;
        if(!a->offset || a->size>4 || a->type>6 || a->type==3)return 0;
        uint32_t value_bytes=nv2a_vertex_value_bytes(a->type,a->size);
        uint32_t base=a->offset&~4095u;
        uint64_t end=(uint64_t)a->offset+(uint64_t)maximum*a->stride+value_bytes;
        if(end>0x100000000ULL || end-base>64*1024*1024)return 0;
        uint32_t bytes=(uint32_t)((end-base+4095)&~4095ULL);
        views[i]=vertex_buffer(base,bytes);
        if(!views[i])return 0;
        c.streams[i][0]=a->offset-base;c.streams[i][1]=a->stride;
        c.streams[i][2]=a->type;c.streams[i][3]=a->size;
    }
    if(!s.fetch_valid || memcmp(&c,&s.last_fetch_constants,sizeof(c))) {
        double constant_start=nv2a_gpu_cpu_begin();
        if(!s.constant_ranges || s.fetch_cursor+sizeof(c)>65536) {if(s.fetch_cursor)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_WRAP,1);s.fetch_cursor=0;}
        s.fetch_first=s.fetch_cursor/16;
        if(!s.fetch_cursor)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_DISCARD,1);
        D3D11_MAPPED_SUBRESOURCE map;
        UploadTrace constant_trace={0};double trace_start=upload_trace_begin();
        HRESULT hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.fetch_constants,0,
            s.fetch_cursor?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&map);
        upload_trace_capture(&constant_trace,trace_start,"fetch_constants",sizeof(c),s.fetch_cursor?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,hr);
        if(FAILED(hr)) {nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,constant_start,0);upload_trace_emit(&constant_trace);return gpu_failure("Map vertex fetch constants",hr);}
        memcpy((uint8_t *)map.pData+s.fetch_cursor,&c,sizeof(c));
        ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.fetch_constants,0);
        nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,constant_start,sizeof(c));
        upload_trace_emit(&constant_trace);
        s.fetch_cursor+=sizeof(c);s.last_fetch_constants=c;s.fetch_valid=1;
    }
    if(!s.fetch_bound || s.bound_fetch_first!=s.fetch_first) {
        if(s.constant_ranges) {
            UINT num=sizeof(c)/16;
            ID3D11DeviceContext1_VSSetConstantBuffers1(s.context1,2,1,&s.fetch_constants,&s.fetch_first,&num);
        } else ID3D11DeviceContext_VSSetConstantBuffers(s.context,2,1,&s.fetch_constants);
        s.fetch_bound=1;s.bound_fetch_first=s.fetch_first;
    }
    if(memcmp(views,s.bound_vertex_views,sizeof(views))) {
        ID3D11DeviceContext_VSSetShaderResources(s.context,0,16,views);
        memcpy(s.bound_vertex_views,views,sizeof(views));
    }
    return 1;
}

static int program_draw(const Nv2aGpuState *,const Nv2aGpuRawVertex *,uint32_t,const uint32_t *,
                        uint32_t,uint32_t,Nv2aGpuTexel,const Nv2aVshProgram *,const float [NV2A_VSH_CONSTANTS][4],
                        uint32_t,uint32_t,const float [2],const Nv2aGpuVertexAttr *,const float [16][4]);

int nv2a_gpu_draw_program(const Nv2aGpuState *g,const Nv2aGpuRawVertex *vertices,uint32_t count,
                          uint32_t primitive,Nv2aGpuTexel decode,const Nv2aVshProgram *program,
                          const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable,uint32_t fog_mode,const float fog_params[2])
{
    return nv2a_gpu_draw_program_indexed(g,vertices,count,NULL,count,primitive,decode,program,
                                       constants,fog_enable,fog_mode,fog_params);
}

int nv2a_gpu_draw_program_indexed(const Nv2aGpuState *g,const Nv2aGpuRawVertex *vertices,uint32_t vertex_count,
                          const uint32_t *indices,uint32_t count,uint32_t primitive,Nv2aGpuTexel decode,
                          const Nv2aVshProgram *program,const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable,uint32_t fog_mode,const float fog_params[2])
{
    return program_draw(g,vertices,vertex_count,indices,count,primitive,decode,program,constants,
                        fog_enable,fog_mode,fog_params,NULL,NULL);
}

int nv2a_gpu_draw_buffers(const Nv2aGpuState *g,const Nv2aGpuVertexAttr attrs[16],const float defaults[16][4],
                          const uint32_t *indices,uint32_t count,uint32_t primitive,Nv2aGpuTexel decode,
                          const Nv2aVshProgram *program,const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable,uint32_t fog_mode,const float fog_params[2])
{
    return program_draw(g,NULL,0,indices,count,primitive,decode,program,constants,
                        fog_enable,fog_mode,fog_params,attrs,defaults);
}

/* Optional hardware/reference comparison. Stream output captures the actual
 * indexed vertex invocation, including GPU buffer fetch and constants. It is
 * disabled for normal play and never changes the framebuffer. */
static void verify_vertex_program(unsigned slot,const Nv2aGpuState *g,
                                 const Nv2aGpuRawVertex *vertices,
                                 const Nv2aGpuVertexAttr *pull,const float defaults[16][4])
{
    static uint64_t started;
    static unsigned comparisons,mismatches;
    if(!started)started=GetTickCount64();
    if(!s.programs[slot].verify_shader || s.programs[slot].verified>=32 || comparisons>=8192)return;
    if(GetTickCount64()-started<(uint64_t)gpu_option("RECOMP_VSH_VERIFY_AFTER",0)*1000)return;
    s.programs[slot].verified++;comparisons++;
    if(!s.verify_output) {
        D3D11_BUFFER_DESC desc={0};desc.ByteWidth=3*sizeof(Nv2aVshOutput);
        desc.BindFlags=D3D11_BIND_STREAM_OUTPUT;
        if(FAILED(ID3D11Device_CreateBuffer(s.device,&desc,NULL,&s.verify_output)))return;
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(ID3D11Device_CreateBuffer(s.device,&desc,NULL,&s.verify_readback)))return;
    }
    if(!s.verify_readback)return;
    UINT offset=0;
    ID3D11DeviceContext_GSSetShader(s.context,s.programs[slot].verify_shader,NULL,0);
    ID3D11DeviceContext_SOSetTargets(s.context,1,&s.verify_output,&offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(s.context,D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_DrawIndexed(s.context,3,s.verify_index_offset,0);
    ID3D11Buffer *none=NULL;
    ID3D11DeviceContext_SOSetTargets(s.context,1,&none,&offset);
    ID3D11DeviceContext_GSSetShader(s.context,NULL,NULL,0);
    ID3D11DeviceContext_IASetPrimitiveTopology(s.context,s.verify_topology);
    ID3D11DeviceContext_CopyResource(s.context,(ID3D11Resource *)s.verify_readback,(ID3D11Resource *)s.verify_output);
    D3D11_MAPPED_SUBRESOURCE map;
    if(FAILED(ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.verify_readback,0,D3D11_MAP_READ,0,&map)))return;
    const Nv2aVshOutput *actual=map.pData;
    Nv2aGpuRawVertex captured[3];Nv2aVshOutput reference[3];int finite_mismatch=0;
    for(unsigned vi=0;vi<3;vi++) {
        float attrs[16][4];Nv2aVshOutput expected;
        UINT id=s.verify_ids[vi];
        if(vertices)memcpy(attrs,vertices[id].attributes,sizeof(attrs));
        else for(unsigned a=0;a<16;a++) {
            memcpy(attrs[a],defaults[a],16);
            const Nv2aGpuVertexAttr *at=&pull[a];
            if(!at->size || !(s.programs[slot].inputs&(1u<<a)))continue;
            const uint8_t *p=(const uint8_t *)((uintptr_t)xbox_GetMemoryOffset()+at->offset+(uint64_t)id*at->stride);
            float *v=attrs[a];v[0]=v[1]=v[2]=0;v[3]=1;
            uint32_t word;
            if(at->type==0) {
                memcpy(&word,p,4);v[0]=((word>>16)&255)/255.0f;v[1]=((word>>8)&255)/255.0f;
                v[2]=(word&255)/255.0f;v[3]=(word>>24)/255.0f;
            } else if(at->type==6) {
                memcpy(&word,p,4);v[0]=((int32_t)(word<<21)>>21)/1023.0f;
                v[1]=((int32_t)(word<<10)>>21)/1023.0f;v[2]=((int32_t)word>>22)/511.0f;
            } else for(unsigned j=0;j<at->size;j++) {
                if(at->type==2)memcpy(&v[j],p+j*4,4);
                else if(at->type==4)v[j]=p[j]/255.0f;
                else {int16_t n;memcpy(&n,p+j*2,2);v[j]=at->type==1?n/32767.0f:(float)n;}
            }
        }
        memcpy(captured[vi].attributes,attrs,sizeof(attrs));
        if(!nv2a_vsh_run(attrs,&expected)){memset(&reference[vi],0,sizeof(reference[vi]));continue;}
        reference[vi]=expected;
        float w=copysignf(fminf(1.8446744073709552e19f,fmaxf(5.421010862427522e-20f,fabsf(expected.pos[3]))),expected.pos[3]);
        expected.pos[0]=(truncf(expected.pos[0]*16)/16/s.last_constants.viewport[0]*2-1)*w;
        expected.pos[1]=(1-truncf(expected.pos[1]*16)/16/s.last_constants.viewport[1]*2)*w;
        expected.pos[2]=expected.pos[2]/s.last_constants.viewport[2]*w;
        expected.pos[3]=w;
        for(unsigned j=0;j<4;j++) {
            expected.d0[j]=isnan(expected.d0[j])?1:fminf(1,fmaxf(0,expected.d0[j]));
            expected.d1[j]=isnan(expected.d1[j])?1:fminf(1,fmaxf(0,expected.d1[j]));
        }
        const float *cpu=(const float *)&expected,*gpu=(const float *)&actual[vi];
        for(unsigned lane=0;lane<32;lane++) {
            if(lane>=12 && lane<16)continue; /* Fog has a separate fixed-function transform. */
            if(cpu[lane]==gpu[lane] || (isnan(cpu[lane])&&isnan(gpu[lane])))continue;
            if(isfinite(cpu[lane])&&isfinite(gpu[lane]) &&
               fabsf(cpu[lane]-gpu[lane])<=.001f*fmaxf(1,fmaxf(fabsf(cpu[lane]),fabsf(gpu[lane]))))continue;
            if(isfinite(cpu[lane])&&isfinite(gpu[lane]))finite_mismatch=1;
            if(mismatches++<128)fprintf(stderr,"[VSH-VERIFY] program=%u sample=%u vertex=%u lane=%u CPU=%.9g GPU=%.9g blend=%u depth=%u\n",
                slot+1,s.programs[slot].verified,id,lane,cpu[lane],gpu[lane],g->blend_enable,g->depth_test);
        }
    }
    static int dumped;
    const char *dump_path=getenv("RECOMP_VSH_VERIFY_DUMP");
    if(finite_mismatch && !dumped && dump_path) {
        FILE *file=fopen(dump_path,"wb");
        if(file) {
            fwrite(&s.programs[slot].key,sizeof(Nv2aVshProgram),1,file);
            fwrite(s.last_vertex_constants.values,sizeof(s.last_vertex_constants.values),1,file);
            fwrite(captured,sizeof(captured),1,file);fwrite(reference,sizeof(reference),1,file);
            fwrite(actual,3*sizeof(*actual),1,file);fwrite(s.last_constants.viewport,16,1,file);
            fclose(file);dumped=1;
            {char shown[MAX_PATH*2];fprintf(stderr,"[VSH-VERIFY] saved program %u reproduction to %s\n",slot+1,xbox_log_path(dump_path,shown,sizeof shown));}
        }
    }
    ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.verify_readback,0);
    if(comparisons==1 || !(comparisons&255))fprintf(stderr,"[VSH-VERIFY] comparisons=%u mismatched-components=%u\n",comparisons,mismatches);
}

/* Gather one vertex's program inputs from raw vertices or guest attribute
 * streams and evaluate it on the CPU. Returns 0 when it cannot be read. */
static int program_vertex_output(uint32_t id,const Nv2aGpuRawVertex *vertices,uint32_t vertex_count,
                          const Nv2aVshProgram *program,const float constants[NV2A_VSH_CONSTANTS][4],
                          const Nv2aGpuVertexAttr *pull,const float defaults[16][4],uint32_t inputs,
                          Nv2aVshOutput *output)
{
    float attrs[16][4]={{0}};
    if(vertices) {
        if(id>=vertex_count)return 0;
        memcpy(attrs,vertices[id].attributes,sizeof(attrs));
    } else for(unsigned a=0;a<16;a++)if(inputs&(1u<<a)) {
        const Nv2aGpuVertexAttr *at=&pull[a];
        memcpy(attrs[a],defaults[a],16);
        if(!at->size)continue;
        uint64_t address=(uint64_t)at->offset+(uint64_t)id*at->stride;
        uint64_t end=address+nv2a_vertex_value_bytes(at->type,at->size);
        if(!((address<0x04000000 && end<=0x04000000) ||
             (address>=0x80000000 && end<=0x84000000)))return 0;
        const uint8_t *p=(const uint8_t *)((uintptr_t)xbox_GetMemoryOffset()+address);
        float *v=attrs[a];v[0]=v[1]=v[2]=0;v[3]=1;
        uint32_t word;
        if(at->type==0) {
            memcpy(&word,p,4);v[0]=((word>>16)&255)/255.0f;v[1]=((word>>8)&255)/255.0f;
            v[2]=(word&255)/255.0f;v[3]=(word>>24)/255.0f;
        } else if(at->type==6) {
            memcpy(&word,p,4);v[0]=((int32_t)(word<<21)>>21)/1023.0f;
            v[1]=((int32_t)(word<<10)>>21)/1023.0f;v[2]=((int32_t)word>>22)/511.0f;
        } else for(unsigned j=0;j<at->size;j++) {
            if(at->type==2)memcpy(&v[j],p+j*4,4);
            else if(at->type==4)v[j]=p[j]/255.0f;
            else {int16_t n;memcpy(&n,p+j*2,2);v[j]=at->type==1?n/32767.0f:(float)n;}
        }
    }
    return nv2a_vsh_evaluate(program,constants,(const float (*)[4])attrs,output);
}

/* Screen-space sprites (HUD panels, glyphs, icons) map one texel to one
 * guest pixel, so at 640x480 a WRAP sampler never reaches past the image
 * edge. Upscaled, the host pixels between a sprite's edge and its first
 * texel center blend in the opposite edge of the texture, which shows as a
 * faint line along HUD frames. Small draws whose coordinates all stay inside
 * the image sample with CLAMP instead; at native resolution that is identical. */
static unsigned program_sprite_clamp(const Nv2aGpuState *g,const Nv2aGpuRawVertex *vertices,
                          uint32_t vertex_count,const uint32_t *indices,uint32_t count,
                          const Nv2aVshProgram *program,const float constants[NV2A_VSH_CONSTANTS][4],
                          const Nv2aGpuVertexAttr *pull,const float defaults[16][4],uint32_t inputs)
{
    if(!count || count>6)return 0;
    unsigned mask=0,sampled=sampled_texture_mask(g);
    for(unsigned i=0;i<4;i++) {
        const Nv2aGpuTexture *t=&g->textures[i];
        if((sampled&(1u<<i)) && t->valid && t->width && t->height && t->dimensionality!=3 && !t->cube &&
           (t->addr_u==1 || t->addr_v==1))mask|=1u<<i;
    }
    for(uint32_t n=0;n<count && mask;n++) {
        Nv2aVshOutput output;
        if(!program_vertex_output(indices?indices[n]:n,vertices,vertex_count,program,constants,pull,defaults,inputs,&output))return 0;
        for(unsigned i=0;i<4;i++)if(mask&(1u<<i)) {
            const Nv2aGpuTexture *t=&g->textures[i];const float *c=output.tex[i];
            int normalized=d3d8_format_is_swizzled(t->color)||d3d8_format_dxt_block_bytes(t->color);
            float q=c[3]?c[3]:1,u=c[0]/q,v=c[1]/q;
            float su=normalized?1.0f:(float)t->width,sv=normalized?1.0f:(float)t->height;
            if(!isfinite(u) || !isfinite(v) || u<-su*1e-4f || v<-sv*1e-4f || u>su*(1+1e-4f) || v>sv*(1+1e-4f))
                mask&=~(1u<<i);
        }
    }
    return mask;
}

static void program_feedback_bounds(const Nv2aGpuState *g,const Nv2aGpuRawVertex *vertices,
                          uint32_t vertex_count,const uint32_t *indices,uint32_t count,
                          const Nv2aVshProgram *program,const float constants[NV2A_VSH_CONSTANTS][4],
                          const Nv2aGpuVertexAttr *pull,const float defaults[16][4],uint32_t inputs)
{
    unsigned mask=feedback_bounds_begin(g,s.feedback_bounds);
    if(!mask)return;
    uint32_t unique[64],unique_count=0;
    if(!count || count>192)goto unbounded;
    for(uint32_t i=0;i<count;i++) {
        uint32_t id=indices?indices[i]:i;
        unsigned found=0;
        for(unsigned j=0;j<unique_count;j++)if(unique[j]==id){found=1;break;}
        if(found)continue;
        if(unique_count==64)goto unbounded;
        unique[unique_count++]=id;
        Nv2aVshOutput output;
        if(!program_vertex_output(id,vertices,vertex_count,program,constants,pull,defaults,inputs,&output))goto unbounded;
        feedback_bounds_add(g,s.feedback_bounds,&output);
    }
    return;
unbounded:
    memset(s.feedback_bounds,0,sizeof(s.feedback_bounds));
}

static int program_draw(const Nv2aGpuState *g,const Nv2aGpuRawVertex *vertices,uint32_t vertex_count,
                          const uint32_t *indices,uint32_t count,uint32_t primitive,Nv2aGpuTexel decode,
                          const Nv2aVshProgram *program,const float constants[NV2A_VSH_CONSTANTS][4],
                          uint32_t fog_enable,uint32_t fog_mode,const float fog_params[2],
                          const Nv2aGpuVertexAttr *pull,const float defaults[16][4])
{
    s.program_pending=0;
    if(!nv2a_gpu_enabled())return 0;
    ID3D11VertexShader *shader=NULL;
    unsigned program_slot=0;
    size_t key_bytes=offsetof(Nv2aVshProgram,words)+program->count*16;
    if(!program->count || program->count>NV2A_VSH_SLOTS)return 0;
    int pulling=pull!=NULL;uint32_t inputs=0;uint8_t formats[16]={0};
    uint64_t hash=0;
    /* Geometry often splits one material into many index batches. Reuse its
     * input analysis and shader after checking the actual instruction bytes;
     * neither a reused program pointer nor its hash alone is sufficient. */
    if(s.last_program_valid &&
       s.programs[s.last_program_slot].pulling==pulling &&
       !memcmp(program,&s.programs[s.last_program_slot].key,key_bytes)) {
        program_slot=s.last_program_slot;
        inputs=s.programs[program_slot].inputs;
        shader=s.programs[program_slot].shader;
    }
    if(!shader)for(unsigned i=0;i<program->count;i++) {
        const uint32_t *w=program->words[i];
        if(((w[2]>>26)&3)==2 || ((w[2]>>11)&3)==2 || ((w[3]>>28)&3)==2)
            inputs|=1u<<((w[1]>>9)&15);
    }
    if(pulling)for(unsigned i=0;i<16;i++)if(inputs&(1u<<i)) {
        if(pull[i].size>4 || pull[i].type>6 || (pull[i].size && pull[i].type==3))return 0;
        /* A byte per attribute makes format conversion static shader code.
         * Unused inputs do not create redundant variants. */
        formats[i]=(uint8_t)(pull[i].size?pull[i].type|(pull[i].size<<3):0x40);
    }
    if(shader && memcmp(s.programs[program_slot].formats,formats,sizeof(formats)))shader=NULL;
    if(!shader) {
        hash=shader_hash(program,key_bytes)^(pulling?0xF04EA95256454354ULL:0);
        if(pulling)hash^=shader_hash(formats,sizeof(formats));
        unsigned bucket=(unsigned)hash & (GPU_PROGRAM_INDEX-1);
        while(s.program_index[bucket]) {
            unsigned i=s.program_index[bucket]-1;
            if(s.programs[i].hash==hash && s.programs[i].pulling==pulling && !memcmp(s.programs[i].formats,formats,sizeof(formats)) && !memcmp(program,&s.programs[i].key,key_bytes)) {
                shader=s.programs[i].shader;program_slot=i;inputs=s.programs[i].inputs;break;
            }
            bucket=(bucket+1)&(GPU_PROGRAM_INDEX-1);
        }
    }
    if(!shader) {
        double miss_start=nv2a_gpu_cpu_begin();
        Nv2aShaderJob *job=nv2a_shader_find(hash,pulling?3:2);
        ID3DBlob *code=NULL,*errors=NULL;ID3D11InputLayout *layout=NULL;Nv2aShaderKey disk={0};
        HRESULT hr=job?nv2a_shader_poll(job,&code,&errors):S_OK;
        if(job)nv2a_shader_job_key(job,&disk);
        if(hr==NV2A_SHADER_PENDING){s.program_pending=1;nv2a_gpu_cpu_count(NV2A_CPU_PROGRAM_PENDING,1);nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);return 0;}
        if(!job) {
        char *body=nv2a_gpu_vsh_source(program);
        if(!body){nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);return 0;}
        char wrapper[1536]={0};
        if(pulling) {
            int pos=snprintf(wrapper,sizeof(wrapper),"Pixel pull_vertex_main(uint id:SV_VertexID){RawVertex v;\n");
            for(unsigned i=0;i<16;i++) {
                if(inputs&(1u<<i))pos+=snprintf(wrapper+pos,sizeof(wrapper)-pos,
                    "v.a%u=vertex_attribute(%u,id,%u,%u);\n",i,i,pull[i].type,pull[i].size);
                else pos+=snprintf(wrapper+pos,sizeof(wrapper)-pos,"v.a%u=0;\n",i);
            }
            snprintf(wrapper+pos,sizeof(wrapper)-pos,"return program_vertex_main(v);}\n");
        }
        size_t length=strlen(nv2a_gpu_shader_source)+strlen(body)+strlen(wrapper)+1;
        char *source=malloc(length);
        if(!source){free(body);nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);return 0;}
        strcpy(source,nv2a_gpu_shader_source);strcat(source,body);strcat(source,wrapper);free(body);
        nv2a_shader_key(source,length-1,pulling?"pull_vertex_main":"program_vertex_main","vs_5_0",&disk);
        /* Created during loading (shader preloading); its layout must match the attribute mode. */
        if(!gpu_option("RECOMP_VSH_VERIFY",0))shader=nv2a_shader_take_vertex(&disk,&layout);
        if(shader && !pulling!=!!layout){RELEASE(shader);RELEASE(layout);}
        if(!shader)hr=nv2a_shader_compile(source,length-1,pulling?"pull_vertex_main":"program_vertex_main",
                              "vs_5_0",hash,pulling?3:2,&code,&errors);
        free(source);
        }
        if(hr==NV2A_SHADER_PENDING){s.program_pending=1;nv2a_gpu_cpu_count(NV2A_CPU_PROGRAM_PENDING,1);nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);return 0;}
        if(FAILED(hr)) {
            if(errors)fprintf(stderr,"[GPU-D3D11] %s\n",(char *)ID3D10Blob_GetBufferPointer(errors));
            RELEASE(errors);nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);return gpu_failure("guest vertex program compile",hr);
        }
        RELEASE(errors);
        double create_start=nv2a_gpu_cpu_begin();
        if(!shader) {
        hr=ID3D11Device_CreateVertexShader(s.device,ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),NULL,&shader);
        if(FAILED(hr)){nv2a_gpu_cpu_end(NV2A_CPU_SHADER_RESOURCE,create_start,0);nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);RELEASE(code);return gpu_failure("CreateVertexShader guest",hr);}
        if(!pulling) {
            D3D11_INPUT_ELEMENT_DESC elements[16]={0};
            for(UINT i=0;i<16;i++) {
                elements[i].SemanticName="ATTR";elements[i].SemanticIndex=i;
                elements[i].Format=DXGI_FORMAT_R32G32B32A32_FLOAT;elements[i].AlignedByteOffset=i*16;
            }
            hr=ID3D11Device_CreateInputLayout(s.device,elements,16,ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code),&layout);
            if(FAILED(hr)){nv2a_gpu_cpu_end(NV2A_CPU_SHADER_RESOURCE,create_start,0);nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);RELEASE(code);RELEASE(shader);return gpu_failure("CreateInputLayout raw",hr);}
        }
        }
        ID3D11GeometryShader *verify_shader=NULL;
        if(code && gpu_option("RECOMP_VSH_VERIFY",0)) {
            const D3D11_SO_DECLARATION_ENTRY entries[]={
                {0,"SV_POSITION",0,0,4,0},{0,"COLOR",0,0,4,0},{0,"COLOR",1,0,4,0},
                {0,"FOG",0,0,1,0},{0,NULL,0,0,3,0},
                {0,"TEXCOORD",0,0,4,0},{0,"TEXCOORD",1,0,4,0},
                {0,"TEXCOORD",2,0,4,0},{0,"TEXCOORD",3,0,4,0}};
            UINT stride=sizeof(Nv2aVshOutput);
            hr=ID3D11Device_CreateGeometryShaderWithStreamOutput(s.device,
                ID3D10Blob_GetBufferPointer(code),ID3D10Blob_GetBufferSize(code),entries,9,
                &stride,1,D3D11_SO_NO_RASTERIZED_STREAM,NULL,&verify_shader);
            if(FAILED(hr))fprintf(stderr,"[VSH-VERIFY] stream output unavailable: %08lx\n",(unsigned long)hr);
        }
        nv2a_gpu_cpu_end(NV2A_CPU_SHADER_RESOURCE,create_start,0);
        RELEASE(code);
        unsigned slot;
        if(s.program_count<GPU_PROGRAMS)slot=s.program_count++;
        else {
            slot=0;
            nv2a_gpu_cpu_count(NV2A_CPU_PROGRAM_EVICT,1);
            for(unsigned i=1;i<GPU_PROGRAMS;i++)if(s.programs[i].used<s.programs[slot].used)slot=i;
            nv2a_shader_resident(&s.programs[slot].disk,-1);
            memset(s.program_index,0,sizeof(s.program_index));
            for(unsigned i=0;i<s.program_count;i++)if(i!=slot)index_program(i);
        }
        RELEASE(s.programs[slot].shader);RELEASE(s.programs[slot].layout);RELEASE(s.programs[slot].verify_shader);
        s.programs[slot].key=*program;s.programs[slot].shader=shader;
        s.programs[slot].layout=layout;
        s.programs[slot].verify_shader=verify_shader;s.programs[slot].verified=0;program_slot=slot;
        s.programs[slot].inputs=inputs;s.programs[slot].pulling=pulling;memcpy(s.programs[slot].formats,formats,sizeof(formats));
        s.programs[slot].hash=hash;s.programs[slot].used=++s.serial;index_program(slot);
        s.programs[slot].disk=disk;s.programs[slot].epoch=0;nv2a_shader_resident(&disk,1);
        nv2a_gpu_cpu_end(NV2A_CPU_PROGRAM_MISS,miss_start,0);
        fprintf(stderr,"[GPU-D3D11] loaded vertex program %u (%u instructions, %s)\n",slot+1,program->count,pulling?"native buffers":"expanded attributes");
    }
    s.programs[program_slot].used=++s.serial;
    if(s.programs[program_slot].epoch!=nv2a_shader_level_epoch) {
        s.programs[program_slot].epoch=nv2a_shader_level_epoch;nv2a_shader_note_level(&s.programs[program_slot].disk);
    }
    s.last_program_slot=program_slot;s.last_program_valid=1;
    /* T2-4 E3(a), [Render] DrawFast bit 0: compare the guest constants in place instead of
     * zeroing and filling a 3 KB GpuVertexConstants per draw first. The tail (fog, flags) is
     * built as the struct holds it, so the compare and the uploaded bytes are the same. */
    int fast_constants=draw_fast()&1;
    struct {float fog[4];uint32_t flags[4];} tail={{fog_params[0],fog_params[1],0,0},{fog_enable,fog_mode,0,0}};
    GpuVertexConstants vc;
    int constants_changed;
    if(fast_constants) {
        constants_changed=!s.vertex_constants_valid ||
            memcmp(constants,s.last_vertex_constants.values,sizeof(vc.values)) ||
            memcmp(&tail,s.last_vertex_constants.fog,sizeof(tail));
        if(draw_fast_verify_due()) {
            memset(&vc,0,sizeof(vc));memcpy(vc.values,constants,sizeof(vc.values));
            vc.fog[0]=fog_params[0];vc.fog[1]=fog_params[1];vc.flags[0]=fog_enable;vc.flags[1]=fog_mode;
            draw_fast_verify_result(constants_changed==(!s.vertex_constants_valid || memcmp(&vc,&s.last_vertex_constants,sizeof(vc))!=0));
        }
        if(constants_changed) {
            memcpy(vc.values,constants,sizeof(vc.values));memcpy(vc.fog,&tail,sizeof(tail));
        }
    } else {
        memset(&vc,0,sizeof(vc));
        memcpy(vc.values,constants,sizeof(vc.values));
        vc.fog[0]=fog_params[0];vc.fog[1]=fog_params[1];vc.flags[0]=fog_enable;vc.flags[1]=fog_mode;
        constants_changed=!s.vertex_constants_valid || memcmp(&vc,&s.last_vertex_constants,sizeof(vc));
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if(constants_changed) {
        UINT bytes=(sizeof(vc)+255)&~255u;
        double constant_start=nv2a_gpu_cpu_begin();
        if(!s.constant_ranges || s.vertex_constant_cursor+bytes>65536) {if(s.vertex_constant_cursor)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_WRAP,1);s.vertex_constant_cursor=0;}
        s.vertex_constant_first=s.vertex_constant_cursor/16;
        if(!s.vertex_constant_cursor)nv2a_gpu_cpu_count(NV2A_CPU_UPLOAD_DISCARD,1);
        UploadTrace constant_trace={0};double trace_start=upload_trace_begin();
        HRESULT hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.vertex_constants,0,
            s.vertex_constant_cursor?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&map);
        upload_trace_capture(&constant_trace,trace_start,"vertex_constants",sizeof(vc),s.vertex_constant_cursor?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,hr);
        if(FAILED(hr)) {nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,constant_start,0);upload_trace_emit(&constant_trace);return gpu_failure("Map vertex constants",hr);}
        memcpy((uint8_t *)map.pData+s.vertex_constant_cursor,&vc,sizeof(vc));
        ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.vertex_constants,0);
        nv2a_gpu_cpu_end(NV2A_CPU_BUFFER_UPLOAD,constant_start,sizeof(vc));
        upload_trace_emit(&constant_trace);
        s.vertex_constant_cursor+=bytes;s.last_vertex_constants=vc;s.vertex_constants_valid=1;
    }
    if(!s.vertex_bound || s.bound_vertex_first!=s.vertex_constant_first) {
        if(s.constant_ranges) {
            UINT num=((sizeof(vc)+255)&~255u)/16;
            ID3D11DeviceContext1_VSSetConstantBuffers1(s.context1,1,1,&s.vertex_constants,&s.vertex_constant_first,&num);
        } else ID3D11DeviceContext_VSSetConstantBuffers(s.context,1,1,&s.vertex_constants);
        s.vertex_bound=1;s.bound_vertex_first=s.vertex_constant_first;
    }
    if(pulling && !prepare_vertex_fetch(pull,defaults,indices,count,inputs))return 0;
    program_feedback_bounds(g,vertices,vertex_count,indices,count,program,constants,pull,defaults,inputs);
    s.sprite_clamp=program_sprite_clamp(g,vertices,vertex_count,indices,count,program,constants,pull,defaults,inputs);
    uint64_t before=s.draw_serial,cpu_before=s_cpu_enabled>0?s_cpu.interval.draw_serial:0;
    int result=draw_batch(g,vertices,sizeof(*vertices),count,indices,vertex_count,primitive,decode,shader,s.programs[program_slot].layout);
    s.sprite_clamp=0;
    if(result && s.draw_serial!=before)cpu_program_completed(cpu_before,s.programs[program_slot].hash);
    if(result && count>=3 && s.draw_serial!=before)
        verify_vertex_program(program_slot,g,vertices,pull,defaults);
    return result;
}

int nv2a_gpu_program_pending(void)
{
    return s.program_pending;
}

int nv2a_gpu_clear(const Nv2aGpuState *g,uint32_t mask,uint32_t color,float depth)
{
    if(!nv2a_gpu_enabled())return 0;
    GpuTarget *target=target_for(g);
    if(!target)return 0;
    ID3D11ShaderResourceView *none[8]={0};
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    memset(s.bound_views,0,sizeof(s.bound_views));
    int whole=g->clip_x==0 && g->clip_y==0 && g->clip_w==target->width && g->clip_h==target->height;
    /* ClearView obeys a rectangle but overwrites every color channel.
     * A masked clear, or a partial depth/stencil clear, is a small native
     * draw with precisely the requested write masks. No RAM round trip. */
    if(((mask&0xF0) && (mask&0xF0)!=0xF0) || (!whole && (mask&3)) || (!whole && !s.context1)) {
        Nv2aGpuState clear=*g;
        memset(clear.textures,0,sizeof(clear.textures));
        clear.use_rc=clear.textured=clear.legacy=clear.blend_enable=clear.alpha_test=0;
        clear.cull_enable=clear.polygon_offset_enable=0;
        clear.color_mask=((mask&0x10)?0x10000:0)|((mask&0x20)?0x100:0)|
            ((mask&0x40)?1:0)|((mask&0x80)?0x1000000:0);
        clear.depth_test=clear.depth_mask=(mask&1)!=0;clear.depth_func=0x207;
        clear.clip_min=0;clear.clip_max=((g->format>>4)&15)==1?65535.0f:16777215.0f;
        /* A clear writes one exact value, without vertex depth division or
         * interpolation rounding. Equal clamp bounds supply that constant
         * directly to SV_Depth using the same CPU normalization as a full clear. */
        if(mask&1)clear.clip_min=clear.clip_max=depth;
        clear.z_clamp=1;
        clear.stencil_test=(mask&2)!=0;clear.stencil_func=0x207;
        clear.stencil_ref=g->stencil_clear;clear.stencil_write_mask=clear.stencil_read_mask=255;
        clear.stencil_fail=clear.stencil_zfail=0x1E00;clear.stencil_zpass=0x1E01;
        Nv2aVshOutput v[4]={0};
        for(int i=0;i<4;i++) {
            v[i].pos[0]=(float)(g->clip_x+((i==1||i==2)?g->clip_w:0));
            v[i].pos[1]=(float)(g->clip_y+((i>=2)?g->clip_h:0));
            v[i].pos[2]=depth;v[i].pos[3]=1;
            nv2a_rc_unpack(color,v[i].d0);
        }
        return nv2a_gpu_draw(&clear,v,4,8,NULL);
    }
    if(mask&0xF0) {
        float rgba[4];nv2a_rc_unpack(color,rgba);
        if(target->format==0xA){rgba[0]=0;rgba[3]=1;}
        if(s.context1) {
            D3D11_RECT r={(LONG)((uint64_t)g->clip_x*target->pixel_width/target->width),(LONG)((uint64_t)g->clip_y*target->pixel_height/target->height),
                (LONG)(((uint64_t)(g->clip_x+g->clip_w)*target->pixel_width+target->width-1)/target->width),(LONG)(((uint64_t)(g->clip_y+g->clip_h)*target->pixel_height+target->height-1)/target->height)};
            ID3D11DeviceContext1_ClearView(s.context1,(ID3D11View *)target->rtv,rgba,&r,1);
        } else ID3D11DeviceContext_ClearRenderTargetView(s.context,target->rtv,rgba);
        target->dirty=1;
    }
    if(mask&3) {
        GpuDepth *z=depth_for(g,target);
        if(!z)return 0;
        float scale=((g->format>>4)&15)==1?65535.0f:16777215.0f;
        UINT flags=((mask&1)?D3D11_CLEAR_DEPTH:0)|((mask&2)&&z->format!=1?D3D11_CLEAR_STENCIL:0);
        if(flags) {
            ID3D11DeviceContext_ClearDepthStencilView(s.context,z->dsv,flags,depth/scale,(UINT8)g->stencil_clear);
            z->write_serial++;
        }
    }
    return 1;
}

/* Keep the completed image on the GPU. DXGI's flip model handles composition,
 * borderless fullscreen, and variable refresh without a staging/GDI round trip.
 * This runs on the same thread as the immediate context; the window thread
 * only handles input/messages and publishes the HWND. */
static int present_init(HWND window)
{
    static const char source[]=
        "Texture2DArray<float4> image:register(t0); SamplerState filter:register(s0);"
        "struct V {float4 p:SV_Position;float2 uv:TEXCOORD;};"
        "V vs(uint id:SV_VertexID){V o;o.uv=float2((id<<1)&2,id&2);"
        "o.p=float4(o.uv.x*2-1,1-o.uv.y*2,0,1);return o;}"
        "float4 ps(V i):SV_Target{return float4(image.Sample(filter,float3(i.uv,0)).rgb,1);}";
    IDXGIDevice *device=NULL;
    IDXGIAdapter *adapter=NULL;
    IDXGIFactory2 *factory=NULL;
    IDXGIFactory5 *factory5=NULL;
    ID3DBlob *vs=NULL,*ps=NULL,*errors=NULL;
    BOOL tearing=FALSE;
    HRESULT hr=ID3D11Device_QueryInterface(s.device,&IID_IDXGIDevice,(void **)&device);
    if(SUCCEEDED(hr))hr=IDXGIDevice_GetAdapter(device,&adapter);
    if(SUCCEEDED(hr))hr=IDXGIAdapter_GetParent(adapter,&IID_IDXGIFactory2,(void **)&factory);
    RELEASE(adapter);RELEASE(device);
    if(FAILED(hr))goto failure;
    if(SUCCEEDED(IDXGIFactory2_QueryInterface(factory,&IID_IDXGIFactory5,(void **)&factory5)))
        IDXGIFactory5_CheckFeatureSupport(factory5,DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing));
    RELEASE(factory5);
    s.vsync=gpu_option("RECOMP_VSYNC",0)!=0;
    s.preserve_aspect=gpu_option("RECOMP_PRESERVE_ASPECT",1)!=0;
    s.present_flags=tearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    DXGI_SWAP_CHAIN_DESC1 d={0};
    d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;
    d.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;d.BufferCount=2;
    d.Scaling=DXGI_SCALING_STRETCH;d.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode=DXGI_ALPHA_MODE_IGNORE;d.Flags=s.present_flags;
    hr=IDXGIFactory2_CreateSwapChainForHwnd(factory,(IUnknown *)s.device,window,&d,NULL,NULL,&s.swapchain);
    if(FAILED(hr))goto failure;
    /* DXGI must not process window transitions behind our borderless handler.
     * Locate the factory from the actual swap chain: a device's parent factory
     * is not necessarily the factory that owns the HWND association. */
    RELEASE(factory);
    hr=IDXGISwapChain1_GetParent(s.swapchain,&IID_IDXGIFactory2,(void **)&factory);
    if(SUCCEEDED(hr))hr=IDXGIFactory2_MakeWindowAssociation(factory,window,
        DXGI_MWA_NO_WINDOW_CHANGES|DXGI_MWA_NO_ALT_ENTER);
    if(FAILED(hr))goto failure;
    RELEASE(factory);
    hr=D3DCompile(source,sizeof(source)-1,"pc_present",NULL,NULL,"vs","vs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors);
    RELEASE(errors);
    if(FAILED(hr))goto failure;
    hr=ID3D11Device_CreateVertexShader(s.device,ID3D10Blob_GetBufferPointer(vs),
        ID3D10Blob_GetBufferSize(vs),NULL,&s.present_vs);
    RELEASE(vs);
    if(FAILED(hr))goto failure;
    hr=D3DCompile(source,sizeof(source)-1,"pc_present",NULL,NULL,"ps","ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&errors);
    RELEASE(errors);
    if(FAILED(hr))goto failure;
    hr=ID3D11Device_CreatePixelShader(s.device,ID3D10Blob_GetBufferPointer(ps),
        ID3D10Blob_GetBufferSize(ps),NULL,&s.present_ps);
    RELEASE(ps);
    if(FAILED(hr))goto failure;
    D3D11_SAMPLER_DESC sampler={0};
    sampler.Filter=gpu_option("RECOMP_SCALE_FILTER",1)?D3D11_FILTER_MIN_MAG_MIP_LINEAR:D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD=D3D11_FLOAT32_MAX;sampler.MaxAnisotropy=1;
    hr=ID3D11Device_CreateSamplerState(s.device,&sampler,&s.present_sampler);
    if(FAILED(hr))goto failure;
    s.window=window;
    fprintf(stderr,"[GPU-D3D11] Native flip presentation: vsync=%d, tearing=%d\n",s.vsync,tearing);
    return 1;
failure:
    RELEASE(factory5);RELEASE(factory);RELEASE(vs);RELEASE(ps);RELEASE(errors);
    s.present_failed=1;
    return gpu_failure("Native presentation initialization",hr);
}

int nv2a_gpu_surface_info(uint32_t va,uint32_t *width,uint32_t *height,
                          uint32_t *pitch,uint32_t *bpp)
{
    if (!s.device || s.failed) return 0;
    check_allocations();
    for (unsigned i=0;i<GPU_TARGETS;i++) {
        GpuTarget *t=&s.targets[i];
        if (t->texture && t->va==va) {
            *width=t->width; *height=t->height; *pitch=t->pitch; *bpp=t->bpp;
            return 1;
        }
    }
    return 0;
}

static int recover_at_frame_boundary(void *window)
{
    if(s.device || s.init)release_render_device();
    int action=nv2a_gpu_recovery_poll(&lifetime.recovery,GetTickCount64());
    if(action<0) {
        if(window && !lifetime.error_posted) {
            lifetime.error_posted=1;
            if(lifetime.fatal_error)
                fprintf(stderr,"[GPU-ERROR] unrecoverable rendering failure; reason=0x%08X; closing game\n",
                    (unsigned)lifetime.reason);
            else fprintf(stderr,"[GPU-RECOVERY] exhausted after %u attempts; reason=0x%08X; closing game\n",
                    lifetime.recovery.attempts,(unsigned)lifetime.reason);
            PostMessageW((HWND)window,PC_WINDOW_RENDERER_ERROR_MESSAGE,
                         (WPARAM)(uint32_t)lifetime.reason,0);
        }
    } else if(action>0) {
        fprintf(stderr,"[GPU-RECOVERY] recreating attempt=%u\n",lifetime.recovery.attempts);
        int injected=gpu_option("RECOMP_GPU_RECOVERY_TEST",0) &&
            lifetime.recovery.attempts<=(unsigned)gpu_option("RECOMP_GPU_RECOVERY_FAIL_ATTEMPTS",0);
        int ready=0;
        if(injected)gpu_failure("Diagnostic recovery creation",E_FAIL);
        else ready=nv2a_gpu_enabled() && (!window || present_init((HWND)window));
        if(ready) {
            nv2a_gpu_recovery_created(&lifetime.recovery,GetTickCount64());
            fprintf(stderr,"[GPU-RECOVERY] device recreated generation=%llu; awaiting healthy presentation\n",
                (unsigned long long)lifetime.generation);
        } else {
            release_render_device();
            nv2a_gpu_recovery_retry(&lifetime.recovery,GetTickCount64());
        }
    }
    /* Consume the failed frame and let the command processor acknowledge its
     * fences. Sleep also bounds an uncapped loading screen's retry loop. */
    Sleep(16);
    return 1;
}

static int present_failure(const char *operation,HRESULT hr,void *window)
{
    gpu_failure(operation,hr);
    return nv2a_gpu_discard_frame()?recover_at_frame_boundary(window):0;
}

static void check_recovery_probe(void)
{
    static int configured;
    static wchar_t path[32768];
    if(!configured) {
        configured=1;
        const char *flag=getenv("RECOMP_GPU_RECOVERY_FLAG");
        if(gpu_option("RECOMP_GPU_RECOVERY_TEST",0) && flag)
            MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,flag,-1,path,32768);
    }
    if(path[0] && s.device && !s.failed &&
       GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES && DeleteFileW(path))
        nv2a_gpu_debug_device_loss();
}

/* Why the last nv2a_gpu_present returned 0 (the CPU presents the frame): 1 no device, 2 device failed, 3 no window,
 * 4 presentation init failed, 5 RECOMP_GPU_READBACK_PRESENT, 6 no GPU target for the flipped address, 7 GetClientRect. */
static volatile int g_present_reason;
int nv2a_gpu_present_reason(void){return g_present_reason;}
/* RECOMP_OVERLAY_TRACE=1 (diagnostic, off by default; R27 tint2): every present, copy six 16x16 boxes of the presented
 * guest target (HUD-free 3D areas) into a 3-deep staging ring and log the mean RGB of each box two presents later as
 * 'P <guest frame> r,g,b x6' (no GPU stall: Map uses DO_NOT_WAIT and skips a sample that is not ready). */
extern FILE *nv2a_overlay_trace_file(void);
extern uint32_t nv2a_overlay_guest_frame(void);
#define OVL_PROBES 6
static void overlay_probe(GpuTarget *target)
{
    FILE *f=nv2a_overlay_trace_file();
    if(!f || !target || !target->texture)return;
    static ID3D11Texture2D *ring[3]; static DXGI_FORMAT fmt[3]; static uint32_t frame[3]; static int used[3]; static unsigned head;
    static const float at[OVL_PROBES][2]={{0.22f,0.12f},{0.78f,0.12f},{0.30f,0.55f},{0.70f,0.55f},{0.50f,0.18f},{0.38f,0.86f}};
    D3D11_TEXTURE2D_DESC td;ID3D11Texture2D_GetDesc(target->texture,&td);
    if(td.Width<64 || td.Height<64 || td.SampleDesc.Count!=1)return;
    for(unsigned n=0;n<3;n++) {   /* drain finished samples, oldest first */
        unsigned k=(head+n)%3;
        if(!used[k])continue;
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)ring[k],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING)continue;
        used[k]=0;
        if(FAILED(hr))continue;
        int bgr=fmt[k]==DXGI_FORMAT_B8G8R8A8_UNORM || fmt[k]==DXGI_FORMAT_B8G8R8X8_UNORM || fmt[k]==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        fprintf(f,"P %u",frame[k]);
        for(unsigned b=0;b<OVL_PROBES;b++) {
            unsigned sum[3]={0,0,0};
            for(unsigned y=0;y<16;y++) {
                const uint8_t *row=(const uint8_t *)m.pData+(size_t)y*m.RowPitch+(size_t)b*16*4;
                for(unsigned x=0;x<16;x++){sum[0]+=row[x*4];sum[1]+=row[x*4+1];sum[2]+=row[x*4+2];}
            }
            unsigned r=bgr?sum[2]:sum[0],g=sum[1],bl=bgr?sum[0]:sum[2];
            fprintf(f," %.1f,%.1f,%.1f",r/256.0,g/256.0,bl/256.0);
        }
        fputc('\n',f);
        ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)ring[k],0);
    }
    unsigned k=head%3;
    if(used[k])return;   /* ring full: skip this present */
    if(!ring[k] || fmt[k]!=td.Format) {
        RELEASE(ring[k]);
        D3D11_TEXTURE2D_DESC d={0};
        d.Width=16*OVL_PROBES;d.Height=16;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=td.Format;
        d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(ID3D11Device_CreateTexture2D(s.device,&d,NULL,&ring[k])))return;
        fmt[k]=td.Format;
    }
    for(unsigned b=0;b<OVL_PROBES;b++) {
        UINT x=(UINT)(at[b][0]*td.Width)-8,y=(UINT)(at[b][1]*td.Height)-8;
        D3D11_BOX box={x,y,0,x+16,y+16,1};
        ID3D11DeviceContext_CopySubresourceRegion(s.context,(ID3D11Resource *)ring[k],0,b*16,0,0,(ID3D11Resource *)target->texture,0,&box);
    }
    frame[k]=nv2a_overlay_guest_frame();used[k]=1;head++;
}

int nv2a_gpu_present(uint32_t va,void *window)
{
    check_recovery_probe();
    if(lifetime.recovery.phase==NV2A_GPU_VERIFYING) {
        ULONGLONG now=GetTickCount64();
        if(window && (IsIconic((HWND)window) || !IsWindowVisible((HWND)window) || s.occluded))
            nv2a_gpu_recovery_suspended(&lifetime.recovery,now);
        nv2a_gpu_recovery_poll(&lifetime.recovery,now);
    }
    if(nv2a_gpu_discard_frame())return recover_at_frame_boundary(window);
    nv2a_texture_pack_frame();
    GpuTarget *target=NULL;
    static int readback=-1;
    if(readback<0)readback=gpu_option("RECOMP_GPU_READBACK_PRESENT",0);
    g_present_reason=!s.device?1:s.failed?2:!window?3:s.present_failed?4:readback?5:0;
    if(g_present_reason)return 0;
    check_allocations();
    for(int i=0;i<GPU_TARGETS;i++)if(s.targets[i].texture && s.targets[i].va==va){target=&s.targets[i];break;}
    if(!target){g_present_reason=6;return 0;}
    if(!s.swapchain && !present_init((HWND)window))
        return nv2a_gpu_discard_frame()?recover_at_frame_boundary(window):0;
    gpu_timing_present(s.device,s.context);
    s.vsync=pc_setting_get(PC_VSYNC,s.vsync);
    s.preserve_aspect=pc_setting_get(PC_ASPECT,s.preserve_aspect);
    {
        int filter=pc_setting_get(PC_FILTER,1);
        if(filter!=s.previous_filter) {
            D3D11_SAMPLER_DESC d={0};
            d.Filter=filter?D3D11_FILTER_MIN_MAG_MIP_LINEAR:D3D11_FILTER_MIN_MAG_MIP_POINT;
            d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
            d.MaxLOD=D3D11_FLOAT32_MAX;d.MaxAnisotropy=1;
            ID3D11SamplerState *replacement=NULL;
            HRESULT hr=ID3D11Device_CreateSamplerState(s.device,&d,&replacement);
            if(FAILED(hr))return present_failure("CreateSamplerState presentation",hr,window);
            RELEASE(s.present_sampler);s.present_sampler=replacement;s.previous_filter=filter;
        }
    }
    RECT client;
    if(!GetClientRect((HWND)window,&client)){g_present_reason=7;return 0;}
    UINT width=(UINT)client.right,height=(UINT)client.bottom;
    if(!width || !height || IsIconic((HWND)window)){Sleep(10);return 1;}
    if(width!=s.present_width || height!=s.present_height) {
        ID3D11DeviceContext_OMSetRenderTargets(s.context,0,NULL,NULL);
        RELEASE(s.backbuffer);
        HRESULT hr=IDXGISwapChain1_ResizeBuffers(s.swapchain,2,width,height,DXGI_FORMAT_UNKNOWN,s.present_flags);
        if(FAILED(hr))return present_failure("ResizeBuffers",hr,window);
        ID3D11Texture2D *back=NULL;
        hr=IDXGISwapChain1_GetBuffer(s.swapchain,0,&IID_ID3D11Texture2D,(void **)&back);
        if(SUCCEEDED(hr))hr=ID3D11Device_CreateRenderTargetView(s.device,(ID3D11Resource *)back,NULL,&s.backbuffer);
        RELEASE(back);
        if(FAILED(hr))return present_failure("swapchain backbuffer",hr,window);
        s.present_width=width;s.present_height=height;
    }
    ID3D11ShaderResourceView *none[8]={0};
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,8,none);
    ID3D11DeviceContext_OMSetRenderTargets(s.context,1,&s.backbuffer,NULL);
    const float black[4]={0,0,0,1};
    ID3D11DeviceContext_ClearRenderTargetView(s.context,s.backbuffer,black);
    D3D11_VIEWPORT viewport={0,0,(float)width,(float)height,0,1};
    {
        float aspect=pc_display_aspect((float)target->width/target->height);
        if((float)width/height>aspect) {
            viewport.Width=height*aspect;
            viewport.TopLeftX=((float)width-viewport.Width)*0.5f;
        } else {
            viewport.Height=width/aspect;
            viewport.TopLeftY=((float)height-viewport.Height)*0.5f;
        }
    }
    D3D11_RECT scissor={0,0,(LONG)width,(LONG)height};
    ID3D11DeviceContext_RSSetViewports(s.context,1,&viewport);
    ID3D11DeviceContext_RSSetScissorRects(s.context,1,&scissor);
    ID3D11DeviceContext_RSSetState(s.context,s.raster);
    ID3D11DeviceContext_OMSetBlendState(s.context,NULL,NULL,~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(s.context,NULL,0);
    ID3D11DeviceContext_IASetInputLayout(s.context,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s.context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s.context,s.present_vs,NULL,0);
    ID3D11DeviceContext_PSSetShader(s.context,s.present_ps,NULL,0);
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,1,&target->srv);
    ID3D11DeviceContext_PSSetSamplers(s.context,0,1,&s.present_sampler);
    ID3D11DeviceContext_Draw(s.context,3,0);
    ID3D11DeviceContext_PSSetShaderResources(s.context,0,1,none);
    overlay_probe(target);
    pc_frontend_render(s.device,s.context,window,width,height);
    /* Presentation borrows the immediate context. Subsequent guest draws
     * must restore its target and sampler bindings even when state matches. */
    s.bound_rtv=NULL;s.bound_dsv=NULL;
    memset(s.bound_views,0,sizeof(s.bound_views));
    memset(s.bound_samplers,0,sizeof(s.bound_samplers));
    invalidate_pipeline();
    double present_start=nv2a_gpu_cpu_begin();
    HRESULT hr=IDXGISwapChain1_Present(s.swapchain,s.vsync?1:0,
        !s.vsync && s.present_flags?DXGI_PRESENT_ALLOW_TEARING:0);
    nv2a_gpu_cpu_end(NV2A_CPU_PRESENT,present_start,0);
    if(hr==S_OK)nv2a_gpu_cpu_count(NV2A_CPU_PRESENT_OK,1);
    if(FAILED(hr))return present_failure("Present",hr,window);
    s.occluded=hr==DXGI_STATUS_OCCLUDED;
    if(s.occluded)nv2a_gpu_recovery_suspended(&lifetime.recovery,GetTickCount64());
    if(hr==S_OK && IsWindowVisible((HWND)window) &&
       nv2a_gpu_recovery_presented(&lifetime.recovery,GetTickCount64()))
        fprintf(stderr,"[GPU-RECOVERY] healthy generation=%llu after %u consecutive presentations\n",
            (unsigned long long)lifetime.generation,NV2A_GPU_RECOVERY_HEALTHY_FRAMES);
    {
        static LARGE_INTEGER frequency;
        static int high_res_pacing=-1;
        int limit=pc_setting_get(PC_FRAME_LIMIT,0);
        LARGE_INTEGER now;
        if(!frequency.QuadPart)QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&now);
        if(limit>0) {
            if(high_res_pacing<0) {
                high_res_pacing=gpu_option("RECOMP_HIGH_RES_PACING",1);
                atexit(close_frame_pacer);
            }
            LONGLONG period=frequency.QuadPart/limit;
            if(limit!=s.previous_limit || !s.present_deadline.QuadPart || now.QuadPart>s.present_deadline.QuadPart+period*4)
                s.present_deadline=now;
            /* Keep the ideal cadence instead of accumulating each sleep's
             * overshoot. With VSync, leave a small scheduling margin so the
             * next Present can reach its refresh rather than missing it. */
            LONGLONG cadence_target=s.present_deadline.QuadPart-(s.vsync?frequency.QuadPart/1000:0);
            double wait_start=nv2a_gpu_cpu_begin();
            nv2a_frame_pacer_wait(&s.frame_pacer,frequency.QuadPart,cadence_target,high_res_pacing);
            nv2a_gpu_cpu_end(NV2A_CPU_PACER,wait_start,0);
            s.present_deadline.QuadPart+=period;
        }
        else s.present_deadline.QuadPart=0;
        s.previous_limit=limit;
    }
    return 1;
}

int nv2a_gpu_debug_pixel(uint32_t va,uint32_t x,uint32_t y,uint32_t *argb)
{
    if(!s.device || s.failed || !argb)return 0;
    GpuTarget *target=NULL;
    for(int i=0;i<GPU_TARGETS;i++)
        if(s.targets[i].texture && s.targets[i].va==va) { target=&s.targets[i];break; }
    if(!target || x>=target->width || y>=target->height)return 0;
    if(!s.debug_pixel) {
        D3D11_TEXTURE2D_DESC desc={0};
        desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.Usage=D3D11_USAGE_STAGING;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(ID3D11Device_CreateTexture2D(s.device,&desc,NULL,&s.debug_pixel)))return 0;
    }
    uint32_t sx=(uint32_t)((uint64_t)x*target->pixel_width/target->width),sy=(uint32_t)((uint64_t)y*target->pixel_height/target->height);
    D3D11_BOX box={sx,sy,0,sx+1,sy+1,1};
    ID3D11DeviceContext_CopySubresourceRegion(s.context,(ID3D11Resource *)s.debug_pixel,0,0,0,0,
        (ID3D11Resource *)target->texture,0,&box);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if(FAILED(ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.debug_pixel,0,D3D11_MAP_READ,0,&mapped)))return 0;
    memcpy(argb,mapped.pData,4);
    ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.debug_pixel,0);
    return 1;
}

int nv2a_gpu_debug_depth(uint32_t va,uint32_t x,uint32_t y,uint32_t *packed)
{
    GpuDepth *depth=NULL;
    if(!s.device || s.failed || !packed)return 0;
    for(unsigned i=0;i<GPU_TARGETS;i++)
        if(s.depths[i].texture && s.depths[i].va==va) { depth=&s.depths[i];break; }
    if(!depth || x>=depth->width || y>=depth->height)return 0;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(depth->texture,&desc);
    desc.Format=depth->format==1?DXGI_FORMAT_R16_TYPELESS:DXGI_FORMAT_R24G8_TYPELESS;
    desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(!s.debug_depth || memcmp(&s.debug_depth_desc,&desc,sizeof(desc))) {
        RELEASE(s.debug_depth);
        if(FAILED(ID3D11Device_CreateTexture2D(s.device,&desc,NULL,&s.debug_depth)))return 0;
        s.debug_depth_desc=desc;
    }
    /* D3D requires an entire depth-stencil subresource copy. */
    ID3D11DeviceContext_CopyResource(s.context,(ID3D11Resource *)s.debug_depth,(ID3D11Resource *)depth->texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if(FAILED(ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)s.debug_depth,0,D3D11_MAP_READ,0,&mapped)))return 0;
    const uint8_t *p=(const uint8_t *)mapped.pData+((size_t)y*depth->pixel_height/depth->height)*mapped.RowPitch;
    *packed=0;
    memcpy(packed,p+((size_t)x*depth->pixel_width/depth->width)*(depth->format==1?2:4),depth->format==1?2:4);
    ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)s.debug_depth,0);
    return 1;
}

static void readback_target(GpuTarget *t)
{
    if(!t->staging) {   /* T2-3 B2.6: lazy */
        D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc(t->texture,&d);
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
        HRESULT hr=ID3D11Device_CreateTexture2D(s.device,&d,NULL,&t->staging);
        if(FAILED(hr)){gpu_failure("CreateTexture2D staging",hr);return;}
    }
        ID3D11DeviceContext_CopyResource(s.context,(ID3D11Resource *)t->staging,(ID3D11Resource *)t->texture);
        D3D11_MAPPED_SUBRESOURCE map;
        HRESULT hr=ID3D11DeviceContext_Map(s.context,(ID3D11Resource *)t->staging,0,D3D11_MAP_READ,0,&map);
        if(FAILED(hr)){gpu_failure("Map framebuffer readback",hr);return;}
        uint8_t *dst=(uint8_t *)xbox_GetMemoryOffset()+t->va;
        for(uint32_t y=0;y<t->height;y++) {
            uint32_t y0=(uint32_t)((uint64_t)y*t->pixel_height/t->height);
            uint32_t y1=(uint32_t)((uint64_t)(y+1)*t->pixel_height/t->height);
            if(y1<=y0)y1=y0+1;
            const uint32_t *row=(const uint32_t *)((const uint8_t *)map.pData+(size_t)y0*map.RowPitch);
            if(t->bpp==4 && t->pixel_height==t->height && t->pixel_width==t->width && !t->swizzled)memcpy(dst+(size_t)y*t->pitch,row,t->width*4);
            else {
                for(uint32_t x=0;x<t->width;x++) {
                    uint32_t x0=(uint32_t)((uint64_t)x*t->pixel_width/t->width);
                    uint32_t x1=(uint32_t)((uint64_t)(x+1)*t->pixel_width/t->width);
                    if(x1<=x0)x1=x0+1;
                    uint32_t p=row[x0];
                    if(y1-y0>1 || x1-x0>1) {
                        uint32_t sum[4]={0};
                        for(uint32_t sy=0;sy<y1-y0;sy++) {
                            const uint32_t *scaled_row=(const uint32_t *)((const uint8_t *)row+(size_t)sy*map.RowPitch);
                            for(uint32_t sx=x0;sx<x1;sx++) {
                                uint32_t value=scaled_row[sx];
                                for(unsigned channel=0;channel<4;channel++)sum[channel]+=(value>>(channel*8))&255;
                            }
                        }
                        uint32_t count=(y1-y0)*(x1-x0);p=0;
                        for(unsigned channel=0;channel<4;channel++)p|=((sum[channel]+count/2)/count)<<(channel*8);
                    }
                    if(t->bpp==4)*(uint32_t *)(dst+target_offset(t,x,y))=p;
                    else { uint16_t *out=(uint16_t *)(dst+target_offset(t,x,y));
                    if(t->format==0xA)*out=(uint16_t)p;
                    else if(t->format==1 || t->format==2)*out=(uint16_t)(((p>>19)&31)<<10|((p>>11)&31)<<5|((p>>3)&31));
                    else *out=(uint16_t)(((p>>19)&31)<<11|((p>>10)&63)<<5|((p>>3)&31)); }
                }
            }
        }
        ID3D11DeviceContext_Unmap(s.context,(ID3D11Resource *)t->staging,0);t->dirty=0;
}
void nv2a_gpu_readback(uint32_t va)
{
    if(!s.device || s.failed)return;
    for(int i=0;i<GPU_TARGETS;i++) {
        GpuTarget *t=&s.targets[i];
        if(!t->texture || t->va!=va || !t->dirty)continue;
        if(t->scratch_backup) {
            GpuTarget backing=scratch_backing(t);
            scratch_restore(&backing,t);
            /* The backing owns the complete guest byte range. A staging
             * resource is required only when a CPU consumer reads it. */
            D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc(backing.texture,&d);
            d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            HRESULT hr=ID3D11Device_CreateTexture2D(s.device,&d,NULL,&backing.staging);
            if(FAILED(hr)){gpu_failure("CreateTexture2D scratch readback",hr);return;}
            readback_target(&backing);RELEASE(backing.staging);t->dirty=0;
        } else readback_target(t);
    }
}
void nv2a_gpu_sync_all(void)
{
    if(!s.device || s.failed)return;
    for(int i=0;i<GPU_TARGETS;i++)if(s.targets[i].texture)nv2a_gpu_readback(s.targets[i].va);
}
void nv2a_gpu_invalidate(uint32_t va)
{
    if(!s.device || s.failed)return;
    for(int i=0;i<GPU_TARGETS;i++) {
        GpuTarget *t=&s.targets[i];
        if(t->texture && t->va==va)discard_target(t,1);
    }
}
#else
int nv2a_gpu_present_reason(void){return 0;}
int nv2a_gpu_program_pending(void){return 0;}
int nv2a_gpu_enabled(void){return 0;}
int nv2a_gpu_discard_frame(void){return 0;}
uint64_t nv2a_gpu_device_generation(void){return 0;}
int nv2a_gpu_debug_device_loss(void){return 0;}
int nv2a_gpu_draw(const Nv2aGpuState *s,const Nv2aVshOutput *v,uint32_t n,uint32_t p,Nv2aGpuTexel t)
{(void)s;(void)v;(void)n;(void)p;(void)t;return 0;}
int nv2a_gpu_clear(const Nv2aGpuState *s,uint32_t m,uint32_t c,float z){(void)s;(void)m;(void)c;(void)z;return 0;}
int nv2a_gpu_draw_program(const Nv2aGpuState *s,const Nv2aGpuRawVertex *v,uint32_t n,uint32_t p,
                          Nv2aGpuTexel t,const Nv2aVshProgram *pr,const float c[NV2A_VSH_CONSTANTS][4],
                          uint32_t f,uint32_t m,const float fp[2])
{(void)s;(void)v;(void)n;(void)p;(void)t;(void)pr;(void)c;(void)f;(void)m;(void)fp;return 0;}
int nv2a_gpu_draw_program_indexed(const Nv2aGpuState *s,const Nv2aGpuRawVertex *v,uint32_t n,
                          const uint32_t *i,uint32_t ni,uint32_t p,Nv2aGpuTexel t,
                          const Nv2aVshProgram *pr,const float c[NV2A_VSH_CONSTANTS][4],
                          uint32_t f,uint32_t m,const float fp[2])
{(void)s;(void)v;(void)n;(void)i;(void)ni;(void)p;(void)t;(void)pr;(void)c;(void)f;(void)m;(void)fp;return 0;}
int nv2a_gpu_draw_buffers(const Nv2aGpuState *s,const Nv2aGpuVertexAttr a[16],const float d[16][4],
                          const uint32_t *i,uint32_t n,uint32_t p,Nv2aGpuTexel t,const Nv2aVshProgram *v,
                          const float c[NV2A_VSH_CONSTANTS][4],uint32_t f,uint32_t m,const float q[2])
{(void)s;(void)a;(void)d;(void)i;(void)n;(void)p;(void)t;(void)v;(void)c;(void)f;(void)m;(void)q;return 0;}
void nv2a_gpu_readback(uint32_t va){(void)va;}
int nv2a_gpu_debug_pixel(uint32_t va,uint32_t x,uint32_t y,uint32_t *argb)
{(void)va;(void)x;(void)y;(void)argb;return 0;}
int nv2a_gpu_debug_depth(uint32_t va,uint32_t x,uint32_t y,uint32_t *packed)
{(void)va;(void)x;(void)y;(void)packed;return 0;}
int nv2a_gpu_present(uint32_t va,void *window){(void)va;(void)window;return 0;}
int nv2a_gpu_surface_info(uint32_t va,uint32_t *w,uint32_t *h,uint32_t *p,uint32_t *b)
{(void)va;(void)w;(void)h;(void)p;(void)b;return 0;}
void nv2a_gpu_sync_all(void){}
void nv2a_gpu_invalidate(uint32_t va){(void)va;}
#endif

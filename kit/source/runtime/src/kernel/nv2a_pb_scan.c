/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include "../platform/pc_switch.h"   /* T2-1: [Render] PbBulk */
/* DMA_GET advances when a word has been captured, before a semaphore or
 * software method can wake its producer. Publishing only at the end of a PUT
 * snapshot leaves GET outside the driver's active ring interval after a fence
 * completes, which makes the XDK incorrectly treat unread commands as free. */
static volatile uint32_t *s_dma_get;
void nv2a_pb_set_get_register(volatile uint32_t *reg) { s_dma_get=reg; }
/* GetWake: progress on DMA_GET wakes a D3D make-space waiter (xbox_memory_layout.c nv2a_set_get). */
void nv2a_gpu_publish_word(volatile uint32_t *word,uint32_t value);
static int get_wake_on(void){static int on=-1;if(on<0){const char *e=getenv("RECOMP_GPU_GET_WAKE");on=!(e&&*e=='0');}return on;}
#define SET_GET(v) do{ if(get_wake_on()) nv2a_gpu_publish_word(s_dma_get,(v)); else *s_dma_get=(v); }while(0)
#include <string.h>

#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u   /* physical P is at this + P */
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;
static uint16_t s_seen_index[8][2048];
static int s_scan_enabled = -1;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_upload(uint32_t method, uint32_t step, const uint32_t *words, uint32_t count);
extern int nv2a_pb_exec_bulk(uint32_t method, const uint32_t *words, uint32_t count);   /* T2-1 */
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;

static void note(uint32_t subch, uint32_t method)
{
    if (subch >= 8 || method >= 0x2000 || (method & 3)) return;
    unsigned index = s_seen_index[subch][method / 4];
    if (index) {
        s_seen[index - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_index[subch][method / 4] = (uint16_t)(s_seen_count + 1);
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk commands from va to end_va. Returns the words consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. This used to skip CALLs, which
 * was invisible until a title used them for real: RenderWare on the Xbox
 * compiles static geometry into pushbuffers and draws it with a CALL, so the
 * whole race track went missing from Burnout 3's main view while the
 * geometry it submits inline (environment maps, the HUD) drew fine.
 * The NV2A has one level of subroutine, so a CALL inside a call is not
 * followed. Addresses are physical, like PUT's, and reach guest memory the
 * same way: through the contiguous window. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va, int in_call,
                        uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = XBOX_CONTIG_BASE | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                if (s_scan_enabled) note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(s_scan_enabled || s_exec_enabled) || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    words = pb_walk(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}

/* Consume the actual PFIFO stream, including ring jumps. GET is the returned
 * position, never a guessed high/low watermark. Pending packet state survives
 * a PUT in the middle of a payload. The NV2A supports one subroutine level. */
uint32_t nv2a_pb_consume(uint32_t start_va, uint32_t end_va)
{
    static uint32_t remaining, method, subch, return_va;
    static int noninc;
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t va = start_va, words = 0, jumps = 0, unknown = 0;
    static struct {uint32_t va,word,payload;} recent[16];
    static unsigned recent_count;
    static int bulk_uploads=-1;
    static int bulk_words=-1;   /* T2-1 E1: [Render] PbBulk / RECOMP_PB_BULK, default 1 (T2-1 flip); 0 = the per-word path */
    if(bulk_uploads<0) {
        const char *option=getenv("RECOMP_PB_BULK_UPLOADS");
        bulk_uploads=!option || atoi(option)!=0;
    }
    if(bulk_words<0) {
        bulk_words=pc_switch("RECOMP_PB_BULK","Render","PbBulk",1)!=0;
        fprintf(stderr,"[PB-BULK] %s\n",bulk_words?"on: index/inline payloads in one call":"off: per-word path");
    }
    if (s_exec_enabled < 0) s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0) s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(s_exec_enabled || s_scan_enabled)) return end_va;
    while ((va != end_va || return_va) && words < 0x100000u) {
        if (va < XBOX_CONTIG_BASE || va > XBOX_CONTIG_BASE + 0x04000000u - 4) {
            unknown++;
            break;
        }
        if(bulk_uploads && s_exec_enabled && remaining && subch==0 &&
           method>=0x0B00 && method<0x0C00) {
            uint32_t captured[32],take=remaining,step=noninc?0:4;
            if(take>32)take=32;
            uint32_t available=(XBOX_CONTIG_BASE+0x04000000u-va)/4;
            if(take>available)take=available;
            if(!return_va && end_va>va && take>(end_va-va)/4)take=(end_va-va)/4;
            if(take>0x100000u-words)take=0x100000u-words;
            if(step) {
                /* A packet may cross from program into constant methods. */
                uint32_t end=method<0x0B80?0x0B80:0x0C00;
                if(take>(end-method)/4)take=(end-method)/4;
            }
            if(take) {
                for(uint32_t i=0;i<take;i++) {
                    captured[i]=*(const volatile uint32_t *)(mem+va+i*4);
                    recent[recent_count & 15].va=va+i*4;
                    recent[recent_count & 15].word=captured[i];
                    recent[recent_count++ & 15].payload=remaining-i;
                    if(s_scan_enabled)note(subch,method+i*step);
                }
                va+=take*4;words+=take;
                /* Publish only after every input has been captured. These
                 * methods cannot signal a guest fence or start a draw. */
                if(s_dma_get)SET_GET(va & 0x0FFFFFFFu);
                nv2a_pb_exec_upload(method,step,captured,take);
                remaining-=take;method+=take*step;
                continue;
            }
        }
        /* T2-1 E1: a whole non-incrementing ARRAY_ELEMENT16/32 or INLINE_ARRAY payload (as far as
         * PUT, the ring end and the budget allow) in one call. Same clamps as the upload branch.
         * GET is published only after every word was consumed. Semaphore, software and flip
         * methods never come here. nv2a_pb_exec_bulk returns 0 to keep the per-word path. */
        if(bulk_words && s_exec_enabled && !s_scan_enabled && remaining && subch==0 && noninc &&
           (method==0x1800 || method==0x1808 || method==0x1818)) {
            uint32_t take=remaining;
            uint32_t available=(XBOX_CONTIG_BASE+0x04000000u-va)/4;
            if(take>available)take=available;
            if(!return_va && end_va>va && take>(end_va-va)/4)take=(end_va-va)/4;
            if(take>0x100000u-words)take=0x100000u-words;
            if(take && nv2a_pb_exec_bulk(method,(const uint32_t *)(mem+va),take)) {
                for(uint32_t i=take>16?take-16:0;i<take;i++) {
                    recent[recent_count & 15].va=va+i*4;
                    recent[recent_count & 15].word=*(const volatile uint32_t *)(mem+va+i*4);
                    recent[recent_count++ & 15].payload=remaining-i;
                }
                va+=take*4;words+=take;remaining-=take;
                if(s_dma_get)SET_GET(va & 0x0FFFFFFFu);
                continue;
            }
        }
        uint32_t w = *(const volatile uint32_t *)(mem + va);
        recent[recent_count & 15].va=va;
        recent[recent_count & 15].word=w;
        recent[recent_count++ & 15].payload=remaining;
        uint32_t header_va = va;
        va += 4; words++;
        if (remaining) {
            if (s_dma_get) SET_GET(va & 0x0FFFFFFFu);
            if (s_scan_enabled) note(subch, method);
            if (s_exec_enabled) nv2a_pb_exec_method(subch, method, w);
            remaining--;
            if (!noninc) method += 4;
        } else if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? w & 0xFFFFFFFCu : w & 0x1FFFFFFCu;
            va = XBOX_CONTIG_BASE | (target & 0x0FFFFFFFu);
            jumps++;
        } else if ((w & 0xFFFF0003u) == 0x00020000u) {
            if (!return_va) { va = header_va; unknown++; break; }
            va = return_va; return_va = 0;
        } else if ((w & 3u) == 2u) {
            if (return_va) { va = header_va; unknown++; break; }
            return_va = va;
            va = XBOX_CONTIG_BASE | (w & 0x0FFFFFFCu);
            s_tot_calls++;
        } else if ((w & 0xE0030003u) == 0 || (w & 0xE0030003u) == 0x40000000u) {
            remaining = (w >> 18) & 0x7FFu;
            subch = (w >> 13) & 7u;
            method = w & 0x1FFCu;
            noninc = (w & 0x40000000u) != 0;
        } else {
            va = header_va; unknown++; break;
        }
        if (s_dma_get) SET_GET(va & 0x0FFFFFFFu);
    }
    s_tot_words += words; s_tot_jumps += jumps;
    s_tot_unknown += unknown; s_tot_segments++;
    if (unknown) {
        static unsigned warnings;
        if (warnings++ < 8)
        {
            fprintf(stderr, "[PB] invalid FIFO at %08X (PUT %08X)\n", va, end_va);
            for(unsigned i=recent_count>16?recent_count-16:0;i<recent_count;i++)
                fprintf(stderr,"[PB] recent %08X = %08X payload=%u\n",recent[i & 15].va,
                    recent[i & 15].word,recent[i & 15].payload);
            if(va>=XBOX_CONTIG_BASE+32 && va<XBOX_CONTIG_BASE+0x04000000u-32) {
                fprintf(stderr,"[PB] remaining=%u method=%X subch=%u return=%08X bytes:",
                    remaining,method,subch,return_va);
                for(int i=-8;i<8;i++)fprintf(stderr," %08X",*(const uint32_t *)(mem+va+i*4));
                fputc('\n',stderr);
            }
        }
    }
    return va;
}

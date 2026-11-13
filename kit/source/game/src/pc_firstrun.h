/* pc_firstrun.h -- automatic first-run defaults from the hardware (FT-RT, R8-LOWEND-PERF "Safe first-run defaults").
 *
 * Pure decision table, header only, no Windows calls, so a unit test can walk it (tests/ft_rt_test.c).
 * pc_storage.c detects the hardware and applies the result ONLY when it has just created the per-user
 * pc_settings.ini from the packaged defaults. An existing settings file is never read or changed.
 *
 * A value of -1 (or NULL for text) means "leave the packaged default alone".
 *
 *   GPU tier from DedicatedVideoMemory of the adapter the game will use (Windows high-performance adapter):
 *     iGPU  < 512 MB       Iris Xe, UHD, Vega without carve-out
 *     mid   512..3583 MB   GTX 1050 / RX 560 2 GB, 3 GB cards
 *     high  >= 3584 MB     4 GB and up (a 4 GB card reports about 4096)
 *   TextureQuality (0 original, 1 2x, 2 4x; 4x is about 2.3 GB of random pack reads, bad on an HDD):
 *     iGPU 0 | mid 1 on an SSD else 0 | high 1, or 2 with >= 5.5 GB VRAM (6 GB class) on an SSD
 *     then RAM < 6000 MB caps it at 0.
 *   FrameLimit: iGPU 60 | mid 60 on a laptop (battery present) else 144 | high 144 (Unlimited removed 11-04).
 *   PreloadShaders: 1 whenever the GPU is known (the shipped default is already 1; written so the file says so).
 *   [Audio] Threads: the only audio-quality knob that exists today (a separate [Audio] Quality, R8 P2-5, is not
 *     implemented). "low" (DSP thread at normal priority, lag guard) at <= 4 logical CPUs, or on a laptop with
 *     <= 8 logical CPUs; otherwise untouched (the shipped "auto" picks by core count by itself).
 *   Disk type decides TextureQuality only. Unknown disk counts as HDD for the 4x step and for mid-tier 2x.
 */
#pragma once

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { PC_DISK_UNKNOWN = 0, PC_DISK_SSD = 1, PC_DISK_HDD = 2 };

typedef struct PcHardware {
    int gpu_known;          /* 0 = no usable DXGI adapter info: GPU-dependent values stay at the packaged defaults */
    unsigned vram_mb;       /* DedicatedVideoMemory of the chosen adapter, MB */
    unsigned ram_mb;        /* physical RAM, MB (0 = unknown) */
    unsigned logical;       /* logical CPUs the process may use (pc_cpu_logical) */
    int battery;            /* 1 = a system battery is present (laptop), 0 = none/unknown */
    int disk;               /* PC_DISK_* of the game volume */
    unsigned desktop_w, desktop_h;  /* primary desktop size in pixels (0 = unknown: display keys stay packaged) */
} PcHardware;

typedef struct PcFirstRun {
    int texture_quality;    /* 0..2, or -1 */
    int frame_limit;        /* 60, 144, or -1 */
    int preload_shaders;    /* 0/1, or -1 */
    const char *audio_threads;   /* "low", or NULL */
    const char *gpu_class;       /* "igpu" / "mid" / "high" / "unknown", for the log */
    int fullscreen;         /* 1 = borderless fullscreen, or -1 */
    int window_w, window_h; /* window = render size, or -1 */
} PcFirstRun;

static __inline void pc_firstrun_decide(const PcHardware *hw, PcFirstRun *out)
{
    out->texture_quality = -1;
    out->frame_limit = -1;
    out->preload_shaders = -1;
    out->audio_threads = (const char *)0;
    out->gpu_class = "unknown";
    out->fullscreen = out->window_w = out->window_h = -1;

    if (hw->gpu_known) {
        int ssd = hw->disk == PC_DISK_SSD;
        int tq;
        if (hw->vram_mb < 512) {
            out->gpu_class = "igpu";
            tq = 0;
            out->frame_limit = 60;
        } else if (hw->vram_mb < 3584) {
            out->gpu_class = "mid";
            tq = ssd ? 1 : 0;
            out->frame_limit = hw->battery ? 60 : 144;
        } else {
            out->gpu_class = "high";
            tq = (hw->vram_mb >= 5632 && ssd) ? 2 : 1;
            out->frame_limit = 144;
        }
        if (hw->ram_mb && hw->ram_mb < 6000) tq = 0;
        out->texture_quality = tq;
        out->preload_shaders = 1;
    }
    if (hw->logical && (hw->logical <= 4 || (hw->battery && hw->logical <= 8)))
        out->audio_threads = "low";
    /* W-03: the first launch is borderless fullscreen at the desktop size. The window size is also the render
     * resolution, so integrated graphics render at most 1080 lines (same aspect, even width). */
    if (hw->desktop_w >= 640 && hw->desktop_h >= 480) {
        unsigned w = hw->desktop_w, h = hw->desktop_h;
        if (!strcmp(out->gpu_class, "igpu") && h > 1080) { w = (w * 1080u / h) & ~1u; h = 1080; }
        if (w > 7680) w = 7680;
        if (h > 4320) h = 4320;   /* the ranges main.c accepts */
        out->fullscreen = 1;
        out->window_w = (int)w;
        out->window_h = (int)h;
    }
}

#ifdef __cplusplus
}
#endif

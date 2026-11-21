/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      4

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
static volatile LONG           g_xa2_busy[XA2_NUM_BUFS];
static volatile LONG           g_xa2_error;
static HANDLE                  g_xa2_buffer_event;
static int                     g_xa2_started;
static unsigned                g_xa2_starved, g_xa2_backpressure;
static int                     g_xa2_was_empty;
/* 1.1.6 [HEALTH]: session totals. g_xa2_starved/backpressure restart at every (re)open; these do not.
 * APU thread only (written in xa2_submit_samples, read through xa2_health_counters). */
static unsigned                g_xa2_starved_total, g_xa2_bp_total;

/* ---- EW-B3: device-loss handling -------------------------------------
 * A lost output device used to freeze the APU thread for ever: OnBufferEnd
 * never fires again, every buffer stays busy and the wait never fails.
 *  - IXAudio2EngineCallback::OnCriticalError reports a lost device;
 *  - xa2_wait_for_space gives up after XA2_STALL_MS and the caller drops
 *    the block (the APU thread stays alive, DirectSound voices keep ending);
 *  - after XA2_REOPEN_MS of trouble the engine is torn down and reopened on
 *    the current default device (also retried every XA2_REOPEN_MS when the
 *    device was missing at start-up).
 * Test hooks (env, default off; times in seconds since the first open):
 *   RECOMP_TEST_XA2_FAIL_AT=<s>       raise a critical error (device lost)
 *   RECOMP_XA2_TEST_STALL_S=<s>       swallow buffer acks for 6 s from t=s
 *   RECOMP_TEST_XA2_INIT_FAIL_S=<s>   opening fails until t=s (no device)  */
#define XA2_STALL_MS      250
#define XA2_REOPEN_MS     2000
#define XA2_TEST_STALL_MS 6000
static volatile LONG           g_xa2_dead;       /* reopen failed, waiting to retry */
static int                     g_xa2_final;      /* public xa2_shutdown(): never reopen */
static int                     g_xa2_engine_cb_set;
static int                     g_xa2_stalled, g_xa2_stall_ms;
static ULONGLONG               g_xa2_bad_since, g_xa2_last_open_try, g_xa2_t0;
static unsigned                g_xa2_dropped, g_xa2_reopens, g_xa2_stalls;
static long long               g_xa2_total_written;
static int                     g_xa2_hooks_read;
static int                     g_xa2_hook_fail_at = -1, g_xa2_hook_stall_s = -1, g_xa2_hook_init_fail_s = -1;
static int                     g_xa2_hook_fail_done, g_xa2_hook_init_logged;

static int  xa2_open(void);
static void xa2_teardown(int close_event);

/* 1.1.6: one line per (re)open describing the default output endpoint's shared-mode mix format and
 * its form factor (speakers, headphones, headset, HDMI/display, ...). A Bluetooth headset in
 * hands-free mode shows as a 8000/16000 Hz mono mix. No device name, no device ID (privacy).
 * Plain COM on the APU thread, already in the MTA; any failure prints nothing. */
#include <mmdeviceapi.h>
static const GUID xa2_clsid_enum = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID xa2_iid_enum   = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const PROPERTYKEY xa2_pkey_format = {{0xF19F064D,0x082C,0x4E27,{0xBC,0x73,0x68,0x82,0xA1,0xBB,0x8E,0x4C}},0};
static const PROPERTYKEY xa2_pkey_form   = {{0x1DA5D803,0xD492,0x4EDD,{0x8C,0x23,0xE0,0xC0,0xFF,0xEE,0x7F,0x0E}},0};
static void xa2_log_endpoint(void)
{
    static const char *const forms[] = {"network","speakers","line","headphones","microphone","headset",
                                        "handset","digital","spdif","display","unknown"};
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    unsigned rate = 0, ch = 0, form = 10;
    if (FAILED(CoCreateInstance(&xa2_clsid_enum, NULL, CLSCTX_ALL, &xa2_iid_enum, (void **)&en)) || !en) return;
    if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) && dev &&
        SUCCEEDED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) && ps) {
        PropVariantInit(&v);
        if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &xa2_pkey_format, &v)) && v.vt == VT_BLOB &&
            v.blob.cbSize >= sizeof(WAVEFORMATEX) && v.blob.pBlobData) {
            const WAVEFORMATEX *w = (const WAVEFORMATEX *)v.blob.pBlobData;
            rate = w->nSamplesPerSec; ch = w->nChannels;
        }
        PropVariantClear(&v);
        PropVariantInit(&v);
        if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &xa2_pkey_form, &v)) && v.vt == VT_UI4 && v.ulVal <= 10)
            form = v.ulVal;
        PropVariantClear(&v);
        fprintf(stderr, "[XA2] output device: mix=%u Hz %u ch, form=%s\n", rate, ch, forms[form]);
    }
    if (ps) ps->lpVtbl->Release(ps);
    if (dev) dev->lpVtbl->Release(dev);
    en->lpVtbl->Release(en);
}

static void xa2_read_hooks(void)
{
    const char *v;
    if (g_xa2_hooks_read) return;
    g_xa2_hooks_read = 1;
    if ((v = getenv("RECOMP_TEST_XA2_FAIL_AT")) && *v) g_xa2_hook_fail_at = atoi(v);
    if ((v = getenv("RECOMP_XA2_TEST_STALL_S")) && *v) g_xa2_hook_stall_s = atoi(v);
    if ((v = getenv("RECOMP_TEST_XA2_INIT_FAIL_S")) && *v) g_xa2_hook_init_fail_s = atoi(v);
}
static ULONGLONG xa2_since_t0_ms(void)
{
    return g_xa2_t0 ? GetTickCount64() - g_xa2_t0 : 0;
}

static void STDMETHODCALLTYPE xa2_pass_start(IXAudio2VoiceCallback *self, UINT32 bytes)
{ (void)self; (void)bytes; }
static void STDMETHODCALLTYPE xa2_pass_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_stream_end(IXAudio2VoiceCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_buffer_start(IXAudio2VoiceCallback *self, void *context)
{ (void)self; (void)context; }
static void STDMETHODCALLTYPE xa2_buffer_end(IXAudio2VoiceCallback *self, void *context)
{
    (void)self;
    int idx = (int)(uintptr_t)context - 1;
    if (g_xa2_hook_stall_s >= 0) {   /* test hook: pretend the device stopped acking */
        ULONGLONG t = xa2_since_t0_ms();
        if (t >= (ULONGLONG)g_xa2_hook_stall_s * 1000 &&
            t < (ULONGLONG)g_xa2_hook_stall_s * 1000 + XA2_TEST_STALL_MS) return;
    }
    if (idx >= 0 && idx < XA2_NUM_BUFS) InterlockedExchange(&g_xa2_busy[idx], 0);
    if (g_xa2_buffer_event) SetEvent(g_xa2_buffer_event);
}
static void STDMETHODCALLTYPE xa2_loop_end(IXAudio2VoiceCallback *self, void *context)
{ (void)self; (void)context; }
static void STDMETHODCALLTYPE xa2_voice_error(IXAudio2VoiceCallback *self, void *context, HRESULT error)
{
    (void)self; (void)context;
    InterlockedExchange(&g_xa2_error, error);
    if (g_xa2_buffer_event) SetEvent(g_xa2_buffer_event);
}
static IXAudio2VoiceCallbackVtbl g_xa2_callbacks = {
    xa2_pass_start, xa2_pass_end, xa2_stream_end, xa2_buffer_start,
    xa2_buffer_end, xa2_loop_end, xa2_voice_error
};
static IXAudio2VoiceCallback g_xa2_callback = { &g_xa2_callbacks };

/* Engine callback: the device went away (unplugged, default format change,
 * exclusive-mode app, ...). Called on an XAudio2 thread: only flag + wake. */
static void STDMETHODCALLTYPE xa2_eng_pass_start(IXAudio2EngineCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_eng_pass_end(IXAudio2EngineCallback *self) { (void)self; }
static void STDMETHODCALLTYPE xa2_eng_critical(IXAudio2EngineCallback *self, HRESULT error)
{
    (void)self;
    if (!InterlockedCompareExchange(&g_xa2_error, error ? error : (LONG)0x88960004, 0))
        fprintf(stderr, "[XA2] critical error 0x%08lX (output device lost); audio will reopen on the default device\n",
                (unsigned long)error);
    if (g_xa2_buffer_event) SetEvent(g_xa2_buffer_event);
}
static IXAudio2EngineCallbackVtbl g_xa2_engine_vtbl = {
    xa2_eng_pass_start, xa2_eng_pass_end, xa2_eng_critical
};
static IXAudio2EngineCallback g_xa2_engine_cb = { &g_xa2_engine_vtbl };

int xa2_init(void)
{
    if (g_xa2_initialized) return 1;
    g_xa2_final = 0;
    return xa2_open();
}

static int xa2_open(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    xa2_read_hooks();
    if (!g_xa2_t0) g_xa2_t0 = GetTickCount64();
    g_xa2_last_open_try = GetTickCount64();
    if (g_xa2_hook_init_fail_s >= 0 && xa2_since_t0_ms() < (ULONGLONG)g_xa2_hook_init_fail_s * 1000) {
        if (!g_xa2_hook_init_logged) {
            g_xa2_hook_init_logged = 1;
            fprintf(stderr, "[XA2] TEST: simulating a missing output device for %d s\n", g_xa2_hook_init_fail_s);
        }
        return 0;
    }
    if (!g_xa2_buffer_event) g_xa2_buffer_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_xa2_buffer_event) return 0;
    memset((void *)g_xa2_busy, 0, sizeof(g_xa2_busy));
    g_xa2_error = 0;
    g_xa2_dead = 0;
    g_xa2_stalled = 0;
    g_xa2_stall_ms = 0;
    g_xa2_bad_since = 0;
    g_xa2_started = 0;
    g_xa2_was_empty = 0;
    g_xa2_starved = g_xa2_backpressure = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        xa2_teardown(0);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }
    if (SUCCEEDED(IXAudio2_RegisterForCallbacks(g_xa2, &g_xa2_engine_cb)))
        g_xa2_engine_cb_set = 1;
    else
        fprintf(stderr, "[XA2] RegisterForCallbacks failed; device loss will be caught by the stall timeout only\n");

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &g_xa2_callback, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_dead = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    xa2_log_endpoint();
    return 1;

fail:
    xa2_teardown(0);
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    g_xa2_final = 1;
    xa2_teardown(1);
}

static void xa2_teardown(int close_event)
{
    if (g_xa2 && g_xa2_engine_cb_set) {
        IXAudio2_UnregisterForCallbacks(g_xa2, &g_xa2_engine_cb);
        g_xa2_engine_cb_set = 0;
    }
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
    if (close_event && g_xa2_buffer_event) {
        CloseHandle(g_xa2_buffer_event);
        g_xa2_buffer_event = NULL;
    }
}

/* Device-loss recovery, APU thread only. g_xa2_initialized stays 1 across a
 * reopen so apu_core.c never sees the backend flip between its calls. */
static void xa2_maintain(void)
{
    ULONGLONG now = GetTickCount64();
    int bad = g_xa2_error || g_xa2_stalled || g_xa2_dead;

    if (!bad) { g_xa2_bad_since = 0; return; }
    if (!g_xa2_bad_since) g_xa2_bad_since = now;
    if (g_xa2_final) return;
    if (now - g_xa2_bad_since < XA2_REOPEN_MS || now - g_xa2_last_open_try < XA2_REOPEN_MS) return;

    /* W-09a: a missing device retries every XA2_REOPEN_MS for as long as it is gone; log the first 5
     * attempts, then one a minute. */
    static unsigned s_reopen_lines;
    static ULONGLONG s_reopen_line_at;
    int say = s_reopen_lines < 5 || now - s_reopen_line_at >= 60000;
    if (say) { s_reopen_lines++; s_reopen_line_at = now; }
    if (say) fprintf(stderr, "[XA2] reopening the default output device (error=%08lX stalled=%d dead=%d, trouble for %u ms)\n",
            (unsigned long)g_xa2_error, g_xa2_stalled, (int)g_xa2_dead,
            (unsigned)(now - g_xa2_bad_since));
    xa2_teardown(0);
    if (xa2_open()) {
        g_xa2_reopens++;
        fprintf(stderr, "[XA2] output reopened (reopen #%u, %lld blocks written so far, %u dropped)\n",
                g_xa2_reopens, g_xa2_total_written, g_xa2_dropped);
    } else {
        g_xa2_initialized = 1;   /* stay "active": blocks are dropped, retry in XA2_REOPEN_MS */
        g_xa2_dead = 1;
        g_xa2_bad_since = now;
        if (say) fprintf(stderr, "[XA2] reopen failed; retrying in %d ms (logged 5 times, then once a minute)\n", XA2_REOPEN_MS);
    }
}

int xa2_is_active(void)
{
    /* Device missing at start-up: retry from the monitor frame (APU thread).
     * Only a 0 -> 1 flip happens here, and only on the first call of a frame. */
    if (!g_xa2_initialized && !g_xa2_final && g_xa2_hooks_read &&
        GetTickCount64() - g_xa2_last_open_try >= XA2_REOPEN_MS) {
        if (xa2_open())
            fprintf(stderr, "[XA2] output device found; XAudio2 started after %llu ms\n", xa2_since_t0_ms());
    }
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized) return 0;
    if (g_xa2_hook_fail_at >= 0 && !g_xa2_hook_fail_done &&
        xa2_since_t0_ms() >= (ULONGLONG)g_xa2_hook_fail_at * 1000) {
        g_xa2_hook_fail_done = 1;
        fprintf(stderr, "[XA2] TEST: simulating device loss now\n");
        xa2_eng_critical(NULL, (HRESULT)0x88960004);
    }
    xa2_maintain();
    if (!g_xa2_initialized || !g_xa2_source || g_xa2_error || g_xa2_dead ||
        num_samples <= 0 || num_samples > XA2_BUF_SAMPLES) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (g_xa2_started && !state.BuffersQueued && !g_xa2_was_empty) {
        g_xa2_starved++;
        g_xa2_starved_total++;
    }
    g_xa2_was_empty = !state.BuffersQueued;

    idx = g_xa2_next_buf;
    for (int tried = 0; tried < XA2_NUM_BUFS; tried++) {
        if (!InterlockedCompareExchange(&g_xa2_busy[idx], 1, 0)) break;
        idx = (idx + 1) % XA2_NUM_BUFS;
        if (tried == XA2_NUM_BUFS - 1) { g_xa2_backpressure++; g_xa2_bp_total++; return 0; }
    }
    copy_samples = num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];
    xbuf.pContext = (void *)(uintptr_t)(idx + 1);

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) {
        InterlockedExchange(&g_xa2_busy[idx], 0);
        InterlockedExchange(&g_xa2_error, hr);
        return 0;
    }

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    g_xa2_total_written++;
    if (g_xa2_stalled) {
        g_xa2_stalled = 0;
        fprintf(stderr, "[XA2] output resumed after a stall (%u dropped blocks so far)\n", g_xa2_dropped);
    }
    g_xa2_stall_ms = 0;
    /* Fill two blocks before starting, so initial scheduling jitter does not
     * create an underrun immediately after the first submission. */
    if (!g_xa2_started && g_xa2_frames_written >= 2) {
        hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        if (FAILED(hr)) { InterlockedExchange(&g_xa2_error, hr); return 0; }
        g_xa2_started = 1;
    }
    if (getenv("RECOMP_AUDIO_STATS")) {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 5000) {
            XAUDIO2_PERFORMANCE_DATA performance;
            IXAudio2_GetPerformanceData(g_xa2,&performance);
            fprintf(stderr, "[XA2] submitted=%d queued=%u underruns=%u backpressure=%u engine-glitches=%u error=%08lX total=%lld dropped=%u stalls=%u reopens=%u\n",
                g_xa2_frames_written, state.BuffersQueued + 1, g_xa2_starved,
                g_xa2_backpressure, performance.GlitchesSinceEngineStarted,g_xa2_error,
                g_xa2_total_written, g_xa2_dropped, g_xa2_stalls, g_xa2_reopens);
            fflush(stderr); last = now;
        }
    }
    return 1;
}

/* Returns 1 when a buffer is (probably) free, 0 on a short timeout, -1 when
 * the caller must drop the block: device lost, reopen pending, or the output
 * has not acked a buffer for XA2_STALL_MS (about 12 blocks). */
int xa2_wait_for_space(int milliseconds)
{
    if (!g_xa2_initialized) return -1;
    if (g_xa2_error || g_xa2_dead) { g_xa2_dropped++; return -1; }
    for (int i = 0; i < XA2_NUM_BUFS; i++)
        if (!InterlockedCompareExchange(&g_xa2_busy[i], 0, 0)) {
            g_xa2_stall_ms = 0;
            return 1;
        }
    if (g_xa2_stalled) { g_xa2_dropped++; return -1; }
    if (WaitForSingleObject(g_xa2_buffer_event, milliseconds) == WAIT_OBJECT_0) {
        if (g_xa2_error) { g_xa2_dropped++; return -1; }
        g_xa2_stall_ms = 0;
        return 1;
    }
    g_xa2_stall_ms += milliseconds > 0 ? milliseconds : 1;
    if (g_xa2_stall_ms >= XA2_STALL_MS) {
        g_xa2_stalled = 1;
        g_xa2_stalls++;
        g_xa2_dropped++;
        fprintf(stderr, "[XA2] output stalled %d ms, dropping blocks until the device recovers\n", g_xa2_stall_ms);
        return -1;
    }
    return 0;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

/* 1.1.6 [HEALTH] (APU thread): session totals, unaffected by device reopens. */
void xa2_health_counters(unsigned *underruns, unsigned *backpressure, unsigned *dropped,
                         unsigned *stalls, unsigned *reopens)
{
    if (underruns) *underruns = g_xa2_starved_total;
    if (backpressure) *backpressure = g_xa2_bp_total;
    if (dropped) *dropped = g_xa2_dropped;
    if (stalls) *stalls = g_xa2_stalls;
    if (reopens) *reopens = g_xa2_reopens;
}

/* 1.1.6 (APU thread): the APU thread was idle (guest stopped the APU, or mcpx_apu_hold) and the
 * output queue ran dry on purpose; do not count the first empty queue after it as an underrun. */
void xa2_note_idle(void)
{
    g_xa2_was_empty = 1;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }
void xa2_health_counters(unsigned *u, unsigned *b, unsigned *d, unsigned *s, unsigned *r)
{ if (u) *u = 0; if (b) *b = 0; if (d) *d = 0; if (s) *s = 0; if (r) *r = 0; }
void xa2_note_idle(void)                              {}
int  xa2_wait_for_space(int milliseconds)             { (void)milliseconds; return -1; }

#endif /* _WIN32 */

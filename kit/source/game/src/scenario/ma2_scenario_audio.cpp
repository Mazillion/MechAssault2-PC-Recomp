// Imported audio clips for custom scenarios (see ma2_scenario.c, SPEC.md [Audio]).
//
// A scenario's MAActionSound (CueEvent, object +0x68) or MAActionPlayVoice (CueID, +0x78) may name
// one of its own clips: cue 0x5C100000 + slot*0x1000 + n after ma2_scenario.c made the ids global.
// The retail cue lookup leaves such actions without a resolved cue, so their Execute returns
// early; the hooks at the top of both Execute functions (recomp_0033.c / recomp_0042.c) hand the
// id here first. Clips play as 2D dialogue on a host XAudio2 voice at the [Audio] Volume setting.
// RECOMP_AUDIO_SILENT=1 (tests) or [Audio] Enabled=0 mutes them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdint>

extern "C" int ma2_pc_ini_get_int(const char *section, const char *key, int fallback);
extern "C" const uint8_t *ma2_scn_clip(uint32_t cue, uint32_t *size);   // ma2_scenario.c

namespace {
std::mutex g_lock;
IXAudio2 *g_xa;
IXAudio2MasteringVoice *g_master;
bool g_failed;
struct Playing { IXAudio2SourceVoice *voice; uint32_t cue; };
std::vector<Playing> g_playing;

bool audio_init()
{
    if (g_xa) return true;
    if (g_failed) return false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_xa->CreateMasteringVoice(&g_master))) {
        if (g_xa) { g_xa->Release(); g_xa = nullptr; }
        g_failed = true;
        fprintf(stderr, "[SCENARIO] audio: XAudio2 unavailable, clips muted\n");
        return false;
    }
    return true;
}

void reap()
{
    for (size_t i = 0; i < g_playing.size();) {
        XAUDIO2_VOICE_STATE st{};
        g_playing[i].voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (!st.BuffersQueued) { g_playing[i].voice->DestroyVoice(); g_playing.erase(g_playing.begin() + i); }
        else i++;
    }
}

// RIFF/WAVE, PCM 16-bit (8-bit and IEEE float accepted too).
bool parse_wav(const uint8_t *p, uint32_t n, WAVEFORMATEX *fmt, const uint8_t **data, uint32_t *bytes)
{
    if (n < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return false;
    bool have_fmt = false; *data = nullptr;
    for (uint32_t pos = 12; pos + 8 <= n;) {
        uint32_t len; memcpy(&len, p + pos + 4, 4);
        const uint8_t *body = p + pos + 8;
        if (pos + 8ull + len > n) len = n - pos - 8;
        if (!memcmp(p + pos, "fmt ", 4) && len >= 16) {
            memset(fmt, 0, sizeof(*fmt));
            memcpy(fmt, body, 16);
            if (fmt->wFormatTag == 0xFFFE && len >= 26) fmt->wFormatTag = (WORD)(body[24] | (body[25] << 8)); // WAVE_FORMAT_EXTENSIBLE
            fmt->cbSize = 0;
            have_fmt = true;
        } else if (!memcmp(p + pos, "data", 4)) {
            *data = body; *bytes = len;
        }
        pos += 8 + len + (len & 1);
    }
    return have_fmt && *data && (fmt->wFormatTag == WAVE_FORMAT_PCM || fmt->wFormatTag == 3) &&
           fmt->nChannels >= 1 && fmt->nChannels <= 2 && fmt->nSamplesPerSec >= 8000 && fmt->nSamplesPerSec <= 96000;
}
}  // namespace

extern "C" int ma2_scn_audio_play(uint32_t cue)
{
    uint32_t size = 0;
    const uint8_t *clip = ma2_scn_clip(cue, &size);
    if (!clip) return 0;
    const char *silent = getenv("RECOMP_AUDIO_SILENT");
    if ((silent && *silent && strcmp(silent, "0")) || !ma2_pc_ini_get_int("Audio", "Enabled", 1)) {
        fprintf(stderr, "[SCENARIO] clip %08X (%u bytes) muted by settings\n", cue, size);
        return 1;
    }
    WAVEFORMATEX fmt; const uint8_t *pcm; uint32_t bytes;
    if (!parse_wav(clip, size, &fmt, &pcm, &bytes)) {
        fprintf(stderr, "[SCENARIO] clip %08X: not a PCM WAV\n", cue);
        return 1;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    if (!audio_init()) return 1;
    reap();
    IXAudio2SourceVoice *voice = nullptr;
    if (FAILED(g_xa->CreateSourceVoice(&voice, &fmt))) {
        fprintf(stderr, "[SCENARIO] clip %08X: CreateSourceVoice failed\n", cue);
        return 1;
    }
    int volume = ma2_pc_ini_get_int("Audio", "Volume", 100);
    if (volume < 0) volume = 0; if (volume > 100) volume = 100;
    voice->SetVolume((float)volume / 100.0f);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = bytes; buffer.pAudioData = pcm; buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (FAILED(voice->SubmitSourceBuffer(&buffer)) || FAILED(voice->Start(0))) {
        voice->DestroyVoice();
        return 1;
    }
    g_playing.push_back({voice, cue});
    fprintf(stderr, "[SCENARIO] clip %08X playing: %u Hz, %u ch, %u bytes, volume %d\n",
            cue, (unsigned)fmt.nSamplesPerSec, (unsigned)fmt.nChannels, bytes, volume);
    return 1;
}

// While the Scenario Editor opened from Custom Scenarios is up, the game's own sound (menu music
// from the APU, clips above) is muted at the Windows mixer: every audio session of this process on
// the default output device (ISimpleAudioVolume), restored on return.
#include <mmdeviceapi.h>
#include <audiopolicy.h>
extern "C" void ma2_scn_audio_mute_process(int mute)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = nullptr; IMMDevice *dev = nullptr; IAudioSessionManager2 *mgr = nullptr;
    IAudioSessionEnumerator *list = nullptr;
    int n = 0, done = 0;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en)) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&list)) && SUCCEEDED(list->GetCount(&n))) {
        for (int i = 0; i < n; i++) {
            IAudioSessionControl *c = nullptr; IAudioSessionControl2 *c2 = nullptr; ISimpleAudioVolume *v = nullptr;
            DWORD pid = 0;
            if (SUCCEEDED(list->GetSession(i, &c)) && SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&c2)) &&
                SUCCEEDED(c2->GetProcessId(&pid)) && pid == GetCurrentProcessId() &&
                SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void **)&v))) {
                v->SetMute(mute ? TRUE : FALSE, nullptr);
                done++;
            }
            if (v) v->Release();
            if (c2) c2->Release();
            if (c) c->Release();
        }
    }
    if (list) list->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    fprintf(stderr, "[SCENARIO] editor: game sound %s (%d session(s))\n", mute ? "muted" : "restored", done);
}

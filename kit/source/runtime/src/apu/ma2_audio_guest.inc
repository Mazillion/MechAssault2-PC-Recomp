/* ma2_audio_guest.inc -- guest DirectSound/XACT helpers moved verbatim out of src\recomp_manual.c
 * (pipeline M0, 2026-10-31): the API lock 0x3C4598, the APU ISR try-enter, the stop assist and the
 * B11 bounded spin-waits. Owned by pipeline track T2 (executor + audio); recomp_manual.c belongs to T1.
 *
 * Not a translation unit: recomp_manual.c #includes it twice, at the two places the code used to be,
 * with MA2_AUDIO_GUEST_PART set to 1 (lock, ISR, stop assist) and then 2 (bounded waits), so the
 * compiled object is unchanged. It relies on what recomp_manual.c defines above each point:
 * GUEST32, g_xbox_mem_offset, g_ebp/g_seh_ebp, ma2_diag(DIAG_AUDIO) and the <xbox/xboxrecomp.h>,
 * <stdio.h> and <stdlib.h> includes (T1 keeps those). Callers: recomp_0095.c (ISR), recomp_0096.c
 * (stop spin; NEW_VOICE waits 0x3AEA7F/0x3AF1D4), recomp_0098.c (XACT packet wait 0x3E51DB).
 * In the kit, src\apu\ lands in runtime\src\apu\, which the game target finds through -I runtime\src. */
/* A0 wait counters (pipeline T2-0), in apu_core.c: g_apu_ws is on under RECOMP_AUDIO_WAIT_STATS or
 * RECOMP_PERF (read once at APU init); off, each hook below costs one branch. Sites: 0/1 the two NEW_VOICE
 * waits, 3 the XACT packet wait, 4/5 stop-assist passes / releases (2 is the DSP command spin, recomp_0095.c). */
extern int g_apu_ws;
extern double apu_ws_now_ms(void);
extern void apu_ws_record(int site, double ms, unsigned iters);
#if !defined(MA2_AUDIO_GUEST_PART)
#error "ma2_audio_guest.inc is included by recomp_manual.c with MA2_AUDIO_GUEST_PART set to 1 or 2"
#elif MA2_AUDIO_GUEST_PART == 1

/* Xbox DirectSound's DPC cannot overlap a passive-level destructor on its
 * single guest CPU. Our native guest threads can run concurrently. Share the
 * SDK's existing recursive API lock for API calls at every native IRQL and
 * while draining deferred audio objects; the original API helper omits it
 * above PASSIVE_LEVEL because the Xbox CPU cannot run another guest thread.
 * Otherwise a buffer's vtable can change while the DPC is invoking it. Device
 * IRQs keep their separate hardware lock and can complete the driver's work. */
void ma2_audio_work_enter(void)
{
    xbox_RtlEnterCriticalSection((PRTL_CRITICAL_SECTION)
        (g_xbox_mem_offset+(uint32_t)0x3C4598));
}
void ma2_audio_work_leave(void)
{
    xbox_RtlLeaveCriticalSection((PRTL_CRITICAL_SECTION)
        (g_xbox_mem_offset+(uint32_t)0x3C4598));
}

/* The APU interrupt (0x003AB500, voice notification 0x8000) releases a
 * finished voice itself: 0x003AAEE1 unlinks it from the DPC's active list
 * (+0x488) and queues it on the deferred list (+0x4A0). On the Xbox the ISR
 * runs atomically on the one CPU, and it already skips while the DPC is
 * walking (+0x84 busy), leaving the voice to the DPC's own poll of the voice
 * state (0x003AECB7). Here the interrupt runs on the APU thread while the DPC
 * and the title's API calls run on others, so its unlink could interleave
 * with theirs: two neighbours unlinked at once left the list head pointing at
 * a self-linked voice, and the DPC's walk span on it forever holding the API
 * lock (the match froze, every other thread waiting on 0x3C4598) or followed a
 * stale link into freed memory (the audio thread crashed). Take the API lock
 * only if it is free; when another thread holds it, skip exactly as the ISR
 * does when the DPC is busy. */
BOOLEAN __stdcall xbox_RtlTryEnterCriticalSectionShadow(PRTL_CRITICAL_SECTION);
static unsigned ma2_audio_list_len(uint32_t head)
{
    unsigned n = 0;
    for (uint32_t e = GUEST32(head); e != head && e >= 0x10000 && e < 0x03FFFF00 && n < 1024; e = GUEST32(e)) n++;
    return n;
}
int ma2_audio_isr_try_enter(uint32_t apu)
{
    static int stats = -1;
    static LONG tries, skips;
    int ok = xbox_RtlTryEnterCriticalSectionShadow((PRTL_CRITICAL_SECTION)
        (g_xbox_mem_offset+(uint32_t)0x3C4598)) ? 1 : 0;
    if (stats < 0) stats = getenv("RECOMP_AUDIO_ISR_STATS") != NULL;
    if (stats) {
        LONG t = InterlockedIncrement(&tries);
        if (!ok) InterlockedIncrement(&skips);
        if (t % 500 == 0 && ok && apu >= 0x10000 && apu < 0x03FFF000)   /* lists are ours to read while we hold the lock */
            fprintf(stderr, "[AUDIO-ISR] voice releases tried=%ld skipped=%ld active=%u/%u/%u deferred=%u\n", t, skips,
                    ma2_audio_list_len(apu + 0x488), ma2_audio_list_len(apu + 0x490), ma2_audio_list_len(apu + 0x498), ma2_audio_list_len(apu + 0x4A0));
    }
    return ok;
}

/* The other side of that skip (2026-10-15). DirectSound stops a voice by marking it stopping
 * (+0x12 bit 0x8000) and spinning in 0x003AE0D2 until the interrupt's 0x003AAEE1 releases it --
 * while holding the API lock. The interrupt then finds the lock taken and skips; the DPC that
 * would poll the voice instead waits for the same lock; so the spin only ends through
 * DirectSound's own 0.5 s timeout (0x003AD46A), again and again: freezes of 1-7 s in big
 * fights (an explosion stopping many sounds), [VOICE-TIMEOUT] repeating on one voice. On the
 * Xbox the interrupt would simply preempt the spinning thread. So the spinning thread, which
 * owns the lock and is at a safe point, runs the interrupt's release for its own voice each
 * pass; 0x003AAEE1 still does nothing until the hardware voice has really stopped.
 * RECOMP_AUDIO_STOP_ASSIST=0 turns it off. */
static volatile uint32_t g_ma2_dsound_core;
void ma2_audio_note_core(uint32_t core)
{
    if (core >= 0x10000 && core < 0x03FFF000 && g_ma2_dsound_core != core) g_ma2_dsound_core = core;
}
uint32_t ma2_audio_stop_assist(uint32_t object, uint32_t *voice)
{
    static int off = -1;
    if (off < 0) { const char *e = getenv("RECOMP_AUDIO_STOP_ASSIST"); off = e && *e == '0'; }
    uint32_t core = g_ma2_dsound_core;
    /* Voice objects live in ordinary RAM or in contiguous memory (0x80000000 up: the streamed
     * and XACT voices -- most of the timeouts in a fight). */
    if (off || !core || !((object >= 0x10000 && object < 0x03FFFF00) || (object >= 0x80000000u && object < 0x83FFFF00u))) return 0;
    if (!(*(uint16_t *)(g_xbox_mem_offset + object + 0x12) & 0x8000)) return 0;
    uint32_t n = *(uint8_t *)(g_xbox_mem_offset + object + 0x64);
    if (n > 3) return 0;
    uint32_t v = *(uint16_t *)(g_xbox_mem_offset + object + 0xA + n * 2);
    if (v >= 0x100) return 0;
    *voice = v;
    return core;
}
void ma2_audio_stop_assist_done(uint32_t object, uint32_t voice)
{
    static LONG passes, released;
    InterlockedIncrement(&passes);
    if (g_apu_ws) {   /* A0: passes, and the time from this thread's first pass on the object to its release */
        static __declspec(thread) uint32_t aw_object;
        static __declspec(thread) double aw_t0;
        if (aw_object != object) { aw_object = object; aw_t0 = apu_ws_now_ms(); }
        apu_ws_record(4, 0.0, 0);
        if (!(*(uint16_t *)(g_xbox_mem_offset + object + 0x12) & 0x8000)) { apu_ws_record(5, apu_ws_now_ms() - aw_t0, 0); aw_object = 0; }
    }
    if (*(uint16_t *)(g_xbox_mem_offset + object + 0x12) & 0x8000) return;
    LONG n = InterlockedIncrement(&released);
    if (n <= 8 || (n & 255) == 0)
        fprintf(stderr, "[AUDIO-STOP] voice %u released by the thread waiting on it (%ld so far, %ld passes)\n",
                voice, n, passes);
}

/* Deferred-list double insert (2026-11-09). A finished voice is queued for the DPC by linking its
 * entry +0x54 onto the DirectSound core's deferred list +0x4A0 (0x003AD345). On the Xbox the DPC
 * drains that list right after the interrupt, before the title can restart the voice. Here the
 * DPC runs on another host thread, so a short voice can be restarted and finish again (interrupt
 * release, or a stop-assist pass) while its entry is still queued. The second tail insert
 * self-links the entry and leaves the list head pointing at it; RemoveEntryList (0x003A65A0) then
 * hands the same voice back forever, so the drain loop 0x003AACE7 calls its poll and Release
 * until the voice is destroyed, then calls slot 6 of the base vtable 0x453448 (sub_3A89C5) with
 * stack garbage: the audio-thread access violation reading 0xFE000268 (eax=1, ebx=core+0x4A0).
 * A voice already queued gets no second entry: the queued one already runs the voice's poll, and
 * that poll reads the voice's current state. RECOMP_AUDIO_DEFER_GUARD=0 turns both checks off. */
static int ma2_audio_defer_guard_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_AUDIO_DEFER_GUARD"); on = !(e && *e == '0'); }
    return on;
}
static int ma2_audio_guest_ptr(uint32_t va)
{
    return (va >= 0x10000 && va < 0x03FFFF00) || (va >= 0x80000000u && va < 0x83FFFF00u);
}
/* 0x003AD345, before the retain: 0 = the voice is already on the deferred list, skip the insert. */
int ma2_audio_defer_insert_ok(uint32_t voice)
{
    static LONG skipped;
    uint32_t entry = voice + 0x54, f;
    if (!ma2_audio_defer_guard_on() || !ma2_audio_guest_ptr(voice)) return 1;
    f = GUEST32(entry);
    if (f == entry || f == 0 || !ma2_audio_guest_ptr(f)) return 1;
    /* Only a real member counts: walk the core's deferred list (callers hold the API lock). An entry
     * unlinked elsewhere without being self-linked again is stale, and the insert goes ahead. */
    uint32_t core = GUEST32(voice + 8), head = core + 0x4A0, e;
    unsigned k = 0;
    if (!ma2_audio_guest_ptr(core)) return 1;
    for (e = GUEST32(head); e != head && e != entry && ma2_audio_guest_ptr(e) && k < 4096; e = GUEST32(e)) k++;
    if (e != entry) {
        static LONG stale;
        LONG s = InterlockedIncrement(&stale);
        if (s <= 4 || (s & 255) == 0)
            fprintf(stderr, "[AUDIO-DEFER] voice object %08X entry links %08X but is not on the deferred list; inserted (%ld so far)\n", voice, f, s);
        return 1;
    }
    LONG n = InterlockedIncrement(&skipped);
    if (n <= 8 || (n & 255) == 0)
        fprintf(stderr, "[AUDIO-DEFER] voice object %08X (hw voice %u) finished again while still queued (position %u); second insert skipped (%ld so far)\n",
                voice, (unsigned)*(uint16_t *)(g_xbox_mem_offset + voice + 0xC), k, n);
    return 0;
}
/* 0x003AAD51, after RemoveEntryList returned entry != head: 0 = do not call into this object. */
int ma2_audio_defer_drain_ok(uint32_t head, uint32_t entry)
{
    static LONG repaired, destroyed;
    uint32_t object = entry - 0x54;
    if (!ma2_audio_defer_guard_on()) return 1;
    if (GUEST32(head) == entry) {
        /* The removal found the entry self-linked, so the list was already corrupt: empty the head
         * (entries cut off behind it keep one leaked reference each) and process this one once. */
        GUEST32(head) = head;
        GUEST32(head + 4) = head;
        LONG n = InterlockedIncrement(&repaired);
        if (n <= 8 || (n & 255) == 0)
            fprintf(stderr, "[AUDIO-DEFER] deferred list %08X head pointed at self-linked entry %08X; list reset (%ld so far)\n",
                    head, entry, n);
    }
    if (!ma2_audio_guest_ptr(object) || GUEST32(object) == 0x453448u || !ma2_audio_guest_ptr(GUEST32(object))) {
        LONG n = InterlockedIncrement(&destroyed);
        if (n <= 8 || (n & 255) == 0)
            fprintf(stderr, "[AUDIO-DEFER] deferred object %08X table %08X is destroyed or invalid; callback and Release skipped (%ld so far)\n",
                    object, ma2_audio_guest_ptr(object) ? GUEST32(object) : 0, n);
        return 0;
    }
    return 1;
}

/* ActiveGuard (2026-11-09): the game/match freeze in big fights (Not It, LMS, Going Commando: the
 * sim stops, the menu stops pulsing, only the APU thread's KeInsertQueueDpc keeps counting).
 * A playing voice sits on one of the DirectSound core's active lists (+0x488/+0x490/+0x498) through
 * its entry +0x4C, which the interrupt's release (0x003AAEE1 -> 0x003AD1A5) unlinks when the hardware
 * voice stops. Here that release is skipped while another thread holds the API lock (the ISR
 * try-enter above), and the stop-wait 0x003AE0D2 can end on DirectSound's own timeout, so a voice can
 * be started again (0x003AE5C7) while its entry is still linked. The start head-inserts the entry
 * again (0x003AE754); when it was already first, that self-links it and leaves the head pointing at
 * it. The DPC's active-list walk (0x003AACE7) restarts from the head whenever the current entry is
 * self-linked, so it polls that voice forever while holding the API lock 0x3C4598, and the game
 * thread blocks on that lock at its next DirectSound or XACT call. Proven on run h3_cu1: the game
 * thread waits on 0x3C4598 at 0x3DDECB (XACT raise lock), held by the DPC thread sampled three times
 * in 0x3AACE7/0x3AECB7. On the Xbox the interrupt preempts the title, so a stopped voice is always
 * unlinked before it can start again.
 * Fix: the start unlinks a still-linked entry before inserting it (what the skipped release would
 * have done to the list); the walk rebuilds a list whose head points at a self-linked entry from the
 * back links, which that insert leaves intact. Logs [AUDIO-ACTIVE]; RECOMP_AUDIO_ACTIVE_GUARD=0 off. */
static int ma2_audio_active_guard_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_AUDIO_ACTIVE_GUARD"); on = !(e && *e == '0'); }
    return on;
}
/* 0x003AE754, before the insert: entry = voice +0x4C. */
void ma2_audio_active_preinsert(uint32_t entry)
{
    static LONG unlinked, odd;
    uint32_t f, b;
    if (!ma2_audio_active_guard_on() || !ma2_audio_guest_ptr(entry)) return;
    f = GUEST32(entry); b = GUEST32(entry + 4);
    if (f == entry || f == 0) return;                       /* not linked: the normal case */
    if (ma2_audio_guest_ptr(f) && ma2_audio_guest_ptr(b) && GUEST32(b) == entry && GUEST32(f + 4) == entry) {
        GUEST32(b) = f;                                     /* RemoveEntryList */
        GUEST32(f + 4) = b;
        GUEST32(entry) = entry;
        GUEST32(entry + 4) = entry;
        LONG n = InterlockedIncrement(&unlinked);
        if (n <= 8 || (n & 255) == 0)
            fprintf(stderr, "[AUDIO-ACTIVE] voice object %08X (hw voice %u) started while still on an active list; unlinked first (%ld so far)\n",
                    entry - 0x4C, (unsigned)*(uint16_t *)(g_xbox_mem_offset + entry - 0x4C + 0xC), n);
        return;
    }
    /* Linked but its neighbours do not point back: leave the list to the walk's repair below. */
    LONG n = InterlockedIncrement(&odd);
    if (n <= 8 || (n & 255) == 0)
        fprintf(stderr, "[AUDIO-ACTIVE] voice object %08X entry links %08X/%08X are inconsistent; inserted as is (%ld so far)\n",
                entry - 0x4C, f, b, n);
}
/* 0x003AAD1D, the walk found entry self-linked: 1 = the head pointed at it and the list was rebuilt. */
int ma2_audio_active_repair(uint32_t head, uint32_t entry)
{
    static LONG repaired;
    uint32_t cur, next = head, first = head, p;
    unsigned n = 0;
    if (!ma2_audio_active_guard_on() || GUEST32(head) != entry || GUEST32(entry) != entry) return 0;
    for (cur = GUEST32(head + 4); ; cur = p) {
        if (cur == head || !ma2_audio_guest_ptr(cur) || ++n > 1024) { first = next; break; }
        GUEST32(cur) = next;                                /* Flink rebuilt from the back chain */
        p = GUEST32(cur + 4);
        if (p == cur) { GUEST32(cur + 4) = head; first = cur; break; }   /* the self-linked entry: front */
        next = cur;
    }
    if (n > 1024) first = head;                             /* back chain loops too: empty the list */
    GUEST32(head) = first;
    if (first != head) GUEST32(first + 4) = head;
    else GUEST32(head + 4) = head;
    LONG k = InterlockedIncrement(&repaired);
    if (k <= 8 || (k & 255) == 0)
        fprintf(stderr, "[AUDIO-ACTIVE] active list %08X head pointed at self-linked entry %08X; rebuilt from back links, %u entries (%ld so far)\n",
                head, entry, n, k);
    return 1;
}

#elif MA2_AUDIO_GUEST_PART == 2

/* ── B11.3/B11.4: bounded audio spin-waits ───────────────────────────────
 * Three DirectSound/XACT loops spin with no yield and no timeout on state that
 * another thread clears: two voice starts wait for the APU frame thread to take
 * NEW_VOICE (recomp_0096.c 0x3AEA7F, 0x3AF1D4), and an XACT flush waits for two
 * packet statuses to leave 0x103 (recomp_0098.c 0x3E51DB). Spin politely
 * (YieldProcessor ~1k, then SwitchToThread) and give up after 250 ms with a log
 * line, which turns a silent freeze into a logged stall. RECOMP_AUDIO_WAIT_MS
 * sets the bound (0 = unbounded, the original behaviour). */
static unsigned ma2_audio_wait_limit_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char *e = getenv("RECOMP_AUDIO_WAIT_MS");
        ms = (e && *e) ? atoi(e) : 250;
        if (ms < 0) ms = 0;
    }
    return (unsigned)ms;
}
/* Returns the wait in ms; *gave_up says whether the bound ended it. */
static double ma2_bounded_spin(volatile uint32_t *a, uint32_t amask, uint32_t aval,
                               volatile uint32_t *b, uint32_t bval, int *gave_up)
{
    LARGE_INTEGER f, t0, t;
    unsigned spins = 0, limit = ma2_audio_wait_limit_ms();
    *gave_up = 0;
#define MA2_STILL_WAITING() (((*a & amask) == aval) || (b && *b == bval))
    if (!MA2_STILL_WAITING()) return 0.0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        if (!MA2_STILL_WAITING()) break;
        if (spins < 1024) { spins++; YieldProcessor(); continue; }
        SwitchToThread();
        QueryPerformanceCounter(&t);
        if (limit && (double)(t.QuadPart - t0.QuadPart) * 1000.0 >= (double)limit * (double)f.QuadPart) {
            *gave_up = 1;
            break;
        }
    }
#undef MA2_STILL_WAITING
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}
void ma2_voice_new_wait(uint32_t site, uint32_t va, volatile uint32_t *state)
{
    static volatile LONG gave_ups, slow;
    int gave_up;
    double ms = ma2_bounded_spin(state, 0x00100000u, 0x00100000u, NULL, 0, &gave_up);
    if (g_apu_ws) apu_ws_record(site == 0x003AEA7Fu ? 0 : 1, ms, 0);
    if (gave_up) {
        LONG n = InterlockedIncrement(&gave_ups);
        if (n <= 8 || (n & 255) == 0)
            fprintf(stderr, "[VOICE-NEW-WAIT] site=%08X state=%08X par=%08X NEW_VOICE still set after %.0f ms; continuing (%ld so far)\n",
                    site, va, *state, ms, n);
    } else if (ms >= 20.0 && ma2_diag(DIAG_AUDIO) && InterlockedIncrement(&slow) <= 40) {
        fprintf(stderr, "[VOICE-NEW-WAIT] site=%08X state=%08X slow wait %.1f ms\n", site, va, ms);
    }
}
void ma2_xact_packet_wait(uint32_t site, uint32_t va, volatile uint32_t *st10, volatile uint32_t *st24)
{
    static volatile LONG gave_ups, slow;
    int gave_up;
    double ms = ma2_bounded_spin(st10, 0xFFFFFFFFu, 0x103u, st24, 0x103u, &gave_up);
    if (g_apu_ws) apu_ws_record(3, ms, 0);
    if (gave_up) {
        LONG n = InterlockedIncrement(&gave_ups);
        if (n <= 8 || (n & 255) == 0)
            fprintf(stderr, "[XACT-WAIT] site=%08X pkt=%08X st=%08X/%08X still pending after %.0f ms; continuing (%ld so far)\n",
                    site, va, *st10, *st24, ms, n);
    } else if (ms >= 20.0 && ma2_diag(DIAG_AUDIO) && InterlockedIncrement(&slow) <= 40) {
        fprintf(stderr, "[XACT-WAIT] site=%08X pkt=%08X slow wait %.1f ms\n", site, va, ms);
    }
}

#else
#error "MA2_AUDIO_GUEST_PART must be 1 or 2"
#endif

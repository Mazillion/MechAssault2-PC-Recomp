/* Cinematic-only missions (e.g. "We're Not in Kansas Anymore") are levels
 * loaded from Cinema_*.mgf. They were staged for a 4:3 frame (most shots letterboxed to about
 * 2.33:1 inside it) at 30 Hz: a plain widened view shows past the authored set edges, and very high
 * rates change scripted motion. While one is loaded, the presenter uses the cinematic view and frame
 * cap configured below (pc_display_cinematic): by default the shot is re-rendered at full output
 * resolution with its authored width (Fill) and capped at 60 FPS.
 *
 * The level archive is not a reliable signal: on Resume it is opened relative
 * to a directory handle, so its name never reaches the path hook. Every level
 * load does open its own \Device\Harddisk0\Partition5\XXXXXXXX.cache, named
 * ~CRC32 of the lower-case "d:\<archive>" path (cinema_2_1.mgf -> 52edeee3,
 * mission_04.mgf -> 1eef6e28; the front end loads 46229b6e after
 * save-and-quit). So a level cache from the table below turns the mode on and
 * any other level cache turns it off. Control.cache and other files are
 * ignored. Regular missions keep their in-mission cutscenes inside their own
 * archive and are unaffected. RECOMP_CINEMATIC_MODE=0 disables this. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "platform/pc_frontend.h"

extern void (*g_xbox_path_hook)(const char *xbox_path);
static void (*previous_hook)(const char *xbox_path);

/* ~CRC32("d:\\cinema_*.mgf") for the 20 cinematic archives on the disc. */
static const uint32_t cinema_caches[] = {
    0x5481117Eu, /* cinema_1_1a.mgf */
    0x19C5A71Eu, /* cinema_1_3.mgf */
    0xABE57B0Eu, /* cinema_1_4.mgf */
    0x52EDEEE3u, /* cinema_2_1.mgf */
    0x282DBD83u, /* cinema_2_3.mgf */
    0x9A0D6193u, /* cinema_2_4.mgf */
    0x16A41603u, /* cinema_3_1a.mgf */
    0x51046CD3u, /* cinema_3_1b.mgf */
    0x8E5AB637u, /* cinema_3_3.mgf */
    0x3C7A6A27u, /* cinema_3_4.mgf */
    0x011A4397u, /* cinema_3_5.mgf */
    0x313DDBD9u, /* cinema_4_1.mgf */
    0x4BFD88B9u, /* cinema_4_3.mgf */
    0xF9DD54A9u, /* cinema_4_4.mgf */
    0x831D07C9u, /* cinema_4_6.mgf */
    0xD0EAAABDu, /* cinema_5_2.mgf */
    0xED8A830Du, /* cinema_5_3.mgf */
    0x5FAA5F1Du, /* cinema_5_4.mgf */
    0x62CA76ADu, /* cinema_5_5.mgf */
    0x256A0C7Du, /* cinema_5_6.mgf */
};

static int starts_with(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
    return 1;
}

/* Parses "XXXXXXXX.cache"; returns 0 for anything else. */
static int level_cache(const char *name, uint32_t *hash)
{
    uint32_t value = 0;
    for (int i = 0; i < 8; i++) {
        int c = tolower((unsigned char)name[i]);
        if (c >= '0' && c <= '9') value = value * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') value = value * 16 + (uint32_t)(c - 'a' + 10);
        else return 0;
    }
    if (!starts_with(name + 8, ".cache") || name[14]) return 0;
    *hash = value;
    return 1;
}

/* View and frame cap for cinematic levels, read once at the first level load (the INI is known by then).
 * [Display] CinematicView: 0 Pillarbox (original 4:3), 1 Fill but crop no further than 16:9,
 *   2 Fill (default), 3 Wide (like gameplay: no letterbox, wider view). Env RECOMP_CINEMATIC_VIEW overrides.
 * [Display] CinematicFrameCap: frames per second while a cinematic plays (default 60; 0 = the normal
 *   frame limit). Env RECOMP_CINEMATIC_FPS overrides. 60 because at 120-150 Hz the civilians of
 *   Cinema_1_1a block each other and stop short of their waypoint (30 and 60 Hz walk it). */
int ma2_pc_ini_get_int(const char *section, const char *key, int fallback);
static int env_or_ini(const char *env, const char *key, int fallback)
{
    const char *e = getenv(env);
    return (e && *e) ? atoi(e) : ma2_pc_ini_get_int("Display", key, fallback);
}
/* Cinemas framed by a full-screen 2D panel drawn in the 4:3 layout (a comm-screen "transmission"):
 * widened, the 3D scene behind the panel shows at both sides, so a fill view keeps these pillarboxed. */
static const uint32_t panel_cinemas[] = {
    0x19C5A71Eu, /* cinema_1_3.mgf */
    0x16A41603u, /* cinema_3_1a.mgf */
    0x011A4397u, /* cinema_3_5.mgf */
};
static int cine_view = -1, cine_fps;
static void ma2_cinematic_configure(void)
{
    cine_view = env_or_ini("RECOMP_CINEMATIC_VIEW", "CinematicView", PC_CINEMATIC_FILL);
    cine_fps = env_or_ini("RECOMP_CINEMATIC_FPS", "CinematicFrameCap", 60);
    fprintf(stderr, "[CINEMATIC] view=%d frame_cap=%d\n", cine_view, cine_fps);
}

static void ma2_cinematic_path(const char *path)
{
    const char *name = strrchr(path, '\\');
    uint32_t hash;
    name = name ? name + 1 : path;
    if (starts_with(path, "\\Device\\Harddisk0\\Partition5\\") && level_cache(name, &hash)) {
        int cinema = 0, view;
        for (size_t i = 0; i < sizeof(cinema_caches) / sizeof(cinema_caches[0]); i++)
            if (cinema_caches[i] == hash) { cinema = 1; break; }
        if (cine_view < 0) ma2_cinematic_configure();
        view = cine_view;
        for (size_t i = 0; i < sizeof(panel_cinemas) / sizeof(panel_cinemas[0]); i++)
            if (panel_cinemas[i] == hash && (view == PC_CINEMATIC_FILL || view == PC_CINEMATIC_FILL_16_9)) view = PC_CINEMATIC_PILLARBOX;
        if (cinema) pc_display_cinematic_config(view, cine_fps);
        pc_display_cinematic(cinema);
    }
    if (previous_hook) previous_hook(path);
}

static void __cdecl ma2_cinematic_install(void)
{
    const char *setting = getenv("RECOMP_CINEMATIC_MODE");
    if (setting && *setting && atoi(setting) == 0) return;
    previous_hook = g_xbox_path_hook;
    g_xbox_path_hook = ma2_cinematic_path;
}

/* Run before main, alongside the C++ static initializers. */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (__cdecl *ma2_cinematic_initializer)(void) = ma2_cinematic_install;

/* ---- RECOMP_CINE_TRACE=<file> (test only, off by default) ----------------------------
 * Logs every camera build (sub_44BE0, game thread) as one line:
 *   C frame time dt wall_ms cinema cam vt ortho fov m00..m33   (camera+0x154, last built matrix)
 * and, every RECOMP_CINE_TRACE_STEP game seconds (default 0.5), every world entity with a node:
 *   E time addr cls x y z
 * so two runs at different frame rates can be compared at the same game time. */
#include <stdio.h>
#include <windows.h>
#include <math.h>
extern ptrdiff_t g_xbox_mem_offset;
static int gspan(uint32_t p, uint32_t n) { return p >= 0x10000u && p <= 0x4000000u - n; }
static uint32_t g32(uint32_t p) { uint32_t v; memcpy(&v, (void *)((uintptr_t)p + g_xbox_mem_offset), 4); return v; }
static float gf(uint32_t p) { float v; memcpy(&v, (void *)((uintptr_t)p + g_xbox_mem_offset), 4); return v; }
static double gd(uint32_t p) { double v; memcpy(&v, (void *)((uintptr_t)p + g_xbox_mem_offset), 8); return v; }
static void cine_trace_entities(FILE *f, double now)
{
    uint32_t game = g32(0x50C958u);
    if (!gspan(game, 0xA8)) return;
    uint32_t r1 = g32(game + 0xA4);
    if (!gspan(r1, 8)) return;
    uint32_t root = g32(r1 + 4);
    if (!gspan(root, 0x30)) return;
    static uint32_t st[8192];
    int sp = 0, n = 0;
    st[sp++] = root;
    while (sp > 0 && n < 100000) {
        uint32_t e = st[--sp], sib = g32(e + 8), child = g32(e + 0xC), nd = g32(e + 0x94);
        n++;
        if (gspan(sib, 0x30) && sp < 8192) st[sp++] = sib;
        if (gspan(child, 0x30) && sp < 8192) st[sp++] = child;
        if (gspan(nd, 0x100) && gspan(g32(e), 0x58))
            fprintf(f, "E %.4f %08X %08X %u %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f\n", now, e, g32(e + 0x28), g32(e + 0x2C),
                    gf(nd + 0xF0), gf(nd + 0xF4), gf(nd + 0xF8), gf(nd + 0xD0), gf(nd + 0xD4), gf(nd + 0xD8),
                    gf(nd + 0xE0), gf(nd + 0xE4), gf(nd + 0xE8));
    }
}
void ma2_cine_trace_camera(uint32_t cam)
{
    static int init; static FILE *f; static double step = 0.5, next_snap = -1.0; static uint32_t last_frame = ~0u;
    static LARGE_INTEGER freq;
    if (!init) {
        init = 1;
        const char *p = getenv("RECOMP_CINE_TRACE"), *s = getenv("RECOMP_CINE_TRACE_STEP");
        if (p && *p) f = fopen(p, "w");
        if (s && atof(s) > 0) step = atof(s);
        QueryPerformanceFrequency(&freq);
    }
    if (!f || !gspan(cam, 0x200)) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    uint32_t frame = g32(0x4E2420u);
    double t = gd(0x4E2440u);
    fprintf(f, "C %u %.5f %.5f %.2f %08X %08X %08X %u %.3f", frame, t, gd(0x4E2428u),
            (double)now.QuadPart * 1000.0 / (double)freq.QuadPart, g32(0x4EB198u), cam, g32(cam),
            *(uint8_t *)((uintptr_t)cam + 0x88 + g_xbox_mem_offset), gf(cam + 0x84));
    for (uint32_t o = 0; o < 64; o += 4) fprintf(f, " %.4f", gf(cam + 0x154 + o));
    {   uint32_t d = g32(0x524AC4u);
        fprintf(f, " P %.4f %.4f VP %.3f %.3f %.3f %.3f DEV %d %d %.3f", gf(cam + 0x194), gf(cam + 0x1A8), gf(cam + 0x90), gf(cam + 0x94),
                gf(cam + 0x98), gf(cam + 0x9C), gspan(d, 0x50) ? (int)g32(d + 0x40) : 0, gspan(d, 0x50) ? (int)g32(d + 0x44) : 0,
                gspan(d, 0x50) ? gf(d + 0x48) : 0.f); }
    fputc('\n', f);
    if (frame != last_frame) {
        last_frame = frame;
        /* RECOMP_CINE_TRACE_UID=<uid>: per frame, the words of that entity (and of the object at
         * its +ptr given by RECOMP_CINE_TRACE_SUB=<offset hex>) that changed since the last frame. */
        static uint32_t want_uid, sub_off, ent, prev[0x400], sprev[0x400]; static int uid_init;
        if (!uid_init) {
            uid_init = 1;
            const char *u = getenv("RECOMP_CINE_TRACE_UID"), *o = getenv("RECOMP_CINE_TRACE_SUB");
            want_uid = u ? (uint32_t)strtoul(u, NULL, 10) : 0;
            sub_off = o ? (uint32_t)strtoul(o, NULL, 16) : 0;
        }
        if (want_uid) {
            if (!ent || !gspan(ent, 0x1000) || g32(ent + 0x2C) != want_uid) {
                ent = 0;
                uint32_t game = g32(0x50C958u), r1 = gspan(game, 0xA8) ? g32(game + 0xA4) : 0;
                uint32_t root = gspan(r1, 8) ? g32(r1 + 4) : 0;
                static uint32_t st[8192]; int sp = 0, n = 0;
                if (gspan(root, 0x30)) st[sp++] = root;
                while (sp > 0 && n < 100000 && !ent) {
                    uint32_t e = st[--sp], sib = g32(e + 8), child = g32(e + 0xC); n++;
                    if (gspan(e, 0x1000) && g32(e + 0x2C) == want_uid && gspan(g32(e), 0x58)) ent = e;
                    if (gspan(sib, 0x30) && sp < 8192) st[sp++] = sib;
                    if (gspan(child, 0x30) && sp < 8192) st[sp++] = child;
                }
                if (ent) { for (uint32_t i = 0; i < 0x400; i++) prev[i] = g32(ent + i * 4); fprintf(f, "U %.5f found %08X vt %08X\n", t, ent, g32(ent));
                         }
            }
            if (ent) {
                { uint32_t nd = g32(ent + 0x94); fprintf(f, "U %.5f P %.3f %.3f %.3f", t, gspan(nd, 0x100) ? gf(nd + 0xF0) : 0.f, gspan(nd, 0x100) ? gf(nd + 0xF4) : 0.f, gspan(nd, 0x100) ? gf(nd + 0xF8) : 0.f); }
                for (uint32_t i = 0; i < 0x400; i++) { uint32_t v = g32(ent + i * 4); if (v != prev[i]) { fprintf(f, " %X=%08X", i * 4, v); prev[i] = v; } }
                uint32_t s = sub_off ? g32(ent + sub_off) : 0;
                if (gspan(s, 0x1000)) {
                    fprintf(f, " |S %08X", s);
                    for (uint32_t i = 0; i < 0x400; i++) { uint32_t v = g32(s + i * 4); if (v != sprev[i]) { fprintf(f, " %X=%08X", i * 4, v); sprev[i] = v; } }
                }
                fputc('\n', f);
            }
        }
        if (next_snap < 0 || t < next_snap - 10 * step) next_snap = floor(t / step) * step + step;
        if (t >= next_snap - 1e-6) { cine_trace_entities(f, t); next_snap = floor(t / step + 1e-6) * step + step; }
    }
}

/* ---- Cinematic fill (CinematicView 1/2) -------------------------------------------------------------
 * The scene target is already the full output size (pc_display_aspect). Each perspective camera's
 * normalized viewport (camera +0x90 x, +0x94 y, +0x98 w, +0x9C h; the cinema's viewport steps letterbox
 * most shots to about 2.3:1 inside the 4:3 frame) is re-laid out on that target: the shot keeps its
 * authored width and is shown at aspect D = max(authored, cap) -- as wide as the screen (or 16:9) allows,
 * never narrower than authored, so only the top and bottom are cropped and a letterboxed shot wider
 * than the screen keeps thin bars. The projection then keeps the authored horizontal extent
 * (ma2_cine_fov). The original rect is remembered per camera so a rect is never rescaled twice; a new
 * rect from the game's viewport setter (sub_44AE0) is picked up because it no longer matches. */
typedef struct { uint32_t cam; float o[4], n[4]; } CineRect;
static CineRect cine_rects[64];
static unsigned cine_rect_next;
static void gfw(uint32_t p, float v) { memcpy((void *)((uintptr_t)p + g_xbox_mem_offset), &v, 4); }
static CineRect *cine_rect_find(uint32_t cam)
{
    for (unsigned i = 0; i < 64; i++) if (cine_rects[i].cam == cam) return &cine_rects[i];
    return NULL;
}
void ma2_cine_camera_fill(uint32_t cam)
{
    float cap = pc_display_cinematic_fill_cap();
    if (!(cap > 0.f) || !gspan(cam, 0x200) || *(uint8_t *)((uintptr_t)cam + 0x88 + g_xbox_mem_offset)) return;
    float r[4] = { gf(cam + 0x90), gf(cam + 0x94), gf(cam + 0x98), gf(cam + 0x9C) };
    CineRect *c = cine_rect_find(cam);
    if (c && !memcmp(r, c->n, sizeof r)) return;                 /* our own layout, unchanged */
    if (!c) { c = &cine_rects[cine_rect_next++ & 63]; c->cam = cam; }
    memcpy(c->o, r, sizeof r); memcpy(c->n, r, sizeof r);
    float screen = pc_display_aspect(4.f / 3.f);
    if (!(r[2] > 0.01f && r[3] > 0.01f) || !(screen > 0.f)) return;
    float authored = (4.f / 3.f) * r[2] / r[3], shown = authored > cap ? authored : cap;
    float h = screen / shown < 1.f ? screen / shown : 1.f, w = h * shown / screen;   /* fraction of target */
    if (w > 1.f) { h /= w; w = 1.f; }
    float cx = 0.5f + (r[0] + r[2] * 0.5f - 0.5f) * (w / r[2]), cy = 0.5f + (r[1] + r[3] * 0.5f - 0.5f) * (h / r[3]);
    float x = cx - w * 0.5f, y = cy - h * 0.5f;
    if (x < 0.f) x = 0.f; if (x > 1.f - w) x = 1.f - w;
    if (y < 0.f) y = 0.f; if (y > 1.f - h) y = 1.f - h;
    c->n[0] = x; c->n[1] = y; c->n[2] = w; c->n[3] = h;
    gfw(cam + 0x90, x); gfw(cam + 0x94, y); gfw(cam + 0x98, w); gfw(cam + 0x9C, h);
    static int logged;
    if (logged++ < 40) fprintf(stderr, "[CINEMATIC] camera %08X viewport %.3f,%.3f %.3fx%.3f (%.2f:1) -> %.3f,%.3f %.3fx%.3f (%.2f:1)\n",
                               cam, r[0], r[1], r[2], r[3], authored, x, y, w, h, shown);
}
/* The game derives the vertical FOV as hfov * k / aspect(viewport); undo the viewport change and keep the
 * authored horizontal extent: tan(v'/2) = tan(v/2) * authored / shown. */
float ma2_cine_fov(uint32_t cam, float fov)
{
    if (!(pc_display_cinematic_fill_cap() > 0.f)) return fov;
    CineRect *c = cine_rect_find(cam);
    float screen = pc_display_aspect(4.f / 3.f);
    if (!c || !(c->o[3] > 0.01f && c->n[3] > 0.01f && c->o[2] > 0.01f) || !(fov > 0.f && fov < 3.1f)) return fov;
    float authored = (4.f / 3.f) * c->o[2] / c->o[3], laid = (4.f / 3.f) * c->n[2] / c->n[3], shown = screen * c->n[2] / c->n[3];
    float v = fov * laid / authored;
    return 2.f * atanf(tanf(v * 0.5f) * authored / shown);
}
/* The cinema shot code asks the display aspect at 0x1C494E and letterboxes the shot (camera viewport,
 * about 2.33:1 inside 4:3) only when the display is narrower than [0x494048]. A fill view answers it with
 * the authored 4:3 so the authored band and its slide-in are kept; ma2_cine_camera_fill then lays that
 * band across the full-resolution target. RECOMP_CINE_TRACE logs each distinct caller once. */
int ma2_cine_authored_aspect(uint32_t ret)
{
    static uint32_t seen[64]; static unsigned n; static int on = -1;
    if (on < 0) { const char *p = getenv("RECOMP_CINE_TRACE"); on = p && *p; }
    if (on && n < 64) {
        unsigned i = 0;
        while (i < n && seen[i] != ret) i++;
        if (i == n) { seen[n++] = ret; fprintf(stderr, "[CINE-ASPECT] caller %08X t=%.3f cinematic=%d\n", ret, gd(0x4E2440u), pc_display_cinematic_active()); }
    }
    return ret == 0x001C494Eu && pc_display_cinematic_fill_cap() > 0.f;
}

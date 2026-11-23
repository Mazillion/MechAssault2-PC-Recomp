/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (from xbe_parser output):
 *   Title:       MechAssault2
 *   Title ID:    0x4D53006B
 *   Base addr:   0x00010000
 *   Entry point: 0x002B94F2
 *   Image size:  5.35 MB (ma2.xbe)
 *   Sections:    45 (.text, D3D, DSOUND, XPP, BINK, ...)
 *   Kernel imports: 154
 */

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <io.h>
#include <xinput.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <intrin.h>   /* 1.1.6: __cpuid for the [LAUNCH] hardware line */
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);
#include "pc_settings.h"
#include "pc_storage.h"
#include "ma2_startup_policy.h"
#include "ma2_release.h"

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/* Native pipeline M0 (2026-10-31): the shared switch reader (env > ini > default) and the CPU
 * topology / thread-role API. pc_cpu_topo.c has no build edge of its own: it compiles here. */
#include "platform/pc_switch.h"
#include "platform/pc_cpu_topo.c"
/* T1-2 FastCode switch (declared in recomp_types.h; set once at boot). */
int g_recomp_fastcode = 0;
int recomp_dispatch_fast_init(void);
int recomp_xmm_selftest(void); /* T1-8, recomp_dispatch.c */

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * ma2.xbe is the real game binary; default.xbe is only the disc launcher.
 * Paths are relative to the process working directory — run from this
 * project root, or put a game_files/ junction next to the .exe.
 */
#define YOUR_GAME_ENTRY_POINT   0x002B94F2  /* XBE entry point VA */
#define YOUR_GAME_XBE_PATH      "game_files\\ma2.xbe"
#define YOUR_GAME_DIR            "game_files"

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

static int g_overlay_from_env;   /* RECOMP_GAME_OVERLAY was set before start-up (W-11 skips the asset check) */
static char g_log_path[MAX_PATH]; /* this run's log file (W-06a crash box) */
static void configure_logging(void)
{
    DWORD out_type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    DWORD err_type = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    const char *console = getenv("RECOMP_CONSOLE");
    if (console && atoi(console)) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    } else if (out_type != FILE_TYPE_DISK && out_type != FILE_TYPE_PIPE &&
               err_type != FILE_TYPE_DISK && err_type != FILE_TYPE_PIPE) {
        char directory[MAX_PATH], path[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", directory, sizeof(directory));
        if (!n || n >= sizeof(directory)) {
            GetModuleFileNameA(NULL, directory, sizeof(directory));
            char *slash = strrchr(directory, '\\');
            if (slash) *slash = 0;
        }
        size_t length = strlen(directory);
        if (length + 40 < sizeof(directory)) {
            strcat(directory, "\\MechAssault2");
            CreateDirectoryA(directory, NULL);
            strcat(directory, "\\Logs");
            CreateDirectoryA(directory, NULL);
            ma2_prune_logs(directory, 19); /* newest 20 logs including this run's (D11) */
            ma2_prune_dumps(directory, 5); /* newest 5 crash/hang dumps (W-08) */
            SYSTEMTIME now;
            GetLocalTime(&now);
            snprintf(path, sizeof(path), "%s\\%04u%02u%02u-%02u%02u%02u-%lu.log", directory,
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                GetCurrentProcessId());
            freopen(path, "w", stderr);
            freopen("NUL", "w", stdout);
            snprintf(g_log_path, sizeof(g_log_path), "%s", path);
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

/* pc_switch.h reads pipeline switches from this file after the environment (pipeline M0). It is
 * the settings file configure_pc() resolves; until then, and in lib-only tests, switches read the
 * environment and their defaults only. */
static char g_pc_switch_ini_path[MAX_PATH];
static int pc_switch_ini_read(const char *section, const char *key, char *out, unsigned out_size)
{
    if (!g_pc_switch_ini_path[0] || !section || !key || !out || out_size < 2) return 0;
    return GetPrivateProfileStringA(section, key, "", out, out_size, g_pc_switch_ini_path) > 0;
}

/* Environment overrides remain available for bring-up probes. Normal launches
 * use the per-user settings, initialized from the packaged defaults. */
/* Returns 1 when the fallback was used (key missing or out of range), 0 for a valid saved value,
 * -1 when the environment already set it. */
static int pc_setting(const char *ini, const char *section, const char *key,
                      const char *env, int fallback, int low, int high,
                      BOOL presence_switch)
{
    char value[24];
    int n, used_fallback;
    if (getenv(env)) return -1;
    n = (int)GetPrivateProfileIntA(section, key, low - 1, ini);
    used_fallback = n < low || n > high;
    if (used_fallback) n = fallback;
    snprintf(value, sizeof(value), "%d", n);
    _putenv_s(env, presence_switch && !n ? "" : value);
    return used_fallback;
}

static void configure_pc(void)
{
    char exe_dir[MAX_PATH], ini[MAX_PATH], layout[24];
    char *slash;
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    slash = strrchr(exe_dir, '\\');
    if (slash) *slash = 0;
    SetCurrentDirectoryA(exe_dir);
    snprintf(ini, sizeof(ini), "%s\\pc_settings.ini", exe_dir);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES)
        snprintf(ini, sizeof(ini), "%s\\..\\pc_settings.ini", exe_dir);
    char defaults_ini[MAX_PATH];
    snprintf(defaults_ini,sizeof(defaults_ini),"%s",ini);
    if (!ma2_pc_settings_path(defaults_ini,ini,sizeof(ini)))
        snprintf(ini,sizeof(ini),"%s",defaults_ini);
    snprintf(g_pc_switch_ini_path, sizeof(g_pc_switch_ini_path), "%s", ini);
    pc_switch_ini = pc_switch_ini_read;

    if (!getenv("RECOMP_PB_EXEC")) _putenv_s("RECOMP_PB_EXEC", "1");
    if (!getenv("RECOMP_GPU")) _putenv_s("RECOMP_GPU", "d3d11");
    if (!getenv("RECOMP_TITLE_KEVENTS")) _putenv_s("RECOMP_TITLE_KEVENTS", "1");
    if (!getenv("RECOMP_VBLANK")) _putenv_s("RECOMP_VBLANK", "1");
    if (!getenv("RECOMP_FB_WINDOW")) _putenv_s("RECOMP_FB_WINDOW", "1");
    if (!getenv("RECOMP_HEAP_RECLAIM")) _putenv_s("RECOMP_HEAP_RECLAIM", "1");
    if (!getenv("RECOMP_ASYNC_IO")) _putenv_s("RECOMP_ASYNC_IO", "1");
    if (!getenv("RECOMP_USB")) _putenv_s("RECOMP_USB", "1");
    /* Split screen (1.1.8 prototype, hidden, off by default): RECOMP_SPLITSCREEN=1 or [Debug]
     * SplitScreen=1. It lifts the menu policy's Add Guest block and puts a second pad on the
     * emulated USB bus (it must be there before the OHCI model starts); the input layer then
     * gives each guest port its own host pad (xinput_device.c, xi_split_slot). */
    pc_setting(ini, "Debug", "SplitScreen", "RECOMP_SPLITSCREEN", 0, 0, 1, TRUE);
    pc_setting(ini, "Debug", "SplitScreenKeyboard", "RECOMP_SPLITSCREEN_KBM", 0, 0, 1, TRUE);   /* P1 = keyboard+mouse only */
    {
        const char *split = getenv("RECOMP_SPLITSCREEN");
        if (split && *split && *split != '0') {
            if (!getenv("RECOMP_SPLITSCREEN_MENUS")) _putenv_s("RECOMP_SPLITSCREEN_MENUS", "1");
            if (!getenv("RECOMP_USB_PADS")) _putenv_s("RECOMP_USB_PADS", "2");
            /* XAPI numbers root ports 1-2 as controllers 3-4 and ports 3-4 as 1-2, so the pads go
             * on ports 3 and 4: the pause menu then shows player 1 as controller 1 and the guest as
             * 2, as on a console (measured: ports 1-2 gave 3 and 4). Outside split screen the bus
             * stays as it was (one pad, port 1). */
            if (!getenv("RECOMP_USB_NDP")) _putenv_s("RECOMP_USB_NDP", "4");
            if (!getenv("RECOMP_USB_PORT")) _putenv_s("RECOMP_USB_PORT", "2");
            fprintf(stderr, "[PC-SPLIT] split screen on: Add Guest enabled, %s pads on the USB bus\n", getenv("RECOMP_USB_PADS"));
        }
    }
    if (!getenv("RECOMP_INPUT_LATCH")) _putenv_s("RECOMP_INPUT_LATCH", "1");
    if (!getenv("RECOMP_MOUSE_GUEST_POLL")) _putenv_s("RECOMP_MOUSE_GUEST_POLL", "1");
    if (!getenv("RECOMP_EXIT_ON_CLOSE")) _putenv_s("RECOMP_EXIT_ON_CLOSE", "1");
    {
        int w_fb = pc_setting(ini, "Display", "WindowWidth", "RECOMP_WINDOW_WIDTH", 1280, 640, 7680, FALSE);
        int h_fb = pc_setting(ini, "Display", "WindowHeight", "RECOMP_WINDOW_HEIGHT", 960, 480, 4320, FALSE);
        /* W-03: one bad or missing size (hand-edited ini) would pair a saved width with the fallback height. */
        if ((w_fb == 1 && h_fb == 0) || (w_fb == 0 && h_fb == 1)) {
            fprintf(stderr, "[PC-SETTINGS] WindowWidth/WindowHeight: one value is missing or out of range; using 1280x960\n");
            _putenv_s("RECOMP_WINDOW_WIDTH", "1280");
            _putenv_s("RECOMP_WINDOW_HEIGHT", "960");
        }
    }
    pc_setting(ini, "Display", "Fullscreen", "RECOMP_FULLSCREEN", 0, 0, 1, FALSE);
    pc_setting(ini, "Display", "PreserveAspect", "RECOMP_PRESERVE_ASPECT", 1, 0, 1, FALSE);
    pc_setting(ini, "Display", "SmoothScaling", "RECOMP_SCALE_FILTER", 1, 0, 1, FALSE);
    pc_setting(ini, "Display", "VSync", "RECOMP_VSYNC", 0, 0, 1, FALSE);
    pc_setting(ini, "Display", "FrameLimit", "RECOMP_FRAME_LIMIT", 144, 0, 1000, FALSE);   /* 0 (old Unlimited) is read as 144 in ma2_pc_settings_initialize */
    /* Resolution now determines scene pixels. Ignore the obsolete saved
     * multiplier; an explicit environment override remains for diagnostics. */
    if(!getenv("RECOMP_RENDER_SCALE"))_putenv_s("RECOMP_RENDER_SCALE","0");
    pc_setting(ini, "Display", "ShowFPS", "RECOMP_SHOW_FPS", 0, 0, 1, FALSE);
    pc_setting(ini, "Display", "DrawDistance", "RECOMP_DRAW_DISTANCE", 50, 0, 100, FALSE);
    pc_setting(ini, "Display", "TextureQuality", "RECOMP_TEXTURE_QUALITY", 0, 0, 2, FALSE);
    pc_setting(ini, "Display", "FMVQuality", "RECOMP_FMV_QUALITY", 0, 0, 1, FALSE);
    pc_setting(ini, "Display", "PreloadShaders", "RECOMP_SHADER_PRELOAD", 1, 0, 1, FALSE);
    pc_setting(ini, "Input", "Keyboard", "RECOMP_KEYBOARD", 1, 0, 1, FALSE);
    pc_setting(ini, "Input", "MouseAim", "RECOMP_MOUSE_AIM", 1, 0, 1, FALSE);
    pc_setting(ini, "Input", "MouseSensitivity", "RECOMP_MOUSE_SENSITIVITY", 100, 10, 500, FALSE);
    pc_setting(ini, "Input", "InvertY", "RECOMP_INVERT_Y", 0, 0, 1, FALSE);
    pc_setting(ini, "Input", "InvertFlight", "RECOMP_INVERT_FLIGHT", 0, 0, 1, FALSE);
    if (!getenv("RECOMP_KEYBOARD_LAYOUT")) {
        GetPrivateProfileStringA("Input", "KeyboardLayout", "pc", layout, sizeof(layout), ini);
        ma2_normalise_layout(layout);
        _putenv_s("RECOMP_KEYBOARD_LAYOUT", layout);
    }
    pc_setting(ini, "Audio", "Enabled", "RECOMP_AC97_READY", 1, 0, 1, TRUE);
    pc_setting(ini, "Audio", "Volume", "RECOMP_AUDIO_VOLUME", 100, 0, 100, FALSE);
    ma2_pc_settings_initialize(ini);
    { char shown[MAX_PATH * 2]; fprintf(stderr, "[LAUNCH] settings=%s\n", xbox_log_path(ini, shown, sizeof shown)); }
    printf("Enter: Start | Space: A/confirm | F: B/back | arrows: menu | WASD: move\n");
    printf("Mouse: aim | F8: capture/release | F11 or Alt+Enter: fullscreen\n");
}

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern void xbox_OhciInit(void);
extern int xbox_OhciOwnsAddress(uint32_t xbox_va);
extern int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_INT_OVERFLOW ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr,"[CRASH] exception %08lX at RIP=%llX RVA=%llX\n", code,
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)(ep->ContextRecord->Rip-(uintptr_t)GetModuleHandleW(NULL)));
        fprintf(stderr,"  Xbox eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
            g_eax,g_ecx,g_edx,g_ebx,g_esi,g_edi,g_esp);
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_OhciOwnsAddress(xbox_va) &&
                xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
                return EXCEPTION_CONTINUE_EXECUTION;

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr,"  Native RVA=0x%llX\n",(unsigned long long)
            (ep->ContextRecord->Rip-(uintptr_t)GetModuleHandleW(NULL)));
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        MEMORY_BASIC_INFORMATION region;
        if(VirtualQuery((void *)fault_addr,&region,sizeof(region)))
            fprintf(stderr,"  Host region base=%p size=%llX state=%lX protect=%lX allocation=%p\n",
                region.BaseAddress,(unsigned long long)region.RegionSize,
                region.State,region.Protect,region.AllocationBase);
        fprintf(stderr,"  Host rcx=%llX rdx=%llX r8=%llX r9=%llX rdi=%llX rsi=%llX\n",
            ep->ContextRecord->Rcx,ep->ContextRecord->Rdx,ep->ContextRecord->R8,
            ep->ContextRecord->R9,ep->ContextRecord->Rdi,ep->ContextRecord->Rsi);
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            uintptr_t image=(uintptr_t)GetModuleHandleW(NULL);
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 96; i++) {
                if (sp[i] >= image && sp[i] < image+0x10000000ULL) {
                    fprintf(stderr, "    [%d] RVA=0x%llX\n", i, (unsigned long long)(sp[i]-image));
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── Unhandled-exception filter: crash dump (easy-wins B1.2, D5) ──────────── */

/* Restores the user's Sticky Keys hotkey; defined in pc_native_settings.cpp. */
extern void ma2_pc_sticky_keys(int suppress);

static void sticky_restore_at_exit(void) { ma2_pc_sticky_keys(0); }

/* Folder for crash dumps: RECOMP_CRASH_DIR (tests), else %LOCALAPPDATA%\MechAssault2\Logs, else the exe folder. */
static int crash_directory(char *directory, size_t size)
{
    DWORD n = GetEnvironmentVariableA("RECOMP_CRASH_DIR", directory, (DWORD)size);
    if (n && n < size) { CreateDirectoryA(directory, NULL); return 1; }
    n = GetEnvironmentVariableA("LOCALAPPDATA", directory, (DWORD)size);
    if (!n || n >= size) {
        GetModuleFileNameA(NULL, directory, (DWORD)size);
        char *slash = strrchr(directory, '\\');
        if (slash) *slash = 0;
    }
    if (strlen(directory) + 24 >= size) return 0;
    strcat(directory, "\\MechAssault2"); CreateDirectoryA(directory, NULL);
    strcat(directory, "\\Logs"); CreateDirectoryA(directory, NULL);
    return 1;
}

/* 1.1.6: the crash and freeze boxes start the report helper, "MA2 Report.exe" beside the game
 * (report-helper/report.c in the public repo): the failure text, a "What happened?" box and Send report,
 * which sends the logs, settings and the dump (names and profile folders removed) to the developer.
 * Every call site keeps the 1.1.5 box as the fallback when the helper is missing or cannot start.
 * Harness runs (RECOMP_SAVE_DIR) show neither, as before. Tests: RECOMP_TEST_REPORT_UI=1 makes a harness
 * run act like a player's (the helper's window stays hidden through RECOMP_REPORT_HIDDEN=1, and a
 * fallback box is written to the log instead of shown); RECOMP_REPORT_HELPER=<path> picks the helper
 * (a missing path tests the fallback). */
static void hang_say(const char *fmt, ...);
static int report_ui_test(void) { const char *t = getenv("RECOMP_TEST_REPORT_UI"); return t && *t && *t != '0'; }
static int report_ui_wanted(void) { return report_ui_test() || (!getenv("RECOMP_SAVE_DIR") && !getenv("RECOMP_ALLOW_MULTI")); }
/* The helper's full path; 0 when it is not there. Also used by the in-game page (pc_native_settings.cpp). */
int ma2_report_helper_path(wchar_t *out, DWORD size)
{
    wchar_t *slash;
    DWORD n = GetEnvironmentVariableW(L"RECOMP_REPORT_HELPER", out, size);
    if (!n || n >= size) {
        n = GetModuleFileNameW(NULL, out, size);
        if (!n || n >= size || !(slash = wcsrchr(out, L'\\')) || (size_t)(slash - out) + 18 >= size) { out[0] = 0; return 0; }
        wcscpy(slash + 1, L"MA2 Report.exe");
    }
    {
        DWORD a = GetFileAttributesW(out);
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
}
/* This run's log (the file stderr writes to): the helper sends it. */
int ma2_report_log_path(wchar_t *out, DWORD size)
{
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stderr));
    out[0] = 0;
    if (h == INVALID_HANDLE_VALUE || !h || GetFileType(h) != FILE_TYPE_DISK) return 0;
    if (!GetFinalPathNameByHandleW(h, out, size, 0)) { out[0] = 0; return 0; }
    if (!wcsncmp(out, L"\\\\?\\", 4) && out[5] == L':') memmove(out, out + 4, (wcslen(out + 4) + 1) * sizeof(wchar_t));
    return out[0] != 0;
}
/* The settings file this run uses (the helper sends it). */
const char *ma2_report_ini_path(void) { return g_pc_switch_ini_path; }
extern void apu_health_request(void);
/* Starts the helper with the given arguments (after the helper path); 1 = started (process handle
 * in *process when asked for). Writes one [REPORT] line either way. */
static int report_helper_start(const wchar_t *args, HANDLE *process)
{
    wchar_t helper[MAX_PATH], log[MAX_PATH], ini[MAX_PATH], cmd[4 * MAX_PATH + 512];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (process) *process = NULL;
    if (!ma2_report_helper_path(helper, MAX_PATH)) { hang_say("[REPORT] helper not found: the 1.1.5 box\n"); return 0; }
    ma2_report_log_path(log, MAX_PATH);
    ini[0] = 0;
    if (g_pc_switch_ini_path[0]) MultiByteToWideChar(CP_ACP, 0, g_pc_switch_ini_path, -1, ini, MAX_PATH);
    _snwprintf(cmd, sizeof cmd / sizeof *cmd - 1, L"\"%s\" %s%s%s%s%s%s%s", helper, args,
               log[0] ? L" --log \"" : L"", log[0] ? log : L"", log[0] ? L"\"" : L"",
               ini[0] ? L" --ini \"" : L"", ini[0] ? ini : L"", ini[0] ? L"\"" : L"");
    cmd[sizeof cmd / sizeof *cmd - 1] = 0;
    if (report_ui_test()) SetEnvironmentVariableW(L"RECOMP_REPORT_HIDDEN", L"1");
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(helper, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        hang_say("[REPORT] the helper could not start (error %lu): the 1.1.5 box\n", GetLastError());
        return 0;
    }
    hang_say("[REPORT] helper started (pid %lu)\n", pi.dwProcessId);
    CloseHandle(pi.hThread);
    if (process) *process = pi.hProcess; else CloseHandle(pi.hProcess);
    return 1;
}
/* A 1.1.5-style box (the fallback); a test run writes it to the log instead. */
static int report_fallback_box(const wchar_t *text, UINT flags)
{
    if (report_ui_test()) {
        char shown[1600];
        WideCharToMultiByte(CP_UTF8, 0, text, -1, shown, sizeof shown, NULL, NULL);
        for (char *c = shown; *c; c++) if (*c == '\n') *c = '|';
        hang_say("[REPORT] fallback box (test run, not shown): %s\n", shown);
        return getenv("RECOMP_TEST_REPORT_BOX_YES") ? IDYES : IDNO;
    }
    return MessageBoxW(NULL, text, L"MechAssault 2", flags | MB_SETFOREGROUND | MB_TOPMOST);
}

/* The crash box (wording 1.1.6): only when the report helper is missing or cannot start. */
static void crash_message(const char *dump)
{
    wchar_t text[3 * MAX_PATH + 800];
    char shown[MAX_PATH * 2];
    int n = _snwprintf(text, sizeof text / sizeof *text, L"MechAssault 2 has stopped unexpectedly.\n\n"
                       L"To report this, run the MechAssault 2 installer again and choose Help > Report a problem. "
                       L"It sends the game's logs with your Windows user name removed, or saves them as a zip in "
                       L"%%LOCALAPPDATA%%\\MechAssault2\\Diagnostics.\n\n");
    if (n < 0) n = 0;
    if (dump && *dump) {
        int k = _snwprintf(text + n, sizeof text / sizeof *text - n, L"A crash report was saved as %hs. Report a problem includes it, "
                           L"with your user name removed.\n\n", xbox_log_path(dump, shown, sizeof shown));
        if (k > 0) n += k;
    }
    {
        int k = _snwprintf(text + n, sizeof text / sizeof *text - n, L"Folder: %%LOCALAPPDATA%%\\MechAssault2\\Logs");
        if (k > 0) n += k;
    }
    if (MA2_SUPPORT_URL[0] && n > 0 && n < (int)(sizeof text / sizeof *text))
        _snwprintf(text + n, sizeof text / sizeof *text - n, L"\n\nReport at: %hs", MA2_SUPPORT_URL);
    text[sizeof text / sizeof *text - 1] = 0;
    report_fallback_box(text, MB_ICONERROR | MB_OK);
}

/* W-06a: one header that says which build wrote this log, on which Windows. The installer writes
 * recomp-install.json beside the exe; a developer build has none ("dev"). */
static void json_field(const char *json, const char *key, char *out, size_t size)
{
    char pattern[64];
    const char *p, *q;
    out[0] = 0;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (!json || !(p = strstr(json, pattern))) return;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return;
    q = strchr(++p, '"');
    if (!q || (size_t)(q - p) >= size) return;
    memcpy(out, p, (size_t)(q - p)); out[q - p] = 0;
}
static void log_build_identity(void)
{
    char exe[MAX_PATH], path[MAX_PATH + 32], version[64], public_version[64], kit[80], *slash, *json = NULL;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    SYSTEMTIME utc = {0};
    unsigned long long bytes = 0;
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (GetFileAttributesExA(exe, GetFileExInfoStandard, &attributes)) {
        bytes = ((unsigned long long)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
        FileTimeToSystemTime(&attributes.ftLastWriteTime, &utc);
    }
    snprintf(path, sizeof(path), "%s", exe);
    slash = strrchr(path, '\\');
    if (slash) {
        FILE *f;
        strcpy(slash + 1, "recomp-install.json");
        if ((f = fopen(path, "rb"))) {
            long size;
            fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
            if (size > 0 && size < 65536 && (json = (char *)malloc((size_t)size + 1))) {
                size = (long)fread(json, 1, (size_t)size, f); json[size] = 0;
            }
            fclose(f);
        }
    }
    json_field(json, "version", version, sizeof(version));
    json_field(json, "public_version", public_version, sizeof(public_version));
    json_field(json, "kit_id", kit, sizeof(kit));
    /* 1.1.6: the installer writes "low_memory_build": {"one_codegen_thread": N, "lighter_optimisation":
     * {file: flags, ...}} only when the game was built with less memory; json_field reads strings only. */
    {
        const char *low = json ? strstr(json, "\"low_memory_build\"") : NULL;
        long cg1 = 0, lighter = 0;
        if (low) {
            const char *p = strstr(low, "\"one_codegen_thread\"");
            if (p && (p = strchr(p, ':'))) cg1 = strtol(p + 1, NULL, 10);
            p = strstr(low, "\"lighter_optimisation\"");
            if (p && (p = strchr(p, '{'))) {
                int depth = 0, colons = 0;
                for (; *p; p++) {
                    if (*p == '{') depth++;
                    else if (*p == '}') { if (--depth == 0) break; }
                    else if (*p == ':' && depth == 1) colons++;
                }
                lighter = colons;
            }
        }
        fprintf(stderr, "[LAUNCH] low_memory_build=%s one_codegen_thread=%ld lighter_optimisation=%ld\n",
                low ? "yes" : "no", cg1, lighter);
    }
    free(json);
    fprintf(stderr, "[LAUNCH] build version=%s exe_version=%s install=%s kit=%s exe_bytes=%llu exe_time=%04u-%02u-%02uT%02u:%02u:%02uZ\n",
            public_version[0] ? public_version : MA2_PUBLIC_VERSION, MA2_PUBLIC_VERSION,
            version[0] ? version : "dev", kit[0] ? kit : "dev", bytes,
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    {
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW os;
        memset(&os, 0, sizeof(os)); os.dwOSVersionInfoSize = sizeof(os);
        if (get && get(&os) == 0)
            fprintf(stderr, "[LAUNCH] windows=%lu.%lu.%lu\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    }
    /* 1.1.6: CPU model and memory (no names or IDs; thread counts and power are in [CPU-TOPO]). */
    {
        int regs[4];
        char brand[49] = {0};
        unsigned long long ram_mb = 0, commit_mb = 0;
        MEMORYSTATUSEX ms;
        __cpuid(regs, (int)0x80000000);
        if ((unsigned)regs[0] >= 0x80000004u) {
            for (int i = 0; i < 3; i++) { __cpuid(regs, (int)(0x80000002u + i)); memcpy(brand + 16 * i, regs, 16); }
            brand[48] = 0;
        }
        { char *b = brand; while (*b == ' ') b++; memmove(brand, b, strlen(b) + 1); }
        for (size_t n = strlen(brand); n && brand[n - 1] == ' '; n--) brand[n - 1] = 0;
        for (char *c = brand; *c; c++) if (*c == '"' || (unsigned char)*c < 32) *c = ' ';
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) { ram_mb = ms.ullTotalPhys >> 20; commit_mb = ms.ullTotalPageFile >> 20; }
        fprintf(stderr, "[LAUNCH] hardware cpu=\"%s\" ram_mb=%llu commit_limit_mb=%llu\n",
                brand[0] ? brand : "unknown", ram_mb, commit_mb);
    }
}

/* <crash dir>\<prefix>-<pid>.dmp; info may be NULL (hang report). Returns 1 when written. */
static int write_dump(const char *prefix, EXCEPTION_POINTERS *info, char *path, size_t size)
{
    char directory[MAX_PATH];
    int written = 0;
    path[0] = 0;
    if (!crash_directory(directory, sizeof(directory))) return 0;
    snprintf(path, size, "%s\\%s-%lu.dmp", directory, prefix, GetCurrentProcessId());
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception;
        exception.ThreadId = GetCurrentThreadId();
        exception.ExceptionPointers = info;
        exception.ClientPointers = FALSE;
        written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                                    info ? &exception : NULL, NULL, NULL) != 0;
        CloseHandle(file);
    }
    return written;
}

/* MiniDumpNormal holds thread stacks and some process memory: the README says so before anyone
 * sends one. The dialog only appears for a real player run (the harness sets RECOMP_SAVE_DIR). */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    static volatile LONG entered;
    char path[MAX_PATH + 40];
    int written;
    /* A second faulting thread waits: returning would end the process under the first one's box (W-08). */
    if (InterlockedExchange(&entered, 1)) { Sleep(INFINITE); }
    {   /* stop any rumble: the game will not get to switch it off */
        XINPUT_VIBRATION z = {0};
        DWORD i;
        for (i = 0; i < 4; i++) XInputSetState(i, &z);
    }
    ma2_pc_sticky_keys(0);
    written = write_dump("crash", info, path, sizeof(path));
    fprintf(stderr, "[CRASH] code=0x%08lX dump=%s ok=%d\n",
            info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0, path, written);
    fflush(stderr);
    if (report_ui_wanted()) {
        /* 1.1.6: the report helper (Send report); the 1.1.5 box only if it cannot start. The health line
         * first, so the sent log ends with the audio and frame-time counters of this moment. */
        wchar_t args[2 * MAX_PATH + 200], wdump[MAX_PATH + 40];
        unsigned long code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
        unsigned long long rva = info && info->ExceptionRecord ?
            (unsigned long long)((uintptr_t)info->ExceptionRecord->ExceptionAddress - (uintptr_t)GetModuleHandleW(NULL)) : 0;
        apu_health_request();
        Sleep(60);
        MultiByteToWideChar(CP_ACP, 0, written ? path : "", -1, wdump, MAX_PATH + 40);
        _snwprintf(args, sizeof args / sizeof *args - 1, L"--crash \"%s\" --sig \"0x%08lX:rva%llX\"", wdump, code, rva);
        args[sizeof args / sizeof *args - 1] = 0;
        if (!report_helper_start(args, NULL)) crash_message(written ? path : NULL);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* RECOMP_TEST_CRASH=<seconds>: a null write on a helper thread once the game has booted, to prove
 * the filter writes crash-<pid>.dmp (1 means 8 s). Off unless the variable is set. */
static DWORD WINAPI test_crash_thread(LPVOID seconds)
{
    Sleep((DWORD)(uintptr_t)seconds * 1000u);
    fprintf(stderr, "[CRASH] test hook firing\n");
    fflush(stderr);
    *(volatile int *)0 = 1;
    return 0;
}
/* RECOMP_TEST_CRASH2=1 (W-08): a second thread faults 50 ms after the first; the box must stay. */
static DWORD WINAPI test_crash_second(LPVOID seconds)
{
    Sleep((DWORD)(uintptr_t)seconds * 1000u + 50u);
    *(volatile int *)0 = 2;
    return 0;
}

/* Hang monitor (W-07). The window has its own message thread, so Windows never says "Not responding"
 * when the game thread stops. After 30 s with no presented frame (and no menu pulse) this writes one
 * [HANG] report -- guest registers, guest return addresses, critical-section waiters -- and
 * hang-<pid>.dmp; after 120 s it offers to close the game (not in harness runs). At most 3 reports a
 * run. Text goes out with WriteFile on the CRT's own stderr handle: a frozen thread may hold the
 * stream lock, and in a windowed exe freopen(stderr) does not change GetStdHandle(STD_ERROR_HANDLE).
 * RECOMP_HANG_MONITOR=0 turns it off. */
LONG xbox_FramebufferFlipCount(void);
DWORD ma2_pc_menu_pulse_ms(void);
int xbox_cs_waiters_dump(char *out, int cap);
static uint32_t *g_hang_regs[7];   /* eax ecx edx ebx esi edi esp of the game thread (thread-local) */
static char g_hang_dump[MAX_PATH + 40];   /* the newest hang-<pid>.dmp, for the report helper */
/* The freeze question comes 90 s after the [HANG] report; RECOMP_TEST_HANG_ASK_S shortens it (tests only). */
static DWORD hang_ask_ms(void) { const char *s = getenv("RECOMP_TEST_HANG_ASK_S"); return s && atoi(s) > 0 ? (DWORD)atoi(s) * 1000u : 90000u; }
static void hang_say(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    int n;
    DWORD wrote;
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(stderr));
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    if (n > 0 && h != INVALID_HANDLE_VALUE) WriteFile(h, line, (DWORD)n, &wrote, NULL);
}
static void hang_report(void)
{
    static char buf[16384];
    char path[MAX_PATH + 40];
    uint32_t r[7];
    int i, n;
    for (i = 0; i < 7; i++) r[i] = g_hang_regs[i] ? *g_hang_regs[i] : 0;
    hang_say("[HANG] no frame for %s\n", xbox_FramebufferFlipCount() ? "30 s" : "120 s since start-up");
    hang_say("[HANG] guest eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
    if (g_xbox_mem_offset && r[6] >= 0x10000u && r[6] < 0x4000000u - 1024) {
        const uint32_t *sp = (const uint32_t *)((uintptr_t)g_xbox_mem_offset + r[6]);
        int shown = 0;
        n = snprintf(buf, sizeof(buf), "[HANG] guest return addresses:");
        for (i = 0; i < 256 && shown < 24 && n < (int)sizeof(buf) - 32; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) { n += snprintf(buf + n, sizeof(buf) - n, " [+%X]%08X", i * 4, v); shown++; }
        }
        hang_say("%s\n", buf);
    }
    n = xbox_cs_waiters_dump(buf, (int)sizeof(buf));
    if (n > 0) {
        DWORD wrote;
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(stderr));
        hang_say("[HANG] critical-section waiters:\n");
        if (h != INVALID_HANDLE_VALUE) WriteFile(h, buf, (DWORD)n, &wrote, NULL);
    }
    if (write_dump("hang", NULL, path, sizeof(path))) { hang_say("[HANG] dump=%s\n", path); snprintf(g_hang_dump, sizeof g_hang_dump, "%s", path); }
    else hang_say("[HANG] dump could not be written (%s)\n", path[0] ? path : "no folder");
}
static DWORD WINAPI hang_monitor(LPVOID unused)
{
    LONG last_flip = xbox_FramebufferFlipCount();
    DWORD last_move = GetTickCount();
    int reported = 0, reports = 0, asked = 0;
    (void)unused;
    for (;;) {
        Sleep(1000);
        LONG f = xbox_FramebufferFlipCount();
        DWORD now = GetTickCount(), pulse = ma2_pc_menu_pulse_ms();
        if (f != last_flip || (pulse && now - pulse < 2000)) {
            if (reported) hang_say("[HANG] resumed after %lu ms\n", (unsigned long)(now - last_move));
            last_flip = f; last_move = now; reported = 0; asked = 0;
            continue;
        }
        /* Start-up before the first presented frame can take longer than 30 s (cold disk, another game
         * running; seen in gate run r26g_cuni: 39.8 s, then normal play): allow 120 s there. */
        DWORD limit = f ? 30000u : 120000u;
        if (!reported && reports < 3 && now - last_move >= limit) {
            reported = 1; reports++;
            hang_report();
        }
        if (!asked && now - last_move >= limit + hang_ask_ms() && (!getenv("RECOMP_SAVE_DIR") || report_ui_test())) {
            int close_game = 0;
            HANDLE helper = NULL;
            wchar_t args[MAX_PATH + 64], wdump[MAX_PATH + 40];
            asked = 1;
            /* 1.1.6: the report helper offers Send report, Close the game (exit 10) and Keep waiting (11);
             * the 1.1.5 Yes/No box only if it cannot start. */
            apu_health_request();
            MultiByteToWideChar(CP_ACP, 0, g_hang_dump, -1, wdump, MAX_PATH + 40);
            _snwprintf(args, sizeof args / sizeof *args - 1, L"--hang \"%s\"", wdump);
            args[sizeof args / sizeof *args - 1] = 0;
            if (report_helper_start(args, &helper)) {
                DWORD code = 0;
                WaitForSingleObject(helper, INFINITE);
                GetExitCodeProcess(helper, &code);
                CloseHandle(helper);
                hang_say("[HANG] report helper closed (exit %lu: %s)\n", code, code == 10 ? "close the game" : "keep waiting");
                close_game = code == 10;
            } else {
                close_game = report_fallback_box(L"MechAssault 2 has stopped responding.\n\nA report was saved in %LOCALAPPDATA%\\MechAssault2\\Logs. "
                                                 L"To report this, run the MechAssault 2 installer again and choose Help > Report a problem.\n\nClose the game?",
                                                 MB_YESNO | MB_ICONWARNING) == IDYES;
            }
            if (close_game) {
                hang_say("[HANG] player closed the game\n");
                ma2_pc_sticky_keys(0);
                ExitProcess(3);
            }
        }
    }
}
/* Called on the game thread (the guest registers are thread-local there). */
static void hang_monitor_start(void)
{
    const char *off = getenv("RECOMP_HANG_MONITOR");
    HANDLE h;
    if (off && !strcmp(off, "0")) return;
    g_hang_regs[0] = &g_eax; g_hang_regs[1] = &g_ecx; g_hang_regs[2] = &g_edx;
    g_hang_regs[3] = &g_ebx; g_hang_regs[4] = &g_esi; g_hang_regs[5] = &g_edi; g_hang_regs[6] = &g_esp;
    h = CreateThread(NULL, 0, hang_monitor, NULL, 0, NULL);
    if (h) CloseHandle(h);
    fprintf(stderr, "[HANG] monitor on (30 s without a frame; RECOMP_HANG_MONITOR=0 turns it off)\n");
}
/* RECOMP_TEST_HANG=<seconds>: suspend the game thread after N s, so no frame is presented again. */
static DWORD WINAPI test_hang_thread(LPVOID game_thread)
{
    const char *s = getenv("RECOMP_TEST_HANG");
    Sleep((DWORD)atoi(s ? s : "20") * 1000u);
    hang_say("[HANG] test hook: suspending the game thread\n");
    SuspendThread((HANDLE)game_thread);
    return 0;
}

/* The single-instance lock (B1.1). ma2_scenario.c restart_game frees it just before the game
 * starts its replacement copy and ends. */
static HANDLE g_instance_lock;
void ma2_single_instance_release(void)
{
    if(g_instance_lock){CloseHandle(g_instance_lock);g_instance_lock=NULL;}
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    configure_logging();
    fprintf(stderr,"[LAUNCH] console=%s\n",GetConsoleWindow()?"present":"none");
    log_build_identity();

    /* Native pipeline M0: RECOMP_AFFINITY=<hex> (test only) restricts the process to those logical
     * CPUs before any other thread starts, to simulate 4- and 8-thread machines. Then measure what
     * the process may use and log one [CPU-TOPO] line. Detection only reads; nothing uses it yet. */
    pc_cpu_affinity_from_env();
    pc_cpu_topo_init();

    /* Laptops (2026-10-15): Windows 11 may run a process under EcoQoS -- efficiency cores at low
     * clocks -- and ignore its timer resolution while its window is in the background, and an
     * i7-1260P run did not recover its frame rate after an alt-tab. Opt the whole process out:
     * it is a game, and its audio and GPU threads keep real-time deadlines either way. */
    {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        PROCESS_POWER_THROTTLING_STATE throttling;
        memset(&throttling,0,sizeof(throttling));
        throttling.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask=PROCESS_POWER_THROTTLING_EXECUTION_SPEED|PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask=0;
        if(!SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&throttling,sizeof(throttling)))
            fprintf(stderr,"[LAUNCH] power throttling opt-out failed (%lu)\n",GetLastError());
    }

    /* Windows timer resolution (easy-wins B1.3): Sleep() and waits otherwise round to ~15.6 ms and the
     * frame pacing, audio and loader threads wake late. Held for the life of the process. */
    {
        MMRESULT timer_result=timeBeginPeriod(1);
        fprintf(stderr,"[LAUNCH] timeBeginPeriod(1)=%u\n",(unsigned)timer_result);
    }

    /* One game per user (B1.1, D4): a second copy would share the same saves and caches. Runs with
     * RECOMP_SAVE_DIR (harness, editor Test) are exempt. The handle lives until exit. */
    {
        /* A copy the game restarted itself (in-game editor Test or a saved scenario: --skip-intro) may
         * start while the old copy is still on its way out: give the lock a few seconds to free. */
        int ok=ma2_single_instance_acquire(L"Local\\MechAssault2Recomp",&g_instance_lock);
        if(!ok&&wcsstr(GetCommandLineW(),L"--skip-intro"))
            for(int i=0;i<50&&!ok;i++){Sleep(100);ok=ma2_single_instance_acquire(L"Local\\MechAssault2Recomp",&g_instance_lock);}
        if(!ok){
            fprintf(stderr,"[LAUNCH] another instance is running, exiting\n");
            MessageBoxW(NULL,L"MechAssault 2 is already running.",L"MechAssault 2",MB_ICONINFORMATION);
            return 0;
        }
    }

    configure_pc();
    DeleteFileA("xbox_kernel.log");   /* W-09a: left beside the exe by older builds (CWD = exe folder) */
    g_overlay_from_env = getenv("RECOMP_GAME_OVERLAY") != NULL;
    if(!getenv("RECOMP_GAME_OVERLAY"))_putenv_s("RECOMP_GAME_OVERLAY",
        GetFileAttributesA("pc_assets\\common.mgf")!=INVALID_FILE_ATTRIBUTES?"pc_assets":"..\\pc_assets");
    if(!getenv("RECOMP_CACHE_NAMESPACE")) {
        char version_path[MAX_PATH], version[64]={0};
        snprintf(version_path,sizeof(version_path),"%s\\cache.version",getenv("RECOMP_GAME_OVERLAY"));
        FILE *file=fopen(version_path,"rb");
        if(file){
            if(fgets(version,sizeof(version),file)){
                size_t end=strcspn(version,"\r\n");version[end]=0;
                if(end>0&&end==strspn(version,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-"))
                    _putenv_s("RECOMP_CACHE_NAMESPACE",version);
            }
            fclose(file);
        }
    }

    printf("=== MechAssault 2 - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    SetUnhandledExceptionFilter(crash_filter);
    atexit(sticky_restore_at_exit);
    if (getenv("RECOMP_TEST_CRASH")) {
        int seconds = atoi(getenv("RECOMP_TEST_CRASH"));
        HANDLE t = CreateThread(NULL, 0, test_crash_thread, (LPVOID)(uintptr_t)(seconds > 1 ? seconds : 8), 0, NULL);
        if (t) CloseHandle(t);
        if (getenv("RECOMP_TEST_CRASH2")) {
            t = CreateThread(NULL, 0, test_crash_second, (LPVOID)(uintptr_t)(seconds > 1 ? seconds : 8), 0, NULL);
            if (t) CloseHandle(t);
        }
    }

    /* W-11: the PC asset overlay is part of every install; without it the game fails later and obscurely. */
    if (
        GetFileAttributesA("pc_assets\\common.mgf") == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesA("..\\pc_assets\\common.mgf") == INVALID_FILE_ATTRIBUTES &&
        !g_overlay_from_env) {
        fprintf(stderr, "[LAUNCH] pc_assets\\common.mgf missing\n");
        if (!getenv("RECOMP_SAVE_DIR"))
        MessageBoxA(NULL, "Game files are missing.\n"
                    "Run the MechAssault 2 installer again on this folder.",
                    "MechAssault 2", MB_ICONERROR);
        return 1;
    }

    /* Step 1: Load XBE */
    if (!load_xbe(YOUR_GAME_XBE_PATH, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Game files are missing.\n"
                    "Run the MechAssault 2 installer again on this folder.",
                    "MechAssault 2", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "[LAUNCH] memory layout failed: exit 1\n");
        if (!getenv("RECOMP_SAVE_DIR"))   /* no box in harness runs (hidden window, nobody to click it) */
        MessageBoxA(NULL, "MechAssault 2 could not reserve the memory it needs.\n\n"
                    "Close overlays and screen recorders, or restart Windows, then try again.\n\n"
                    "Details: the newest .log in %LOCALAPPDATA%\\MechAssault2\\Logs.",
                    "MechAssault 2", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    fprintf(stderr,"Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* D3D's BlockUntilNotBusy polls *[device+0x30] against device+0x2C
     * (0x0040F635-0x0040F648). Complete that guest-memory GPU fence using
     * the runtime's acknowledgement worker, following the allocated device. */
    xbox_Nv2aMirrorFence(0x0041BC68u, 0x2Cu, 0x30u);
    extern void ma2_gpu_software_method(uint32_t);
    extern void nv2a_pb_set_software_method(void (*)(uint32_t));
    nv2a_pb_set_software_method(ma2_gpu_software_method);

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        ma2_pc_storage_initialize(YOUR_GAME_DIR);
    }
    {   /* --quick-test (editor Test): movie skipping starts before the first movie */
        extern void ma2_scn_startup(void);
        ma2_scn_startup();
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
    /* The embedded XUSB driver's queues/free TD pool are protected by raising
     * to dispatch priority on Xbox. Keep its API calls and USB DPC serialized
     * across native threads without coupling unrelated audio/GPU callbacks. */
    xbox_SetDispatchSerializationRange(0x0041D000u,0x0042584Cu);

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* T1-2 FastCode ([Cpu] FastCode / RECOMP_FASTCODE, default 1 since T1
     * session 6: M01b fixed-dt A/B -0.9 ms/frame; 0 = the old code, kept as
     * the fallback). Set once here, before any guest code runs: the fast copies made
     * by tools/fastcode.py (plain guest pointer, inline flat ICALL probe) run
     * instead of the originals, and the tank step swaps its clock in guest
     * memory instead of the TLS copy. */
    {
        int fc = pc_switch("RECOMP_FASTCODE", "Cpu", "FastCode", 1) ? 1 : 0;
        int probe = fc ? recomp_dispatch_fast_init() : 0;
        g_recomp_fastcode = fc;
        if (fc)
            fprintf(stderr, "[FASTCODE] on: flat ICALL probe %s (%d override VAs masked)\n",
                    probe >= 0 ? "on" : "OFF", probe);
        else
            fprintf(stderr, "[FASTCODE] off (old code path)\n");
    }
    /* T1-8: XMM helper lane self-test (one line; FAIL = the build-time
     * RECOMP_XMM_INTRIN helpers differ from the lane-wise reference). */
    {
        int xbad = recomp_xmm_selftest();
        fprintf(stderr, "[XMM-SELFTEST] %s (%d bad lanes)\n", xbad ? "FAIL" : "ok", xbad);
    }

    /* XAPI is part of the guest image and discovers controllers through USB.
     * Route its OHCI register accesses to the runtime's gamepad model. */
    xbox_OhciInit();

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    hang_monitor_start();
    if (getenv("RECOMP_TEST_HANG")) {
        HANDLE self = NULL, h;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        h = CreateThread(NULL, 0, test_hang_thread, self, 0, NULL);
        if (h) CloseHandle(h);
    }

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    /* T1-0: the guest game runs inline on this thread. Register it (TH-1 registry; the placement
     * policy stays a no-op until T1-5) and, with RECOMP_PERF_CPU=1 only, start the 5 s
     * [PERF-CPU]/[PERF-LP]/[PERF-RATE] reporter. */
    pc_thread_role_enter(PC_ROLE_GAME);
    pc_perf_cpu_start();

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        { char shown[MAX_PATH * 2]; fprintf(stderr, "Cannot open XBE: %s\n", xbox_log_path(path, shown, sizeof shown)); }
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}

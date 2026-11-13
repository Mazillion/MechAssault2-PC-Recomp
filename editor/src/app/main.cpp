// MechAssault 2 Scenario Editor: Win32 + Direct3D 11 + Dear ImGui host.
#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>
#include <dbghelp.h>
#include <dxgi.h>
#include <eh.h>
#include <csignal>
#include <stdlib.h>

#include <chrono>
#include <string>
#include <vector>

#include "app.h"
#include "automation.h"
#include "theme.h"
#include "version.h"
#include "ma2models.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11DepthStencilView* g_dsv = nullptr;
static ID3D11Texture2D* g_depth = nullptr;
static UINT g_resize_w = 0, g_resize_h = 0;
static bool g_occluded = false;
static ed::App* g_app = nullptr;
static HWND g_hwnd = nullptr;
static bool g_automation = false;
static bool g_shutdown_saved = false;  // WM_QUERYENDSESSION already wrote the recovery slot

// Errors that end the editor: a message box for a person, stderr under automation (never a window).
static void fatal_box(HWND owner, const std::wstring& msg) {
    if (g_automation) { ma2::log_stderr("fatal: " + ma2::narrow(msg) + "\n"); return; }
    MessageBoxW(owner, msg.c_str(), L"MA2 Scenario Editor", MB_ICONERROR);
}

// Crash handler: writes a minidump and a short log to %LOCALAPPDATA%\MechAssault2\Logs, then tries
// to save the open scenario to the autosave slot so it can be recovered at the next start.
// The save runs on its own thread and is waited for at most `timeout_ms`: a fault inside a lock the crashing thread holds
// (an archive reader's mutex, say) would otherwise hang the handler after the dump was written. *timed_out says why a
// false result is false. The thread also guards itself against a second fault.
static bool emergency_save(unsigned timeout_ms = 15000, bool* timed_out = nullptr) {
    bool finished = false;
    bool ok = ma2::run_with_timeout([] { return g_app && g_app->emergency_save(); }, timeout_ms, &finished);
    if (timed_out) *timed_out = !finished;
    return ok;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_EXECUTE_HANDLER;
    fs::path dir = ed::Settings::local_dir() / L"Logs";
    std::error_code ec;
    fs::create_directories(dir, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"ScenarioEditor-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    fs::path dmp = dir / (std::wstring(stamp) + L".dmp"), log = dir / (std::wstring(stamp) + L".txt");
    HANDLE f = CreateFileW(dmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
                          &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    if (FILE* lf = _wfopen(log.c_str(), L"w")) {
        HMODULE mod = nullptr;
        void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &mod);
        wchar_t mname[MAX_PATH] = L"?";
        if (mod) GetModuleFileNameW(mod, mname, MAX_PATH);
        char line[2048];
        snprintf(line, sizeof line, "MA2 Scenario Editor " MA2SE_VERSION " (" MA2SE_GIT ") crash\nexception %08lX at %p (%ls+0x%llX)\n",
                 ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0, addr, mname,
                 (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
        fputs(ma2::redact_user_paths(std::string_view(line)).c_str(), lf);  // no user name in a log players may post
        fclose(lf);
    }
    bool save_timed_out = false;
    bool saved = emergency_save(15000, &save_timed_out);
    std::wstring msg = L"The editor ran into a problem and has to close.\n\n";
    msg += saved ? L"Your work was saved to the recovery slot; the editor offers to restore it the next time it starts."
           : save_timed_out ? L"Saving your work to the recovery slot did not finish (the editor was stuck), so the last automatic recovery copy, if any, is what remains."
                            : L"Your work could not be saved to the recovery slot (there may have been nothing unsaved, or the save failed).";
    msg += L"\n\nA crash report was written to:\n" + dir.native();
    if (!g_automation) MessageBoxW(g_hwnd, msg.c_str(), L"MA2 Scenario Editor", MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void create_targets() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    D3D11_TEXTURE2D_DESC bd;
    back->GetDesc(&bd);
    back->Release();
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = bd.Width;
    dd.Height = bd.Height;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    g_dev->CreateTexture2D(&dd, nullptr, &g_depth);
    if (g_depth) g_dev->CreateDepthStencilView(g_depth, nullptr, &g_dsv);
}

static void release_targets() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_dsv) { g_dsv->Release(); g_dsv = nullptr; }
    if (g_depth) { g_depth->Release(); g_depth = nullptr; }
}

static void release_device() {
    release_targets();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->ClearState(); g_ctx->Flush(); g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    // Flip model: the compositor shows each presented frame whole. The legacy blit model let a covered or partly covered window
    // flash random bits of old frames while another window was in front (player report 2026-10-25).
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {  // any hardware failure (RDP, an old or broken driver), not only UNSUPPORTED: fall back to WARP
        release_device();
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swap,
                                           &g_dev, &got, &g_ctx);
    }
    if (FAILED(hr)) return false;
    // DXGI's own Alt+Enter would switch this window to exclusive fullscreen (the editor uses Alt for click-through).
    {
        IDXGIFactory* fac = nullptr;
        if (g_swap && SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&fac))) && fac) {
            fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            fac->Release();
        }
    }
    create_targets();
    return true;
}

// Writes the current back buffer to a 32-bit BMP, or a PNG when the name ends in .png (developer
// screenshots: --shot, --play shot, --automation screenshot).
static bool save_backbuffer(const std::wstring& path, int* out_w = nullptr, int* out_h = nullptr) {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return false;
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    g_dev->CreateTexture2D(&d, nullptr, &st);
    bool ok = false;
    if (st) {
        g_ctx->CopyResource(st, back);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint8_t> px(size_t(d.Width) * d.Height * 4);
            for (UINT y = 0; y < d.Height; y++) {
                const uint8_t* s = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
                uint8_t* o = &px[size_t(d.Height - 1 - y) * d.Width * 4];
                for (UINT x = 0; x < d.Width; x++) { o[x * 4] = s[x * 4 + 2]; o[x * 4 + 1] = s[x * 4 + 1]; o[x * 4 + 2] = s[x * 4]; o[x * 4 + 3] = 255; }
            }
            g_ctx->Unmap(st, 0);
            if (out_w) *out_w = int(d.Width);
            if (out_h) *out_h = int(d.Height);
            if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".png") == 0) {
                ma2m::Image img;
                img.width = int(d.Width);
                img.height = int(d.Height);
                img.rgba.resize(px.size());
                for (UINT y = 0; y < d.Height; y++)  // px is bottom-up BGRA (BMP order)
                    for (UINT x = 0; x < d.Width; x++) {
                        const uint8_t* q = &px[(size_t(d.Height - 1 - y) * d.Width + x) * 4];
                        uint8_t* o = &img.rgba[(size_t(y) * d.Width + x) * 4];
                        o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; o[3] = 255;
                    }
                ok = ma2m::WritePng(ma2::narrow(path), img);
                st->Release();
                back->Release();
                return ok;
            }
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(ih);
            ih.biWidth = LONG(d.Width);
            ih.biHeight = LONG(d.Height);
            ih.biPlanes = 1;
            ih.biBitCount = 32;
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = DWORD(fh.bfOffBits + px.size());
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (f) {
                fwrite(&fh, sizeof fh, 1, f);
                fwrite(&ih, sizeof ih, 1, f);
                fwrite(px.data(), 1, px.size(), f);
                fclose(f);
                ok = true;
            }
        }
        st->Release();
    }
    back->Release();
    return ok;
}

// Developer input player (--play script.txt): drives the editor headless through ImGui's input
// queue, one command per line, for repeatable checks of every control. Commands:
//   move X Y | click X Y [L|R|M] | down X Y [L|R|M] | up [L|R|M] | drag X1 Y1 X2 Y2 [frames] [L|R|M]
//   wheel N | key NAME [ctrl|shift|ctrl+shift] (NAME: A-Z, Delete, Escape, Home, Enter, Tab, F1, F5,
//   Up/Down/Left/Right) | hold NAME FRAMES | type TEXT | wait FRAMES | shot FILE.bmp | dump FILE
//   script --flag [arg] | exit
// Mouse coordinates are client pixels of the 1600x900 window.
struct Player {
    std::vector<std::string> lines;
    size_t pc = 0;
    int wait = 0;
    std::vector<std::pair<ImGuiKey, int>> release;   // keys to release after N frames
    std::vector<std::pair<int, int>> mouse_release;  // buttons to release after N frames
    struct Drag { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; int n = 0, i = 0, button = 0; bool active = false; } drag;
    std::wstring shot;                               // saved after this frame renders
    std::string dump;
    bool exit = false;
    float mx = -1, my = -1;                          // scripted mouse, re-sent every frame
};

static ImGuiKey key_named(const std::string& n) {
    if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') return ImGuiKey(ImGuiKey_A + (n[0] - 'A'));
    static const std::pair<const char*, ImGuiKey> k[] = {{"Delete", ImGuiKey_Delete}, {"Escape", ImGuiKey_Escape}, {"Home", ImGuiKey_Home},
        {"Enter", ImGuiKey_Enter}, {"Tab", ImGuiKey_Tab}, {"F1", ImGuiKey_F1}, {"F5", ImGuiKey_F5}, {"Up", ImGuiKey_UpArrow},
        {"Down", ImGuiKey_DownArrow}, {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow}, {"Comma", ImGuiKey_Comma},
        {"F2", ImGuiKey_F2}, {"F9", ImGuiKey_F9}, {"PageUp", ImGuiKey_PageUp}};
    for (auto& [s, v] : k) if (n == s) return v;
    return ImGuiKey_None;
}

static int button_named(const std::string& b) { return b == "R" ? 1 : b == "M" ? 2 : 0; }

// Feeds this frame's input.
static void player_step(Player& p, ed::App& app) {
    ImGuiIO& io = ImGui::GetIO();
    // The backend queues the real cursor each frame while the app looks focused; keep ours last.
    auto pos = [&](float x, float y) { p.mx = x; p.my = y; io.AddMousePosEvent(x, y); };
    if (p.mx >= 0) io.AddMousePosEvent(p.mx, p.my);
    for (auto it = p.release.begin(); it != p.release.end();) {
        if (--it->second <= 0) { io.AddKeyEvent(it->first, false); it = p.release.erase(it); } else ++it;
    }
    for (auto it = p.mouse_release.begin(); it != p.mouse_release.end();) {
        if (--it->second <= 0) { io.AddMouseButtonEvent(it->first, false); it = p.mouse_release.erase(it); } else ++it;
    }
    if (p.drag.active) {
        float t = float(++p.drag.i) / float(p.drag.n);
        pos(p.drag.x0 + (p.drag.x1 - p.drag.x0) * t, p.drag.y0 + (p.drag.y1 - p.drag.y0) * t);
        if (p.drag.i >= p.drag.n) { io.AddMouseButtonEvent(p.drag.button, false); p.drag.active = false; p.wait = 2; }
        return;
    }
    if (p.wait > 0) { p.wait--; return; }
    while (p.pc < p.lines.size()) {
        const std::string line = p.lines[p.pc++];
        auto word = [&](size_t i) {
            auto w = ma2::split(line, ' ');
            std::vector<std::string> v;
            for (auto& x : w) if (!x.empty()) v.push_back(x);
            return i < v.size() ? v[i] : std::string();
        };
        auto num = [&](size_t i) { return float(atof(word(i).c_str())); };
        std::string cmd = word(0), rest = line.size() > cmd.size() + 1 ? line.substr(cmd.size() + 1) : "";
        if (cmd == "move") {
            pos(num(1), num(2));
            p.wait = 1;
            return;
        } else if (cmd == "click") {
            int btn = button_named(word(3));
            pos(num(1), num(2));
            io.AddMouseButtonEvent(btn, true);
            p.mouse_release.push_back({btn, 2});
            p.wait = 3;
            return;
        } else if (cmd == "down") {
            pos(num(1), num(2));
            io.AddMouseButtonEvent(button_named(word(3)), true);
            p.wait = 1;
            return;
        } else if (cmd == "up") {
            io.AddMouseButtonEvent(button_named(word(1)), false);
            p.wait = 1;
            return;
        } else if (cmd == "drag") {
            int n = int(num(5));
            p.drag = {num(1), num(2), num(3), num(4), n > 0 ? n : 20, 0, button_named(word(6)), true};
            pos(p.drag.x0, p.drag.y0);
            io.AddMouseButtonEvent(p.drag.button, true);
            return;
        } else if (cmd == "wheel") {
            io.AddMouseWheelEvent(0, num(1));
            p.wait = 1;
            return;
        } else if (cmd == "key") {
            std::string mods = word(2);
            if (mods.find("ctrl") != std::string::npos) { io.AddKeyEvent(ImGuiMod_Ctrl, true); p.release.push_back({ImGuiMod_Ctrl, 3}); }
            if (mods.find("shift") != std::string::npos) { io.AddKeyEvent(ImGuiMod_Shift, true); p.release.push_back({ImGuiMod_Shift, 3}); }
            if (mods.find("alt") != std::string::npos) { io.AddKeyEvent(ImGuiMod_Alt, true); p.release.push_back({ImGuiMod_Alt, 3}); }
            ImGuiKey k = key_named(word(1));
            if (k != ImGuiKey_None) { io.AddKeyEvent(k, true); p.release.push_back({k, 2}); }
            p.wait = 4;
            return;
        } else if (cmd == "hold") {
            ImGuiKey k = key_named(word(1));
            int n = int(num(2));
            if (k != ImGuiKey_None) { io.AddKeyEvent(k, true); p.release.push_back({k, n}); }
            p.wait = n + 2;
            return;
        } else if (cmd == "type") {
            io.AddInputCharactersUTF8(rest.c_str());
            p.wait = 2;
            return;
        } else if (cmd == "wait") {
            p.wait = int(num(1));
            return;
        } else if (cmd == "shot") {
            p.shot = ma2::widen(rest);
            return;
        } else if (cmd == "dump") {
            p.dump = rest;
            return;
        } else if (cmd == "script") {
            app.script(ma2::widen(word(1)), ma2::widen(rest.size() > word(1).size() + 1 ? rest.substr(word(1).size() + 1) : ""));
        } else if (cmd == "exit") {
            p.exit = true;
            return;
        }
    }
    p.exit = true;
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resize_w = LOWORD(lp);
            g_resize_h = HIWORD(lp);
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            if (g_app && !g_app->confirm_close()) return 0;
            break;
        case WM_QUERYENDSESSION:
            // Logoff / restart / Windows Update: the process is ended without WM_CLOSE, often seconds after this message. Save to the
            // recovery slot now (not later in WM_ENDSESSION) and tell Windows why it has to wait; never veto the shutdown.
            g_shutdown_saved = false;
            if (g_app && g_app->has_unsaved()) {
                ShutdownBlockReasonCreate(hwnd, L"Saving your unsaved scenario to the recovery slot...");
                g_shutdown_saved = emergency_save(8000);
                ShutdownBlockReasonDestroy(hwnd);
            }
            return TRUE;
        case WM_ENDSESSION:
            // wp true = the session is really ending. Normally the save above is enough; this covers an end without a query.
            if (wp && g_app && !g_shutdown_saved && g_app->has_unsaved()) emergency_save(5000);
            if (!wp) g_shutdown_saved = false;  // cancelled: the editor keeps running, its own slot stays current
            return 0;
        case WM_DPICHANGED: {
            // The window moved to a monitor with another scaling (or the scaling was changed): rescale the UI, and take the size
            // Windows suggests so the window keeps its physical size. The borderless from-game window keeps covering its monitor.
            if (g_app) g_app->set_base_dpi(ma2::dpi_to_scale(HIWORD(wp)));
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            if (r && !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_POPUP))
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT plen = DragQueryFileW(drop, 0, nullptr, 0);  // no MAX_PATH limit (long paths)
            std::wstring path(plen + 1, L'\0');
            if (plen && DragQueryFileW(drop, 0, path.data(), plen + 1)) {
                path.resize(plen);
                if (g_app && g_app->confirm_close()) g_app->open_path(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(crash_filter);
    // Three ways to die silently (no dump, no recovery save) go through the filter instead: std::terminate / abort,
    // the CRT invalid-parameter handler, and a stack overflow (the guarantee leaves the filter enough stack).
    set_terminate([] { RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    });
    // A stray abort() (the CRT's, not ImGui's) reaches the crash filter too instead of the "Abort / Retry / Ignore" box.
    signal(SIGABRT, [](int) { RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 0, nullptr); });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    { ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee); }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    // The game's icon with a yellow "SE" badge (res/ma2se.ico, resource 101): title bar, Alt+Tab, taskbar.
    HICON icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON icon_sm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, icon, LoadCursor(nullptr, IDC_ARROW),
                   nullptr, nullptr, L"MA2ScenarioEditor", icon_sm};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MA2 Scenario Editor", WS_OVERLAPPEDWINDOW, 60, 40, int(1600 * scale),
                              int(960 * scale), nullptr, nullptr, inst, nullptr);
    g_hwnd = hwnd;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring shot, play, automation, settings_file;
    bool automation_show = false;
    std::vector<std::pair<std::wstring, std::wstring>> script;
    std::wstring open_file;
    // Opened from the game's Custom Scenarios screen (ux/spec-ingame-editor.md 6): --from-game <game HWND> --saved-list <file>
    // [--file <scenario>]. Borderless over the game's whole monitor; exit code 2 = play the test= file.
    bool from_game = false;
    HWND game_hwnd = nullptr;
    std::wstring saved_list;
    for (int i = 1; argv && i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--from-game") {
            from_game = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') game_hwnd = (HWND)(uintptr_t)wcstoull(argv[++i], nullptr, 0);
            continue;
        }
        if ((a == L"--from-game-result" || a == L"--saved-list") && i + 1 < argc) { saved_list = argv[++i]; continue; }
        if (a == L"--file" && i + 1 < argc) { open_file = argv[++i]; continue; }
        if (a == L"--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == L"--play" && i + 1 < argc) play = argv[++i];
        else if (a == L"--automation") automation = i + 1 < argc && argv[i + 1][0] != L'-' ? argv[++i] : L"ma2se";
        else if (a == L"--automation-show") automation_show = true;
        else if (a == L"--settings" && i + 1 < argc) settings_file = argv[++i];
        else if (a.size() > 2 && a[0] == L'-' && a[1] == L'-') script.push_back({a, i + 1 < argc && argv[i + 1][0] != L'-' ? argv[++i] : L""});
        else open_file = a;
    }
    if (argv) LocalFree(argv);
    g_automation = !automation.empty();
    // Automation runs on its own settings (so tests never change the user's preferences or
    // recovery slot): --settings FILE, else %TEMP%\MA2SE-automation\ScenarioEditor.ini.
    if (g_automation && settings_file.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        settings_file = (fs::path(tmp) / L"MA2SE-automation" / L"ScenarioEditor.ini").native();
    }
    if (!settings_file.empty()) ed::Settings::set_ini_override(settings_file);
    // Automation: nothing may ever pop up on the user's screen. Question boxes and file pickers are
    // answered by the automation client from the start (opening the file below runs before the pipe
    // is up), and Windows' own crash / missing-disk boxes are switched off.
    if (g_automation) {
        ed::automation_dialogs().active = true;
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    }
    // Automation: the window is never activated; it stays hidden unless --automation-show (then it
    // appears without taking focus).
    bool headless = !shot.empty() || !play.empty() || (g_automation && !automation_show);
    // SWP_NOACTIVATE: without it this call activates the (never shown) window and takes the
    // keyboard focus from whatever the user is typing in.
    if (headless) SetWindowPos(hwnd, nullptr, 0, 0, 1600, 900, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!create_device(hwnd)) {
        fatal_box(hwnd, L"Direct3D 11 is not available.");
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);
    if (g_automation && automation_show) {
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 1600, 900, SWP_NOMOVE | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    } else if (!headless && from_game) {
        // "Inside" the game: no title bar, the game's whole monitor (rcMonitor covers the taskbar), in front, with the focus
        // (the game is the foreground process that started us, so Windows lets us take it).
        HMONITOR mon = game_hwnd && IsWindow(game_hwnd) ? MonitorFromWindow(game_hwnd, MONITOR_DEFAULTTONEAREST)
                                                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        scale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);  // the game's monitor, not the primary one
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        UpdateWindow(hwnd);
    } else if (!headless) {
        ShowWindow(hwnd, SW_SHOWMAXIMIZED);
        UpdateWindow(hwnd);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No keyboard navigation: it keeps panels focused after a click, which swallowed the map's
    // keyboard shortcuts (WASD, T, Q/E, Del) until the map was clicked.
    io.IniFilename = nullptr;
    // ImGui's duplicate-ID warning panel is for developers: on only with MA2SE_ID_AUDIT=1, which
    // also makes --play dumps list every conflict hovered (App::debug_state).
    io.ConfigDebugHighlightIdConflicts = _wgetenv(L"MA2SE_ID_AUDIT") != nullptr;
    // ImGui's own error recovery asserts (a mismatched Begin/End or PushID) are for developers too: with the audit flag off they
    // recover quietly instead of raising a C++ runtime box in front of the user.
    io.ConfigErrorRecoveryEnableAssert = io.ConfigDebugHighlightIdConflicts;
    io.ConfigErrorRecoveryEnableTooltip = io.ConfigDebugHighlightIdConflicts;
    // The MechAssault 2 front-end look (theme.cpp, ux/style-guide.md): the only place colours, shapes and fonts are set.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    theme::apply(style);
    ImGuiStyle raw_style = style;  // the theme before any DPI scaling: WM_DPICHANGED rebuilds the style from it
    raw_style.FontSizeBase = 16.f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FontSizeBase = 16.f;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    theme::load_fonts(io, windir);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    ed::App app;
    g_app = &app;
    app.set_raw_style(raw_style);
    std::string err;
    if (!app.init(hwnd, g_dev, g_ctx, scale, &err)) {
        fatal_box(hwnd, ma2::widen(err));
        return 1;
    }
    app.background = headless || g_automation;
    if (from_game) {
        app.start_from_game(open_file);
        // The game always sends --file for the selected scenario; after an editor crash its note promises the autosave back.
        if (!headless && !g_automation) app.offer_recovery();
    } else {
        // Offer the recovery slot before the first save or exit can delete it, also when the editor was started with a file
        // (double-click, Open with, drag onto the exe).
        if (!open_file.empty()) app.open_path(open_file);
        if (!headless && !g_automation) app.offer_recovery();  // Yes replaces the opened file with the recovered work
        // Scenarios an older editor saved (before the SavedWith stamp) are offered an update; not when the game started the editor.
        if (!headless && !g_automation) app.offer_update_old_scenarios(false);
    }
    ed::Automation automation_srv;
    if (g_automation) {
        std::string aerr;
        if (!automation_srv.start(automation, &aerr)) {
            ma2::log_stderr("automation: " + aerr + "\n");
            return 2;
        }
    }
    int frame_no = 0;
    Player player;
    if (!play.empty()) {
        Bytes b;
        ma2::read_file(play, b);
        for (auto& l : ma2::split(std::string(b.begin(), b.end()), '\n')) {
            std::string t = ma2::trim(l);
            if (!t.empty() && t[0] != '#') player.lines.push_back(t);
        }
    }

    // Recreates the device after it was removed or reset (driver update, GPU reset, a game
    // switching display modes) or when the swap chain cannot be resized.
    auto recover_device = [&]() {
        ImGui_ImplDX11_Shutdown();
        app.device_lost();
        release_device();
        for (int attempt = 0; attempt < 50 && !create_device(hwnd); attempt++) Sleep(200);
        if (!g_dev) {
            bool saved = app.emergency_save();  // first: the box waits for a click, and the work must already be on disk
            fatal_box(hwnd, saved ? L"The graphics device stopped responding and could not be restarted. Your work is in the recovery slot."
                                  : L"The graphics device stopped responding and could not be restarted.");
            ExitProcess(1);
        }
        ImGui_ImplDX11_Init(g_dev, g_ctx);
        std::string e;
        if (!app.device_restored(g_dev, g_ctx, &e)) {
            bool saved = app.emergency_save();
            fatal_box(hwnd, L"The graphics device could not be restarted: " + ma2::widen(e) +
                                (saved ? L"\n\nYour work is in the recovery slot." : L""));
            ExitProcess(1);
        }
    };

    auto last = std::chrono::steady_clock::now();
    bool done = false;
    while (!done) {
        if (frame_no == 2) for (auto& [c, a] : script) app.script(c, a);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        // Automation keeps drawing while the window is covered or minimized (screenshots).
        if (!g_automation && g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) { Sleep(15); continue; }
        g_occluded = false;
        // Not the foreground window (and not scripted): a steady low rate, so a covered editor neither burns the GPU nor shows
        // frames in bursts as the compositor throttles it.
        if (!g_automation && play.empty() && shot.empty() && GetForegroundWindow() != hwnd && !IsIconic(hwnd)) {
            static auto last_bg = std::chrono::steady_clock::time_point{};
            auto t = std::chrono::steady_clock::now();
            if (t - last_bg < std::chrono::milliseconds(66)) { Sleep(8); continue; }
            last_bg = t;
        }
        if (g_resize_w && g_resize_h) {
            release_targets();
            HRESULT rhr = g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            if (FAILED(rhr)) recover_device();
            else create_targets();
        }
        if (!g_rtv) { recover_device(); if (!g_rtv) { Sleep(50); continue; } }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (!play.empty()) {
            dt = 1.f / 60.f;  // repeatable strokes and timers
            if (player.exit) break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // Scripted input goes after the backend's own per-frame mouse update, which otherwise
        // overrides it while someone uses the real mouse on another window.
        if (!play.empty() && frame_no > 3) player_step(player, app);
        if (g_automation && frame_no > 3) automation_srv.pre_frame(app);
        ImGui::NewFrame();
        RECT rc;
        GetClientRect(hwnd, &rc);
        static int last_w = 1600, last_h = 900;  // a minimized window has no client area
        if (rc.right - rc.left > 0 && rc.bottom - rc.top > 0) { last_w = rc.right - rc.left; last_h = rc.bottom - rc.top; }
        app.frame(dt, last_w, last_h);
        if (g_automation && automation_srv.quit_due()) {  // a few frames after the reply went out
            if (automation_srv.quit_forced()) break;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);  // asks about unsaved changes (dialogs answer)
        }
        if (app.quit_requested()) {
            app.clear_quit();
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        ImGui::Render();
        const float clear[4] = {0.10f, 0.11f, 0.13f, 1.f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);
        app.render3d();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Headless shots wait for queued models (at most 600 frames), so nothing is a loading box.
        if (!shot.empty() && ++frame_no >= 10 && (app.models_pending() == 0 || frame_no >= 600)) {
            save_backbuffer(shot);
            break;
        }
        if (shot.empty()) frame_no++;
        if (!player.shot.empty()) { save_backbuffer(player.shot); player.shot.clear(); }
        if (g_automation) automation_srv.post_render(app, [](const std::wstring& p, int* w, int* h) { return save_backbuffer(p, w, h); });
        if (!player.dump.empty()) {
            std::string d = app.debug_state();
            ma2::write_file(fs::path(ma2::widen(player.dump)), d.data(), d.size());
            player.dump.clear();
        }
        HRESULT hr = g_swap->Present(headless ? 0 : 1, 0);
        if (g_automation && headless) Sleep(8);  // ~60-100 frames a second, light on the CPU
        g_occluded = hr == DXGI_STATUS_OCCLUDED;
        app.end_frame();
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || g_dev->GetDeviceRemovedReason() != S_OK) recover_device();
    }
    automation_srv.stop();
    // Back to the game: the result file (opened=, saved= lines, test= with code 2) it reads, then the exit code (0 back, 2 play).
    const int exit_code = from_game ? app.exit_code() : 0;
    if (from_game && !saved_list.empty()) app.write_saved_list(saved_list);
    app.shutdown();
    app.end_frame();
    g_app = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_targets();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();
    return exit_code;
}

// MechAssault 2 Recomp Installer: the single-file installer's native shell.
//
// The kit (a Python runtime, the xboxrecomp toolkit, the PC release source and patch set, assets,
// the Enhancer scripts and the installer itself) is appended to this exe by pack_installer.py:
//
//   [stub exe][payload: "MA2KIT01", u32 count, count x {u16 len, path utf-8, u64 size, u64 packed,
//    u8 method (0 stored, 1 LZMS)}, data...][trailer: u64 offset, u64 size, char id[16], "MA2KEND1"]
//
// Run without arguments it unpacks the kit to %TEMP%\MA2RecompInstaller-<id> (a small themed splash with
// progress; a stock TaskDialog if the splash can't be created),
// runs the installer window (python\pythonw.exe installer\gui.py) and deletes the unpacked kit
// when the window closes. Run with arguments it is the command-line installer: the arguments go
// to installer\install.py (--help lists them).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <compressapi.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <cmath>

#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

namespace fs = std::filesystem;

static const wchar_t *TITLE = L"MechAssault 2 Recomp Installer";

struct Trailer { uint64_t offset, size; char id[16]; char magic[8]; };

static std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring self_path() {
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size()));
    return p;
}

static void fail(const std::wstring &message, bool console) {
    if (console) fwprintf(stderr, L"%s\n", message.c_str());
    else MessageBoxW(nullptr, message.c_str(), TITLE, MB_ICONERROR | MB_OK);
}

// ---------------------------------------------------------------- unpacking

struct Unpack {
    std::wstring exe, dest;
    std::atomic<uint64_t> done{0};
    uint64_t total = 1;
    std::atomic<bool> cancel{false}, finished{false};
    std::wstring error;
};

static bool read_at(HANDLE f, uint64_t offset, void *buf, size_t n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    auto *p = (uint8_t *)buf;
    while (n) {
        DWORD chunk = (DWORD)std::min<size_t>(n, 1u << 30), got = 0;
        if (!ReadFile(f, p, chunk, &got, nullptr) || !got) return false;
        p += got; n -= got;
    }
    return true;
}

// The trailer is the last 40 bytes the packer wrote. A signed release has an Authenticode certificate table
// appended after it (the PE security directory names its file offset; signtool pads to 8 bytes before it), so the
// trailer is looked for at the end of the file first, then just before the certificate table. A trailer counts only
// if its payload starts with "MA2KIT01" and ends where the trailer begins.
static bool trailer_at(HANDLE f, uint64_t end, Trailer &t) {
    char head[8];
    return end > (uint64_t)sizeof t && read_at(f, end - sizeof t, &t, sizeof t) && !memcmp(t.magic, "MA2KEND1", 8) &&
           t.offset + t.size == end - sizeof t && read_at(f, t.offset, head, 8) && !memcmp(head, "MA2KIT01", 8);
}

static uint64_t certificate_table(HANDLE f, uint64_t file_size) {
    IMAGE_DOS_HEADER dos; DWORD sig; IMAGE_FILE_HEADER fh; WORD magic;
    if (!read_at(f, 0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
    uint64_t pe = (uint64_t)(uint32_t)dos.e_lfanew, opt = pe + 4 + sizeof fh;
    if (!read_at(f, pe, &sig, 4) || sig != IMAGE_NT_SIGNATURE || !read_at(f, pe + 4, &fh, sizeof fh) || !read_at(f, opt, &magic, 2)) return 0;
    uint64_t dirs = opt + (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? 112 : magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ? 96 : 0);
    if (dirs == opt || fh.SizeOfOptionalHeader < dirs - opt + 8 * (IMAGE_DIRECTORY_ENTRY_SECURITY + 1)) return 0;
    IMAGE_DATA_DIRECTORY security;
    if (!read_at(f, dirs + 8 * IMAGE_DIRECTORY_ENTRY_SECURITY, &security, sizeof security)) return 0;
    if (!security.VirtualAddress || !security.Size || (uint64_t)security.VirtualAddress + security.Size > file_size) return 0;
    return security.VirtualAddress;                // for this directory the "address" is a file offset
}

static bool read_trailer(const std::wstring &exe, Trailer &t) {
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    uint64_t end = (uint64_t)size.QuadPart;
    bool ok = trailer_at(f, end, t);
    if (!ok) {
        uint64_t cert = certificate_table(f, end);
        for (uint64_t pad = 0; cert && !ok && pad < 8 && pad < cert; pad++)   // signtool aligns the table to 8 bytes
            ok = trailer_at(f, cert - pad, t);
    }
    CloseHandle(f);
    return ok;
}

static void unpack(Unpack *u, Trailer t) {
    HANDLE f = CreateFileW(u->exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    DECOMPRESSOR_HANDLE dec = nullptr;
    auto finish = [&](const wchar_t *err) {
        if (err && u->error.empty()) u->error = err;
        if (dec) CloseDecompressor(dec);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        u->finished = true;
    };
    if (f == INVALID_HANDLE_VALUE) return finish(L"Cannot read the installer file.");
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &dec)) return finish(L"Windows decompression is unavailable.");
    std::vector<uint8_t> payload_head(12);
    if (!read_at(f, t.offset, payload_head.data(), 12) || memcmp(payload_head.data(), "MA2KIT01", 8))
        return finish(L"The installer file is damaged (no kit). Download it again.");
    uint32_t count; memcpy(&count, payload_head.data() + 8, 4);
    struct Entry { std::string path; uint64_t size, packed; uint8_t method; };
    std::vector<Entry> entries; entries.reserve(count);
    uint64_t pos = t.offset + 12, total = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t len;
        if (!read_at(f, pos, &len, 2)) return finish(L"The installer file is damaged (index).");
        Entry e; e.path.resize(len);
        if (!read_at(f, pos + 2, e.path.data(), len) || !read_at(f, pos + 2 + len, &e.size, 8) ||
            !read_at(f, pos + 10 + len, &e.packed, 8) || !read_at(f, pos + 18 + len, &e.method, 1))
            return finish(L"The installer file is damaged (index).");
        pos += 19 + len; total += e.size; entries.push_back(std::move(e));
    }
    u->total = total ? total : 1;
    std::vector<uint8_t> packed, data;
    for (auto &e : entries) {
        if (u->cancel) return finish(L"Cancelled.");
        packed.resize((size_t)e.packed);
        if (e.packed && !read_at(f, pos, packed.data(), (size_t)e.packed)) return finish(L"The installer file is damaged (data).");
        pos += e.packed;
        const uint8_t *bytes = packed.data();
        if (e.method == 1) {
            data.resize((size_t)e.size);
            SIZE_T out = 0;
            if (!Decompress(dec, packed.data(), (SIZE_T)e.packed, data.data(), (SIZE_T)e.size, &out) || out != e.size)
                return finish(L"The installer file is damaged (a file did not unpack).");
            bytes = data.data();
        }
        fs::path target = fs::path(u->dest) / fs::path(widen(e.path));
        std::error_code ec; fs::create_directories(target.parent_path(), ec);
        HANDLE o = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (o == INVALID_HANDLE_VALUE) return finish(L"Cannot write to the temporary folder. If your antivirus is blocking it, allow %TEMP%\\MA2RecompInstaller-* and run the installer again.");
        uint64_t left = e.size; const uint8_t *p = bytes; bool ok = true;
        while (left && ok) {
            DWORD chunk = (DWORD)std::min<uint64_t>(left, 1u << 30), wrote = 0;
            ok = WriteFile(o, p, chunk, &wrote, nullptr) && wrote == chunk;
            p += chunk; left -= chunk;
        }
        CloseHandle(o);
        if (!ok) return finish(L"Cannot write to the temporary folder (disk full?).");
        u->done += e.size;
    }
    // Marks a complete unpack so a second run of the same installer can reuse it.
    HANDLE m = CreateFileW((fs::path(u->dest) / L".unpacked").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
    finish(nullptr);
}

static HRESULT CALLBACK progress_cb(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR ref) {
    auto *u = (Unpack *)ref;
    if (msg == TDN_CREATED) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
    if (msg == TDN_TIMER) {
        SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(u->done * 1000 / u->total), 0);
        if (u->finished) SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
    }
    if (msg == TDN_BUTTON_CLICKED && !u->finished) { u->cancel = true; return S_FALSE; }
    return S_OK;
}

// ---------------------------------------------------------------- themed splash (GDI)
// A small borderless window in the installer's gunmetal + amber look. The colours are fixed (the private fonts
// aren't loaded yet): Bahnschrift if Windows has it, else Segoe UI. Cancel is a link, Esc also cancels.

static const COLORREF K_BG = RGB(11, 14, 17), K_PANEL = RGB(27, 35, 42), K_DIM = RGB(122, 84, 16), K_AMBER = RGB(255, 178, 26),
                      K_TEXT = RGB(220, 227, 232), K_MUTED = RGB(142, 157, 168);

struct Splash {
    Unpack *u = nullptr; int dpi = 96; RECT cancel = {}; int fixed = -1;     // fixed >= 0: draw this percent (screenshot build)
    HFONT title = nullptr, body = nullptr, small = nullptr, kicker = nullptr;
};

static HFONT make_font(int dpi, int pt, bool bold, const wchar_t *face) {
    HFONT f = CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    return f;
}

static void splash_draw(HWND h, Splash *s, HDC dc) {
    RECT rc; GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom, d = s->dpi;
    auto S = [&](int v) { return MulDiv(v, d, 96); };
    HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, W, H); HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(K_BG); FillRect(mem, &rc, bg); DeleteObject(bg);
    int pct = s->fixed >= 0 ? s->fixed : (int)(s->u->done * 100 / s->u->total);
    // outline with chamfered top-left and bottom-right corners
    int cut = S(14);
    POINT pts[7] = {{cut, 0}, {W - 1, 0}, {W - 1, H - 1 - cut}, {W - 1 - cut, H - 1}, {0, H - 1}, {0, cut}, {cut, 0}};
    HPEN pen = CreatePen(PS_SOLID, 1, K_DIM); HGDIOBJ op = SelectObject(mem, pen); Polyline(mem, pts, 7); SelectObject(mem, op); DeleteObject(pen);
    HPEN amber = CreatePen(PS_SOLID, 2, K_AMBER); op = SelectObject(mem, amber);
    MoveToEx(mem, W - 1 - S(16), 1, nullptr); LineTo(mem, W - 2, 1); LineTo(mem, W - 2, S(16));             // corner ticks
    MoveToEx(mem, 1, H - 1 - S(16), nullptr); LineTo(mem, 1, H - 2); LineTo(mem, S(16), H - 2);
    SelectObject(mem, op);
    // emblem: hexagon + chevron
    int cx = W - S(52), cy = S(48), r = S(24);
    POINT hex[7]; for (int i = 0; i < 7; i++) { double a = 3.14159265 / 3 * (i % 6); hex[i] = {cx + (LONG)(r * cos(a)), cy + (LONG)(r * sin(a))}; }
    HBRUSH fill = CreateSolidBrush(K_PANEL); HGDIOBJ ob = SelectObject(mem, fill); op = SelectObject(mem, amber); Polygon(mem, hex, 6);
    MoveToEx(mem, cx - r / 4, cy + r / 6, nullptr); LineTo(mem, cx, cy - r / 5); LineTo(mem, cx + r / 4, cy + r / 6);
    SelectObject(mem, op); SelectObject(mem, ob); DeleteObject(fill); DeleteObject(amber);
    SetBkMode(mem, TRANSPARENT);
    auto text = [&](HFONT f, COLORREF c, const wchar_t *str, int x, int y, UINT fmt = DT_LEFT) {
        SelectObject(mem, f); SetTextColor(mem, c); RECT r2 = {x, y, W - S(90), y + S(40)}; DrawTextW(mem, str, -1, &r2, fmt | DT_NOPREFIX | DT_SINGLELINE);
    };
    text(s->kicker, K_AMBER, L"UNPACKING", S(24), S(18));
    text(s->title, K_TEXT, L"MechAssault 2 Recomp Installer", S(24), S(34));
    wchar_t line[64]; swprintf(line, 64, L"Preparing... %d %%", pct);
    text(s->body, K_MUTED, line, S(24), S(72));
    // progress bar
    RECT track = {S(24), S(100), W - S(24), S(106)};
    HBRUSH tb = CreateSolidBrush(K_PANEL); FillRect(mem, &track, tb); DeleteObject(tb);
    RECT bar = track; bar.right = track.left + (track.right - track.left) * pct / 100;
    HBRUSH ab = CreateSolidBrush(K_AMBER); FillRect(mem, &bar, ab); DeleteObject(ab);
    // cancel link (right aligned under the bar)
    SelectObject(mem, s->small); SetTextColor(mem, K_AMBER);
    SIZE sz; GetTextExtentPoint32W(mem, L"Cancel", 6, &sz);
    s->cancel = {W - S(24) - sz.cx - S(6), S(112), W - S(24) + S(2), S(112) + sz.cy + S(6)};
    TextOutW(mem, s->cancel.left + S(3), s->cancel.top + S(2), L"Cancel", 6);
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
}

static LRESULT CALLBACK splash_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto *s = (Splash *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_TIMER:
        if (s && s->u && s->u->finished) { PostQuitMessage(0); return 0; }
        InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (s) splash_draw(h, s, dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)}; ScreenToClient(h, &p);
        return (s && PtInRect(&s->cancel, p)) ? HTCLIENT : HTCAPTION;
    }
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        if (s && PtInRect(&s->cancel, p)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
        break;
    }
    case WM_LBUTTONUP: { POINT p = {(short)LOWORD(l), (short)HIWORD(l)}; if (s && s->u && PtInRect(&s->cancel, p)) s->u->cancel = true; return 0; }
    case WM_KEYDOWN: if (w == VK_ESCAPE && s && s->u) s->u->cancel = true; return 0;
    case WM_CLOSE: if (s && s->u) s->u->cancel = true; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Shows the splash until the unpack finishes. Returns false if the window could not be created (caller falls back).
static bool run_splash(Unpack &u, int fixed_pct, const wchar_t *shot_path) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {}; wc.lpfnWndProc = splash_proc; wc.hInstance = inst; wc.lpszClassName = L"MA2RecompSplash";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    int dpi = (int)GetDpiForSystem(); if (dpi < 96) dpi = 96;
    int W = MulDiv(420, dpi, 96), H = MulDiv(150, dpi, 96);
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - H) / 3;
    DWORD ex = WS_EX_APPWINDOW | (shot_path ? WS_EX_LAYERED | WS_EX_TOOLWINDOW : 0);
    HWND h = CreateWindowExW(ex, wc.lpszClassName, TITLE, WS_POPUP, x, y, W, H, nullptr, nullptr, inst, nullptr);
    if (!h) return false;
    Splash s; s.u = &u; s.dpi = dpi; s.fixed = fixed_pct;
    const wchar_t *face = L"Bahnschrift";
    s.title = make_font(dpi, 15, true, face); s.kicker = make_font(dpi, 9, true, face); s.body = make_font(dpi, 10, false, L"Segoe UI"); s.small = make_font(dpi, 10, false, L"Segoe UI");
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&s);
    int cut = MulDiv(14, dpi, 96);
    POINT poly[6] = {{cut, 0}, {W, 0}, {W, H - cut}, {W - cut, H}, {0, H}, {0, cut}};
    SetWindowRgn(h, CreatePolygonRgn(poly, 6, WINDING), TRUE);
    if (shot_path) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);          // screenshot build: fully transparent
    ShowWindow(h, SW_SHOWNOACTIVATE); UpdateWindow(h);
    if (shot_path) {                                                         // PrintWindow into a 24-bit BMP, then stop
        HDC dc = GetDC(h); HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, W, H); SelectObject(mem, bmp);
        PrintWindow(h, mem, 2);
        BITMAPINFOHEADER bi = {sizeof bi, W, -H, 1, 24, BI_RGB};
        int stride = (W * 3 + 3) & ~3; std::vector<uint8_t> px((size_t)stride * H);
        GetDIBits(mem, bmp, 0, H, px.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
        bi.biHeight = -H; BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi + px.size()), 0, 0, (DWORD)(sizeof fh + sizeof bi)};
        FILE *f = _wfopen(shot_path, L"wb"); if (f) { fwrite(&fh, sizeof fh, 1, f); fwrite(&bi, sizeof bi, 1, f); fwrite(px.data(), 1, px.size(), f); fclose(f); }
        DeleteObject(bmp); DeleteDC(mem); ReleaseDC(h, dc); DestroyWindow(h); return true;
    }
    SetTimer(h, 1, 100, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DestroyWindow(h);
    for (HFONT f : {s.title, s.kicker, s.body, s.small}) DeleteObject(f);
    return true;
}

static bool unpack_kit(Unpack &u, const Trailer &t, bool console) {
    // Reuse an earlier unpack only if it is complete (a killed run or an antivirus may have left holes).
    if (fs::exists(fs::path(u.dest) / L".unpacked")) {
        if (fs::exists(fs::path(u.dest) / L"python" / L"python.exe") && fs::exists(fs::path(u.dest) / L"python" / L"pythonw.exe") &&
            fs::exists(fs::path(u.dest) / L"installer" / L"install.py") && fs::exists(fs::path(u.dest) / L"installer" / L"gui.py")) return true;
    }
    std::error_code ec; fs::remove_all(u.dest, ec); fs::create_directories(u.dest, ec);
    std::thread worker(unpack, &u, t);
    if (console) {
        int last = -1;
        while (!u.finished) {
            int pct = (int)(u.done * 100 / u.total);
            if (pct / 10 != last) { last = pct / 10; fwprintf(stderr, L"Unpacking the installer... %d%%\n", pct); }
            Sleep(100);
        }
    } else if (run_splash(u, -1, nullptr)) {
        if (!u.finished) u.cancel = true;
    } else {
        TASKDIALOGCONFIG c = {sizeof c};
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = TITLE;
        c.hInstance = GetModuleHandleW(nullptr);
        c.dwFlags |= TDF_USE_HICON_MAIN;
        c.hMainIcon = (HICON)LoadImageW(c.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
        c.pszMainInstruction = L"Preparing the installer";
        c.pszContent = L"Unpacking the recompilation toolkit, the PC release source and its tools...";
        c.pfCallback = progress_cb; c.lpCallbackData = (LONG_PTR)&u;
        TaskDialogIndirect(&c, nullptr, nullptr, nullptr);
        if (!u.finished) u.cancel = true;
    }
    worker.join();
    if (!u.error.empty()) {
        fs::remove_all(u.dest, ec);
        if (u.error != L"Cancelled.") fail(u.error, console);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- running

static std::wstring quote(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') { slashes++; continue; }
        if (ch == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0; out += ch;
    }
    out.append(slashes * 2, L'\\'); out += L'"';
    return out;
}

static HANDLE std_in, std_out, std_err;   // as this process received them (redirected, or none)

static DWORD run(const std::wstring &cmd, const std::wstring &cwd, bool console) {
    STARTUPINFOW si = {sizeof si};
    HANDLE in = std_in, out = std_out, err = std_err;
    BOOL inherit = FALSE;
    if (console && out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_CHAR) {
        si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = in; si.hStdOutput = out; si.hStdError = err ? err : out;
        for (HANDLE h : {in, out, err}) if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        inherit = TRUE;
    }
    PROCESS_INFORMATION pi = {};
    std::wstring line = cmd;
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, inherit, 0, nullptr, cwd.c_str(), &si, &pi)) {
        DWORD e = GetLastError();
        if (e == 2 || e == 3 || e == 5 || e == 225)
            fail(L"Your antivirus removed or blocked a file the installer needs (error " + std::to_wstring(e) + L"). Restore it from quarantine, "
                 L"or allow %TEMP%\\MA2RecompInstaller-*, then run the installer again.", console);
        else
            fail(L"Could not start the installer (" + std::to_wstring(e) + L").", console);
        return console ? 1 : 3;                   // 3: the message was shown already (see wWinMain)
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code;
}

#ifdef SPLASH_SHOT      // test build only: draw the splash at 63 % into a BMP (window fully transparent) and exit
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Unpack u; u.total = 100; u.done = 63;
    return argc > 1 && run_splash(u, 63, argv[1]) ? 0 : 1;
}
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool console = argc > 1;
    std_in = GetStdHandle(STD_INPUT_HANDLE); std_out = GetStdHandle(STD_OUTPUT_HANDLE); std_err = GetStdHandle(STD_ERROR_HANDLE);
    bool redirected = std_out && std_out != INVALID_HANDLE_VALUE && GetFileType(std_out) != FILE_TYPE_UNKNOWN;
    if (console && !redirected) {                 // from a console: show everything in it
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE *f; _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
    } else if (console) {                         // redirected: the stub's own messages follow the redirection
        HANDLE h = std_err && std_err != INVALID_HANDLE_VALUE ? std_err : std_out;
        int fd = _open_osfhandle((intptr_t)h, _O_TEXT);
        if (fd >= 0) _dup2(fd, _fileno(stderr));
    }
    if (argc == 2 && !wcscmp(argv[1], L"--check-payload")) {  // tests and signing checks: find the kit, read nothing else
        Trailer t;
        if (!read_trailer(self_path(), t)) { fwprintf(stderr, L"payload: not found\n"); return 1; }
        fwprintf(stderr, L"payload: ok, %llu bytes at %llu, id %.16hs\n", (unsigned long long)t.size, (unsigned long long)t.offset, t.id);
        return 0;
    }
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS}; InitCommonControlsEx(&icc);

    std::wstring caller_cwd(32768, L'\0');        // console runs keep the folder they were started in: relative paths mean what the user meant
    caller_cwd.resize(GetCurrentDirectoryW((DWORD)caller_cwd.size(), caller_cwd.data()));
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MechAssault2RecompInstaller");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { fail(L"The MechAssault 2 Recomp Installer is already running.", console); return 1; }

    Unpack u; u.exe = self_path();
    Trailer t;
    if (!read_trailer(u.exe, t)) { fail(L"The installer file is damaged or incomplete. Download it again.", console); return 1; }
    wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH, temp);
    u.dest = std::wstring(temp) + L"MA2RecompInstaller-" + widen(std::string(t.id, 16));
    {   // The mutex above proves no other installer is running, so unpacked kits of other versions (left by killed runs, several hundred MB each) are dead weight.
        std::error_code ec; const wchar_t *prefix = L"MA2RecompInstaller-";
        std::wstring mine = fs::path(u.dest).filename().wstring();
        std::vector<fs::path> old;
        for (fs::directory_iterator it(fs::path(temp), ec), end; !ec && it != end; it.increment(ec)) {
            std::wstring name = it->path().filename().wstring();
            if (_wcsnicmp(name.c_str(), prefix, wcslen(prefix)) == 0 && _wcsicmp(name.c_str(), mine.c_str()) != 0 && it->is_directory(ec)) old.push_back(it->path());
        }
        for (const fs::path &p : old) fs::remove_all(p, ec);
    }
    if (!unpack_kit(u, t, console)) return 1;

    // A clean Python environment for the kit's runtime and everything it starts.
    for (const wchar_t *v : {L"PYTHONHOME", L"PYTHONPATH", L"PYTHONSTARTUP", L"PYTHONEXECUTABLE", L"VIRTUAL_ENV",
                           L"TCL_LIBRARY", L"TK_LIBRARY", L"TCLLIBPATH"})    // another Python's Tcl/Tk (Anaconda, MSYS2) crashes tk.Tk()
        SetEnvironmentVariableW(v, nullptr);
    SetEnvironmentVariableW(L"PYTHONNOUSERSITE", L"1");
    SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
    SetEnvironmentVariableW(L"MA2_INSTALLER_EXE", u.exe.c_str());

    fs::path kit(u.dest);
    std::wstring cmd;
    if (console) {
        cmd = quote((kit / L"python" / L"python.exe").wstring()) + L" -B -s " + quote((kit / L"installer" / L"install.py").wstring());
        for (int i = 1; i < argc; i++) cmd += L" " + quote(argv[i]);
    } else {
        cmd = quote((kit / L"python" / L"pythonw.exe").wstring()) + L" -B -s " + quote((kit / L"installer" / L"gui.py").wstring());
    }
    DWORD code = run(cmd, console && !caller_cwd.empty() ? caller_cwd : kit.wstring(), console);

    // Leave nothing behind: the unpacked kit goes when the installer closes.
    std::error_code ec;
    for (int attempt = 0; attempt < 10 && fs::exists(kit); attempt++) { fs::remove_all(kit, ec); if (fs::exists(kit)) Sleep(500); }
    ReleaseMutex(single);
    // The window closes with 0, or with 3 after it has shown its own "could not start" box. Anything else is a crash with nobody watching.
    if (!console && code != 0 && code != 3)
        fail(L"The installer closed unexpectedly (code " + std::to_wstring(code) + L"). A log may be in %LOCALAPPDATA%\\MechAssault2\\Logs.", false);
    return (int)code;
}
#endif

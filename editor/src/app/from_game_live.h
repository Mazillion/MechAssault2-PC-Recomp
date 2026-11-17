// The editor started from the game's Custom Scenarios, live ( section 6b): the game process
// stays up the whole time and the two windows take turns on screen as if they were one app. The game creates the shared
// block and passes its name with --from-game-live; the layout is the game's Ma2seLive (ma2_scenario.c), field for field.
//
// Switching: the side that comes in shows its window just below the other one at the same place, without activation,
// draws two frames and sets `ready`; the side going out then hides itself (the incoming window becomes the active one).
// The hidden game is muted and idles; the hidden editor draws nothing.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace ed {

struct Ma2seLive {
    uint32_t magic, version, size, game_pid, editor_pid, quiet;
    uint64_t game_hwnd, editor_hwnd;
    int32_t game_fullscreen, rect[4];
    volatile LONG on_screen, want, ready;          // 1 game, 2 editor
    volatile LONG request, request_seq, done_seq;  // editor -> game: 1 back to the game, 2 test
    volatile LONG game_state, game_seq;            // game -> editor: 0 idle, 1 starting a test, 2 testing, 3 the test failed
    int32_t red, blue, skill, reserved;
    char mode[32];
    wchar_t file[520];
    wchar_t note[256];
};
static_assert(sizeof(Ma2seLive) == 1696, "the game's Ma2seLive layout");

class FromGameLive {
public:
    enum { NONE = 0, GAME = 1, EDITOR = 2 };
    enum { REQ_RETURN = 1, REQ_TEST = 2 };
    enum { GS_IDLE = 0, GS_STARTING = 1, GS_TESTING = 2, GS_FAILED = 3 };
    static constexpr const wchar_t* kAppId = L"MechAssault2.Recomp";  // the game's AppUserModelID: one taskbar button

    // Before any window: the game's identity (only when started by the game).
    static void set_app_id() {
        using Fn = HRESULT(WINAPI*)(PCWSTR);
        if (HMODULE sh = LoadLibraryW(L"shell32.dll"))
            if (auto f = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(sh, "SetCurrentProcessExplicitAppUserModelID")))) f(kAppId);
    }

    bool open(const std::wstring& name) {
        map_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!map_) return false;
        L_ = static_cast<Ma2seLive*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ma2seLive)));
        if (!L_ || L_->magic != 0x4C32414Du || L_->version != 1 || L_->size != sizeof(Ma2seLive)) { close(); return false; }
        game_ = reinterpret_cast<HWND>(static_cast<uintptr_t>(L_->game_hwnd));
        game_proc_ = OpenProcess(SYNCHRONIZE, FALSE, L_->game_pid);
        return true;
    }
    // The window exists (hidden): placed and styled like the game's, the game's icon, then the game is told we are here.
    void attach(HWND hwnd) {
        hwnd_ = hwnd;
        if (!L_) return;
        if (game_ && IsWindow(game_)) {
            if (HICON big = reinterpret_cast<HICON>(GetClassLongPtrW(game_, GCLP_HICON))) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
                HICON sm = reinterpret_cast<HICON>(GetClassLongPtrW(game_, GCLP_HICONSM));
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm ? sm : big));
            }
        }
        place();
        L_->editor_hwnd = reinterpret_cast<uintptr_t>(hwnd);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&L_->editor_pid), static_cast<LONG>(GetCurrentProcessId()));
        game_seen_ = L_->game_seq;
    }
    bool active() const { return L_ != nullptr; }
    bool quiet() const { return L_ && L_->quiet; }
    bool on_screen() const { return L_ && L_->on_screen == EDITOR; }
    bool hidden() const { return L_ && L_->on_screen != EDITOR && L_->want != EDITOR; }  // nothing to draw
    bool game_gone() const { return game_proc_ && WaitForSingleObject(game_proc_, 0) == WAIT_OBJECT_0; }
    HWND game() const { return game_; }

    // First frames drawn (still hidden): ask to come on screen.
    void want_on_screen() {
        if (!L_ || L_->on_screen == EDITOR || L_->want == EDITOR) return;
        InterlockedExchange(&L_->ready, NONE);
        InterlockedExchange(&L_->want, EDITOR);
    }
    void request_return(const std::wstring& file) {
        if (!L_) return;
        write_file(file);
        L_->request = REQ_RETURN;
        InterlockedIncrement(&L_->request_seq);
        returning_ = true;
        return_at_ = GetTickCount();
    }
    void request_test(const std::wstring& file, const std::string& mode, int red, int blue, int skill) {
        if (!L_) return;
        write_file(file);
        memset(L_->mode, 0, sizeof L_->mode);
        strncpy_s(L_->mode, mode.c_str(), _TRUNCATE);
        L_->red = red; L_->blue = blue; L_->skill = skill;
        L_->request = REQ_TEST;
        InterlockedIncrement(&L_->request_seq);
    }
    bool returning() const { return returning_; }
    // Back to the game asked for and the game has taken over (or gave no sign for 30 s): the editor can end.
    bool return_done() const { return returning_ && L_ && (L_->on_screen == GAME || GetTickCount() - return_at_ > 30000); }
    // A change of the game's state since the last call (GS_*), or -1.
    int game_event(std::wstring* note) {
        if (!L_ || L_->game_seq == game_seen_) return -1;
        game_seen_ = L_->game_seq;
        if (note) *note = std::wstring(L_->note, wcsnlen(L_->note, 256));
        return L_->game_state;
    }

    // Once per loop pass, before drawing. `presented` = frames presented since the last call.
    void step(int presented) {
        if (!L_) return;
        if (L_->want == EDITOR && L_->on_screen != EDITOR) {
            if (!shown_) {
                place();
                SetWindowPos(hwnd_, game_ && IsWindowVisible(game_) ? game_ : HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
                shown_ = true;
                frames_ = 0;
            } else if (L_->ready != EDITOR && (frames_ += presented) >= 2) {
                InterlockedExchange(&L_->ready, EDITOR);
            }
        } else if (L_->want != EDITOR) {
            shown_ = false;
        }
        // The game has drawn below us: the editor goes out of sight.
        if (L_->want == GAME && L_->ready == GAME && L_->on_screen == EDITOR) {
            if (!L_->quiet && game_) SetForegroundWindow(game_);
            ShowWindow(hwnd_, SW_HIDE);
            InterlockedExchange(&L_->on_screen, GAME);
            InterlockedExchange(&L_->ready, NONE);
            InterlockedExchange(&L_->want, NONE);
        }
    }
    // The game is gone: the editor carries on on its own, on screen.
    void detach_after_game_ended() {
        if (!L_) return;
        const bool was_quiet = L_->quiet != 0;
        close();
        if (!IsWindowVisible(hwnd_)) {
            if (was_quiet) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            else { ShowWindow(hwnd_, SW_SHOW); SetForegroundWindow(hwnd_); }
        }
    }
    void close() {
        if (L_) UnmapViewOfFile(L_);
        if (map_) CloseHandle(map_);
        if (game_proc_) CloseHandle(game_proc_);
        L_ = nullptr; map_ = nullptr; game_proc_ = nullptr;
        returning_ = false;
    }

private:
    void write_file(const std::wstring& file) {
        memset(L_->file, 0, sizeof L_->file);
        wcsncpy_s(L_->file, file.c_str(), _TRUNCATE);
    }
    // The game's place: its monitor without a frame when it is full screen, else its window rectangle with a frame.
    // Quiet (a hidden test game): off screen, a tool window that is never activated.
    void place() {
        RECT r{L_->rect[0], L_->rect[1], L_->rect[2], L_->rect[3]};
        if (game_ && IsWindow(game_)) {
            RECT g;
            if (IsWindowVisible(game_) && GetWindowRect(game_, &g)) r = g;
        }
        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_VISIBLE;
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        if (L_->quiet) ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        const bool full = L_->game_fullscreen != 0 || (game_ && (GetWindowLongPtrW(game_, GWL_STYLE) & WS_POPUP));
        style |= full ? WS_POPUP : WS_OVERLAPPEDWINDOW;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }

    Ma2seLive* L_ = nullptr;
    HANDLE map_ = nullptr, game_proc_ = nullptr;
    HWND hwnd_ = nullptr, game_ = nullptr;
    bool shown_ = false, returning_ = false;
    int frames_ = 0;
    DWORD return_at_ = 0;
    LONG game_seen_ = 0;
};

}  // namespace ed

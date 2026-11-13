// Automation (developer flag --automation [pipe-name]): drives the editor from another process
// without touching the real mouse, keyboard or focus. A local named pipe (\\.\pipe\<name>, current
// user only, no remote clients) takes newline-delimited JSON commands; input goes straight into
// ImGui's IO, screenshots come from the D3D11 back buffer, and items can be found by label through
// ImGui's test-engine hooks (IMGUI_ENABLE_TEST_ENGINE, implemented here without the Test Engine).
// Client: tools/edctl.py. Protocol: tools/AUTOMATION.md.
#pragma once
#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/json.h"

namespace ed {

class App;

// Set while automation runs: OS dialogs (message boxes, file pickers) are answered from here
// instead of being shown, so nothing pops up or blocks the editor.
struct AutomationDialogs {
    bool active = false;
    int answer = IDNO;            // IDYES / IDNO / IDCANCEL for message boxes
    std::wstring next_file;       // the next file or folder picker returns this (then clears it); empty = cancel
    std::string last_question;    // what the last dialog asked
};
AutomationDialogs& automation_dialogs();

class Automation {
public:
    bool start(const std::wstring& pipe_name, std::string* err);
    void stop();
    // Main thread, each frame after the platform backend's NewFrame and before ImGui::NewFrame.
    void pre_frame(App& app);
    // After ImGui::Render and the 3D pass, before Present; shot(path) saves the back buffer.
    void post_render(App& app, const std::function<bool(const std::wstring&, int*, int*)>& shot);
    // True once, a few frames after a quit command (so its reply reaches the client).
    bool quit_due() { return quit_countdown_ > 0 && --quit_countdown_ == 0; }
    bool quit_forced() const { return quit_force_; }

private:
    struct Pending {
        ma2::Json cmd;
        std::string reply;
        bool done = false;
    };
    void server();
    void finish(const ma2::Json& reply);
    void fail(const std::string& why);
    bool step(App& app);  // runs the current command one frame; true when it finished

    std::wstring pipe_;
    std::thread thread_;
    bool stop_ = false;
    HANDLE pipe_handle_ = INVALID_HANDLE_VALUE;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Pending*> queue_;
    Pending* cur_ = nullptr;
    int phase_ = 0, wait_ = 0, tries_ = 0;
    float mx_ = -1, my_ = -1;  // automation mouse, re-sent every frame
    struct Release { int key; int frames; };
    std::vector<Release> keys_up_;
    std::wstring shot_path_;
    int quit_countdown_ = 0;
    bool quit_force_ = false;
};

}  // namespace ed

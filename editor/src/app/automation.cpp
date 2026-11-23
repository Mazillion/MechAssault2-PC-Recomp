#include "automation.h"

#include <sddl.h>

#include <algorithm>
#include <cstdio>

#include "../core/util.h"
#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"

using ma2::Json;

// ---------------------------------------------------------------------------------------------
// Item registry: ImGui's test-engine hooks (IMGUI_ENABLE_TEST_ENGINE) record every item's ID,
// label, rectangle and window each frame; queries use the previous complete frame.

namespace {
struct ItemRec {
    ImGuiID id = 0;
    std::string label, window;
    ImRect bb, clip;  // clip: the part of bb inside its window
    ImRect win;       // the window's own clip rectangle
    float win_scroll_y = 0;  // the window's vertical scroll range: 0 = it never scrolls
    float text_w = 0;        // the label's drawn width, "##" part cut
    int flags = 0;           // ImGuiItemStatusFlags from ItemInfo
    bool visible = false;
};
std::vector<ItemRec> g_items_cur, g_items_prev;
// Labels reported before their item is added (tab items call ItemInfo first).
struct EarlyLabel { ImGuiID id; std::string label; int flags; float text_w; };
std::vector<EarlyLabel> g_early_labels;
float label_width(const char* label) {
    if (!label || !*label) return 0;
    const char* hash = strstr(label, "##");
    return ImGui::CalcTextSize(label, hash).x;
}
}  // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if (!id) return;
    ImGuiWindow* w = ctx->CurrentWindow;
    ItemRec r;
    r.id = id;
    r.bb = bb;
    for (auto it = g_early_labels.begin(); it != g_early_labels.end(); ++it)
        if (it->id == id) { r.label = it->label; r.flags = it->flags; r.text_w = it->text_w; g_early_labels.erase(it); break; }
    if (w) {
        r.window = w->Name;
        r.win = w->InnerClipRect;
        r.win_scroll_y = w->ScrollMax.y;
        r.visible = w->Active && !w->SkipItems && bb.Overlaps(w->ClipRect);
        r.clip = bb;
        r.clip.ClipWithFull(w->ClipRect);
    }
    g_items_cur.push_back(std::move(r));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    for (auto it = g_items_cur.rbegin(); it != g_items_cur.rend(); ++it)
        if (it->id == id) {
            if (label) { it->label = label; it->text_w = label_width(label); }
            it->flags = int(flags);
            return;
        }
    if (label && g_early_labels.size() < 256) g_early_labels.push_back({id, label, int(flags), label_width(label)});
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {
    for (auto& r : g_items_prev)
        if (r.id == id && !r.label.empty()) return r.label.c_str();
    return nullptr;
}

namespace ed {

AutomationDialogs& automation_dialogs() {
    static AutomationDialogs d;
    return d;
}

static std::string visible_label(const std::string& l) {
    size_t p = l.find("##");
    return p == std::string::npos ? l : l.substr(0, p);
}

static Json rect_json(const ImRect& r) {
    Json j = Json::object();
    j.with("x", Json::number(r.Min.x)).with("y", Json::number(r.Min.y)).with("w", Json::number(r.GetWidth())).with("h", Json::number(r.GetHeight()));
    j.with("cx", Json::number(std::floor((r.Min.x + r.Max.x) * 0.5f))).with("cy", Json::number(std::floor((r.Min.y + r.Max.y) * 0.5f)));
    return j;
}

// An item whose own label does not fit its rectangle (a button, selectable, radio, link), or that reaches past its window
// sideways, or past the bottom of a window that cannot scroll. Inputs, sliders, checkboxes and tree nodes draw their label
// outside or around the item, so only the window test applies to them.
static bool item_clipped(const ItemRec& r) {
    const float tol = 2.f;
    if (!r.visible || r.label == r.window || r.window.compare(0, 9, "##Tooltip") == 0) return false;   // a window's own title item is not a control
    const float side = ImGui::GetStyle().WindowPadding.x + tol;   // a selectable's highlight and a full-width button may use the window's padding
    if (r.bb.Min.x < r.win.Min.x - side || r.bb.Max.x > r.win.Max.x + side) return true;
    if (r.win_scroll_y <= 0.5f && (r.bb.Min.y < r.win.Min.y - tol || r.bb.Max.y > r.win.Max.y + tol)) return true;
    if (r.flags & (ImGuiItemStatusFlags_Inputable | ImGuiItemStatusFlags_Checkable | ImGuiItemStatusFlags_Openable)) return false;
    return r.text_w > r.bb.GetWidth() + tol && r.text_w > 0 && r.label.compare(0, 2, "##") != 0;
}

static Json item_json(const ItemRec& r) {
    Json j = Json::object();
    char id[16];
    snprintf(id, sizeof id, "#%08X", r.id);
    j.with("id", Json::string(id)).with("label", Json::string(visible_label(r.label))).with("full_label", Json::string(r.label));
    j.with("window", Json::string(r.window)).with("visible", Json::boolean(r.visible)).with("rect", rect_json(r.bb));
    j.with("win", rect_json(r.win)).with("text_w", Json::number(std::round(r.text_w * 10.f) / 10.f)).with("clipped", Json::boolean(item_clipped(r)));
    return j;
}

// "Units/Enemy": the last part is the item's visible label (or its full "Label##id"); earlier
// parts narrow it to a window name or the editor tab (Map, Terrain, Players, Units...). "#0123ABCD"
// is an item ID. Exact labels win; otherwise a unique substring match.
// Old tab names (before the task-ordered tab row) still work in item paths and as tab buttons, so older scripts keep running.
static std::string tab_alias(const std::string& lowered) {
    // Terrain, Units and Props and Players are pages of the Build tab now; the page names stand for it too.
    static const char* const build[] = {"terrain", "units and props", "units", "sides & units", "players", "select", "place", "scatter", "roads & walls", "sculpt", "paint", "water", "starts"};
    for (const char* b : build)
        if (lowered == b) return "build";
    if (lowered == "messages") return "objectives & triggers";
    return lowered;
}

static const ItemRec* find_item(const std::string& query, const std::string& tab, std::string* why) {
    if (query.size() == 9 && query[0] == '#') {
        ImGuiID id = ImGuiID(strtoul(query.c_str() + 1, nullptr, 16));
        for (auto& r : g_items_prev) if (r.id == id) return &r;
        *why = "no item with ID " + query;
        return nullptr;
    }
    std::vector<std::string> parts = ma2::split(query, '/');
    auto attempt = [&](const std::string& last, const std::vector<std::string>& scope) -> std::vector<const ItemRec*> {
        auto in_scope = [&](const ItemRec& r) {
            for (auto& sc : scope) {
                std::string s = ma2::lower(ma2::trim(sc));
                if (s.empty()) continue;
                if (ma2::lower(r.window).find(s) == std::string::npos && ma2::lower(tab) != s && ma2::lower(tab) != tab_alias(s) &&
                    ma2::lower(r.label).find(s) == std::string::npos)
                    return false;
            }
            return true;
        };
        std::vector<const ItemRec*> exact, sub;
        std::string l = ma2::lower(ma2::trim(last));
        for (auto& r : g_items_prev) {
            if (r.label.empty() || !in_scope(r)) continue;
            std::string v = ma2::lower(visible_label(r.label)), full = ma2::lower(r.label);
            if (v == l || full == l) exact.push_back(&r);
            else if (v.find(l) != std::string::npos) sub.push_back(&r);
        }
        if (!exact.empty()) {
            // Prefer a visible one.
            std::stable_partition(exact.begin(), exact.end(), [](const ItemRec* r) { return r->visible; });
            return exact;
        }
        return sub;
    };
    std::vector<const ItemRec*> got;
    if (parts.size() > 1) got = attempt(parts.back(), std::vector<std::string>(parts.begin(), parts.end() - 1));
    if (got.empty()) got = attempt(query, {});  // a label with "/" in it ("Show / hide helpers")
    if (got.empty() && parts.size() == 1 && tab_alias(ma2::lower(ma2::trim(query))) != ma2::lower(ma2::trim(query)))
        got = attempt(tab_alias(ma2::lower(ma2::trim(query))), {});  // "Sides & Units" -> the Units and Props tab button
    if (got.empty()) {
        // Widgets that report no label (combos): an item made at a window's top level has the ID
        // ImHashStr(label, window ID), so try the query as a label in each window.
        std::string l = ma2::trim(parts.back());
        std::vector<std::string> scope(parts.begin(), parts.end() - 1);
        for (auto& r : g_items_prev) {
            if (!r.label.empty()) continue;
            ImGuiWindow* w = ImGui::FindWindowByName(r.window.c_str());
            if (!w) continue;
            bool in = true;
            for (auto& sc : scope) {
                std::string s = ma2::lower(ma2::trim(sc));
                if (!s.empty() && ma2::lower(r.window).find(s) == std::string::npos && ma2::lower(tab) != s) in = false;
            }
            if (in && r.id == ImHashStr(l.c_str(), 0, w->ID)) {
                const_cast<ItemRec&>(r).label = l;  // known from now on (this frame's list)
                return &r;
            }
        }
    }
    if (got.size() == 1 || (!got.empty() && ma2::iequals(visible_label(got[0]->label), ma2::trim(parts.back())))) return got[0];
    if (got.empty()) *why = "no item matches \"" + query + "\"";
    else {
        *why = "\"" + query + "\" matches " + std::to_string(got.size()) + " items:";
        for (size_t i = 0; i < got.size() && i < 8; i++) *why += " [" + got[i]->label + " in " + got[i]->window + "]";
    }
    return nullptr;
}

static ImGuiKey key_from_name(std::string n) {
    static const std::pair<const char*, const char*> alias[] = {{"esc", "Escape"}, {"del", "Delete"}, {"up", "UpArrow"},
        {"down", "DownArrow"}, {"left", "LeftArrow"}, {"right", "RightArrow"}, {"return", "Enter"}, {",", "Comma"}, {"pgup", "PageUp"},
        {"pgdn", "PageDown"}, {"space", "Space"}, {"shift", "LeftShift"}, {"ctrl", "LeftCtrl"}, {"control", "LeftCtrl"},
        {"alt", "LeftAlt"}};
    for (auto& [a, b] : alias) if (ma2::iequals(n, a)) n = b;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
        if (ma2::iequals(n, ImGui::GetKeyName(ImGuiKey(k)))) return ImGuiKey(k);
    return ImGuiKey_None;
}

static int button_from_name(const std::string& b) {
    std::string s = ma2::lower(b);
    return s == "right" || s == "r" ? 1 : s == "middle" || s == "m" ? 2 : 0;
}

// Modifier keys: class 0 ctrl, 1 shift, 2 alt; role 0 = the left key, 1 = the right key, 2 = ImGui's ImGuiMod_* alias key
// (what io.KeyCtrl / KeyShift / KeyAlt are derived from). Returns -1 for any other key.
static const int kModKeys[3][3] = {{ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl, ImGuiKey_ReservedForModCtrl},
                                   {ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_ReservedForModShift},
                                   {ImGuiKey_LeftAlt, ImGuiKey_RightAlt, ImGuiKey_ReservedForModAlt}};
static const int kModFlags[3] = {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt};
static const char* const kModNames[3] = {"ctrl", "shift", "alt"};

static int mod_class(int key, int* role = nullptr) {
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++)
            if (kModKeys[c][r] == key) { if (role) *role = r; return c; }
    return -1;
}

// "ctrl+shift" -> bit set (1 ctrl, 2 shift, 4 alt).
static int parse_mods(const std::string& m) {
    std::string s = ma2::lower(m);
    return (s.find("ctrl") != std::string::npos || s.find("control") != std::string::npos ? 1 : 0) | (s.find("shift") != std::string::npos ? 2 : 0) |
           (s.find("alt") != std::string::npos ? 4 : 0);
}

static ma2::Json names_json(const std::vector<std::string>& v) {
    ma2::Json a = ma2::Json::array();
    for (auto& s : v) a.push(ma2::Json::string(s));
    return a;
}

void Automation::send_key(ImGuiIO& io, int key, bool down) {
    if (down == (sent_.count(key) != 0)) return;
    int c = mod_class(key, nullptr), role = 0;
    if (c >= 0) mod_class(key, &role);
    // The alias key goes in as the ImGuiMod_* flag, the form the platform backend uses.
    io.AddKeyEvent(c >= 0 && role == 2 ? ImGuiKey(kModFlags[c]) : ImGuiKey(key), down);
    if (down) sent_.insert(key); else sent_.erase(key);
}

void Automation::sync_mods(ImGuiIO& io) {
    for (int c = 0; c < 3; c++) {
        const bool cmd = (cmd_mods_ & (1 << c)) != 0;
        const bool l = cmd || held_.count(kModKeys[c][0]) != 0, r = held_.count(kModKeys[c][1]) != 0;
        const bool m = l || r || held_.count(kModKeys[c][2]) != 0;
        send_key(io, kModKeys[c][0], l);
        send_key(io, kModKeys[c][1], r);
        send_key(io, kModKeys[c][2], m);
    }
}

void Automation::set_held(ImGuiIO& io, int key, bool down) {
    if (down) held_.insert(key); else held_.erase(key);
    if (mod_class(key) >= 0) sync_mods(io);
    else send_key(io, key, down);
}

void Automation::mouse_button(ImGuiIO& io, int btn, bool down) {
    io.AddMouseButtonEvent(btn, down);
    if (btn >= 0 && btn < 3) mouse_held_[btn] = down;
}

// Lifts every key and mouse button this automation put down. Returns what was lifted.
std::vector<std::string> Automation::release_all(ImGuiIO& io) {
    std::vector<std::string> names;
    std::set<int> all = sent_;
    for (int k : held_) all.insert(k);
    for (int k : all) {
        int c = mod_class(k);
        int role = 0;
        if (c >= 0) mod_class(k, &role);
        // The alias key goes with its modifier, unless it was held alone (ModShift...).
        if (c >= 0 && role == 2 && (all.count(kModKeys[c][0]) || all.count(kModKeys[c][1]))) continue;
        names.push_back(ImGui::GetKeyName(ImGuiKey(k)));
    }
    held_.clear();
    keys_up_.clear();
    cmd_mods_ = 0;
    for (int k : std::set<int>(sent_)) send_key(io, k, false);
    for (int b = 0; b < 3; b++)
        if (mouse_held_[b]) {
            io.AddMouseButtonEvent(b, false);
            mouse_held_[b] = false;
            names.push_back(b == 0 ? "MouseLeft" : b == 1 ? "MouseRight" : "MouseMiddle");
        }
    return names;
}

// ---------------------------------------------------------------------------------------------
// Pipe server (its own thread): one client at a time, one JSON command per line, one reply line.

static std::wstring user_sddl() {
    HANDLE tok = nullptr;
    std::wstring out;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
    DWORD n = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> buf(n);
    if (GetTokenInformation(tok, TokenUser, buf.data(), n, &n)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            out = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";  // the current user only
            LocalFree(sid);
        }
    }
    CloseHandle(tok);
    return out;
}

bool Automation::start(const std::wstring& pipe_name, std::string* err) {
    pipe_ = L"\\\\.\\pipe\\" + pipe_name;
    automation_dialogs().active = true;
    // Probe that the name is free (a second editor with the same name would fight over it).
    HANDLE probe = CreateFileW(pipe_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        if (err) *err = "another editor already uses the automation pipe " + ma2::narrow(pipe_);
        return false;
    }
    thread_ = std::thread([this] { server(); });
    return true;
}

void Automation::server() {
    std::wstring sddl = user_sddl();
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    if (!sddl.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;
    while (!stop_) {
        HANDLE h = CreateNamedPipeW(pipe_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 1 << 16, 1 << 16, 0, sa.lpSecurityDescriptor ? &sa : nullptr);
        if (h == INVALID_HANDLE_VALUE) { Sleep(200); continue; }
        pipe_handle_ = h;
        bool ok = ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        std::string buf;
        char tmp[4096];
        DWORD n = 0;
        while (ok && !stop_ && ReadFile(h, tmp, sizeof tmp, &n, nullptr) && n) {
            buf.append(tmp, n);
            for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
                std::string line = ma2::trim(buf.substr(0, nl));
                buf.erase(0, nl + 1);
                if (line.empty()) continue;
                Pending p;
                std::string perr;
                if (!Json::parse(line, p.cmd, &perr) || p.cmd.type != Json::Object) {
                    Json r = Json::object();
                    r.with("ok", Json::boolean(false)).with("error", Json::string("not a JSON object: " + perr));
                    p.reply = r.dump();
                } else {
                    std::unique_lock<std::mutex> lk(mu_);
                    queue_.push_back(&p);
                    cv_.wait(lk, [&] { return p.done || stop_; });
                    if (!p.done) {
                        queue_.erase(std::remove(queue_.begin(), queue_.end(), &p), queue_.end());
                        if (cur_ == &p) cur_ = nullptr;
                        p.reply = "{\"ok\":false,\"error\":\"editor is closing\"}";
                    }
                }
                p.reply += "\n";
                DWORD w = 0;
                WriteFile(h, p.reply.data(), DWORD(p.reply.size()), &w, nullptr);
                FlushFileBuffers(h);
            }
        }
        DisconnectNamedPipe(h);
        CloseHandle(h);
        pipe_handle_ = INVALID_HANDLE_VALUE;
    }
    if (sd) LocalFree(sd);
}

void Automation::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    // Wake a blocked ConnectNamedPipe / ReadFile.
    HANDLE c = CreateFileW(pipe_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
    CancelSynchronousIo(thread_.native_handle());
    thread_.join();
}

void Automation::finish(const Json& reply) {
    // The command's `mods` come up now (after its last mouse / key event); `mods_seen` is what the app had in ImGui just before.
    Json with_mods;
    const Json* rp = &reply;
    if (cmd_mods_) {
        ImGuiIO& io = ImGui::GetIO();
        std::string seen;
        for (int c = 0; c < 3; c++)
            if (io.KeyMods & kModFlags[c]) seen += (seen.empty() ? "" : "+") + std::string(kModNames[c]);
        with_mods = reply;
        with_mods.with("mods_seen", Json::string(seen));
        rp = &with_mods;
        cmd_mods_ = 0;
        sync_mods(io);
    }
    cmd_mods_on_ = false;
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_) {
        cur_->reply = rp->dump();
        cur_->done = true;
        queue_.erase(std::remove(queue_.begin(), queue_.end(), cur_), queue_.end());
        cur_ = nullptr;
    }
    cv_.notify_all();
}

void Automation::fail(const std::string& why) {
    Json r = Json::object();
    r.with("ok", Json::boolean(false)).with("error", Json::string(why));
    finish(r);
}

// ---------------------------------------------------------------------------------------------
// Per frame

void Automation::pre_frame(App& app) {
    g_items_prev.swap(g_items_cur);
    g_items_cur.clear();
    g_early_labels.clear();
    GImGui->TestEngineHookItems = true;
    ImGuiIO& io = ImGui::GetIO();
    // The backend queues the real cursor while the window looks focused; ours goes last.
    if (mx_ >= 0) io.AddMousePosEvent(mx_, my_);
    for (auto it = keys_up_.begin(); it != keys_up_.end();) {
        if (--it->frames <= 0) {
            int k = it->key;
            it = keys_up_.erase(it);
            set_held(io, k, false);
        } else {
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cur_ && !queue_.empty()) {
            cur_ = queue_.front();
            phase_ = wait_ = tries_ = 0;
            cmd_mods_on_ = false;
        }
    }
    if (!cur_) return;
    if (wait_ > 0) { wait_--; return; }
    step(app);
}

void Automation::post_render(App&, const std::function<bool(const std::wstring&, int*, int*)>& shot) {
    if (shot_path_.empty() || !cur_) return;
    int w = 0, h = 0;
    bool ok = shot(shot_path_, &w, &h);
    Json r = Json::object();
    r.with("ok", Json::boolean(ok)).with("path", Json::string(ma2::narrow(shot_path_))).with("width", Json::number(w)).with("height", Json::number(h));
    if (!ok) r.with("error", Json::string("could not read the back buffer"));
    shot_path_.clear();
    finish(r);
}

bool Automation::step(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    const Json& c = cur_->cmd;
    std::string cmd = c.str("cmd");
    Json ok = Json::object();
    ok.with("ok", Json::boolean(true));
    auto pos = [&](float x, float y) { mx_ = x; my_ = y; io.AddMousePosEvent(x, y); };
    // `mods` ("ctrl", "shift", "alt", joined with +) on mouse, key, click_item and hover_item: the modifiers go down one frame
    // before the command's first event and come up when it finishes (finish()), so nothing stays stuck.
    if (!cmd_mods_on_ && (cmd == "mouse" || cmd == "key" || cmd == "click_item" || cmd == "hover_item")) {
        cmd_mods_on_ = true;
        if (int m = parse_mods(c.str("mods"))) {
            cmd_mods_ = m;
            sync_mods(io);
            wait_ = 1;
            return false;
        }
    }
    auto merged = [&](const std::string& json) {  // a lean-read reply (JSON text from the app) plus ok
        Json r;
        std::string e;
        if (!Json::parse(json, r, &e) || r.type != Json::Object) { fail("internal: bad reply " + e); return; }
        r.with("ok", Json::boolean(!r.get("error")));
        finish(r);
    };

    if (cmd == "ping") { finish(ok); return true; }
    if (cmd == "state") {
        Json s;
        std::string e;
        Json::parse(app.automation_state(), s, &e);
        s.with("ok", Json::boolean(true));
        // What this automation holds down: all three lists must be empty when a test is at rest.
        std::vector<std::string> mods, keys, buttons;
        for (int cl = 0; cl < 3; cl++)
            if (sent_.count(kModKeys[cl][2])) mods.push_back(kModNames[cl]);
        for (int k : sent_) {
            int role = 0;
            if (mod_class(k, &role) >= 0 && role == 2) continue;
            keys.push_back(ImGui::GetKeyName(ImGuiKey(k)));
        }
        for (int b = 0; b < 3; b++)
            if (mouse_held_[b]) buttons.push_back(b == 0 ? "left" : b == 1 ? "right" : "middle");
        s.with("mods_down", names_json(mods)).with("keys_down", names_json(keys)).with("mouse_down", names_json(buttons));
        finish(s);
        return true;
    }
    if (cmd == "dump") { ok.with("text", Json::string(app.debug_state())); finish(ok); return true; }
    if (cmd == "release_all") {
        ok.with("released", names_json(release_all(io)));
        finish(ok);
        return true;
    }
    if (cmd == "palette") { merged(app.automation_palette(int(c.num("limit", 20)))); return true; }
    if (cmd == "sel") { merged(app.automation_objects(0, "", false, 0, 0, 0, 50)); return true; }
    if (cmd == "ground") {
        if (!c.get("x") || !c.get("z")) { fail("ground needs \"x\" and \"z\" (world metres)"); return true; }
        merged(app.automation_ground(float(c.num("x")), float(c.num("z"))));
        return true;
    }
    if (cmd == "fail_next_build") { ma2::ScenarioIO::test_fail_alloc = true; finish(ok); return true; }  // F-V5R-1: the next Save / Test / autosave build throws std::bad_alloc once
    if (cmd == "reach") {
        if (!c.get("x") || !c.get("z")) { fail("reach needs \"x\" and \"z\" (world metres)"); return true; }
        merged(app.automation_reach(float(c.num("x")), float(c.num("z"))));
        return true;
    }
    if (cmd == "triggers") { merged(app.automation_triggers()); return true; }
    if (cmd == "trigger") { merged(app.automation_trigger(uint32_t(c.num("uid", 0)))); return true; }
    if (cmd == "env") { merged(app.automation_env()); return true; }
    if (cmd == "cutscene") { merged(app.automation_cutscene(uint32_t(c.num("uid", 0)))); return true; }
    if (cmd == "handles") { merged(app.automation_handles()); return true; }
    if (cmd == "joints") { merged(app.automation_joints(float(c.num("near_px", 400)), int(c.num("limit", 40)))); return true; }
    if (cmd == "project") {
        if (!c.get("x") || !c.get("z")) { fail("project needs \"x\", \"z\" and optionally \"y\" (world metres)"); return true; }
        merged(app.automation_project(float(c.num("x")), float(c.num("y", 0)), float(c.num("z"))));
        return true;
    }
    if (cmd == "objects") {
        const bool near_given = c.get("cx") && c.get("cz");
        merged(app.automation_objects(1, c.str("filter"), near_given, float(c.num("cx")), float(c.num("cz")), float(c.num("r", 50)), int(c.num("limit", 20))));
        return true;
    }
    if (cmd == "wait_frames") {
        if (phase_ == 0) { phase_ = 1; wait_ = std::max(0, int(c.num("n", 1)) - 1); return false; }
        finish(ok);
        return true;
    }
    if (cmd == "screenshot") {
        std::string p = c.str("path");
        if (p.empty()) { fail("screenshot needs \"path\" (.png or .bmp)"); return true; }
        shot_path_ = ma2::widen(p);  // saved in post_render, after this frame is drawn
        return false;
    }
    if (cmd == "text") {
        if (phase_ == 0) { io.AddInputCharactersUTF8(c.str("text").c_str()); phase_ = 1; wait_ = 2; return false; }  // once (was typed twice)
        finish(ok);
        return true;
    }
    if (cmd == "key") {
        ImGuiKey k = key_from_name(c.str("key"));
        if (k == ImGuiKey_None) { fail("unknown key \"" + c.str("key") + "\" (ImGui names: A, F5, Escape, Delete, Comma, UpArrow...)"); return true; }
        std::string action = c.str("action", "press");
        if (action.empty()) action = "press";
        if (action != "press" && action != "down" && action != "up") { fail("key action must be press, down or up"); return true; }
        if (phase_ == 0) {
            int hold = action == "press" ? 2 + int(c.num("hold_frames", 0)) : 1 << 30;
            if (action == "up") {
                keys_up_.erase(std::remove_if(keys_up_.begin(), keys_up_.end(), [&](const Release& r) { return r.key == k; }), keys_up_.end());
                set_held(io, k, false);  // a modifier key also clears its ImGuiMod_* alias (unless the other side is still down)
            } else {
                set_held(io, k, true);
                if (action == "press") keys_up_.push_back({k, hold});
            }
            phase_ = 1;
            wait_ = action == "press" ? hold + 1 : 1;
            return false;
        }
        finish(ok);
        return true;
    }
    if (cmd == "mouse") {
        std::string action = c.str("action", "move");
        int btn = button_from_name(c.str("button", "left"));
        float x = float(c.num("x", mx_)), y = float(c.num("y", my_));
        if (action == "move") {
            if (phase_ == 0) { pos(x, y); phase_ = 1; wait_ = 1; return false; }
        } else if (action == "down" || action == "up") {
            if (phase_ == 0) {
                pos(x, y);
                mouse_button(io, btn, action == "down");
                phase_ = 1;
                wait_ = 1;
                return false;
            }
        } else if (action == "click" || action == "double") {
            int clicks = action == "double" ? 2 : 1;
            // 0 move, then down/up pairs one frame apart, then 2 frames for the app to react.
            // A heavy map can take more than ImGui's 0.3 s double-click window for the four frames of a scripted double click:
            // widen it for this command only (a real user's double click is unaffected).
            static float saved_dbl = 0;
            if (phase_ == 0 && clicks == 2) { saved_dbl = io.MouseDoubleClickTime; io.MouseDoubleClickTime = 5.f; }
            if (phase_ == clicks * 2 + 1 && clicks == 2 && saved_dbl > 0) { io.MouseDoubleClickTime = saved_dbl; saved_dbl = 0; }
            if (phase_ == 0) { pos(x, y); phase_ = 1; return false; }
            if (phase_ <= clicks * 2) {
                mouse_button(io, btn, phase_ % 2 == 1);
                phase_++;
                return false;
            }
            if (phase_ == clicks * 2 + 1) { phase_++; wait_ = 2; return false; }
        } else if (action == "drag") {
            float x2 = float(c.num("x2", x)), y2 = float(c.num("y2", y));
            int n = std::max(2, int(c.num("frames", 20)));
            // Move first, press on the next frame: pressing in the same frame as the jump from the old cursor
            // position gave the app that whole jump as drag delta (a scripted orbit spun the camera).
            if (phase_ == 0) { pos(x, y); phase_ = -1; return false; }
            if (phase_ == -1) { mouse_button(io, btn, true); phase_ = 1; return false; }
            if (phase_ <= n) {
                float t = float(phase_) / float(n);
                pos(x + (x2 - x) * t, y + (y2 - y) * t);
                phase_++;
                return false;
            }
            if (phase_ == n + 1) { mouse_button(io, btn, false); phase_++; wait_ = 2; return false; }
        } else if (action == "wheel") {
            if (phase_ == 0) {
                if (c.get("x")) pos(x, y);
                io.AddMouseWheelEvent(float(c.num("dx", 0)), float(c.num("delta", 1)));
                phase_ = 1;
                wait_ = 2;
                return false;
            }
        } else {
            fail("mouse action must be move, down, up, click, double, drag or wheel");
            return true;
        }
        finish(ok);
        return true;
    }
    if (cmd == "items") {
        std::string f = ma2::lower(c.str("filter"));
        bool all = c.flag("all");
        const size_t limit = size_t(std::clamp(int(c.num("limit", 60)), 1, 2000));
        size_t count = 0;
        Json list = Json::array();
        for (auto& r : g_items_prev) {
            if (r.label.empty() || (!all && !r.visible)) continue;
            if (!f.empty() && ma2::lower(r.label).find(f) == std::string::npos && ma2::lower(r.window).find(f) == std::string::npos) continue;
            count++;
            if (list.a.size() < limit) list.push(item_json(r));
        }
        ok.with("items", list).with("count", Json::number(double(count))).with("truncated", Json::boolean(count > list.a.size()));
        finish(ok);
        return true;
    }
    if (cmd == "find_item" || cmd == "hover_item" || cmd == "click_item") {
        if (phase_ == 0) {
            std::string why;
            const ItemRec* r = find_item(c.str("item"), app.tab_label(), &why);
            if (!r) {
                // It may appear on the next frames (a tab or popup just opened).
                if (++tries_ < 6) return false;
                fail(why);
                return true;
            }
            if (cmd == "find_item") {
                ok.with("item", item_json(*r));
                finish(ok);
                return true;
            }
            if (!r->visible || r->clip.GetHeight() < r->bb.GetHeight() - 0.5f) {
                // Scroll its window (or the scrolling parent of a child window) to bring it into view.
                ImGuiWindow* w = ImGui::FindWindowByName(r->window.c_str());
                while (w && w->ScrollMax.y <= 0 && w->ParentWindow && (w->Flags & ImGuiWindowFlags_ChildWindow)) w = w->ParentWindow;
                if (w && w->ScrollMax.y > 0 && tries_ < 12) {
                    float want = w->Scroll.y + (r->bb.Min.y + r->bb.Max.y) * 0.5f - (w->InnerClipRect.Min.y + w->InnerClipRect.Max.y) * 0.5f;
                    ImGui::SetScrollY(w, std::clamp(want, 0.f, w->ScrollMax.y));
                    tries_++;
                    wait_ = 2;
                    return false;
                }
                if (!r->visible) { fail("\"" + r->label + "\" is not visible (clipped, and its window cannot scroll to it)"); return true; }
            }
            cur_->cmd.with("_item", item_json(*r));
            // The middle of the item's visible part (a half-scrolled item is clicked where it shows).
            pos(std::floor((r->clip.Min.x + r->clip.Max.x) * 0.5f), std::floor((r->clip.Min.y + r->clip.Max.y) * 0.5f));
            phase_ = 1;
            wait_ = 1;  // hover first: small buttons ignore a click in the frame the mouse arrives
            return false;
        }
        if (cmd == "hover_item") {
            if (phase_ == 1) { phase_ = 2; wait_ = int(c.num("frames", 2)); return false; }
        } else {
            int btn = button_from_name(c.str("button", "left"));
            int clicks = c.flag("double") ? 2 : 1;
            if (phase_ <= clicks * 2) {
                mouse_button(io, btn, phase_ % 2 == 1);
                phase_++;
                return false;
            }
            if (phase_ == clicks * 2 + 1) { phase_++; wait_ = 2; return false; }
        }
        if (const Json* it = cur_->cmd.get("_item")) ok.with("item", *it);
        finish(ok);
        return true;
    }
    if (cmd == "script") {
        // Once: the second pass only waits for the frames after it (developer flags that add things ran twice).
        if (phase_ == 0) { app.script(ma2::widen(c.str("flag")), ma2::widen(c.str("arg"))); phase_ = 1; wait_ = 2; return false; }
        finish(ok);
        return true;
    }
    if (cmd == "open") {
        if (phase_ == 0) {
            app.open_path(fs::path(ma2::widen(c.str("path"))));
            phase_ = 1;
            wait_ = 3;
            return false;
        }
        finish(ok);
        return true;
    }
    if (cmd == "windows") {
        // Every open window (top level, popups, children) with its rectangle: layout_check asserts each lies inside the client area.
        Json list = Json::array();
        const ImVec2 vs = ImGui::GetMainViewport()->Size;
        for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows) {
            if (!w->Active || w->Hidden || (w->Flags & ImGuiWindowFlags_Tooltip)) continue;
            Json j = Json::object();
            j.with("name", Json::string(w->Name)).with("child", Json::boolean((w->Flags & ImGuiWindowFlags_ChildWindow) != 0));
            j.with("popup", Json::boolean((w->Flags & ImGuiWindowFlags_Popup) != 0)).with("scroll_max", Json::number(std::round(w->ScrollMax.y * 10.f) / 10.f));
            j.with("rect", rect_json(ImRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y))));
            list.push(std::move(j));
        }
        ok.with("windows", list).with("client", Json::object().with("w", Json::number(vs.x)).with("h", Json::number(vs.y)));
        finish(ok);
        return true;
    }
    if (cmd == "dialogs") {
        AutomationDialogs& d = automation_dialogs();
        std::string a = ma2::lower(c.str("answer"));
        if (a == "yes") d.answer = IDYES;
        else if (a == "no") d.answer = IDNO;
        else if (a == "cancel") d.answer = IDCANCEL;
        if (const Json* f = c.get("next_file")) d.next_file = ma2::widen(f->s);
        // An open "Please confirm" popup (Delete trigger...) is answered now as well.
        if (d.answer == IDYES || d.answer == IDNO) {
            std::string q = app.answer_confirm(d.answer == IDYES);
            if (!q.empty()) d.last_question = q;
        }
        ok.with("answer", Json::string(d.answer == IDYES ? "yes" : d.answer == IDNO ? "no" : "cancel"));
        ok.with("last_question", Json::string(d.last_question));
        finish(ok);
        return true;
    }
    if (cmd == "window_size" || cmd == "window") {
        // Client area in pixels; the next frame resizes the swap chain, so wait_frames 3 before a screenshot.
        const int w = int(c.num("w", 0)), h = int(c.num("h", 0));
        if (!automation_set_client_size(w, h)) { fail("window_size needs w and h (320..8192)"); return true; }
        ok.with("w", Json::number(w)).with("h", Json::number(h));
        finish(ok);
        return true;
    }
    if (cmd == "quit") {
        quit_force_ = c.flag("force");
        quit_countdown_ = 6;
        finish(ok);
        return true;
    }
    fail("unknown command \"" + cmd + "\" (ping, state, dump, sel, ground, objects, palette, release_all, screenshot, mouse, key, text, "
         "wait_frames, window_size, windows, items, find_item, hover_item, click_item, script, open, dialogs, quit)");
    return true;
}

}  // namespace ed

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
    bool visible = false;
};
std::vector<ItemRec> g_items_cur, g_items_prev;
// Labels reported before their item is added (tab items call ItemInfo first).
std::vector<std::pair<ImGuiID, std::string>> g_early_labels;
}  // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if (!id) return;
    ImGuiWindow* w = ctx->CurrentWindow;
    ItemRec r;
    r.id = id;
    r.bb = bb;
    for (auto it = g_early_labels.begin(); it != g_early_labels.end(); ++it)
        if (it->first == id) { r.label = it->second; g_early_labels.erase(it); break; }
    if (w) {
        r.window = w->Name;
        r.visible = w->Active && !w->SkipItems && bb.Overlaps(w->ClipRect);
        r.clip = bb;
        r.clip.ClipWithFull(w->ClipRect);
    }
    g_items_cur.push_back(std::move(r));
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    for (auto it = g_items_cur.rbegin(); it != g_items_cur.rend(); ++it)
        if (it->id == id) {
            if (label) it->label = label;
            return;
        }
    if (label && g_early_labels.size() < 256) g_early_labels.push_back({id, label});
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

static Json item_json(const ItemRec& r) {
    Json j = Json::object();
    char id[16];
    snprintf(id, sizeof id, "#%08X", r.id);
    j.with("id", Json::string(id)).with("label", Json::string(visible_label(r.label))).with("full_label", Json::string(r.label));
    j.with("window", Json::string(r.window)).with("visible", Json::boolean(r.visible)).with("rect", rect_json(r.bb));
    return j;
}

// "Units/Enemy": the last part is the item's visible label (or its full "Label##id"); earlier
// parts narrow it to a window name or the editor tab (Map, Terrain, Players, Units...). "#0123ABCD"
// is an item ID. Exact labels win; otherwise a unique substring match.
// Old tab names (before the task-ordered tab row) still work in item paths and as tab buttons, so older scripts keep running.
static std::string tab_alias(const std::string& lowered) {
    if (lowered == "sides & units" || lowered == "players") return "units and props";
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
        {"pgdn", "PageDown"}, {"space", "Space"}};
    for (auto& [a, b] : alias) if (ma2::iequals(n, a)) n = b;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
        if (ma2::iequals(n, ImGui::GetKeyName(ImGuiKey(k)))) return ImGuiKey(k);
    return ImGuiKey_None;
}

static int button_from_name(const std::string& b) {
    std::string s = ma2::lower(b);
    return s == "right" || s == "r" ? 1 : s == "middle" || s == "m" ? 2 : 0;
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
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_) {
        cur_->reply = reply.dump();
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
            io.AddKeyEvent(ImGuiKey(it->key), false);
            it = keys_up_.erase(it);
        } else {
            ++it;
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!cur_ && !queue_.empty()) {
            cur_ = queue_.front();
            phase_ = wait_ = tries_ = 0;
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
    auto mods_down = [&](const std::string& m, int hold) {
        std::string s = ma2::lower(m);
        if (s.find("ctrl") != std::string::npos) { io.AddKeyEvent(ImGuiMod_Ctrl, true); keys_up_.push_back({ImGuiMod_Ctrl, hold}); }
        if (s.find("shift") != std::string::npos) { io.AddKeyEvent(ImGuiMod_Shift, true); keys_up_.push_back({ImGuiMod_Shift, hold}); }
        if (s.find("alt") != std::string::npos) { io.AddKeyEvent(ImGuiMod_Alt, true); keys_up_.push_back({ImGuiMod_Alt, hold}); }
    };

    if (cmd == "ping") { finish(ok); return true; }
    if (cmd == "state") { Json s; std::string e; Json::parse(app.automation_state(), s, &e); s.with("ok", Json::boolean(true)); finish(s); return true; }
    if (cmd == "dump") { ok.with("text", Json::string(app.debug_state())); finish(ok); return true; }
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
        if (phase_ == 0) {
            int hold = action == "press" ? 2 + int(c.num("hold_frames", 0)) : 1 << 30;
            if (action == "up") {
                io.AddKeyEvent(k, false);
                keys_up_.erase(std::remove_if(keys_up_.begin(), keys_up_.end(), [&](const Release& r) { return r.key == k; }), keys_up_.end());
            } else {
                mods_down(c.str("mods"), action == "press" ? hold : 1 << 30);
                io.AddKeyEvent(k, true);
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
                io.AddMouseButtonEvent(btn, action == "down");
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
                io.AddMouseButtonEvent(btn, phase_ % 2 == 1);
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
            if (phase_ == -1) { io.AddMouseButtonEvent(btn, true); phase_ = 1; return false; }
            if (phase_ <= n) {
                float t = float(phase_) / float(n);
                pos(x + (x2 - x) * t, y + (y2 - y) * t);
                phase_++;
                return false;
            }
            if (phase_ == n + 1) { io.AddMouseButtonEvent(btn, false); phase_++; wait_ = 2; return false; }
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
        Json list = Json::array();
        for (auto& r : g_items_prev) {
            if (r.label.empty() || (!all && !r.visible)) continue;
            if (!f.empty() && ma2::lower(r.label).find(f) == std::string::npos && ma2::lower(r.window).find(f) == std::string::npos) continue;
            list.push(item_json(r));
            if (list.a.size() >= 400) break;
        }
        ok.with("items", list);
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
                io.AddMouseButtonEvent(btn, phase_ % 2 == 1);
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
    if (cmd == "quit") {
        quit_force_ = c.flag("force");
        quit_countdown_ = 6;
        finish(ok);
        return true;
    }
    fail("unknown command \"" + cmd + "\" (ping, state, dump, screenshot, mouse, key, text, wait_frames, items, find_item, "
         "hover_item, click_item, script, open, dialogs, quit)");
    return true;
}

}  // namespace ed

// Preferences window (camera, hotkeys, general, testing), hotkey matching, UI scale and the
// running Test game.
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>

#include "ui_tips.h"
#include "theme.h"
#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace ed {

using namespace ma2;

static void tip(const char* text) { ImGui::SetItemTooltip("%s", text); }

static int current_mods() {
    const ImGuiIO& io = ImGui::GetIO();
    return (io.KeyCtrl ? KM_CTRL : 0) | (io.KeyShift ? KM_SHIFT : 0) | (io.KeyAlt ? KM_ALT : 0);
}

bool App::pressed(int act, bool repeat) const {
    int mods = current_mods();
    for (auto& k : settings_.keys[act])
        if (k.key && k.mods == mods && ImGui::IsKeyPressed(ImGuiKey(k.key), repeat)) return true;
    return false;
}

bool App::pressed_quarter(int act, bool repeat) const {
    int mods = current_mods(), q = settings_.hold_quarter;
    for (auto& k : settings_.keys[act])
        if (k.key && !(k.mods & q) && (k.mods | q) == mods && ImGui::IsKeyPressed(ImGuiKey(k.key), repeat)) return true;
    return false;
}

bool App::holding(int mod) const { return (current_mods() & mod) != 0; }

std::string App::turn_keys_text() const {
    std::string l = key_text(settings_.keys[A_TURN_LEFT][0]), r = key_text(settings_.keys[A_TURN_RIGHT][0]);
    return (l.empty() ? "Q" : l) + " / " + (r.empty() ? "E" : r);
}

bool App::held(int act) const {
    int mods = current_mods() & ~KM_SHIFT;
    for (auto& k : settings_.keys[act])
        if (k.key && (k.mods & ~KM_SHIFT) == mods && ImGui::IsKeyDown(ImGuiKey(k.key))) return true;
    return false;
}

const char* App::skill_name(int s) {
    static const char* n[] = {"Recruit", "Regular", "Veteran", "Elite", "Mixed"};
    return n[std::clamp(s, 0, 4)];
}

void App::apply_ui_scale() {
    ui_scale_dirty_ = false;
    ImGuiStyle& st = ImGui::GetStyle();
    if (raw_style_) {
        // From the unscaled theme, so a DPI change (window moved to another monitor) does not compound rounding.
        st = *raw_style_;
        st.ScaleAllSizes(base_dpi_ * settings_.ui_scale);
    } else {
        if (!base_style_) base_style_ = new ImGuiStyle(st);  // the start-up style (already DPI-scaled)
        st = *base_style_;
        st.ScaleAllSizes(settings_.ui_scale);
    }
    st.FontScaleDpi = base_dpi_ * settings_.ui_scale;
    dpi_ = base_dpi_ * settings_.ui_scale;
}

// Keys a hotkey can use: not modifiers, mouse buttons or gamepad.
static bool bindable(int key) {
    if (key >= ImGuiKey_LeftCtrl && key <= ImGuiKey_RightSuper) return false;
    if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown) return false;
    if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) return false;
    if (key == ImGuiKey_ReservedForModCtrl || key == ImGuiKey_ReservedForModShift || key == ImGuiKey_ReservedForModAlt ||
        key == ImGuiKey_ReservedForModSuper)
        return false;
    return true;
}

void App::ui_preferences() {
    if (open_prefs_) {
        ImGui::OpenPopup("Settings");
        open_prefs_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640 * dpi_, 660 * dpi_), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    prefs_open_ = theme::begin_modal("Settings", &open);
    if (!prefs_open_) {
        capture_act_ = -1;
        return;
    }
    bool changed = false;
    if (capture_act_ < 0 && ImGui::GetCurrentContext()->OpenPopupStack.Size <= 1 && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput) {
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::BeginTabBar("##prefs")) {
        // --- General
        if (ImGui::BeginTabItem("General")) {
            settings_folders();
            ImGui::SeparatorText("Safety copy");
            form_label("Recovery copy every", form_col());
            changed |= ImGui::SliderInt("##autosave", &settings_.autosave_minutes, 0, 30,
                                        settings_.autosave_minutes ? "%d min" : "off");
            tip("While you have unsaved changes the editor keeps a recovery copy and offers it back if it closes unexpectedly. "
                "0 = off.");
            ImGui::SeparatorText("Helpers at start");
            changed |= ImGui::Checkbox("Show helpers (trigger areas, AI paths, markers)", &settings_.helpers_on);
            tip("Draw the things players never see (trigger areas, AI paths, markers, lights, cameras) when a map opens. H switches them at any time.");
            changed |= ImGui::Checkbox("Show the helper legend", &settings_.legend_on);
            tip("The small Helpers box in the corner of the map that says what each helper colour means.");
            ImGui::SeparatorText("Look");
            if (ImGui::Checkbox("Scene lights: street lamps and other lights light the scene", &settings_.scene_lights)) {
                r_.params.scene_lights = settings_.scene_lights;
                changed = true;
            }
            tip("Shows lamp light on the ground and nearby models, like the game, so you can judge the look of a scene. Costs a little speed, so it is off by default. Also under View.");
            if (ImGui::Checkbox("Game-style ground texture fade (switches off in 128 m squares)", &settings_.fast_terrain_detail)) {
                r_.params.fast_terrain_detail = settings_.fast_terrain_detail;
                changed = true;
            }
            tip("On (default): the fine ground texture switches off in 128 m squares, exactly as the game does near its camera, so the ground looks "
                "like the game's. Off: it fades out smoothly with distance instead (no visible band, a little darker far away).");
            ImGui::SeparatorText("Size of the editor's text and buttons");
            float sc = settings_.ui_scale;
            form_label("UI scale", form_col());
            if (ImGui::SliderFloat("##uiscale", &sc, 0.75f, 2.f, "%.2fx")) { settings_.ui_scale = sc; changed = true; }
            if (ImGui::IsItemDeactivatedAfterEdit()) ui_scale_dirty_ = true;
            tip("Bigger or smaller text and buttons. Applies when you let go of the slider.");
            ImGui::Spacing();
            if (ImGui::Button("Reset general settings")) {
                settings_.reset_general();
                ui_scale_dirty_ = true;
                changed = true;
            }
            ImGui::EndTabItem();
        }
        // --- Camera
        if (ImGui::BeginTabItem("Camera")) {
            ImGui::SeparatorText("Mouse");
            changed |= ImGui::Checkbox("Right-drag moves the map", &settings_.right_drag_pan);
            tip("Hold the right mouse button and drag to move around the map. A right click without moving still stops the tool or "
                "clears the selection.");
            ImGui::BeginDisabled(!settings_.right_drag_pan);
            changed |= ImGui::Checkbox("Reverse right-drag (the camera follows the mouse)", &settings_.invert_drag_pan);
            tip("Off: the ground sticks to the mouse, like dragging a sheet of paper. On: the camera moves with the mouse, so the ground slides the other way.");
            why_disabled(!settings_.right_drag_pan, "Tick \"Right-drag moves the map\" above first.");
            ImGui::EndDisabled();
            ImGui::TextDisabled("Middle-drag turns the view. The wheel zooms.");
            changed |= ImGui::Checkbox("Invert middle-drag left / right", &settings_.invert_rotate);
            tip("Dragging left turns the view the other way.");
            changed |= ImGui::Checkbox("Invert middle-drag up / down", &settings_.invert_pitch);
            tip("Dragging up tilts the view the other way.");
            changed |= ImGui::Checkbox("Invert the mouse wheel", &settings_.invert_zoom);
            tip("Wheel forward zooms out instead of in.");
            ImGui::SeparatorText("Speeds");
            form_label("Panning", form_col());
            changed |= ImGui::SliderFloat("##pan", &settings_.pan_speed, 0.2f, 4.f, "%.1fx");
            tip("Keyboard and right-drag panning. Hold Shift with the keys to go faster still.");
            form_label("Turning", form_col());
            changed |= ImGui::SliderFloat("##turn", &settings_.rotate_speed, 0.2f, 4.f, "%.1fx");
            tip("How fast middle-drag and the turn keys turn the view. 1.0x is the standard speed.");
            form_label("Zooming", form_col());
            changed |= ImGui::SliderFloat("##zoom", &settings_.zoom_speed, 0.2f, 4.f, "%.1fx");
            tip("How far one notch of the wheel zooms. 1.0x is the standard speed.");
            ImGui::SeparatorText("Window edge");
            changed |= ImGui::Checkbox("Scroll when the mouse touches the window edge", &settings_.edge_scroll);
            tip("Like a strategy game: push the mouse against any edge of the window (also over the menu bar, the status bar or the "
                "taskbar) to move the map. Pushing harder moves faster.");
            ImGui::BeginDisabled(!settings_.edge_scroll);
            form_label("Edge scrolling speed", form_col());
            changed |= ImGui::SliderFloat("##edge", &settings_.edge_speed, 0.2f, 4.f, "%.1fx");
            tip("How fast the map moves while the mouse pushes against the window edge. 1.0x is the standard speed.");
            why_disabled(!settings_.edge_scroll, "Tick \"Scroll when the mouse touches the window edge\" above first.");
            ImGui::EndDisabled();
            ImGui::Spacing();
            if (ImGui::Button("Reset camera settings")) { settings_.reset_camera(); changed = true; }
            tip("Puts the mouse options and speeds on this page back to how they were when you first installed the editor.");
            ImGui::EndTabItem();
        }
        // --- Hotkeys
        if (ImGui::BeginTabItem("Keys")) {
            ImGui::TextWrapped("Click a key to change it, then press the new key (with Ctrl, Shift or Alt if you like). Esc cancels. "
                               "Each action can have two keys.");
            if (capture_act_ >= 0) {
                ImGui::TextColored(theme::tok().accent, "Press a key for \"%s\"...", act_label(capture_act_));
                for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
                    if (!bindable(key) || !ImGui::IsKeyPressed(ImGuiKey(key), false)) continue;
                    if (key == ImGuiKey_Escape && current_mods() == 0) { capture_act_ = -1; break; }
                    settings_.keys[capture_act_][capture_slot_] = {key, current_mods()};
                    capture_act_ = -1;
                    changed = true;
                    break;
                }
            } else {
                ImGui::TextDisabled(" ");
            }
            // Hold keys: Shift, Ctrl or Alt, held with a click or a key. Each must be different; there are only three, so a choice
            // another one uses swaps the two (and says so) instead of leaving two actions on one key.
            ImGui::SeparatorText("Hold keys");
            {
                struct Hold { const char* label; const char* help; int* mod; };
                Hold holds[3] = {
                    {"Place freely (no snapping)", "Hold it while placing: no grid, and roads, walls, fences, gates and bridges do not join ends or stand on roofs.", &settings_.hold_free},
                    {"Click through to objects behind", "Hold it and click: picks the next object under the mouse (cycles through overlapping ones); while placing, puts it on the ground under a roof or deck.", &settings_.hold_through},
                    {"Quarter turn (with Turn left / right)", "Hold it with Turn left / right for a 90-degree turn; while dragging to aim, turns in 15-degree steps.", &settings_.hold_quarter},
                };
                static const int kMods[3] = {KM_SHIFT, KM_CTRL, KM_ALT};
                for (int h = 0; h < 3; h++) {
                    ImGui::PushID(1000 + h);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(holds[h].label);
                    tip(holds[h].help);
                    ImGui::SameLine(260 * dpi_);
                    ImGui::SetNextItemWidth(110 * dpi_);
                    if (ImGui::BeginCombo("##m", mod_name(*holds[h].mod))) {
                        for (int m : kMods) {
                            int other = -1;
                            for (int o = 0; o < 3; o++) if (o != h && *holds[o].mod == m) other = o;
                            std::string item = std::string(mod_name(m)) + (other >= 0 ? std::string("  (swaps with: ") + holds[other].label + ")" : "");
                            if (ImGui::Selectable(item.c_str(), *holds[h].mod == m) && *holds[h].mod != m) {
                                hold_swap_note_.clear();
                                if (other >= 0) {
                                    *holds[other].mod = *holds[h].mod;
                                    hold_swap_note_ = std::string("Swapped: \"") + holds[other].label + "\" is now " + mod_name(*holds[other].mod) + ".";
                                }
                                *holds[h].mod = m;
                                changed = true;
                            }
                        }
                        ImGui::EndCombo();
                    }
                    tip(holds[h].help);
                    ImGui::PopID();
                }
                // Reverse the brush works only with the Terrain shaping tools, where the three keys above do nothing, so it
                // may share one of their keys (its default Ctrl is also Quarter turn's): no swapping, just say so.
                {
                    static const char* kRevHelp = "Hold it while shaping the ground (Terrain tab): Raise / Lower, Plateau up / down and Smooth / Roughen use the other half; with Flatten a click picks the height there.";
                    ImGui::PushID(1003);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Reverse the brush");
                    tip(kRevHelp);
                    ImGui::SameLine(260 * dpi_);
                    ImGui::SetNextItemWidth(110 * dpi_);
                    if (ImGui::BeginCombo("##m", mod_name(settings_.hold_reverse))) {
                        for (int m : kMods)
                            if (ImGui::Selectable(mod_name(m), settings_.hold_reverse == m) && settings_.hold_reverse != m) {
                                settings_.hold_reverse = m;
                                changed = true;
                            }
                        ImGui::EndCombo();
                    }
                    tip(kRevHelp);
                    for (int o = 0; o < 3; o++)
                        if (*holds[o].mod == settings_.hold_reverse)
                            ImGui::TextDisabled("  Same key as \"%s\": fine, that one works only while placing or clicking.", holds[o].label);
                    ImGui::PopID();
                }
                if (!hold_swap_note_.empty()) ImGui::TextColored(theme::tok().warn, "%s", hold_swap_note_.c_str());
                // The quarter turn is the Turn keys plus its hold key: flag a shortcut that is the same combination.
                std::string qclash;
                for (int t : {int(A_TURN_LEFT), int(A_TURN_RIGHT)})
                    for (auto& tk : settings_.keys[t]) {
                        if (!tk.key || (tk.mods & settings_.hold_quarter)) continue;
                        KeyBind q{tk.key, tk.mods | settings_.hold_quarter};
                        for (int b = 0; b < A_COUNT; b++)
                            for (auto& bk : settings_.keys[b])
                                if (bk == q && qclash.find(act_label(b)) == std::string::npos) qclash += (qclash.empty() ? "" : ", ") + std::string(act_label(b));
                    }
                if (!qclash.empty()) ImGui::TextColored(theme::tok().error, "Quarter turn clashes with: %s. Change that key or the quarter-turn key.", qclash.c_str());
                if (settings_.hold_through != KM_ALT)
                    ImGui::TextDisabled("%s+click now clicks through; it no longer adds to the selection (use the other of Shift / Ctrl).", mod_name(settings_.hold_through));
            }
            ImGui::BeginChild("##keys", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.8f), ImGuiChildFlags_Borders);
            if (ImGui::BeginTable("##keytable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 2.2f);
                ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("Second key", ImGuiTableColumnFlags_WidthStretch, 1.2f);
                ImGui::TableSetupColumn("##conflict", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableHeadersRow();
                const char* section = "";
                for (int a = 0; a < A_COUNT; a++) {
                    if (strcmp(section, act_section(a)) != 0) {
                        section = act_section(a);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", section);
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(act_label(a));
                    std::string clash;
                    for (int k = 0; k < 2; k++) {
                        ImGui::TableNextColumn();
                        ImGui::PushID(a * 2 + k);
                        const KeyBind& kb = settings_.keys[a][k];
                        bool waiting = capture_act_ == a && capture_slot_ == k;
                        std::string t = waiting ? "(press a key)" : kb.key ? key_text(kb) : "-";
                        if (ImGui::Button(t.c_str(), ImVec2(-ImGui::GetFrameHeight() - 4 * dpi_, 0))) { capture_act_ = a; capture_slot_ = k; }
                        tip("Click, then press the new key.");
                        ImGui::SameLine(0, 2 * dpi_);
                        ImGui::BeginDisabled(!kb.key);
                        if (ImGui::Button("x", ImVec2(ImGui::GetFrameHeight(), 0))) { settings_.keys[a][k] = KeyBind(); changed = true; }
                        ImGui::EndDisabled();
                        tip("Remove this key.");
                        ImGui::PopID();
                        if (kb.key)
                            for (int b = 0; b < A_COUNT; b++)
                                for (int j = 0; j < 2; j++)
                                    if ((b != a || j != k) && settings_.keys[b][j] == kb && clash.find(act_label(b)) == std::string::npos)
                                        clash += (clash.empty() ? "" : ", ") + std::string(act_label(b));
                    }
                    ImGui::TableNextColumn();
                    if (!clash.empty()) {
                        ImGui::TextColored(theme::tok().error, "Also: %s", clash.c_str());
                        tip("The same key does both. Change one of them.");
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
            if (ImGui::Button("Reset all keys")) { settings_.reset_keys(); capture_act_ = -1; changed = true; }
            tip("Puts every shortcut back to its default. Your own choices are lost.");
            ImGui::EndTabItem();
        } else if (capture_act_ >= 0) {
            capture_act_ = -1;
        }
        // --- Testing
        if (ImGui::BeginTabItem("Testing")) {
            changed |= ImGui::Checkbox("Quick test: skip menus and movies, and return to the editor when you leave the game", &settings_.quick_test);
            tip("Test (F5) starts the game straight in your map or mission, without the start-up movies and menus. When you quit the "
                "match or close the game, the editor comes back to the front.");
            ImGui::TextWrapped("Untick it to start the game the normal way (title screen, intro movies), for example to try the menus "
                               "the way players see them.");
            ImGui::Spacing();
            if (ImGui::Button("Reset testing settings")) { settings_.reset_testing(); changed = true; }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (changed) {
        settings_.save();
        objects_dirty_ = true;
    }
    ImGui::Separator();
    ImGui::TextDisabled("Changes are saved as you make them.");
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 90 * dpi_);
    if (ImGui::Button("Close", ImVec2(90 * dpi_, 0)) && capture_act_ < 0) ImGui::CloseCurrentPopup();
    if (!open) capture_act_ = -1;
    theme::end_modal();
}

// Test: notices when the game started by Test exits (quick test brings the editor back) and
// reports a crash from its exit code.
// True once the process has a visible top-level window: the game is up and showing.
static bool process_has_window(DWORD pid) {
    struct Ctx { DWORD pid; bool found; } ctx{pid, false};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == c->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == nullptr) {
            RECT r;
            if (GetWindowRect(h, &r) && r.right - r.left > 200 && r.bottom - r.top > 150) c->found = true;
        }
        return c->found ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// A game process that `parent` started (MechAssault 2.exe starts a second copy of itself and exits),
// opened for waiting; null if there is none.
static HANDLE game_child_of(DWORD parent) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE found = nullptr;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == parent && _wcsicmp(pe.szExeFile, L"MechAssault 2.exe") == 0)
            found = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
    CloseHandle(snap);
    return found;
}

void App::poll_test_process() {
    if (!test_proc_) return;
    if (WaitForSingleObject(test_proc_, 0) == WAIT_OBJECT_0 && !test_game_up_) {
        // The process Test started handed over to a second copy of the game: follow that one, so the
        // editor does not say "Back from the game" while the game is still loading.
        if (HANDLE child = game_child_of(GetProcessId(test_proc_))) {
            CloseHandle(test_proc_);
            test_proc_ = child;
        }
    }
    if (WaitForSingleObject(test_proc_, 0) != WAIT_OBJECT_0) {
        // The "Game started" note has done its job once the game's window is up (ui_message_box closes it).
        if (!test_game_up_ && process_has_window(GetProcessId(test_proc_))) test_game_up_ = true;
        if (status_time_ < 1) {
            status_ = "Testing in the game... (the editor comes back when you leave the game)";
            status_time_ = 2;
        }
        return;
    }
    DWORD code = 0;
    GetExitCodeProcess(test_proc_, &code);
    CloseHandle(test_proc_);
    test_proc_ = nullptr;
    test_game_up_ = false;
    if (!background && IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    if (!background) SetForegroundWindow(hwnd_);
    if (!background && GetForegroundWindow() != hwnd_) {
        FLASHWINFO fw{sizeof(fw), hwnd_, FLASHW_ALL | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&fw);
    }
    if (code >= 0xC0000000u) {
        char hex[16];
        snprintf(hex, sizeof hex, "0x%08lX", code);
        status_ = "Back from the game (it closed unexpectedly).";
        status_time_ = 8;
        message("Test", std::string("The game closed unexpectedly (error ") + hex +
                            "). If it happens again, try removing the last objects you added, and run Check scenario. The game's crash "
                            "reports are in %LOCALAPPDATA%\\MechAssault2\\Logs.");
    } else {
        status_ = "Back from the game.";
        status_time_ = 5;
    }
}

void App::set_raw_style(const ImGuiStyle& unscaled) {
    delete raw_style_;
    raw_style_ = new ImGuiStyle(unscaled);
}

}  // namespace ed

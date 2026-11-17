// The action editors shared by the Objectives & Triggers tab and the cutscene timeline: one settings body per action
// class (message text, sound pickers, links to units and triggers...), written straight into the world's own objects.
// Moved out of tab_triggers so the timeline's Event editor shows the very same controls.
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include "theme.h"
#include "../core/cutscene.h"
#include "../core/triggers.h"
#include "../core/mission_extras.h"
#include "imgui_internal.h"
#include "ui_tips.h"
#include "trigger_ui.h"
#include "app.h"
#include "imgui.h"
#include "imgui_stdlib.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

void App::trig_touch(const char* label) {
    push_undo_once(label);
    doc_->dirty = true;
    objects_dirty_ = true;
}

void App::go_to_object(uint32_t uid) {
    if (!doc_) return;
    const WObj* o = object_by_uid(doc_->world, uid);
    Vec3 p;
    if (o && get_pos(*o, p)) { cam_.tx = p.x; cam_.tz = p.z; }
}

void App::ui_float(WObj& o, const char* key, const char* label, const char* tip) {
    float v = o.f32(key);
    form_label(label, form_col());
    if (ImGui::InputFloat((std::string("##") + key).c_str(), &v, 0, 0, "%.2f")) { trig_touch("trigger setting"); o.set_f32(key, v); }
    if (tip) ImGui::SetItemTooltip("%s", tip);
}

void App::ui_int(WObj& o, const char* key, const char* label, const char* tip) {
    int v = o.i32(key);
    form_label(label, form_col());
    if (ImGui::InputInt((std::string("##") + key).c_str(), &v, 0, 0)) { trig_touch(label); o.set_i32(key, v); }
    if (tip) ImGui::SetItemTooltip("%s", tip);
}

void App::ui_bool(WObj& o, const char* key, const char* label, const char* tip) {
    bool b = o.i32(key) != 0;
    if (ImGui::Checkbox(label, &b)) { trig_touch(label); o.set_i32(key, b ? 1 : 0); }
    if (tip) ImGui::SetItemTooltip("%s", tip);
}

void App::ui_str(WObj& o, const char* key, const char* label) {
    std::string v = o.str(key);
    form_label(label, form_col());
    if (ImGui::InputText((std::string("##") + key).c_str(), &v)) { trig_touch(label); o.set_str(key, v); }
}

void App::ui_enum(WObj& o, const char* key, const char* label, std::vector<std::pair<int, const char*>> opts) {
    int cur = o.i32(key);
    std::string shown = "Other (" + std::to_string(cur) + ")";
    for (auto& [v, n] : opts) if (v == cur) shown = n;
    form_label(label, form_col());
    if (begin_combo((std::string("##") + key).c_str(), shown.c_str())) {
        for (auto& [v, n] : opts) if (ImGui::Selectable(n, v == cur)) { trig_touch(label); o.set_i32(key, v); }
        ImGui::EndCombo();
    }
}

// ----------------------------------------------------------------------------- listening

// Length of a WAV the editor made (a 44-byte header, 16-bit PCM): milliseconds.
static int wav_length_ms(const Bytes& wav) {
    if (wav.size() < 44) return 0;
    uint32_t byte_rate = 0;
    std::memcpy(&byte_rate, &wav[28], 4);
    return byte_rate ? int((uint64_t(wav.size() - 44) * 1000) / byte_rate) : 0;
}

void App::audition_stop() {
    if (!automation) PlaySoundW(nullptr, nullptr, 0);
    audition_clip_ = -1;
    audition_buf_.clear();
}

void App::audition_play(int clip) {
    if (!doc_) return;
    audition_stop();
    auto it = doc_->audio.find(clip);
    if (it == doc_->audio.end() || it->second.wav.size() < 44) return;
    audition_buf_ = it->second.wav;
    audition_clip_ = clip;
    audition_ms_ = wav_length_ms(audition_buf_);
    audition_end_ = ImGui::GetTime() + audition_ms_ / 1000.0;
    if (!automation) PlaySoundW(reinterpret_cast<LPCWSTR>(audition_buf_.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

bool App::audition_playing(int clip) const {
    if (audition_clip_ < 0 || ImGui::GetTime() >= audition_end_) return false;
    return clip < 0 || clip == audition_clip_;
}

// Play / Stop beside an own clip. The button turns into Stop while the clip plays.
bool App::audition_button(int clip, const char* id) {
    const bool on = audition_playing(clip);
    const std::string l = std::string(on ? "Stop##aud" : "Play##aud") + id;
    const bool hit = ImGui::SmallButton(l.c_str());
    ImGui::SetItemTooltip(on ? "Stop the clip." : "Hear this clip (as the game plays it).");
    if (hit) {
        if (on) audition_stop();
        else audition_play(clip);
    }
    return hit;
}

void App::audition_retail_button(const char* id) {
    ImGui::BeginDisabled();
    ImGui::SmallButton((std::string("Play##aud") + id).c_str());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The game's own sounds play only in the game for now.");
}

// A link to another object: unit, trigger or marker.
// set_key: the action can also act on a whole set (AISet / ActionSet): retail reinforcements show, wake and
// change the side of a hidden set in one action each (Mission_05 "2nd Wave Catch").
void App::ui_link(WObj& o, const char* key, const char* label, bool triggers, const char* set_key, std::function<void()>& later) {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    (void)s;
    uint32_t u = o.u32(key);
    const uint32_t su = set_key ? o.u32(set_key) : 0;
    const WObj* tgt = u ? object_by_uid(w, u) : nullptr;
    ImGui::PushID(key);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(form_col());
    if (su && !u) {
        const WObj* so = object_by_uid(w, su);
        int n = so ? so->i32("MoeEntitySetCount") : 0;
        if (!so) ImGui::TextColored(theme::tok().warn, "missing set #%u", su);
        else ImGui::TextDisabled("set '%s' (%d unit%s)", so->description().c_str(), n, n == 1 ? "" : "s");
    } else if (u && !tgt) ImGui::TextColored(theme::tok().warn, "%s", unit_label(w, u).c_str());
    else ImGui::TextDisabled("%s", unit_label(w, u).c_str());
    std::vector<uint32_t> picked;
    for (uint32_t x : selection_) if (const WObj* po = object_by_uid(w, x)) if (!is_trigger_shape(po->cls)) picked.push_back(x);
    // Pick on map: the next click on a unit sets this link (right-click or Esc cancels). Beside Use selection.
    const bool armed = tool_ == Tool::PickPoint && pick_.kind == PickTarget::LinkUnit && pick_.uid == o.uid() && pick_.key == key;
    if (theme::small_toggle_button("Pick on map", armed)) {
        const Tab back = tab_;
        pick_ = PickTarget();
        pick_.kind = PickTarget::LinkUnit;
        pick_.uid = o.uid();
        pick_.key = key;
        pick_.set_key = set_key ? set_key : "";
        pick_.back = back;
        tool_ = Tool::PickPoint;
        status_ = "Click the unit on the map. Right-click or " + kt(A_CANCEL) + " cancels.";
        status_time_ = 8;
    }
    ImGui::SetItemTooltip("Then click the unit (or marker) on the map: it becomes the target here.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use selection") && !selection_.empty()) {
        if (set_key && picked.size() > 1) {
            // Several units: they become one new set the action acts on.
            uint32_t me = o.uid();
            std::string nm;
            for (int k = 1; nm.empty(); k++) {  // "Units 1", "Units 2"...: the first name no set has yet
                std::string c = "Units " + std::to_string(k);
                bool used = false;
                for (auto& x : w.objs) used |= x.cls == key_of("MoeEntitySet") && x.description() == c;
                if (!used) nm = c;
            }
            std::string sk = set_key, ek = key;
            later = [this, me, nm, picked, sk, ek]() {
                push_undo("trigger setting");
                uint32_t ns = entity_set_from(nm, picked);
                if (WObj* a = object_by_uid(doc_->world, me)) { a->set_u32(sk, ns); a->set_u32(ek, 0); }
                world_changed();
                doc_->dirty = true;
            };
        } else {
            trig_touch("trigger setting");
            o.set_u32(key, *selection_.begin());
            if (set_key) o.set_u32(set_key, 0);
        }
    }
    ImGui::SetItemTooltip(set_key ? "Click something on the map first, then press this to use it here. With several units selected, they become one set this acts on."
                                  : "Click something on the map first (a unit, a marker...), then press this to use it here.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Go") && u) { go_to_object(u); select_uid(u); }
    ImGui::SetItemTooltip("Select it and move the camera to it.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) { trig_touch("trigger setting"); o.set_u32(key, 0); if (set_key) o.set_u32(set_key, 0); }
    if (set_key) {
        static const uint32_t K_SET = key_of("MoeEntitySet");
        ImGui::SetNextItemWidth(150 * dpi_);
        if (begin_combo("##set", "or a set...", ImGuiComboFlags_HeightLarge)) {
            for (auto& x : w.objs)
                if (x.cls == K_SET) {
                    std::string l = (x.description().empty() ? "Set #" + std::to_string(x.uid()) : x.description()) + " (" +
                                    std::to_string(x.i32("MoeEntitySetCount")) + ")##" + std::to_string(x.uid());
                    if (ImGui::Selectable(l.c_str(), x.uid() == su)) { trig_touch("trigger setting"); o.set_u32(set_key, x.uid()); o.set_u32(key, 0); }
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Act on every unit of a named set (Triggers > Sets). Select several units and press Use selection to make one.");
    }
    if (triggers) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * dpi_);
        if (begin_combo("##trg", "or a trigger...", ImGuiComboFlags_HeightLarge)) {
            // A filter on top: a level lists hundreds of triggers (S-trig: only 9 rows showed).
            static std::string tf;
            if (ImGui::IsWindowAppearing()) { tf.clear(); ImGui::SetKeyboardFocusHere(); }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##tfilter", "Filter", &tf);
            const std::string lf = lower(tf);
            const std::set<uint32_t> own = cutscene_owned_triggers(w);  // those live on the timeline
            const auto& names = gd_.names();
            int shown = 0;
            for (auto& x : w.objs) {
                const std::string cls = names.label(x.cls);
                if (cls.rfind("MATrigger", 0) != 0 || own.count(x.uid())) continue;
                std::string nm = trig_display_name(x, cls);
                if (!lf.empty() && lower(nm + " " + trig_kind_label(cls)).find(lf) == std::string::npos) continue;
                if (++shown > 400) { ImGui::TextDisabled("More triggers than shown: use the filter."); break; }
                std::string l = nm + "##" + std::to_string(x.uid());
                if (ImGui::Selectable(l.c_str(), x.uid() == u)) { trig_touch("trigger setting"); o.set_u32(key, x.uid()); }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PopID();
}

void App::ui_cue(WObj& o, const char* key, bool music, bool voice) {
    Scenario& s = *doc_;
    const auto& cues = music ? retail_music_cues() : voice ? retail_voice_cues() : retail_sound_cues();
    uint32_t cur = o.u32(key);
    std::string lab = cur ? std::to_string(cur) : "(choose)";
    for (auto& c : cues) if (c.id == cur) lab = c.label;
    const bool own = !music && cur >= kAudioBase && cur < kAudioBase + 100000u;
    if (own) {
        auto it = s.audio.find(int(cur - kAudioBase));
        lab = "Your clip " + std::to_string(cur - kAudioBase) + (it != s.audio.end() ? "  " + it->second.source : "  (not imported)");
    }
    // Play beside the picker: own clips play from memory; a game cue plays only in the game (for now).
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 52 * dpi_);
    if (open_cue_for_ && open_cue_for_ == o.uid()) {
        open_cue_for_ = 0;
        ImGui::OpenPopup(ImHashStr("##ComboPopup", 0, ImGui::GetID("##cue")));
    }
    if (begin_combo("##cue", lab.c_str(), ImGuiComboFlags_HeightLarge)) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##cf", "Filter (words, mission, trigger name)", &filter);
        const std::string lf = lower(filter);
        auto hit = [&](const CueInfo& c) {
            return lf.empty() || lower(c.label).find(lf) != std::string::npos || lower(c.detail).find(lf) != std::string::npos;
        };
        if (!music)
            for (auto& [n, c] : s.audio) {
                std::string l = "Your clip " + std::to_string(n) + "  " + c.source;
                if (!lf.empty() && lower(l).find(lf) == std::string::npos) continue;
                ImGui::PushID(n + 300000);
                audition_button(n, ("pick" + std::to_string(n)).c_str());
                ImGui::SameLine();
                if (ImGui::Selectable(l.c_str(), cur == kAudioBase + uint32_t(n))) { trig_touch("trigger setting"); o.set_u32(key, kAudioBase + uint32_t(n)); }
                ImGui::PopID();
            }
        // The game's own cues, by mission: the words the player reads with the cue first.
        int shown = 0;
        for (size_t i = 0; i < cues.size() && shown <= 1000;) {
            size_t e = i;
            while (e < cues.size() && cues[e].mission == cues[i].mission) e++;
            bool any = false;
            for (size_t k = i; k < e && !any; k++) any = hit(cues[k]);
            if (any) {
                std::string pretty = cues[i].mission;
                for (char& ch : pretty) if (ch == '_') ch = ' ';
                if (pretty.empty()) pretty = "Other";
                if (!lf.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                else if (cur) for (size_t k = i; k < e; k++) if (cues[k].id == cur) { ImGui::SetNextItemOpen(true, ImGuiCond_Appearing); break; }
                if (ImGui::TreeNode((pretty + "##m" + cues[i].mission).c_str())) {
                    for (size_t k = i; k < e; k++) {
                        if (!hit(cues[k])) continue;
                        ImGui::PushID(int(cues[k].id));
                        if (ImGui::Selectable(cues[k].label.c_str(), cues[k].id == cur)) { trig_touch("trigger setting"); o.set_u32(key, cues[k].id); }
                        if (!cues[k].words.empty()) ImGui::SetItemTooltip("%s", cues[k].detail.c_str());
                        ImGui::PopID();
                        shown++;
                    }
                    ImGui::TreePop();
                }
            }
            i = e;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (own) audition_button(int(cur - kAudioBase), "cue");
    else audition_retail_button("cue");
    if (!music && s.audio.empty()) hint_text("Import your own WAV clips on the Objectives & Triggers tab to play them here.");
}

void App::ui_text(WObj& o, const char* key, const char* label, bool optional) {
    Scenario& s = *doc_;
    uint32_t id = o.u32(key);
    ImGui::TextUnformatted(label);
    if (std::string* tx = own_text(s, id)) {
        std::string v = *tx;
        if (ImGui::InputTextMultiline((std::string("##") + key).c_str(), &v, ImVec2(-1, 52 * dpi_), ImGuiInputTextFlags_WordWrap)) { trig_touch("message text"); *tx = v; }
        if (optional && ImGui::SmallButton("No caption")) { trig_touch("message text"); o.set_u32(key, 0); }
    } else {
        std::string words = id ? gd_.game_text(id) : std::string();
        if (!id) ImGui::TextDisabled("(none)");
        else if (words.empty()) ImGui::TextDisabled("The game's own text (not found on this disc)");
        else ImGui::TextWrapped("\"%s\"", words.c_str());
        if (id) ImGui::SetItemTooltip("The game's own words (text %u). Write my own text replaces them in this map.", id);
        if (ImGui::SmallButton(((id ? "Write my own text##" : "Add text##") + std::string(key)).c_str())) {
            trig_touch("message text");
            o.set_u32(key, add_text(s, ""));
        }
    }
}

// The click on a unit (or marker) for a Pick on map link. Stays armed on a miss; ends after a pick.
void App::pick_link_unit(float mx, float my) {
    Scenario& s = *doc_;
    int h = pick_object(mx, my);
    const WObj* o = h >= 0 ? &s.world.objs[size_t(h)] : nullptr;
    if (!o || is_trigger_shape(o->cls) || !o->find("CorporalPos")) {
        status_ = "Click a unit or marker on the map. Right-click or " + kt(A_CANCEL) + " cancels.";
        status_time_ = 6;
        return;
    }
    WObj* a = object_by_uid(s.world, pick_.uid);
    if (!a) { pick_ = PickTarget(); tool_ = Tool::Select; return; }
    push_undo("trigger setting");
    a->set_u32(pick_.key.c_str(), o->uid());
    if (!pick_.set_key.empty()) a->set_u32(pick_.set_key.c_str(), 0);
    if (pick_.key == "AttackUID") a->set_u32("AttackPlayer", 1);  // an attack order's unit target (chooser): not the player
    const std::string name = o->description().empty() ? "unit " + std::to_string(o->uid()) : o->description();
    const Tab back = pick_.back;
    pick_ = PickTarget();
    tool_ = Tool::Select;
    tab_ = back;
    objects_dirty_ = true;
    s.dirty = true;
    status_ = "Picked " + name + ".";
    status_time_ = 5;
}

// The settings of one action: a widget block per action class. `trigger` is the trigger (or cue holder) the action sits under.
void App::ui_action_body(WObj& tr, WObj& a, std::function<void()>& later) {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const std::string ac = gd_.names().label(a.cls);
    auto trigger_name = [&](const WObj& x) { return trig_display_name(x, gd_.names().label(x.cls)); };
    if (ac == "MAActionBroadcast") {
        // Speaker: the game draws no caption, so the export puts "Speaker: text" into the message body.
        {
            uint32_t cid = a.u32("CaptionTextID");
            std::string* sp = cid ? own_text(s, cid) : nullptr;
            form_label("Speaker (optional)", form_col());
            if (cid && !sp) {  // a game title text from a copied level: kept, or replaced by the author's own
                std::string words = gd_.game_text(cid);
                ImGui::TextDisabled("%s", words.empty() ? "(the game's own title)" : words.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Write my own speaker")) { trig_touch("message speaker"); a.set_u32("CaptionTextID", add_text(s, "")); }
            } else {
                std::string v = sp ? *sp : std::string();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##speaker", "e.g. Command, Pilot", &v)) {
                    trig_touch("message speaker");
                    if (sp) {
                        if (v.empty()) { s.strings.erase(int(cid - kTextBase)); a.set_u32("CaptionTextID", 0); }
                        else *sp = v;
                    } else if (!v.empty()) {
                        a.set_u32("CaptionTextID", add_text(s, v));
                    }
                }
                ImGui::SetItemTooltip("Who is speaking. The player reads \"Speaker: message\". Leave empty for a plain message.");
            }
        }
        ui_text(a, "BodyTextID", "Message", false);
        {
            uint32_t bid = a.u32("BodyTextID"), cid = a.u32("CaptionTextID");
            std::string* bt = bid ? own_text(s, bid) : nullptr;
            std::string* sp = cid ? own_text(s, cid) : nullptr;
            std::string body = bt ? *bt : bid ? gd_.game_text(bid) : std::string();
            form_label("In the game:", form_col());
            ImGui::TextWrapped("\"%s\"", speaker_line(sp ? *sp : std::string(), body).c_str());
        }
        ui_float(a, "TimeDisplayedFor", "Seconds shown", "How long the message stays on screen; 0 = until the player closes it.");  // the long label was cut off by the label column
        ui_bool(a, "PauseGame", "Pause the game while it shows");
        if (ImGui::SmallButton("Add a sound to play with it")) {
            uint32_t me = a.uid(), trg = tr.uid();
            later = [this, me, trg]() {
                push_undo("add sound");
                uint32_t snd = add_action_object(*doc_, trg, "MAActionSound", "Play a sound");
                if (!snd) return;
                // It goes right after this message (not at the end of the trigger), and its picker opens.
                WTree tt;
                tt.build(doc_->world);
                int ti2 = tt.find(trg);
                if (ti2 >= 0) {
                    const auto& kids = tt.kids[size_t(ti2)];
                    int pos = -1, n = int(kids.size());
                    for (int k = 0; k < n; k++) if (doc_->world.objs[size_t(kids[size_t(k)])].uid() == me) pos = k;
                    for (int steps = n - 2 - pos; pos >= 0 && steps > 0; steps--) wedit::move_sibling(doc_->world, snd, -1);
                }
                open_cue_for_ = snd;
                world_changed();
            };
        }
        ImGui::SetItemTooltip("Adds \"Play a sound\" right after this message, for a radio transmission: the message and its sound start together.");
    } else if (ac == "MAActionSound") {
        ui_cue(a, "CueEvent", false);
    } else if (ac == "MAActionPlayVoice") {
        ui_cue(a, "CueID", false, true);
    } else if (ac == "MAActionMusic") {
        ui_cue(a, "MusicCue", true);
        ui_int(a, "IntensityLevel", "Intensity", "The game's level for this cue (most of the game's own actions use 3000).");
    } else if (ac == "MAActionMissionVariableSet") {
        int var = a.i32("MissionVariableSet");
        int cur = -1;  // the objective whose MARoot slot uses this variable (retail slots use other numbers)
        for (int i = 0; i < int(s.objectives.size()) && cur < 0; i++) if (objective_var(s, i) == var) cur = i;
        std::string lab = cur >= 0 ? std::to_string(cur + 1) + ". " + s.objectives[size_t(cur)] : "variable " + std::to_string(var);
        form_label("Objective", form_col());
        if (begin_combo("##objective", lab.c_str())) {
            for (int i = 0; i < int(s.objectives.size()); i++)
                if (ImGui::Selectable((std::to_string(i + 1) + ". " + s.objectives[size_t(i)]).c_str(), i == cur)) {
                    trig_touch("trigger setting");
                    a.set_i32("MissionVariableSet", claim_objective_var(s, i));
                    mark_trigger_ = tr.uid();  // its radar target, next frame (see the top of tab_triggers)
                    mark_objective_ = i;
                }
            ImGui::EndCombo();
        }
        if (s.objectives.empty()) ImGui::TextDisabled("Add objectives on the Objectives & Triggers tab.");
        if (cur < 0) ui_int(a, "MissionVariableSet", "Variable (advanced)");  // An objective's own number is not shown
        ui_bool(a, "VariableState", "Set (untick: clear)");
    } else if (ac == "MAActionGameCondition") {
        ui_enum(a, "GameCondition", "Result", {{2, "Win the mission"}, {1, "Lose the mission"}});
    } else if (ac == "MAActionShow") {
        ui_link(a, "ActionEntity", "Unit:", false, "AISet", later);
        ui_bool(a, "Enable", "Show it (untick: hide it)");
    } else if (ac == "MAActionRemoveEntity") {
        ui_link(a, "ActionEntity", "Unit:", false, "ActionSet", later);
        ui_bool(a, "ShowKillEffect", "With an explosion");
    } else if (ac == "MAActionAI") {
        ui_link(a, "ActionEntity", "Unit or trigger:", true, "AISet", later);
        ui_bool(a, "EnableAI", "Switch on (untick: off)");
    } else if (ac == "MAActionActivateCinema") {
        uint32_t cur = a.u32("cinemaID");
        static const uint32_t K_CINEMA = key_of("MACinema");
        std::string lab = cur ? unit_label(w, cur) : "(choose)";
        form_label("Cutscene", form_col());
        if (begin_combo("##cutscene", lab.c_str())) {
            for (auto& o : w.objs)
                if (o.cls == K_CINEMA) {
                    std::string l = (o.description().empty() ? "Cutscene #" + std::to_string(o.uid()) : o.description()) + "##" + std::to_string(o.uid());
                    if (ImGui::Selectable(l.c_str(), o.uid() == cur)) { trig_touch("trigger setting"); a.set_u32("cinemaID", o.uid()); }
                }
            ImGui::EndCombo();
        }
        uint32_t me = a.uid();
        if (ImGui::SmallButton("New cutscene")) {
            later = [this, me]() {
                push_undo("new cutscene");
                if (uint32_t u = new_cutscene()) if (WObj* o = object_by_uid(doc_->world, me)) o->set_u32("cinemaID", u);
            };
        }
        ImGui::SetItemTooltip("Builds a real cutscene (camera, shot, flight path, focus marker) at the view centre and picks it.");
    } else if (ac == "MAActionTeleport") {
        uint32_t mk = a.u32("MarkerUID");
        ImGui::Text("Destination: %s", mk ? "a marker on the map" : "(none yet)");
        ImGui::SameLine();
        if (mk && ImGui::SmallButton("Select")) { select_uid(mk); go_to_object(mk); }
        ImGui::SameLine();
        if (theme::small_toggle_button("Pick on map", picking(PickTarget::LinkMarker, a.uid()))) {
            pick_ = PickTarget();
            pick_.kind = PickTarget::LinkMarker;
            pick_.uid = a.uid();
            pick_.key = "MarkerUID";
            pick_.null_marker = false;
            pick_.name = trigger_name(tr) + " - teleport marker";
            tool_ = Tool::PickPoint;
        }
        ImGui::SetItemTooltip("Then click the map where the player should appear.");
        ui_float(a, "Heading", "Facing (degrees)");
    } else if (ac == "MAActionNation") {
        ui_link(a, "ActionEntity", "Unit:", false, "AISet", later);
        ui_enum(a, "Nation", "Side", {{6, "Player's side"}, {5, "Enemy"}, {0, "Neutral"}});
        // A unit whose AI is off stays inert after a side change, so it is woken too (on by default; the tick undoes it).
        {
            static const uint32_t K_AI = key_of("MAActionAI");
            const uint32_t unit = a.u32("ActionEntity"), me = a.uid(), trig = tr.uid();
            const WObj* uo = unit ? object_by_uid(w, unit) : nullptr;
            if (uo && uo->find("m_bAIEnabled") && uo->i32("m_bAIEnabled") == 0) {
                uint32_t wake = 0;
                for (auto& o : w.objs)
                    if (o.parent() == trig && o.cls == K_AI && o.u32("ActionEntity") == unit && o.i32("EnableAI") == 1) { wake = o.uid(); break; }
                static uint32_t auto_action = 0, auto_unit = 0;  // the default is applied once per (action, unit): unticking sticks
                const std::string nm = uo->description();
                auto add_wake = [this, trig, unit, nm]() {
                    if (uint32_t wa = add_action_object(*doc_, trig, "MAActionAI", "Wake " + nm))
                        if (WObj* ao = object_by_uid(doc_->world, wa)) { ao->set_u32("ActionEntity", unit); ao->set_i32("EnableAI", 1); }
                };
                if (!wake && (auto_action != me || auto_unit != unit)) {
                    auto_action = me;
                    auto_unit = unit;
                    later = [this, add_wake]() { push_undo("wake the unit"); add_wake(); world_changed(); };
                }
                bool on = wake != 0;
                if (ImGui::Checkbox("Wake it too", &on)) {
                    auto_action = me;
                    auto_unit = unit;
                    if (on) later = [this, add_wake]() { push_undo("wake the unit"); add_wake(); world_changed(); };
                    else later = [this, wake]() { push_undo("do not wake the unit"); wedit::remove_subtree(doc_->world, wake); world_changed(); };
                }
                ImGui::SetItemTooltip("This unit's AI is off, so after the side change it would stand still. Ticked: the same trigger also switches its AI on.");
            }
        }
        // A neutral 'Mech or vehicle is placed EMPTY (boardable, no pilot). Changing its side does not put a pilot in it,
        // so an empty unit made an enemy stood in one pose for ever (game run, row 6; with a pilot it walked up and attacked).
        {
            const uint32_t unit = a.u32("ActionEntity"), me = a.uid();
            const WObj* uo = unit ? object_by_uid(w, unit) : nullptr;
            if (uo && uo->find("StartAbandoned") && a.i32("Nation") != 0 && io_ && (uo->i32("StartAbandoned") != 0 || uo->i32("Nation") == 0)) {
                const bool empty = uo->i32("StartAbandoned") != 0;
                static uint32_t crew_action = 0, crew_unit = 0;  // applied once per (action, unit): unticking sticks
                // One pilot-adding path (merged): crew_for_side_change gives the retail form of a side-changing unit
                // (Nation 0, crewed, asleep, StartAbandoned 0, AttackPilot 1, no owner controller); "Wake it too" above switches the AI on.
                auto crew = [this, unit]() {
                    push_undo("put a pilot in the unit");
                    io_->crew_for_side_change(*doc_, unit);
                    world_changed();
                };
                auto chain = [&later](std::function<void()> f) {
                    std::function<void()> prev = later;
                    later = [prev, f]() { if (prev) prev(); f(); };
                };
                if (empty && (crew_action != me || crew_unit != unit)) {
                    crew_action = me;
                    crew_unit = unit;
                    chain(crew);
                }
                bool on = !empty;
                if (ImGui::Checkbox("It has a pilot from the start", &on)) {
                    crew_action = me;
                    crew_unit = unit;
                    if (on) chain(crew);
                    else chain([this, unit]() { push_undo("empty the unit"); io_->apply_unit_side(*doc_, unit, 0); world_changed(); });
                }
                ImGui::SetItemTooltip("A neutral unit is empty (the player can climb in). An empty unit made an enemy has nobody to fight, so it stands still. Ticked: a pilot is in it from the start (it stays neutral until this action, and the player cannot climb in).");
                if (empty) {
                    ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
                    ImGui::TextWrapped("%s is empty (no pilot): it will not fight after the side change. Tick 'It has a pilot from the start' to put a pilot in it.", uo->description().c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
    } else if (ac == "MAActionChangePlayerVehicle") {
        // The game wants a "player configuration" (Mad Dog, Blood Asp, Battle Armor...), not a unit: a plain unit template did
        // nothing in the game run. The configurations are the game's own (GlobalEntities).
        static const uint32_t K_PCFG = key_of("MAPlayerConfiguration");
        uint32_t cur = a.u32("ConfigTemplateUID");
        std::string lab = "(choose)";
        std::vector<std::pair<uint32_t, std::string>> cfgs;
        for (auto& g : io_->catalog().global().objs)
            if (g.cls == K_PCFG) {
                std::string d = g.description().empty() ? "Configuration #" + std::to_string(g.uid()) : g.description();
                if (d.size() > 21 && d.compare(d.size() - 21, 21, " Player Configuration") == 0) d.resize(d.size() - 21);
                else if (d.size() > 14 && d.compare(d.size() - 14, 14, " Configuration") == 0) d.resize(d.size() - 14);
                if (g.uid() == cur) lab = d;
                cfgs.push_back({g.uid(), d});
            }
        if (cur && lab == "(choose)") lab = "Not a vehicle configuration (#" + std::to_string(cur) + ")";
        form_label("Player becomes", form_col());
        if (begin_combo("##pcfg", lab.c_str())) {
            for (auto& c : cfgs) {
                std::string l = c.second + "##" + std::to_string(c.first);
                if (ImGui::Selectable(l.c_str(), c.first == cur)) {
                    trig_touch("trigger setting");
                    a.set_u32("ConfigTemplateUID", c.first);
                    a.set_i32("PlayerNum", 0);
                    a.set_i32("UseHeading", 1);
                    a.set_i32("SetAsDefaultConfig", 0);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The player's 'Mech or suit changes into this one where the player stands.");
        ui_float(a, "Heading", "Facing (degrees)");
        if (!cur || lab.rfind("Not a vehicle", 0) == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().error);
            ImGui::TextWrapped("Pick one of the vehicle configurations in the list: anything else leaves the player unchanged.");
            ImGui::PopStyleColor();
        }
    } else if (ac == "MAActionMoeEFX") {
        // The effect plays at the unit that set the trigger off (retail: 43 of 44 under a circle, 1 under a box). A timer has no
        // such unit and showed nothing in the game run.
        {
            // The effect numbers the game's own missions use (a census of all 44 retail uses). The game's data gives them no names, so
            // only the one proven in a game run is described; the rest say how often the real missions use them. Any other number can be typed below.
            struct Fx { uint32_t id; int uses; const char* what; };
            static const Fx kFx[] = {
                {125706, 3, "Large explosion"},
                {383, 7, nullptr}, {32550, 5, nullptr}, {126121, 4, nullptr}, {125102, 4, nullptr}, {125320, 3, nullptr}, {117616, 2, nullptr},
                {125106, 2, nullptr}, {36145, 2, nullptr}, {31517, 2, nullptr}, {125687, 1, nullptr}, {126143, 1, nullptr}, {366, 1, nullptr},
                {125580, 1, nullptr}, {125311, 1, nullptr}, {125312, 1, nullptr}, {125139, 1, nullptr}, {413, 1, nullptr}, {116486, 1, nullptr},
                {125410, 1, nullptr},
            };
            auto fx_label = [](const Fx& f) {
                if (f.what) return std::string(f.what) + " (" + std::to_string(f.id) + ")";
                return "Effect " + std::to_string(f.id) + " (used " + std::to_string(f.uses) + (f.uses == 1 ? " time)" : " times)");
            };
            const uint32_t cur = a.u32("Moe Effect ID");
            std::string lab = "Effect " + std::to_string(cur) + " (typed)";
            for (const Fx& f : kFx)
                if (f.id == cur) lab = fx_label(f);
            form_label("Effect", form_col());
            if (begin_combo("##fxpick", lab.c_str())) {
                for (const Fx& f : kFx) {
                    const std::string l = fx_label(f) + "##fx" + std::to_string(f.id);
                    if (ImGui::Selectable(l.c_str(), f.id == cur)) { trig_touch("trigger setting"); a.set_u32("Moe Effect ID", f.id); }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("The visual effects the game's own missions play. The game gives them no names: the large explosion (a fireball, then smoke) is the one checked in the game; "
                                  "the others say how many times the real missions use them, so try them to see. Any other number can be typed below.");
        }
        ui_int(a, "Moe Effect ID", "Effect number", "The game's effect number: pick one above, or type another (the game's own missions use values from 366 to 126143).");
        const std::string pc = gd_.names().label(tr.cls);
        if (pc != "MATriggerRadius" && pc != "MATriggerCuboidalVolume" && pc != "MATriggerNationRadius") {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::tok().warn);
            ImGui::TextWrapped("This effect plays where the unit that sets the trigger off stands. A timer has no such unit, so nothing shows: put this action on a 'When units enter a circle' or box trigger.");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Plays where the unit that sets the trigger off stands.");
        }
    } else if (ac == "MAActionCheckPointSave") {
        ui_str(a, "suffix", "Name");
    } else if (ac == "MAActionSwapSetIn" || ac == "MAActionSwapSetOut") {
        static const uint32_t K_SWAPSET = key_of("MASwapSet");
        uint32_t cur = a.u32("dwSwapSet");
        std::string lab = cur ? unit_label(w, cur) : "(choose)";
        form_label("Swap set", form_col());
        if (begin_combo("##swapset", lab.c_str())) {
            for (auto& o : w.objs)
                if (o.cls == K_SWAPSET) {
                    std::string l = (o.description().empty() ? "Swap set #" + std::to_string(o.uid()) : o.description()) + "##" + std::to_string(o.uid());
                    if (ImGui::Selectable(l.c_str(), o.uid() == cur)) { trig_touch("trigger setting"); a.set_u32("dwSwapSet", o.uid()); }
                }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled(ac == "MAActionSwapSetIn" ? "Its units appear in the level." : "Its units leave the level.");
        uint32_t me = a.uid(), cu = cur;
        if (cu && ImGui::SmallButton("Edit its units")) {
            // Opens the Sets view on the set the swap set swaps (members list + Add selected).
            const WObj* so = object_by_uid(w, cu);
            uint32_t es = so ? so->u32("dwEntitySwapSet") : 0;
            if (es) { set_uid_ = es; trig_mode_ = 3; }
            select_uid(cu);
        }
        ImGui::SetItemTooltip("Opens the Sets view on this swap set's units: select units on the map, then press Add selected.");
        if (cu) ImGui::SameLine();
        if (ImGui::SmallButton("New swap set")) {
            later = [this, me]() {
                push_undo("new swap set");
                Scenario& sc = *doc_;
                uint32_t next = next_uid(sc.world);
                uint32_t grp = ensure_group(sc.world, root_uid(sc.world), "Swap Sets", next);
                uint32_t sw = io_->add_part(sc, key_of("MASwapSet"), grp, nullptr);
                uint32_t set = add_entity_set("New swap set");
                if (sw && set) {
                    if (WObj* o = object_by_uid(sc.world, sw)) { o->set_str("Description", "New swap set"); o->set_u32("dwEntitySwapSet", set); o->set_i32("bSwappedIn", 0); }
                    if (WObj* o = object_by_uid(sc.world, me)) o->set_u32("dwSwapSet", sw);
                    selection_ = {sw};
                }
                world_changed();
            };
        }
        ImGui::SetItemTooltip("Creates an empty swap set (a swap set and its entity set) and picks it; then add its units in Properties.");
    } else {
        ui_common_fields(a);
    }
}

}  // namespace ed

// Objectives & Triggers tab > Sets: the level's MoeEntitySet lists (teams, a cutscene's cast: triggers and actions
// act on all members at once) and MASwapSet switches. Real objects, as in the retail levels: a set
// holds MoeEntitySetCount and MoeEntitySetMember0..n, a swap set points at a set (dwEntitySwapSet).
#include <algorithm>

#include "../core/triggers.h"
#include "app.h"
#include "imgui.h"
#include "theme.h"
#include "ui_tips.h"
#include "imgui_stdlib.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {

std::vector<uint32_t> members_of(const WObj& s) {
    std::vector<uint32_t> out;
    int n = s.i32("MoeEntitySetCount");
    for (int k = 0; k < n; k++) out.push_back(s.u32("MoeEntitySetMember" + std::to_string(k)));
    return out;
}

void write_members(WObj& s, const std::vector<uint32_t>& m) {
    int old = s.i32("MoeEntitySetCount");
    for (int k = 0; k < old; k++) s.erase(key_of("MoeEntitySetMember" + std::to_string(k)));
    for (size_t k = 0; k < m.size(); k++) s.set_u32("MoeEntitySetMember" + std::to_string(k), m[k]);
    s.set_i32("MoeEntitySetCount", int(m.size()));
}

// Retail links both ways: a set lists its members (MoeEntitySetMember0..n) and every member unit lists its sets
// (SetCount, SetMember0..n; e.g. Mission_09b's "Uziel" 124535 holds set 129259 and that set holds it).
std::vector<uint32_t> unit_sets_of(const WObj& u) {
    std::vector<uint32_t> out;
    int n = u.i32("SetCount");
    for (int k = 0; k < n; k++) out.push_back(u.u32("SetMember" + std::to_string(k)));
    return out;
}

void write_unit_sets(WObj& u, const std::vector<uint32_t>& v) {
    int old = u.i32("SetCount");
    for (int k = 0; k < old; k++) u.erase(key_of("SetMember" + std::to_string(k)));
    for (size_t k = 0; k < v.size(); k++) u.set_u32("SetMember" + std::to_string(k), v[k]);
    u.set_i32("SetCount", int(v.size()));
}

// The set's member list becomes `m`, and the units that joined or left get (or lose) the matching back link.
void set_members(Wdf& w, WObj& s, const std::vector<uint32_t>& m) {
    std::vector<uint32_t> old = members_of(s);
    write_members(s, m);
    const uint32_t me = s.uid();
    for (uint32_t u : old)
        if (std::find(m.begin(), m.end(), u) == m.end())
            if (WObj* o = u ? object_by_uid(w, u) : nullptr) {
                std::vector<uint32_t> v = unit_sets_of(*o);
                auto it = std::remove(v.begin(), v.end(), me);
                if (it != v.end()) { v.erase(it, v.end()); write_unit_sets(*o, v); }
            }
    for (uint32_t u : m)  // also for members that were already listed: repairs a set made without the back link
        if (WObj* o = u ? object_by_uid(w, u) : nullptr) {
            std::vector<uint32_t> v = unit_sets_of(*o);
            if (std::find(v.begin(), v.end(), me) == v.end()) { v.push_back(me); write_unit_sets(*o, v); }
        }
}

}  // namespace

// An empty MoeEntitySet under the level's MoeGroupSet (made when missing). The retail prototype comes with two blank
// member slots, which show as "(none)" and would be null units for the game's set walkers: clear them.
uint32_t App::add_entity_set(const std::string& name) {
    Scenario& s = *doc_;
    static const uint32_t K_SET = key_of("MoeEntitySet"), K_GSET = key_of("MoeGroupSet");
    uint32_t gs = 0;
    for (auto& o : s.world.objs) if (o.cls == K_GSET) { gs = o.uid(); break; }
    if (!gs) gs = io_->add_part(s, K_GSET, root_uid(s.world), nullptr);
    uint32_t u = gs ? io_->add_part(s, K_SET, gs, nullptr) : 0;
    if (WObj* o = u ? object_by_uid(s.world, u) : nullptr) { write_members(*o, {}); o->set_str("Description", name); }
    return u;
}

// A new set holding these units (with the units' back links), for "act on these units" pickers.
uint32_t App::entity_set_from(const std::string& name, const std::vector<uint32_t>& units) {
    uint32_t u = add_entity_set(name);
    if (WObj* o = u ? object_by_uid(doc_->world, u) : nullptr) set_members(doc_->world, *o, units);
    return u;
}

// After objects were deleted: take their uids out of every set (a member that no longer exists would be a null unit
// for the game's set walkers and shows as "missing #n" in the Sets view).
int App::drop_dead_set_members() {
    Wdf& w = doc_->world;
    static const uint32_t K_SET = key_of("MoeEntitySet");
    std::unordered_set<uint32_t> uids;
    for (auto& o : w.objs) uids.insert(o.uid());
    int dropped = 0;
    for (auto& o : w.objs) {
        if (o.cls != K_SET) continue;
        std::vector<uint32_t> m = members_of(o), keep;
        for (uint32_t u : m) if (uids.count(u)) keep.push_back(u);
        if (keep.size() != m.size()) { dropped += int(m.size() - keep.size()); write_members(o, keep); }
    }
    return dropped;
}

void App::tab_sets() {
    Scenario& s = *doc_;
    Wdf& w = s.world;
    const bool ro = s.read_only;
    const auto& names = gd_.names();
    static const uint32_t K_SET = key_of("MoeEntitySet"), K_SWAP = key_of("MASwapSet");
    auto name_of = [&](uint32_t u) -> std::string {
        const WObj* o = u ? object_by_uid(w, u) : nullptr;
        if (!o) return u ? "missing #" + std::to_string(u) : "(none)";
        return o->description().empty() ? names.label(o->cls) + " #" + std::to_string(u) : o->description();
    };
    auto touch = [&](const char* label) { push_undo_once(label); s.dirty = true; objects_dirty_ = true; };

    ImGui::TextWrapped("A set is a named list of units. Triggers and actions can switch, show or remove all members at once; "
                       "in the missions they also hold a team (Red Stuff, Blue Stuff) or a cutscene's cast.");
    {
        struct Dis { explicit Dis(bool d) { ImGui::BeginDisabled(d); } ~Dis() { ImGui::EndDisabled(); } } d(ro);
        if (ImGui::Button("New set")) {
            push_undo("new set");
            if (uint32_t u = add_entity_set("New set")) {
                set_uid_ = u;
                world_changed();
            }
        }
        ImGui::SetItemTooltip("Add an empty set. Select units on the map, then press Add selected.");
        ImGui::SameLine();
        if (theme::danger_button("Delete") && set_uid_) {
            uint32_t victim = set_uid_;
            int users = int(used_by(w, victim, [&](uint32_t c) { return names.label(c).rfind("MATrigger", 0) == 0; }).size());
            std::string q = "Delete the set \"" + name_of(victim) + "\"?";
            if (users) q += "\n\n" + std::to_string(users) + " action(s) or swap set(s) use it and will have nothing to act on.";
            q += "\n\nYou can undo this with " + kt(A_UNDO) + ".";
            confirm(q, [this, victim]() {
                if (!doc_) return;
                push_undo("delete set");
                if (WObj* so = object_by_uid(doc_->world, victim)) set_members(doc_->world, *so, {});  // the units forget it
                wedit::remove_subtree(doc_->world, victim);
                set_uid_ = 0;
                world_changed();
                doc_->dirty = true;
            });
        }
    }

    static std::string filter;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sf", "Filter sets", &filter);
    std::string lf = lower(filter);
    ImGui::BeginChild("##slist", ImVec2(0, 170 * dpi_), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
    int shown = 0;
    for (auto& o : w.objs) {
        if (o.cls != K_SET) continue;
        std::string nm = name_of(o.uid());
        if (!lf.empty() && lower(nm).find(lf) == std::string::npos) continue;
        if (++shown > 600) { ImGui::TextDisabled("More sets than shown: use the filter."); break; }
        std::string l = nm + "  - " + std::to_string(o.i32("MoeEntitySetCount")) + " members";
        ImGui::PushID(int(o.uid()));
        if (ImGui::Selectable(l.c_str(), o.uid() == set_uid_)) set_uid_ = o.uid();
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled("This level has no sets.");
    ImGui::EndChild();

    uint32_t jump = 0;
    WObj* cur = set_uid_ ? object_by_uid(w, set_uid_) : nullptr;
    if (cur && cur->cls != K_SET) cur = nullptr;
    if (cur) {
        ImGui::SeparatorText("Selected set");
        ImGui::BeginDisabled(ro);
        std::string nm = cur->description();
        form_label("Name", form_col());
        if (ImGui::InputText("##setname", &nm)) { touch("rename set"); cur->set_str("Description", nm); }
        std::vector<uint32_t> m = members_of(*cur);
        ImGui::Text("Members (%zu)", m.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Add selected")) {
            touch("add to set");
            for (uint32_t u : selection_) if (std::find(m.begin(), m.end(), u) == m.end() && u != set_uid_) m.push_back(u);
            set_members(w, *cur, m);
        }
        ImGui::SetItemTooltip("Add the units selected on the map to this set.");
        int del = -1;
        ImGui::BeginChild("##members", ImVec2(0, 190 * dpi_), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        for (size_t k = 0; k < m.size(); k++) {
            ImGui::PushID(int(k));
            if (ImGui::SmallButton("x")) del = int(k);
            ImGui::SetItemTooltip("Take it out of the set (the unit itself stays).");
            ImGui::SameLine();
            if (ImGui::Selectable(name_of(m[k]).c_str(), false) && m[k]) jump = m[k];
            ImGui::SetItemTooltip("Select it on the map.");
            ImGui::PopID();
        }
        if (m.empty()) ImGui::TextDisabled("Empty. Select units on the map and press Add selected.");
        ImGui::EndChild();
        if (del >= 0) { touch("remove from set"); m.erase(m.begin() + del); set_members(w, *cur, m); }
        ImGui::EndDisabled();
    }

    // Swap sets: switches that exchange one set of characters for another.
    std::vector<uint32_t> swaps;
    for (auto& o : w.objs) if (o.cls == K_SWAP) swaps.push_back(o.uid());
    // Advanced detail (retail-only machinery stays reachable but out of the way): collapsed by default.
    if (!swaps.empty() && theme::section(("Advanced: swap sets (" + std::to_string(swaps.size()) + ")###swapsets").c_str())) {
        ImGui::TextDisabled("Switches in the game's levels that exchange one set of units for another (actions 'Swap set in / out').");
        ImGui::BeginDisabled(ro);
        for (uint32_t su : swaps) {
            WObj* o = object_by_uid(w, su);
            if (!o) continue;
            ImGui::PushID(int(su));
            ImGui::TextUnformatted(name_of(su).c_str());
            uint32_t cs = o->u32("dwEntitySwapSet");
            form_label("Swaps in", form_col());
            if (begin_combo("##swapsin", name_of(cs).c_str())) {
                for (auto& x : w.objs) {
                    if (x.cls != K_SET) continue;
                    std::string l = name_of(x.uid()) + "##" + std::to_string(x.uid());
                    if (ImGui::Selectable(l.c_str(), x.uid() == cs)) { touch("swap set"); o->set_u32("dwEntitySwapSet", x.uid()); }
                }
                ImGui::EndCombo();
            }
            bool in = o->i32("bSwappedIn") != 0;
            if (ImGui::Checkbox("Swapped in at start", &in)) { touch("swap set"); o->set_i32("bSwappedIn", in ? 1 : 0); }
            ImGui::SetItemTooltip("Kept as the game's flag. In the game the linked units are visible at the start either way; hide the ones that should appear later with a 'Show or hide units' (hide) action at mission start.");
            ImGui::SameLine();
            if (cs && ImGui::SmallButton("Open set")) set_uid_ = cs;
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }
    if (jump) select_uid(jump);
}

}  // namespace ed

// Lean reads for --automation (src/app/automation.cpp): the selected objects, a filtered object list and the ground at a point,
// as small JSON replies. They reuse the editor's own values (get_pos, Terrain::height_at, typemap_at,
// glue_family) instead of re-deriving them. Protocol: tools/AUTOMATION.md.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "trigger_ui.h"
#include "../core/cutscene.h"
#include "../core/json.h"
#include "../core/triggers.h"
#include "../core/weather.h"

namespace ed {

const char* fog_preset_label(float start, float end);  // app_environment.cpp

using namespace ma2;
using namespace ma2::world;
using ma2::Json;

namespace {
double r2(double v) { return std::round(v * 100.0) / 100.0; }
double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

// What the saved file holds at (x, z), worked out independently of Terrain::height_at: decode the 8-bit samples that
// Terrain::samples() writes (stored row order) and split each cell on the game's diagonal. It must equal height_at everywhere
// ("one ground"); the `ground` read reports both so a difference shows at once.
struct SavedGround {
    const Terrain& t;
    std::vector<uint8_t> s;
    explicit SavedGround(const Terrain& tt) : t(tt), s(tt.samples()) {}
    float grid(int gx, int gz) const {
        return t.min_h + float(s[size_t(t.h - 1 - gz) * t.w + gx]) / 255.f * std::max(0.001f, t.max_h - t.min_h);
    }
    float at(float x, float z) const {
        if (t.w < 2 || t.h < 2) return 0.f;
        float fx = std::clamp(x / t.scale, 0.f, float(t.w - 1)), fz = std::clamp(z / t.scale, 0.f, float(t.h - 1));
        int x0 = std::min(int(fx), t.w - 2), z0 = std::min(int(fz), t.h - 2);
        float tx = fx - float(x0), tz = fz - float(z0);
        float a = grid(x0, z0), b = grid(x0 + 1, z0), c = grid(x0, z0 + 1), d = grid(x0 + 1, z0 + 1);
        return tx >= tz ? a + (b - a) * tx + (d - b) * tz : a + (c - a) * tz + (d - c) * tx;
    }
};

}  // namespace

// mode 0: the selected objects (cap 50). mode 1: every positioned object matching `filter` (substring of the name or class,
// any case) and, with use_near, within r metres of (cx, cz) (nearest first); `limit` rows, `count` = all matches.
std::string App::automation_objects(int mode, const std::string& filter, bool use_near, float cx, float cz, float r, int limit) const {
    Json out = Json::object();
    if (!doc_) {
        out.with("rows", Json::array()).with("count", Json::number(0)).with("truncated", Json::boolean(false));
        return out.dump();
    }
    App* me = const_cast<App*>(this);
    const std::string f = lower(filter);
    struct Hit { const WObj* o; Vec3 p; double d; std::string name, cls; };
    std::vector<Hit> hits;
    for (auto& o : doc_->world.objs) {
        if (mode == 0 && !selection_.count(o.uid())) continue;
        Vec3 p;
        if (!get_pos(o, p)) continue;
        double d = 0;
        if (use_near) {
            d = std::hypot(double(p.x) - cx, double(p.z) - cz);
            if (d > r) continue;
        }
        std::string cls = gd_.names().label(o.cls), name;
        if (const TemplateInfo* t = cat_ ? cat_->find(o.template_id()) : nullptr) name = palette_plain_name(*t);
        if (name.empty()) name = o.description();
        if (!f.empty() && lower(name).find(f) == std::string::npos && lower(o.description()).find(f) == std::string::npos &&
            lower(cls).find(f) == std::string::npos)
            continue;
        hits.push_back({&o, p, d, std::move(name), std::move(cls)});
    }
    if (mode == 1 && use_near) std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.d < b.d; });
    const size_t cap = mode == 0 ? 50 : size_t(std::clamp(limit, 1, 500));
    Json rows = Json::array();
    const Terrain& t = doc_->terrain;
    const SavedGround saved(t);
    const SheetSet ss = hits.empty() ? SheetSet() : me->sheets(false);
    for (auto& h : hits) {
        if (rows.a.size() >= cap) break;
        const WObj& o = *h.o;
        // ground_drawn: Terrain::height_at, the surface the editor uses and the view draws (8-bit steps, split cells).
        // ground_saved: the same point worked out from the bytes the file will hold. They are equal.
        Json row = Json::object();
        row.with("uid", Json::number(o.uid())).with("name", Json::string(h.name)).with("class", Json::string(h.cls));
        row.with("x", Json::number(r2(h.p.x))).with("y", Json::number(r2(h.p.y))).with("z", Json::number(r2(h.p.z)));
        row.with("y3", Json::number(r3(h.p.y)));  // The height to the millimetre (a layer step is 0.03 m)
        if (int ly = 0; true) { bool fx = false; if (place::layer_of(o, ly, fx)) row.with("layer", Json::string(place::format_layer(ly, fx))); }
        me->stand_fields(row, o, ss, false);  // Gap (foot minus the surface under it) and on (the sheet it stands on)
        row.with("heading", Json::number(r2(std::remainder(heading(o) * 57.2958f, 360.f))));
        { float tp, tr; get_tilt(o, tp, tr); row.with("pitch", Json::number(r2(tp * 57.29578f))).with("roll", Json::number(r2(tr * 57.29578f))); }  // degrees; road pieces keep theirs in RoadRotation
        row.with("ground_drawn", Json::number(r2(t.height_at(h.p.x, h.p.z))));
        row.with("ground_saved", Json::number(r2(saved.at(h.p.x, h.p.z))));
        if (int fam = me->glue_family(o)) row.with("snap_family", Json::number(fam));
        if (mode == 0) {
            // What can be cut and where it stands now (sel only; the model test reads files).
            crop::Rect cr;
            const bool cut = crop::of(o, cr);
            Json ca = Json::array();
            if (cut) for (float v : {cr.x0, cr.x1, cr.z0, cr.z1}) ca.push(Json::number(r3(v)));
            row.with("crop", cut ? std::move(ca) : Json());
            const SizeInfo si = me->size_info(o);
            static const char* const kinds[] = {"none", "road", "plate", "run", "water"};
            Json sz = Json::object();
            sz.with("kind", Json::string(kinds[int(si.kind)]));
            if (si.kind == SizeKind::Road || si.kind == SizeKind::Plate || si.kind == SizeKind::Run) {
                crop::Rect whole, now;
                if (me->whole_rect(o, si, whole)) {
                    now = cut ? cr : whole;
                    const bool run = si.kind == SizeKind::Run;
                    const float mx = run && !si.along_x ? 0.1f : 0.f, mz = run && si.along_x ? 0.1f : 0.f;  // a run piece's uncut axis carries 5 cm of margin a side
                    const float ww = whole.x1 - whole.x0 - mx, wl = whole.z1 - whole.z0 - mz, nw = now.x1 - now.x0 - mx, nl = now.z1 - now.z0 - mz;
                    // w = across, l = along (a road's or plate's z; a run piece's long axis)
                    const bool swap = run && si.along_x;
                    sz.with("w", Json::number(r2(swap ? nl : nw))).with("l", Json::number(r2(swap ? nw : nl)));
                    sz.with("whole_w", Json::number(r2(swap ? wl : ww))).with("whole_l", Json::number(r2(swap ? ww : wl)));
                    if (run) sz.with("along_x", Json::boolean(si.along_x));
                }
            }
            if (si.kind == SizeKind::Water) {   // The water's own width / length (description), and its fixed-size flag is the `why`
                float ww = 0, wl = 0;
                me->water_size_of(o, nullptr, ww, wl);
                sz.with("w", Json::number(r2(ww))).with("l", Json::number(r2(wl)));
            }
            row.with("size", std::move(sz));
            if (!si.why.empty()) row.with("why", Json::string(si.why));
            row.with("indestructible", Json::number(o.i32("Indestructible")));
            row.with("has_wreck", Json::boolean(o.u32("DestroyedBuilding") != 0));
            if (std::vector<App::Joint> js; me->object_joints(o, js)) {  // where the next piece joins (a cut piece: its cut ends)
                Json ja = Json::array();
                for (const App::Joint& j : js) {
                    Json p = Json::array();
                    p.push(Json::number(r2(j.p.x)));
                    p.push(Json::number(r2(j.p.y)));
                    p.push(Json::number(r2(j.p.z)));
                    ja.push(std::move(p));
                }
                row.with("joints", std::move(ja));
            }
        }
        if (use_near) row.with("dist", Json::number(r2(h.d)));
        // Screen position of the pivot (what a click aims at); absent when it is off the map view.
        float sx, sy;
        if (me->cam_.project({h.p.x, h.p.y, h.p.z}, me->vp_w, me->vp_h, sx, sy) && sx >= 0 && sy >= 0 && sx < me->vp_w && sy < me->vp_h)
            row.with("sx", Json::number(std::round(me->vp_x + sx))).with("sy", Json::number(std::round(me->vp_y + sy)));
        rows.push(std::move(row));
    }
    out.with(mode == 0 ? "sel" : "objects", rows);
    out.with("count", Json::number(double(hits.size()))).with("truncated", Json::boolean(hits.size() > rows.a.size()));
    return out.dump();
}

std::string App::automation_ground(float x, float z) const {
    Json out = Json::object();
    if (!doc_) return out.with("error", Json::string("no scenario is open")).dump();
    const Terrain& t = doc_->terrain;
    out.with("x", Json::number(r2(x))).with("z", Json::number(r2(z))).with("inside", Json::boolean(t.inside(x, z)));
    const SavedGround saved(t);
    out.with("drawn", Json::number(r2(t.height_at(x, z))));  // the surface the editor uses and the view draws
    out.with("saved", Json::number(r2(saved.at(x, z))));     // the same point from the bytes the file will hold (equal)
    out.with("fine", Json::number(r3(t.fine_at(x, z))));    // the brushes' working layer (debugging strokes only)
    out.with("step", Json::number(r3(t.step())));            // one stored height step, metres
    out.with("slope", Json::number(r3(slope_at(t, x, z))));  // Rise over run of the ground triangle under the point (the cursor readout's slope)
    out.with("min_h", Json::number(r2(t.min_h))).with("max_h", Json::number(r2(t.max_h)));
    // The colour map texel under the point (r, g, b, a) and, with ground layers, the eight layer weights there.
    if (const Image& cm = doc_->colormap; !cm.px.empty() && t.size_x() > 0 && t.size_z() > 0) {
        const int px = std::clamp(int(x / t.size_x() * float(cm.w)), 0, cm.w - 1), py = std::clamp(int((1.f - z / t.size_z()) * float(cm.h)), 0, cm.h - 1);
        const uint32_t p = cm.px[size_t(py) * size_t(cm.w) + size_t(px)];
        Json c = Json::array();
        for (int k = 0; k < 4; k++) c.push(Json::number(double((p >> (8 * k)) & 255)));
        out.with("color", c);
    }
    if (const GroundLayers& g = doc_->ground; g.enabled && g.weights && g.w > 0 && g.h > 0 && t.size_x() > 0 && t.size_z() > 0) {
        const int wx = std::clamp(int(x / t.size_x() * float(g.w)), 0, g.w - 1), wy = std::clamp(int((1.f - z / t.size_z()) * float(g.h)), 0, g.h - 1);
        Json wj = Json::array();
        for (int k = 0; k < kGroundMaxLayers; k++) wj.push(Json::number(double(g.texel(wx, wy)[k])));
        out.with("weights", wj);
    }
    int v = typemap_at(*doc_, x, z);
    if (v >= 0) {
        const char* l = tmtype::label(uint16_t(v));
        out.with("type", Json::string(l ? l : "Other")).with("type_value", Json::number(v));
    } else {
        out.with("type", Json::string("none")).with("type_value", Json::number(-1));
    }
    return out.dump();
}

// The `triggers` read: the trigger list as the tab shows it (your own first, then the level's), counts of what is left out.
std::string App::automation_triggers() const {
    Json out = Json::object();
    if (!doc_) return out.with("error", Json::string("no scenario is open")).dump();
    const Wdf& w = doc_->world;
    const auto& names = gd_.names();
    WTree t;
    t.build(w);
    const std::vector<char> kind = trigger_kinds(w, names);
    const TrigList tl = trigger_list(t, kind, trig_uid_);
    std::unordered_map<uint32_t, int> orders;
    static const uint32_t K_EV = key_of("MAEvent");
    for (auto& o : w.objs) if (o.cls == K_EV && o.u32("TriggerID")) orders[o.u32("TriggerID")]++;
    Json rows = Json::array();
    int n_yours = 0, n_level = 0, shown = 0;
    for (auto& r : tl.rows) {
        (r.level ? n_level : n_yours)++;
        if (shown >= 200) continue;
        shown++;
        const WObj& o = w.objs[size_t(r.idx)];
        int acts = 0;
        for (int k : t.kids[size_t(r.idx)]) acts += kind[size_t(k)] == 2;
        Json j = Json::object();
        j.with("uid", Json::number(o.uid())).with("name", Json::string(o.description())).with("kind", Json::string(trig_kind_label(names.label(o.cls))))
            .with("section", Json::string(r.level ? "level" : "yours")).with("depth", Json::number(r.depth))
            .with("enabled", Json::boolean(!o.find("TriggerEnabled") || o.i32("TriggerEnabled") != 0))
            .with("repeat", Json::boolean(o.i32("TriggerAutoReset") != 0)).with("actions", Json::number(acts))
            .with("orders", Json::number(orders.count(o.uid()) ? orders[o.uid()] : 0));
        rows.push(std::move(j));
    }
    out.with("rows", std::move(rows)).with("yours", Json::number(n_yours)).with("level", Json::number(n_level))
        .with("cutscene_hidden", Json::number(tl.cutscene_hidden)).with("picked", Json::number(trig_uid_))
        .with("level_open", Json::boolean(trig_level_open_));
    return out.dump();
}

// The `trigger` read: one trigger's actions and AI orders (uid 0 = the one the tab shows).
std::string App::automation_trigger(uint32_t uid) const {
    Json out = Json::object();
    if (!doc_) return out.with("error", Json::string("no scenario is open")).dump();
    Scenario& s = *doc_;
    const Wdf& w = s.world;
    const auto& names = gd_.names();
    if (!uid) uid = trig_uid_;
    const WObj* tr = uid ? object_by_uid(w, uid) : nullptr;
    if (!tr || names.label(tr->cls).rfind("MATrigger", 0) != 0) return out.with("error", Json::string("no trigger" + (uid ? " #" + std::to_string(uid) : std::string()) + " (pass \"uid\", or pick one)")).dump();
    WTree t;
    t.build(w);
    const std::vector<char> kind = trigger_kinds(w, names);
    const TrigList tl = trigger_list(t, kind, uid);
    bool level = false;
    for (auto& r : tl.rows) if (w.objs[size_t(r.idx)].uid() == uid) level = r.level;
    out.with("uid", Json::number(uid)).with("name", Json::string(tr->description())).with("kind", Json::string(trig_kind_label(names.label(tr->cls))))
        .with("cls", Json::string(names.label(tr->cls))).with("section", Json::string(level ? "level" : "yours"))
        .with("enabled", Json::boolean(!tr->find("TriggerEnabled") || tr->i32("TriggerEnabled") != 0)).with("repeat", Json::boolean(tr->i32("TriggerAutoReset") != 0));
    if (names.label(tr->cls) == "MATriggerTimer") out.with("seconds", Json::number(r2(tr->f32("TrgTimerDurationSeconds"))));
    Json acts = Json::array();
    int n_acts = 0;
    int ti = t.find(uid);
    if (ti >= 0)
        for (int k : t.kids[size_t(ti)]) {
            if (kind[size_t(k)] != 2) continue;
            const WObj& a = w.objs[size_t(k)];
            n_acts++;
            Json j = Json::object();
            j.with("uid", Json::number(a.uid())).with("cls", Json::string(names.label(a.cls))).with("label", Json::string(trig_kind_label(names.label(a.cls))));
            for (const char* key : {"ActionEntity", "MarkerUID", "cinemaID"}) if (a.find(key)) j.with(key, Json::number(a.u32(key)));
            acts.push(std::move(j));
        }
    out.with("actions", std::move(acts));
    if (names.label(tr->cls) == "MATriggerDestroyed" || names.label(tr->cls) == "MATriggerLogic") {  // the units / triggers it watches
        Json wt = Json::array();
        for (uint32_t r : reference_children(w, uid))
            if (const WObj* ro_ = object_by_uid(w, r)) wt.push(Json::number(ro_->u32("ReferenceUID")));
        out.with("watches", std::move(wt));
    }
    Json ords = Json::array();
    int n_ords = 0;
    for (uint32_t ev : orders_of_trigger(w, uid)) {
        n_ords++;
        const uint32_t unit = order_unit(w, ev), beh = order_behaviour(w, ev);
        const WObj* b = beh ? object_by_uid(w, beh) : nullptr;
        const WObj* u = unit ? object_by_uid(w, unit) : nullptr;
        const std::string bc = b ? names.label(b->cls) : std::string();
        const uint32_t g = order_path(w, ev);
        Json j = Json::object();
        j.with("uid", Json::number(ev)).with("unit", Json::number(unit))
            .with("kind", Json::string(bc == "MAPatrol" ? "patrol" : bc == "MADefend" ? "defend" : bc == "MAFollow" ? "follow" : bc == "MAAttack" ? "attack" : "none"))
            .with("points", Json::number(g ? ai_path_length(s, g) : 0))
            .with("unit_side", u && u->find("Nation") ? Json::number(u->i32("Nation")) : Json())
            .with("unit_ai", u && u->find("m_bAIEnabled") ? Json::number(u->i32("m_bAIEnabled")) : Json());
        ords.push(std::move(j));
    }
    out.with("orders", std::move(ords));
    Json warn = Json::array();
    if (names.label(tr->cls) == "MATriggerDestroyed" && reference_children(w, uid).empty() && !tr->u32("DestroySet"))
        warn.push(Json::string("No units yet, so this fires the moment the mission starts."));
    if (n_acts == 0 && n_ords == 0) warn.push(Json::string("No actions yet: this trigger does nothing when it fires."));
    out.with("warnings", std::move(warn));
    return out.dump();
}

// The `cutscene` read. total / shot lengths come from the same reader the preview and the check use (core/cutscene).
std::string App::automation_cutscene(uint32_t uid) {
    Json out = Json::object();
    if (!doc_) return out.with("error", Json::string("no scenario is open")).dump();
    const Wdf& w = doc_->world;
    static const uint32_t K_CIN = key_of("MACinema"), K_ACT_CIN = key_of("MAActionActivateCinema"), K_TIMER = key_of("MATriggerTimer");
    if (!uid) uid = cine_uid_;
    if (!uid) for (auto& o : w.objs) if (o.cls == K_CIN) { uid = o.uid(); break; }
    const WObj* cin = object_by_uid(w, uid);
    if (!cin || cin->cls != K_CIN) return out.with("error", Json::string("no cutscene" + (uid ? " #" + std::to_string(uid) : std::string()) + " (pass \"uid\", or pick one)")).dump();
    auto pt = [](Vec3 p) { Json a = Json::array(); a.push(Json::number(r2(p.x))); a.push(Json::number(r2(p.y))); a.push(Json::number(r2(p.z))); return a; };
    const std::vector<ShotInfo> shots = cutscene_shots(w, uid);
    out.with("uid", Json::number(uid)).with("name", Json::string(cin->description())).with("total", Json::number(r2(cutscene_total(shots))));
    Json by = Json::array();  // the triggers whose action plays it, and when (a timer's delay; -1 = another kind of trigger)
    for (auto& a : w.objs) {
        if (a.cls != K_ACT_CIN || a.u32("cinemaID") != uid) continue;
        const WObj* tr = object_by_uid(w, a.parent());
        Json r = Json::object();
        r.with("trigger", Json::number(tr ? tr->uid() : 0)).with("at", Json::number(tr && tr->cls == K_TIMER ? r2(tr->f32("TrgTimerDurationSeconds")) : -1.0));
        by.push(std::move(r));
    }
    out.with("played_by", std::move(by));
    Json js = Json::array();
    int n = 0;
    for (auto& sh : shots) {
        Json r = Json::object();
        r.with("n", Json::number(++n)).with("uid", Json::number(sh.ctrl)).with("start", Json::number(r2(sh.start))).with("len", Json::number(r2(sh.len)))
            .with("move_len", Json::number(r2(sh.move_len))).with("still", Json::boolean(sh.still)).with("fov", Json::number(r2(sh.fov)))
            .with("init", Json::number(sh.init)).with("exit", Json::number(sh.exit)).with("eye_path", Json::number(sh.eye_path)).with("focus", Json::number(sh.focus));
        Json eye = Json::array();
        for (auto& p : sh.eye) eye.push(pt(p));
        r.with("eye", std::move(eye));
        r.with("look", sh.has_look ? pt(sh.look) : Json());
        js.push(std::move(r));
    }
    out.with("shots", std::move(js));
    {   // The events (an action or an order at a moment) and what runs when it ends
        auto lane_of = [&](const CutEvent& e) -> const char* {
            const WObj* o = object_by_uid(w, e.obj);
            if (!o) return "Other";
            if (e.order) return "Units";
            const std::string c = gd_.names().label(o->cls);
            if (c == "MAActionSound" || c == "MAActionPlayVoice" || c == "MAActionMusic") return "Sound";
            if (c == "MAActionBroadcast") return "Text";
            if (c == "MAActionShow" || c == "MAActionRemoveEntity" || c == "MAActionAI" || c == "MAActionNation" || c == "MAActionTeleport") return "Units";
            return "Other";
        };
        auto row = [&](const CutEvent& e, bool end) {
            Json r = Json::object();
            const WObj* o = object_by_uid(w, e.obj);
            r.with("uid", Json::number(e.obj)).with("t", Json::number(r2(e.t))).with("shot", Json::number(e.shot + 1)).with("offset", Json::number(r2(e.offset)))
                .with("lane", Json::string(end ? "Other" : lane_of(e))).with("label", Json::string(cut_event_label(e)))
                .with("cls", Json::string(o ? gd_.names().label(o->cls) : std::string())).with("order", Json::boolean(e.order)).with("holder", Json::number(e.holder));
            if (e.order) r.with("unit", Json::number(e.unit));
            return r;
        };
        Json ev = Json::array(), ea = Json::array();
        for (auto& e : cutscene_events(w, gd_.names(), uid)) ev.push(row(e, false));
        for (auto& e : cutscene_end_actions(w, gd_.names(), uid)) ea.push(row(e, true));
        out.with("events", std::move(ev));
        out.with("end_actions", std::move(ea));
        out.with("selected_event", Json::number(tl_.ev));
    }
    if (frame_.on && frame_.cin == uid) {
        Json fr = Json::object();
        const ma2::Vec3 fe = cam_eye_now();
        fr.with("on", Json::boolean(true)).with("ctrl", Json::number(frame_.ctrl)).with("which", Json::number(frame_.which)).with("eye", pt(fe))
            .with("fov_deg", Json::number(r2(cam_.fov * 57.29578f)));
        out.with("framing", std::move(fr));
    } else {
        out.with("framing", Json());
    }
    {   // the labels drawn over the map at each shot's start / end ("2a", "2b"): where to click to pick the marker (screen px of the window)
        Json gl = Json::array();
        for (auto& g : shot_glyphs_) {
            Json r = Json::object();
            r.with("label", Json::string(g.label)).with("shot", Json::number(g.uid)).with("x", Json::number(r2(vp_x + g.sx))).with("y", Json::number(r2(vp_y + g.sy)));
            gl.push(std::move(r));
        }
        out.with("glyphs", std::move(gl));
    }
    Json pv = Json::object();
    pv.with("on", Json::boolean(cpv_.on && cpv_.cin == uid)).with("t", Json::number(r2(cpv_.t))).with("paused", Json::boolean(cpv_.on && cpv_.cin == uid && cpv_.paused));
    out.with("preview", std::move(pv));
    {   // the timeline strip (app_timeline.cpp): where it is on screen, the picked shot and the playhead
        Json st = Json::object(), rc = Json::object();
        rc.with("x", Json::number(tl_.rect[0])).with("y", Json::number(tl_.rect[1])).with("w", Json::number(tl_.rect[2])).with("h", Json::number(tl_.rect[3]));
        st.with("shown", Json::boolean(tl_h_ > 0)).with("folded", Json::boolean(tl_h_ > 0 && tl_h_ < 100 * dpi_)).with("auto_folded", Json::boolean(tl_.auto_folded))
            .with("shot", Json::number(tl_.shot + 1)).with("playhead", Json::number(r2(tl_.playhead))).with("rect", std::move(rc));
        out.with("strip", std::move(st));
    }
    return out.dump();
}

// The Environment tab's values in one small read: the look (and the light alone), the sky and its time, fog, weather,
// lightning, building lights, the sun or moon, whether the sky is in view and whether the amber sky-time line shows.
std::string App::automation_env() {
    Json out = Json::object();
    if (!doc_ || !io_) return out.with("error", Json::string("no scenario is open")).dump();
    Scenario& s = *doc_;
    const int ei = find_class(s.world, K_ENV);
    if (ei < 0) return out.with("error", Json::string("this map has no environment settings")).dump();
    const WObj& env = s.world.objs[size_t(ei)];
    const weather::Look* exact = weather::current_look(*io_, s);
    const weather::Look* light = weather::current_look_light(*io_, s);
    const std::string sky = io_->sky_name(s);
    out.with("look", Json::string(exact ? exact->label : ""));
    out.with("look_light", Json::string(light ? light->label : ""));
    out.with("look_label", Json::string(look_label_now(s)));
    out.with("sky", Json::string(sky)).with("sky_words", Json::string(sky.empty() ? "" : sky_words(sky)));
    bool sky_night = false;
    const bool mm = env_mismatch(s, &sky_night);
    out.with("sky_night", Json::boolean(sky_night)).with("mismatch_line", Json::boolean(mm)).with("light_night", Json::boolean(weather::light_is_night(*io_, s)));
    const float fs = env.f32("FogStart"), fe = env.f32("FogEnd");
    float ds = 0, de = 0;
    const bool have_default = light ? weather::look_fog(*io_, *light, &ds, &de) : weather::own_fog(*io_, s, &ds, &de);
    const bool default_on = have_default && std::fabs(fs - ds) < 1 && std::fabs(fe - de) < 1;
    const char* preset = default_on ? "Default" : fog_preset_label(fs, fe);
    const uint32_t fc = env.u32("FogColor");
    Json col = Json::array();
    for (int sh : {16, 8, 0}) col.push(Json::number((fc >> sh) & 255));
    Json fog = Json::object();
    fog.with("start", Json::number(r2(fs))).with("end", Json::number(r2(fe))).with("colour", std::move(col)).with("preset", Json::string(preset ? preset : ""));
    if (have_default) fog.with("default", Json::object().with("start", Json::number(r2(ds))).with("end", Json::number(r2(de))));
    fog.with("scale", Json::number(r2(fog_scale_)));
    out.with("fog", std::move(fog));
    out.with("precip", Json::string(weather::precip(env) == weather::Precip::Rain ? weather::rain_label(weather::rain_level(env)) : weather::precip_label(weather::precip(env))));
    Json lc = Json::array();
    for (const char* k : {"LightningColorRed", "LightningColorGreen", "LightningColorBlue"}) lc.push(Json::number(env.u32(k, 255)));
    out.with("lightning", Json::object().with("on", Json::boolean(weather::lightning(env))).with("min", Json::number(env.u32("FlashTimeMin", 3000) / 1000.0))
                                         .with("max", Json::number(env.u32("FlashTimeMax", 15000) / 1000.0)).with("colour", std::move(lc)));
    out.with("building_lights", Json::boolean(env.i32("IsNightTime") != 0));
    const Vec3 sun = env.vec3("DirectionalLightDirection");
    float yaw = std::atan2(sun.x, sun.z) * 57.2958f;
    if (yaw < 0) yaw += 360.f;
    out.with("sun", Json::object().with("direction", Json::number(r2(yaw))).with("height", Json::number(r2(std::asin(std::clamp(-sun.y, -1.f, 1.f)) * 57.2958f)))
                                  .with("body", Json::string(weather::light_is_night(*io_, s) ? "Moon" : "Sun")));
    out.with("sky_out_of_view", Json::boolean(cam_.pitch >= 0.5f));
    if (env.find("FogTop") && env.find("FogBottom"))
        out.with("ground_fog", Json::object().with("from", Json::number(r2(env.f32("FogBottom")))).with("up_to", Json::number(r2(env.f32("FogTop")))));
    out.with("helpers_faded", Json::number(helpers_faded_));
    // What the view draws this frame: precipitation sprites (snow flakes / rain particles) and the lightning flash.
    const float px10 = vp_h > 0 && cam_.fov > 0 ? precip_stats_.size / (2.f * 10.f * std::tan(cam_.fov * 0.5f)) * vp_h : 0.f;
    out.with("precip_draw", Json::object().with("sprites", Json::number(double(precip_stats_.sprites))).with("rate", Json::number(r2(precip_stats_.rate)))
                                        .with("life", Json::number(r2(precip_stats_.life))).with("size_m", Json::number(r2(precip_stats_.size))).with("px_at_10m", Json::number(r2(px10))));
    out.with("flash_now", Json::object().with("model", Json::number(r2(r_.params.flash.x))).with("sky", Json::number(r2(r_.params.sky_flash))));
    return out.dump();
}

}  // namespace ed

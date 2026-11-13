// Viewport tools: terrain brushes, object placement, scatter, move/rotate/delete, clipboard.
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "app.h"
#include "../core/triggers.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

void App::apply_brush(const Vec3& c, float dt, bool first) {
    Terrain& t = doc_->terrain;
    float R = brush_radius_, S = brush_strength_;
    int gx0 = std::max(0, int((c.x - R) / t.scale)), gx1 = std::min(t.w - 1, int((c.x + R) / t.scale) + 1);
    int gz0 = std::max(0, int((c.z - R) / t.scale)), gz1 = std::min(t.h - 1, int((c.z + R) / t.scale) + 1);
    static Rng rng(12345);
    // A shaping stroke keeps the half of its pair it started with, whatever the reverse key does after.
    const Tool tool = stroke_tool_;
    const bool square = brush_square_ && sculpt_tool(tool);
    auto weight = [&](int gx, int gz) {
        float dx = gx * t.scale - c.x, dz = gz * t.scale - c.z;
        float d = square ? std::max(std::abs(dx), std::abs(dz)) : std::sqrt(dx * dx + dz * dz);
        if (d >= R) return 0.f;
        float f = 1.f - d / R;
        return f * f * (3 - 2 * f);
    };
    if (sculpt_tool(tool)) {
        // A click (one frame) used to lift the ground ~0.4 m at the default speed: nothing showed, so a first-time user
        // thought the tool did nothing (fresh-eyes #6). The first dab of a stroke makes a visible bump.
        // Fresh-eyes 3 #6: 3 m over a 40 m brush still read as a faint patch; a click now makes 7 m at 50%.
        const float kick = first ? S * (tool == Tool::Roughen ? 6.f : 14.f) : 0.f;
        if (first) { stroke_change_ = 0; stroke_clamped_ = false; roughen_seed_ = uint32_t(rng.next()); }
        std::vector<float> src;
        if (tool == Tool::Smooth) src = t.hgt;
        // Review: things standing on the ground ride the stroke (lowering the ground under a building left it
        // floating, raising buried it). Only what rests on the ground moves (its model's lowest point within 1.5 m
        // of the terrain); a prop on a roof, a flyer or a bridge deck stays. Attached parts move with their unit.
        // Playtest 10-26: one switch (Terrain tab, 'Objects follow the terrain', on by default); off = nothing moves.
        struct Rider { int i; float x, z, h0; };
        std::vector<Rider> riders;
        // The parent tree of the world is built once per stroke (it only changes when objects are added or removed), not every frame.
        static WTree wt_cache;
        static const void* wt_doc = nullptr;
        static size_t wt_n = 0;
        if (settings_.objects_follow_terrain && (first || wt_doc != doc_.get() || wt_n != doc_->world.objs.size())) {
            wt_cache.build(doc_->world);
            wt_doc = doc_.get();
            wt_n = doc_->world.objs.size();
        }
        WTree& wt = wt_cache;
        for (int i = 0; settings_.objects_follow_terrain && i < int(doc_->world.objs.size()); i++) {
            const WObj& o = doc_->world.objs[size_t(i)];
            Vec3 p, q;
            static const uint32_t K_ROAD = key_of("MARoad");
            const bool road = o.cls == K_ROAD;
            if (o.cls == K_WATER || !get_pos(o, p)) continue;
            if ((p.x - c.x) * (p.x - c.x) + (p.z - c.z) * (p.z - c.z) > (R + 80) * (R + 80)) continue;  // cheap pre-check
            if (int pa = wt.parent_of[size_t(i)]; pa >= 0 && get_pos(doc_->world.objs[size_t(pa)], q)) continue;
            const Vec3 fo = footprint_offset(i);
            const float x = p.x + fo.x, z = p.z + fo.z;
            if ((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z) >= R * R) continue;
            const float h0 = t.height_at(x, z);
            // Review BB2A-8: retail objects can sit sunk into the ground (Bogged Down 'Bay_VTOL_Blue' 5 m under);
            // they ride too and keep their depth (anything from 30 m under to 1.5 m over its resting height).
            // BB2A-9: street pieces (MARoad) ride as well, or lowering a city left the streets over a pit.
            const float dy = p.y - (h0 + (o.cls == K_LEVELSTART ? 2.f : 0.f) + (road ? 0.f : model_foot(o)));
            if (dy > (road ? 3.f : 1.5f) || dy < -30.f) continue;
            riders.push_back({i, x, z, h0});
        }
        for (int gz = gz0; gz <= gz1; gz++)
            for (int gx = gx0; gx <= gx1; gx++) {
                float w = weight(gx, gz);
                if (w <= 0) continue;
                float& h = t.ref(gx, gz);
                const float h_in = h;
                switch (tool) {
                    case Tool::Raise: h += (S * 45.f * dt + kick) * w; break;
                    case Tool::Lower: h -= (S * 45.f * dt + kick) * w; break;
                    case Tool::Flatten: h += (flatten_height_ - h) * std::min(1.f, S * 10.f * dt) * w; break;
                    case Tool::Plateau: {
                        float k = w > 0.35f ? 1.f : w / 0.35f;
                        if (h < plateau_height_) h += (plateau_height_ - h) * k;
                        break;
                    }
                    case Tool::PlateauDown: {  // terraces, pits, riverbeds: ground above the height is cut down to it
                        float k = w > 0.35f ? 1.f : w / 0.35f;
                        if (h > plateau_height_) h += (plateau_height_ - h) * k;
                        break;
                    }
                    case Tool::Smooth: {
                        float acc = 0;
                        int n = 0;
                        for (int dz = -1; dz <= 1; dz++)
                            for (int dx = -1; dx <= 1; dx++) {
                                int x = std::clamp(gx + dx, 0, t.w - 1), z = std::clamp(gz + dz, 0, t.h - 1);
                                acc += src[size_t(z) * t.w + x];
                                n++;
                            }
                        h += (acc / n - h) * std::min(1.f, S * 12.f * dt) * w;
                        break;
                    }
                    case Tool::Roughen: {
                        // Fresh random steps each frame averaged out to under 1 m on 8 m cells: a whole stroke showed
                        // nothing (review BB2A-7). Each stroke now keeps one bump pattern and grows it like Raise
                        // (first dab +-2.5 m at 50%, then +-4 m a second while held). Bumps are ~24 m across: one-cell
                        // spikes were too small for the ground lighting to show.
                        auto lat = [&](int x, int z) {
                            uint32_t n = uint32_t(x) * 73856093u ^ uint32_t(z) * 19349663u ^ roughen_seed_;
                            n = (n ^ (n >> 13)) * 0x5bd1e995u;
                            return float((n ^ (n >> 15)) & 0xFFFF) / 65535.f - 0.5f;
                        };
                        int lx = gx / 3, lz = gz / 3;
                        float fx = (gx % 3) / 3.f, fz = (gz % 3) / 3.f;
                        fx = fx * fx * (3 - 2 * fx), fz = fz * fz * (3 - 2 * fz);
                        float r = (lat(lx, lz) * (1 - fx) + lat(lx + 1, lz) * fx) * (1 - fz) +
                                  (lat(lx, lz + 1) * (1 - fx) + lat(lx + 1, lz + 1) * fx) * fz;
                        h += r * (S * 16.f * dt + kick * 1.7f) * w;
                        break;
                    }
                    default: break;
                }
                const float hc = std::clamp(h, t.min_h, t.max_h);
                if (hc != h) stroke_clamped_ = true;
                h = hc;
                // What actually moved, after the height limit: ground already at the map's 100 m ceiling reported
                // "Raised by up to 4 m" while nothing changed (review M01b: the hills around the city sit at 100 m).
                stroke_change_ = std::max(stroke_change_, std::fabs(h - h_in));
            }
        r_.update_terrain_rect(t, gx0, gz0, gx1, gz1);
        auto lift = [&](int i, float dy) {
            std::vector<int> sub;
            wt.subtree(i, sub);
            for (int k : sub) {
                Vec3 q;
                if (get_pos(doc_->world.objs[size_t(k)], q)) set_pos(doc_->world.objs[size_t(k)], {q.x, q.y + dy, q.z});
            }
            objects_dirty_ = true;
        };
        // A joined bridge moves as one (playtest 10-26): every piece of a run any rider belongs to moves by the riders' mean
        // change, so no piece is pulled out of line.
        std::vector<int> run;
        std::unordered_map<int, std::pair<float, int>> run_dy;  // run -> (sum of dy, riders)
        for (const Rider& rd : riders) {
            const float dy = t.height_at(rd.x, rd.z) - rd.h0;
            const TemplateInfo* bt = cat_ ? cat_->find(doc_->world.objs[size_t(rd.i)].template_id()) : nullptr;
            if (bt && snap_family(*bt) == 2) {
                if (run.empty()) run = snap_runs(2);
                if (int r = run[size_t(rd.i)]; r >= 0) { auto& a = run_dy[r]; a.first += dy; a.second++; continue; }
            }
            if (std::fabs(dy) >= 1e-4f) lift(rd.i, dy);
        }
        for (int i = 0; i < int(run.size()); i++)
            if (auto it = run[size_t(i)] >= 0 ? run_dy.find(run[size_t(i)]) : run_dy.end(); it != run_dy.end())
                if (const float dy = it->second.first / float(it->second.second); std::fabs(dy) >= 1e-4f) lift(i, dy);
        doc_->dirty = true;
        lightmap_stale_ = true;
        stale_sun_only_ = false;
        return;
    }
    if (tool_ == Tool::PaintColor) {
        Image& img = doc_->colormap;
        if (img.px.empty()) return;
        doc_->colormap_custom = true;
        float pr = paint_color_[0] * 255, pg = paint_color_[1] * 255, pb = paint_color_[2] * 255;
        float k = std::min(1.f, S * 6.f * dt);
        int px0 = std::max(0, int((c.x - R) / t.size_x() * img.w)), px1 = std::min(img.w - 1, int((c.x + R) / t.size_x() * img.w) + 1);
        int py0 = std::max(0, int((1 - (c.z + R) / t.size_z()) * img.h)), py1 = std::min(img.h - 1, int((1 - (c.z - R) / t.size_z()) * img.h) + 1);
        for (int py = py0; py <= py1; py++)
            for (int px = px0; px <= px1; px++) {
                float x = (px + 0.5f) / img.w * t.size_x(), z = (1 - (py + 0.5f) / img.h) * t.size_z();
                float d = std::sqrt((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z));
                if (d >= R) continue;
                float w = brush_falloff(d, R, brush_hardness_) * k;
                uint32_t& p = img.px[size_t(py) * img.w + px];
                float r = float(p & 255), g = float((p >> 8) & 255), b = float((p >> 16) & 255);
                r += (pr - r) * w; g += (pg - g) * w; b += (pb - b) * w;
                // Alpha is the detail-texture blend mask: keep it.
                p = (p & 0xFF000000u) | uint32_t(std::clamp(int(b), 0, 255)) << 16 | uint32_t(std::clamp(int(g), 0, 255)) << 8 |
                    uint32_t(std::clamp(int(r), 0, 255));
            }
        r_.set_colormap(img);
        doc_->dirty = true;
        return;
    }
    if (tool_ == Tool::PaintTexture) {
        // Texture painting runs 4x the sculpt rate so one pass at the default speed covers the ground (a
        // first-time user saw no change from a whole stroke at 6 x speed).
        constexpr float kTextureRate = 24.f;
        // The colour map's alpha blends the two ground textures (255 = A, 0 = B; verified in game);
        // the colour stays unless "Show it in its own colours" is on (below).
        if (paint_neutral_colour_ && !doc_->colormap.px.empty()) {
            // The game shows ground = 4 x DetailBlend x texture x colour map, so a texture painted over a green
            // colour map still reads green. Move the colour under the brush to the neutral value (1 / (4 x
            // DetailBlend)) so the texture shows in its own colours; the Colour tint tool tints it again.
            Image& cm = doc_->colormap;
            doc_->colormap_custom = true;
            float neutral = std::clamp(1.f / (4.f * std::max(0.05f, r_.detail.blend)), 0.f, 1.f) * 255.f;
            float kk = std::min(1.f, S * kTextureRate * dt);
            int qx0 = std::max(0, int((c.x - R) / t.size_x() * cm.w)), qx1 = std::min(cm.w - 1, int((c.x + R) / t.size_x() * cm.w) + 1);
            int qy0 = std::max(0, int((1 - (c.z + R) / t.size_z()) * cm.h)), qy1 = std::min(cm.h - 1, int((1 - (c.z - R) / t.size_z()) * cm.h) + 1);
            for (int py = qy0; py <= qy1; py++)
                for (int px = qx0; px <= qx1; px++) {
                    float x = (px + 0.5f) / cm.w * t.size_x(), z = (1 - (py + 0.5f) / cm.h) * t.size_z();
                    float d = std::sqrt((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z));
                    if (d >= R) continue;
                    float w = brush_falloff(d, R, brush_hardness_) * kk;
                    uint32_t& p = cm.px[size_t(py) * cm.w + px];
                    float ch[3] = {float(p & 255), float((p >> 8) & 255), float((p >> 16) & 255)};
                    for (float& v : ch) v += (neutral - v) * w;
                    p = (p & 0xFF000000u) | uint32_t(ch[2] + 0.5f) << 16 | uint32_t(ch[1] + 0.5f) << 8 | uint32_t(ch[0] + 0.5f);
                }
            r_.set_colormap(cm);
        }
        if (doc_->ground.enabled && doc_->ground.weights) {
            paint_ground_layers(c.x, c.z, R, std::min(1.f, S * kTextureRate * dt));
            return;
        }
        Image& img = doc_->colormap;
        if (img.px.empty()) return;
        doc_->colormap_custom = true;
        float target = paint_texture_ ? 0.f : 255.f;
        float k = std::min(1.f, S * kTextureRate * dt);
        int px0 = std::max(0, int((c.x - R) / t.size_x() * img.w)), px1 = std::min(img.w - 1, int((c.x + R) / t.size_x() * img.w) + 1);
        int py0 = std::max(0, int((1 - (c.z + R) / t.size_z()) * img.h)), py1 = std::min(img.h - 1, int((1 - (c.z - R) / t.size_z()) * img.h) + 1);
        for (int py = py0; py <= py1; py++)
            for (int px = px0; px <= px1; px++) {
                float x = (px + 0.5f) / img.w * t.size_x(), z = (1 - (py + 0.5f) / img.h) * t.size_z();
                float d = std::sqrt((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z));
                if (d >= R) continue;
                float w = brush_falloff(d, R, brush_hardness_) * k;
                uint32_t& p = img.px[size_t(py) * img.w + px];
                float a = float(p >> 24);
                a += (target - a) * w;
                // Round toward the target so strokes reach 0 / 255.
                int ai = target > a ? int(std::ceil(a)) : int(std::floor(a));
                p = (p & 0x00FFFFFFu) | uint32_t(std::clamp(ai, 0, 255)) << 24;
            }
        r_.set_colormap(img);
        doc_->dirty = true;
        return;
    }
    if (tool_ == Tool::PaintType) {
        if (doc_->typemap.empty()) return;
        int W = doc_->typemap_w, H = doc_->typemap_h;
        uint16_t value = uint16_t(std::clamp(paint_type_value_, 0, 0xFFFF));
        // Stored row 0 is z = 0.
        int x0 = std::max(0, int((c.x - R) / t.size_x() * W)), x1 = std::min(W - 1, int((c.x + R) / t.size_x() * W) + 1);
        int y0 = std::max(0, int((c.z - R) / t.size_z() * H)), y1 = std::min(H - 1, int((c.z + R) / t.size_z() * H) + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float wx = (x + 0.5f) / W * t.size_x(), wz = (y + 0.5f) / H * t.size_z();
                if ((wx - c.x) * (wx - c.x) + (wz - c.z) * (wz - c.z) >= R * R) continue;
                uint16_t& v = doc_->typemap[size_t(y) * W + x];
                v = value;
            }
        doc_->typemap_dirty = true;
        doc_->dirty = true;
        overlay_dirty_ = true;
        show_typemap_ = true;
    }
    if (tool_ == Tool::PaintCulture) {
        // Steps the chosen culture type's level field (0 = none .. top = densest) toward full or none; the
        // game scatters that type's plants by the level (app_culture.cpp). Row 0 is z = 0.
        if (culture_.map.empty() || plant_pick_ < 0 || plant_pick_ >= int(culture_.types.size())) return;
        const CultureType& ct = culture_.types[size_t(plant_pick_)];
        const int top = (1 << ct.bits) - 1, W = culture_.w, H = culture_.h;
        const uint16_t mask = uint16_t(top << ct.shift);
        float k = std::min(1.f, S * 8.f * dt) + (first ? 0.25f : 0.f);
        int x0 = std::max(0, int((c.x - R) / t.size_x() * W)), x1 = std::min(W - 1, int((c.x + R) / t.size_x() * W) + 1);
        int y0 = std::max(0, int((c.z - R) / t.size_z() * H)), y1 = std::min(H - 1, int((c.z + R) / t.size_z() * H) + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float wx = (x + 0.5f) / W * t.size_x(), wz = (y + 0.5f) / H * t.size_z();
                float d = std::sqrt((wx - c.x) * (wx - c.x) + (wz - c.z) * (wz - c.z));
                if (d >= R) continue;
                float f = 1.f - d / R;
                if (rng.unit() >= k * (0.35f + 0.65f * f)) continue;  // one level step per hit, so Speed sets how fast it fills
                uint16_t& v = culture_.map[size_t(y) * W + x];
                int level = (v & mask) >> ct.shift;
                level = plant_erase_ ? std::max(0, level - 1) : std::min(top, level + 1);
                v = uint16_t((v & ~mask) | (level << ct.shift));
            }
        for (auto& ty : culture_.types) ty.inst.clear();  // regenerate the scatter around the eye
        culture_.at[0] = culture_.at[1] = 1e30f;
        doc_->dirty = true;
        overlay_dirty_ = true;
    }
}

void App::place_object(const Vec3& at) {
    if (!place_road_.empty()) {  // a road piece (no template)
        uint32_t u = io_->place_road(*doc_, place_road_, at, place_heading_);
        if (!u) return;
        // Joined to the end of a tilted piece: it starts at that end's height (road_snap), not on the ground.
        if (road_snapped_ && !std::isnan(road_snap_y_))
            if (WObj* ro = object_by_uid(doc_->world, u))
                if (Vec3 q; get_pos(*ro, q)) set_pos(*ro, {q.x, road_snap_y_, q.z});
        road_chain_uid_ = u;  // the next piece joins this one's open end (chain), whichever end is chosen with Q / E
        road_attach_ = 0;
        // Name it after its kind and number ("Long straight (60 m) 3"; the first has no number): every retail piece is just "Road", which
        // tells pieces apart nowhere (object list, inspector).
        if (const RoadPiece* rp = find_road_piece(place_road_)) {
            static const uint32_t K_ROAD_ = key_of("MARoad");
            std::string want = place_road_;
            for (char& ch : want) if (ch == '/') ch = '\\';
            int n = 0;
            WObj* mine = nullptr;
            for (auto& o : doc_->world.objs)
                if (o.cls == K_ROAD_) {
                    if (iequals(o.str("RoadDefinition"), want)) n++;
                    if (o.uid() == u) mine = &o;
                }
            if (mine && mine->find("Description") && !rp->friendly.empty()) {
                std::string nm = rp->friendly;
                nm[0] = char(std::toupper(static_cast<unsigned char>(nm[0])));
                mine->set_str("Description", n > 1 ? nm + " " + std::to_string(n) : nm);  // the first one unnumbered, like "Atlas", "Atlas 2"
            }
        }
        selection_ = {u};
        world_changed();
        return;
    }
    // Game proof (tree 2): buildings added to a level that has none of its own (Mission_08, 06b) never show up in the
    // game; they do on levels that already hold some. Tanks and vehicles show anywhere. Warn instead of staying silent.
    std::string no_class_note;
    if (const TemplateInfo* ti = cat_ ? cat_->find(place_template_) : nullptr)
        if (ti->cls_name == "MABuilding") {
            uint32_t k = key_of("MABuilding");
            bool any = false;
            for (auto& o : doc_->world.objs) if (o.cls == k) { any = true; break; }
            if (!any) no_class_note = "This level has no buildings of its own. In game tests, a building added here did not appear. Try a level that already has some.";
        }
    uint32_t root = io_->place_template(*doc_, place_template_, at, place_heading_);
    if (!root) return;
    if (!no_class_note.empty()) { status_ = no_class_note; status_time_ = 12; }
    // Units take the side chosen on the Units tab ("Placing for") in missions (retail copies are often
    // crewed enemies or MP pickups); in multiplayer maps 'Mechs and vehicles stand empty for players,
    // while turrets and people take the chosen team (Red / Blue team turrets are Nation 1 / 2: the
    // dictionary census of the "- Red" / "- Blue" templates and the game's team -> nation map 0 -> 1, 1 -> 2).
    static const uint32_t unit_classes[] = {key_of("MAMech"), key_of("MATank"), key_of("MAVehicle"), key_of("MAFlyer"),
                                            key_of("MATurret"), key_of("MAPerson")};
    static const uint32_t K_TURRET = key_of("MATurret"), K_PERSON = key_of("MAPerson");
    for (auto& o : doc_->world.objs)
        if (o.uid() == root) {
            bool team_unit = doc_->multiplayer && (o.cls == K_TURRET || o.cls == K_PERSON);
            if (std::find(std::begin(unit_classes), std::end(unit_classes), o.cls) != std::end(unit_classes))
                io_->apply_unit_side(*doc_, root, team_unit ? place_team_ : doc_->multiplayer ? 0 : place_side_);
            // Retail copies are mostly saved AI-off (asleep until a trigger wakes them); a unit the user places for a side
            // in a mission should act on its own, so its AI starts on.
            if (WObj* ro = object_by_uid(doc_->world, root); ro && !doc_->multiplayer && place_side_ != 0 && ro->find("m_bAIEnabled"))
                ro->set_i32("m_bAIEnabled", 1);  // (apply_unit_side may add objects: look the unit up again)
            break;
        }
    // MP pickup templates are named "Owens - Abandoned"; a crewed copy (enemy / player's side) drops the word.
    for (auto& o : doc_->world.objs)
        if (o.uid() == root) {
            std::string d = o.description();
            static const std::string kAb = " - Abandoned";
            if (o.find("StartAbandoned") && !o.i32("StartAbandoned") && d.size() > kAb.size() &&
                d.compare(d.size() - kAb.size(), kAb.size(), kAb) == 0)
                o.set_str("Description", d.substr(0, d.size() - kAb.size()));
            break;
        }
    // A new object is named after its palette entry ("Temporary Autocannon Turret 2"), not after the retail object it was
    // copied from ("1stBase 1 Temporary Autocannon Turret": the source level's group prefix, audit G6).
    if (const TemplateInfo* ti = cat_ ? cat_->find(place_template_) : nullptr; ti && !ti->name.empty())
        if (WObj* ro = object_by_uid(doc_->world, root)) {
            std::set<std::string> used;
            for (auto& o : doc_->world.objs) if (o.uid() != root) used.insert(o.description());
            std::string base = palette_plain_name(*ti);  // "Wide bridge 01", not "Wide bridge 01 (Env_WideBridge01)"
            std::string nm = base;
            for (int n = 2; used.count(nm); n++) nm = base + " " + std::to_string(n);
            ro->set_str("Description", nm);
        }
    {
        WTree t;
        t.build(doc_->world);
        std::vector<int> sub;
        if (int ri = t.find(root); ri >= 0) t.subtree(ri, sub);
        for (int i : sub) {
            WObj& o = doc_->world.objs[size_t(i)];
            // A template carries the set links of the retail object it was cut from (Wide bridge 02: SetMember0 = M03's
            // 'cp 2 buildings', a set that does not list the copy); objects_at_start reads them, so a new object would take
            // that set's start state. A new object belongs to no set.
            if (int n = o.i32("SetCount"); n > 0) {
                for (int k = 0; k < n; k++) o.erase(key_of("SetMember" + std::to_string(k)));
                o.set_i32("SetCount", 0);
            }
        }
    }
    selection_ = {root};
    lift_to_foot(root);  // the model's bottom, not its origin, goes on the ground
    // Clicked on a roof or bridge deck (UX G10): up onto it by the click's height above the ground.
    if (float dy = at.y - doc_->terrain.height_at(at.x, at.z); dy > 0.25f && place_on_top_) {
        WTree t;
        t.build(doc_->world);
        std::vector<int> sub;
        if (int ri = t.find(root); ri >= 0) t.subtree(ri, sub);
        for (int i : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + dy, q.z});
        }
    }
    // A bridge placed on its own stands on its pillars at the clicked spot (bridge_foot_height); a joined one keeps the chain's height.
    bool set_height = piece_snapped_;
    float want_y = snap_origin_y_;
    if (!set_height && bridge_foot_height(at, want_y)) {
        set_height = true;
        status_ = "Bridges stand on their pillars; lower them or build from a hilltop to cross a valley. Hold " + std::string(mod_name(settings_.hold_free)) +
                  " to place the next piece without joining.";
        status_time_ = 8;
    }
    // A foundation joined to another keeps that one's height unless the ground under it is higher (then it rests on that).
    if (set_height && piece_snapped_)
        if (WObj* ro = object_by_uid(doc_->world, root); ro && is_foundation_obj(*ro))
            if (float fy; foundation_rest(*ro, fy)) want_y = std::max(want_y, fy);
    // A wall or bridge piece joined to another (piece_snap) stands at that piece's height, as retail runs do: no steps on a slope.
    if (set_height) {
        Vec3 rp;
        WObj* ro = object_by_uid(doc_->world, root);
        if (ro && get_pos(*ro, rp)) {
            const float dy = want_y - rp.y;
            WTree t;
            t.build(doc_->world);
            std::vector<int> sub;
            if (int ri = t.find(root); ri >= 0) t.subtree(ri, sub);
            for (int i : sub) {
                Vec3 q;
                if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + dy, q.z});
            }
        }
    }
    // A foundation joined to a sheet of others on rising ground rests higher than its neighbour: the sheet would step.
    // The plates it joins (the whole connected sheet) come up to its top so the sheet stays one level (playtest 11-04: no steps
    // between joined plates); a lower plate never pulls the sheet down. Same undo step as the placing.
    if (piece_snapped_)
        if (WObj* ro = object_by_uid(doc_->world, root); ro && is_foundation_obj(*ro)) {
            GpuModel* mg = obj_model(*ro);
            Vec3 mp;
            if (mg && mg->state == GpuModel::Ready && get_pos(*ro, mp)) {
                const float top = mp.y + mg->bmax[1];
                const std::vector<int> run = snap_runs(3);
                int mine = -1;
                for (size_t i = 0; i < doc_->world.objs.size(); i++) if (doc_->world.objs[i].uid() == root) mine = int(i);
                int raised = 0;
                float most = 0;
                // A sheet with a tilted plate in it (a ramp) is shaped by hand: leave its heights alone.
                bool tilted = false;
                if (mine >= 0 && run[size_t(mine)] >= 0)
                    for (size_t i = 0; i < run.size(); i++)
                        if (run[i] == run[size_t(mine)] && (doc_->world.objs[i].f32("CorporalPitch") != 0 || doc_->world.objs[i].f32("CorporalRoll") != 0)) tilted = true;
                if (!tilted && mine >= 0 && run[size_t(mine)] >= 0)
                    for (size_t i = 0; i < run.size(); i++) {
                        if (int(i) == mine || run[i] != run[size_t(mine)]) continue;
                        WObj& o = doc_->world.objs[i];
                        GpuModel* g2 = obj_model(o);
                        Vec3 q;
                        if (!g2 || g2->state != GpuModel::Ready || !get_pos(o, q)) continue;
                        const float dy = top - (q.y + g2->bmax[1]);
                        if (dy <= 0.01f) continue;
                        set_pos(o, {q.x, q.y + dy, q.z});
                        raised++;
                        most = std::max(most, dy);
                    }
                if (raised) {
                    char msg[200];
                    snprintf(msg, sizeof msg, "The ground is higher here: raised %d joined foundation%s (up to %.1f m) so the sheet stays level. Ctrl+Z undoes it.",
                             raised, raised == 1 ? "" : "s", most);
                    status_ = msg;
                    status_time_ = 6;
                }
            }
        }
    // A fixed-size water (several layers, not resizable) is a flat tile whose centre goes where you click. With
    // "Dig a basin" (the default) its surface is that ground and a basin is dug under it once its size is known
    // (next frame, from its model: finish_fixed_water); else it sits 1 m above the ground. The Select tool takes over.
    for (auto& o : doc_->world.objs)
        if (o.uid() == root && o.cls == K_WATER) {
            set_pos(o, {at.x, water_new_dig_ ? at.y : at.y + 1.f, at.z});
            tool_ = Tool::Select;
            if (water_new_dig_) {
                water_fixed_pending_ = root;
                water_fixed_tries_ = 0;
                status_ = "Water placed in a basin dug under it; players wade in it. Ctrl+Z undoes both.";
            } else
                status_ = "Water placed: its surface is 1 m above where you clicked. Drag the green arrow to raise or lower it.";
            status_time_ = 8;
        }
    world_changed();
}

void App::scatter(const Vec3& c) {
    if (scatter_set_.empty()) return;
    static Rng rng(777);
    float a = rng.unit() * 6.2831853f, r = std::sqrt(rng.unit()) * brush_radius_;
    Vec3 p{c.x + std::cos(a) * r, 0, c.z + std::sin(a) * r};
    if (!doc_->terrain.inside(p.x, p.z)) return;
    // Keep some spacing from other trees placed by this brush.
    for (auto& v : views_) {
        float dx = v.pos.x - p.x, dz = v.pos.z - p.z;
        if (dx * dx + dz * dz < 25.f) return;
    }
    p.y = doc_->terrain.height_at(p.x, p.z);
    uint32_t keep = place_template_;
    float keep_h = place_heading_;
    place_template_ = scatter_set_[size_t(rng.next() % scatter_set_.size())];
    place_heading_ = rng.unit() * 6.2831853f;
    place_object(p);
    place_template_ = keep;
    place_heading_ = keep_h;
    // place_object rebuilt nothing yet; add a temporary view so spacing works within one stroke.
    ObjView v;
    v.pos = p;
    views_.push_back(v);
}

void App::move_selection(const Vec3& d, bool snap) {
    WTree t;
    t.build(doc_->world);
    // Glued pieces (app_glue.cpp) move with the selection as one rigid body: following the ground piece by piece would
    // pull their joints apart.
    const std::vector<uint32_t> movers = glue_group(selection_);
    if (movers.size() > selection_.size()) snap = false;
    else for (uint32_t u : selection_) if (const WObj* o = object_by_uid(doc_->world, u); o && o->find(kGlueProp)) snap = false;
    for (uint32_t u : movers) {
        int ri = t.find(u);
        if (ri < 0) continue;
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int i : sub) {
            WObj& o = doc_->world.objs[size_t(i)];
            Vec3 p;
            if (!get_pos(o, p)) continue;
            float above = p.y - doc_->terrain.height_at(p.x, p.z);
            Vec3 q{p.x + d.x, p.y + d.y, p.z + d.z};
            if (snap) q.y = doc_->terrain.height_at(q.x, q.z) + above;
            set_pos(o, q);
        }
    }
    doc_->dirty = true;
    objects_dirty_ = true;
}

void App::rotate_selection(float delta) {
    if (selection_.empty()) return;
    push_undo("rotate");
    if (Vec3 gp; gizmo_pivot(gp) && glue_turn(0, delta, gp)) return;  // one glued piece: about its glued joint, the rest following (app_glue.cpp)
    WTree t;
    t.build(doc_->world);
    for (uint32_t u : selection_) {
        int ri = t.find(u);
        if (ri < 0) continue;
        WObj& root = doc_->world.objs[size_t(ri)];
        Vec3 c;
        if (!get_pos(root, c)) continue;
        // Turn about the middle of the footprint, where the gizmo sits (R9: a road piece's origin is its
        // start end, so turning about it swung the piece away), and the attached parts with it.
        const Vec3 off = footprint_offset(ri);
        const Vec3 pv{c.x + off.x, c.y, c.z + off.z};
        float h = world::heading(root) + delta;
        world::set_heading(root, h);
        std::vector<int> sub;
        t.subtree(ri, sub);
        float cs = std::cos(delta), sn = std::sin(delta);
        for (int i : sub) {
            WObj& o = doc_->world.objs[size_t(i)];
            Vec3 p;
            if (!get_pos(o, p)) continue;
            float dx = p.x - pv.x, dz = p.z - pv.z;
            if (i != ri || off.x != 0 || off.z != 0) set_pos(o, {pv.x + cs * dx + sn * dz, p.y, pv.z - sn * dx + cs * dz});
            if (i != ri && o.find("CorporalHeading")) world::set_heading(o, world::heading(o) + delta);
        }
    }
    doc_->dirty = true;
    objects_dirty_ = true;
}

void App::delete_selection(const char* undo_label) {
    if (selection_.empty() || !editable()) return;
    push_undo(undo_label);
    // What the delete takes away that the game or the triggers rely on, to warn about it.
    std::set<uint32_t> gone;
    int mode_pieces = 0;
    {
        static const uint32_t K_OBJ = key_of("MAObjectiveType"), K_FLAGC = key_of("MAFlagCTF"), K_SB = key_of("MASnatchItBase"),
                              K_SC = key_of("MASnatchItDataCore");
        WTree t;
        t.build(doc_->world);
        for (uint32_t u : selection_) {
            int i = t.find(u);
            if (i < 0) continue;
            std::vector<int> sub;
            t.subtree(i, sub);
            for (int k : sub) {
                const WObj& o = doc_->world.objs[size_t(k)];
                gone.insert(o.uid());
                mode_pieces += o.cls == K_OBJ || o.cls == K_FLAGC || o.cls == K_SB || o.cls == K_SC;
            }
        }
    }
    int trigger_refs = 0;
    for (auto& tr : doc_->triggers) {
        auto refs = [&](const std::map<std::string, std::string>& args) {
            for (auto& [k, v] : args)
                if (k == "unit" || k == "units" || k == "target")
                    for (auto& x : split(v, ',')) if (!trim(x).empty() && gone.count(uint32_t(strtoul(x.c_str(), nullptr, 10)))) trigger_refs++;
        };
        for (auto& c : tr.conditions) refs(c.args);
        for (auto& e : tr.effects) refs(e.args);
    }
    for (uint32_t u : selection_) wedit::remove_subtree(doc_->world, u);
    // References to deleted objects (set members, kill references) are dropped too.
    std::unordered_set<uint32_t> uids;
    for (auto& o : doc_->world.objs) uids.insert(o.uid());
    std::vector<uint32_t> dead;
    static const uint32_t K_EREF = key_of("MoeEntityReference");
    for (auto& o : doc_->world.objs)
        if (o.cls == K_EREF && !uids.count(o.u32("ReferenceUID"))) dead.push_back(o.uid());
    for (uint32_t u : dead) wedit::remove_subtree(doc_->world, u);
    // Road pieces the editor placed live in a road group under a "Placed Roads" group (ScenarioIO::place_road):
    // once the last piece is gone those two are empty containers the user never placed, so they go too (UX R8).
    {
        static const uint32_t K_RGROUP = key_of("MARoadGroup"), K_GROUP = key_of("MAGroup");
        for (int pass = 0; pass < 2; pass++) {
            WTree t;
            t.build(doc_->world);
            std::vector<uint32_t> empty;
            for (size_t i = 0; i < doc_->world.objs.size(); i++) {
                const WObj& o = doc_->world.objs[i];
                if (!t.kids[i].empty()) continue;
                int p = t.parent_of[i];
                bool placed_rg = o.cls == K_RGROUP && p >= 0 && doc_->world.objs[size_t(p)].description() == "Placed Roads";
                bool placed_g = o.cls == K_GROUP && o.description() == "Placed Roads";
                if (placed_rg || placed_g) empty.push_back(o.uid());
            }
            for (uint32_t u : empty) wedit::remove_subtree(doc_->world, u);
        }
    }
    drop_dead_set_members();
    selection_.clear();
    world_changed();
    if (mode_pieces || trigger_refs) {
        std::string w = "You deleted ";
        if (mode_pieces) w += "an object a game type needs (a power plant, flag or Snatch It piece)";
        if (mode_pieces && trigger_refs) w += ", and ";
        if (trigger_refs) w += "a unit that " + std::to_string(trigger_refs) + " trigger setting(s) use";
        w += ". Press Ctrl+Z to undo it, or see Check scenario for what to fix.";
        message("Deleted something important", w);
    } else {
        status_ = "Deleted. Ctrl+Z undoes it.";
        status_time_ = 4;
    }
}

// Ctrl+X: the clipboard gets the selection, then it is deleted (one undo step, "Undo cut").
void App::cut_selection() {
    if (!doc_ || selection_.empty() || !editable()) return;
    copy_selection();
    if (clipboard_.empty()) return;
    size_t n = clipboard_.size();
    delete_selection("cut");
    if (status_time_ > 0 && status_.rfind("Deleted", 0) == 0) {
        status_ = std::to_string(n) + " object(s) cut. Ctrl+V pastes them under the mouse, Ctrl+Z puts them back.";
        status_time_ = 4;
    }
}

void App::copy_selection() {
    if (!doc_ || selection_.empty()) return;
    clipboard_.clear();
    std::unordered_set<uint32_t> keep = cat_->ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t tmp = 0x40000000u;  // temporary ids, renumbered on paste
    for (uint32_t u : selection_) {
        Clip c;
        c.objs = wedit::clone_subtree(doc_->world, u, tmp, &keep);
        if (c.objs.empty() || !get_pos(c.objs.front(), c.pos)) continue;
        clipboard_.push_back(std::move(c));
    }
    status_ = std::to_string(clipboard_.size()) + " object(s) copied";
    status_time_ = 3;
}

void App::paste(const Vec3& at) {
    if (clipboard_.empty() || !editable()) return;
    push_undo("paste");
    Vec3 centre{0, 0, 0};
    for (auto& c : clipboard_) { centre.x += c.pos.x; centre.z += c.pos.z; }
    centre.x /= float(clipboard_.size());
    centre.z /= float(clipboard_.size());
    uint32_t next = next_uid(doc_->world);
    uint32_t grp = ensure_group(doc_->world, terrain_uid(doc_->world), "Placed Objects", next);
    selection_.clear();
    for (auto& c : clipboard_) {
        Wdf tmp;
        tmp.objs = c.objs;
        uint32_t root = io_->clone_from_world(*doc_, tmp, c.objs.front().uid(), grp);
        if (!root) continue;
        ScenarioIO::move_subtree_to(*doc_, root, {c.pos.x - centre.x + at.x, 0, c.pos.z - centre.z + at.z}, 0, false, nullptr);
        selection_.insert(root);
    }
    world_changed();
    status_ = "Pasted " + std::to_string(selection_.size()) + (selection_.size() == 1 ? " object" : " objects") + ". Ctrl+Z undoes it.";
    status_time_ = 4;
}

// Ctrl+A: every object drawn on the map (the View filters and helper switches decide what that is).
void App::select_all() {
    selection_.clear();
    for (auto& v : views_)
        if (v.index == v.root) selection_.insert(v.uid);
    status_ = std::to_string(selection_.size()) + " objects selected. Esc clears the selection.";
    status_time_ = 4;
}

// How far the object's origin sits above the model's lowest point: placing or snapping to the ground puts the
// model's bottom on the terrain (buildings and bridges whose origin is mid-model otherwise sink in).
void App::lift_to_foot(uint32_t root) {
    WTree wt;
    wt.build(doc_->world);
    int ri = wt.find(root);
    if (ri < 0) return;
    const WObj& r = doc_->world.objs[size_t(ri)];
    Vec3 p;
    if (!get_pos(r, p) || !has_ground_model(r)) return;
    // A placed copy keeps its source level's height above its own ground (cliffs and tree groups sunk on purpose,
    // aircraft in the air); a new object goes on the ground: its model's lowest point at the terrain height.
    float dy = doc_->terrain.height_at(p.x, p.z) + model_foot(r) - p.y;
    // A foundation goes a little above the highest ground under it (playtest 11-01: plates clipped through flat ground).
    if (float fy; foundation_rest(r, fy)) dy = fy - p.y;
    if (std::fabs(dy) < 0.001f) return;
    std::vector<int> sub;
    wt.subtree(ri, sub);
    for (int i : sub) {
        Vec3 q;
        if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + dy, q.z});
    }
}

bool App::has_ground_model(const ma2::WObj& o) {
    if (o.cls == K_WATER || o.cls == K_LEVELSTART || o.cls == key_of("MARoad")) return false;
    std::string mp = o.str("Corporal3DObj");
    if (mp.empty()) return false;
    GpuModel* gm = obj_model(o);
    if (gm && gm->state == GpuModel::Pending) models_.pump(5000);
    return gm && gm->state == GpuModel::Ready;
}

float App::model_foot(const ma2::WObj& o) {
    if (!has_ground_model(o)) return 0;
    return -obj_model(o)->bmin[1];
}

void App::snap_selection_to_ground() {
    if (selection_.empty()) return;
    push_undo("snap to ground");
    WTree t;
    t.build(doc_->world);
    for (uint32_t u : selection_) {
        int ri = t.find(u);
        if (ri < 0) continue;
        WObj& o = doc_->world.objs[size_t(ri)];
        Vec3 p;
        if (!get_pos(o, p)) continue;
        float g = doc_->terrain.height_at(p.x, p.z) + (o.cls == K_LEVELSTART ? 2.f : 0.f) + model_foot(o);
        if (o.cls == key_of("MARoad") && io_) g = io_->road_rest_height(*doc_, o.str("RoadDefinition"), p, heading(o));
        if (float fy; foundation_rest(o, fy)) g = fy;  // a foundation: just above the highest ground under it
        move_selection({0, 0, 0}, false);
        float dy = g - p.y;
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int i : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + dy, q.z});
        }
    }
    world_changed();
}

}  // namespace ed

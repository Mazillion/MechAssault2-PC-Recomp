// Viewport tools: terrain brushes, object placement, scatter, move/rotate/delete, clipboard.
#include <algorithm>
#include <cmath>
#include <cstring>
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
    if (sculpt_tool(tool)) {
        // Raise / Lower / Roughen are one pass per stroke. Each grid point moves by at most the stroke's amount x the biggest brush
        // weight it has felt, worked out from the ground at the stroke's start (stroke_h0_): holding still, going back and forth and a
        // fast or slow drag all give the same bank, and a click is a dab of the full amount.
        if (first) {
            stroke_change_ = 0;
            stroke_clamped_ = false;
            roughen_seed_ = uint32_t(rng.next());
            stroke_h0_ = t.hgt;
            stroke_cap_.reset(t.hgt.size());
            stroke_top_ = -1e9f;
            stroke_bot_ = 1e9f;
            stroke_stepped_ = false;
            // What stands on the ground (and on flat pieces) rides the stroke: taken once, when the stroke begins, and carried by
            // the change of the surface under each object after every frame that moved a step, so no frame can lift anything twice.
            // Playtest 10-26: one switch (Terrain tab, 'Objects follow the ground', on by default); off = nothing moves.
            stroke_standing_ = settings_.objects_follow_terrain ? ground_standing() : std::vector<Standing>();
        }
        const float amount = ma2::cap_amount(settings_.raise_amount, settings_.build_up, stroke_held_);
        std::vector<float> src;
        if (tool == Tool::Smooth) src = t.hgt;
        for (int gz = gz0; gz <= gz1; gz++)
            for (int gx = gx0; gx <= gx1; gx++) {
                const float w = ma2::sculpt_falloff(gx * t.scale - c.x, gz * t.scale - c.z, R, brush_hardness_, square);
                if (w <= 0) continue;
                const size_t i = size_t(gz) * t.w + gx;
                float& h = t.hgt[i];
                const float h_in = h;
                switch (tool) {
                    case Tool::Raise: h = ma2::stroke_height(stroke_h0_[i], 1.f, amount, stroke_cap_.touch(i, w)); break;
                    case Tool::Lower: h = ma2::stroke_height(stroke_h0_[i], -1.f, amount, stroke_cap_.touch(i, w)); break;
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
                        // One bump pattern per stroke (bumps ~24 m across: one-cell spikes were too small for the ground lighting to show),
                        // the lattice noise -0.5..0.5 x 2 times the stroke's amount.
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
                        h = ma2::stroke_height(stroke_h0_[i], r * 2.f, amount, stroke_cap_.touch(i, w));
                        break;
                    }
                    default: break;
                }
                const float hc = std::clamp(h, t.min_h, t.max_h);
                if (hc != h) stroke_clamped_ = true;
                h = hc;
                // What actually moved, after the height limit: ground already at the map's 100 m ceiling reported
                // "Raised by up to 4 m" while nothing changed (review M01b: the hills around the city sit at 100 m).
                // Measured on the drawn ground (the saved steps): the fine layer under it moves in smaller amounts than the game can show.
                stroke_change_ = std::max(stroke_change_, std::fabs(t.stored(h) - t.stored(stroke_h0_[i])));
                stroke_top_ = std::max(stroke_top_, t.stored(h));
                stroke_bot_ = std::min(stroke_bot_, t.stored(h));
                stroke_stepped_ |= t.stored(h) != t.stored(h_in);
            }
        sculpt_rect_[0] = std::min(sculpt_rect_[0], gx0);
        sculpt_rect_[1] = std::min(sculpt_rect_[1], gz0);
        sculpt_rect_[2] = std::max(sculpt_rect_[2], gx1);
        sculpt_rect_[3] = std::max(sculpt_rect_[3], gz1);
        sculpt_pending_ = true;
        return;
    }
    // Painting by the stroke: like the sculpt brushes, one stroke is one pass. Each texel moves from what it was
    // when the stroke began toward the target by strength x the biggest brush weight it has felt, so holding still adds nothing, a click is
    // a visible dab, and 100% paints it fully at the centre. The touched rectangle goes to the card once per frame (flush_paint).
    if (tool_ == Tool::PaintColor || tool_ == Tool::PaintTexture) {
        Image& cm = doc_->colormap;
        const bool tex = tool_ == Tool::PaintTexture;
        const bool layers = tex && doc_->ground.enabled && doc_->ground.weights && doc_->ground.count() >= 3;
        if (first || paint_base_.size() != cm.px.size()) {
            paint_base_ = cm.px;
            paint_cap_.reset(cm.px.size());
        }
        if (layers && (first || !paint_base_w_ || paint_wcap_.w.size() != size_t(doc_->ground.w) * size_t(doc_->ground.h))) {
            paint_base_w_ = doc_->ground.weights;   // shared with the undo step, so the first write below clones: this one stays as it was
            paint_wcap_.reset(size_t(doc_->ground.w) * size_t(doc_->ground.h));
        }
        const float strength = brush_strength_;
        if (!cm.px.empty()) {
            doc_->colormap_custom = true;
            // The game shows ground = 4 x DetailBlend x texture x colour map, so a texture painted over a green colour map still reads green.
            // Move the colour under the brush to the neutral value (1 / (4 x DetailBlend)) so the texture shows in its own colours; the Colour
            // tint tool tints it again. Rubbing a texture out leaves the colours alone.
            const bool neutral_on = tex && paint_neutral_colour_ && !(layers && stroke_rev_);
            const float neutral = std::clamp(1.f / (4.f * std::max(0.05f, r_.detail.blend)), 0.f, 1.f) * 255.f;
            // The colour map's alpha blends the two ground textures (255 = A, 0 = B; verified in game), unless layers carry the blend.
            const float alpha_to = (paint_texture_ == 0) != stroke_rev_ ? 255.f : 0.f;
            const float tint[3] = {paint_color_[0] * 255.f, paint_color_[1] * 255.f, paint_color_[2] * 255.f};
            const int qx0 = std::max(0, int((c.x - R) / t.size_x() * cm.w)), qx1 = std::min(cm.w - 1, int((c.x + R) / t.size_x() * cm.w) + 1);
            const int qy0 = std::max(0, int((1 - (c.z + R) / t.size_z()) * cm.h)), qy1 = std::min(cm.h - 1, int((1 - (c.z - R) / t.size_z()) * cm.h) + 1);
            for (int py = qy0; py <= qy1; py++)
                for (int px = qx0; px <= qx1; px++) {
                    const float x = (px + 0.5f) / cm.w * t.size_x(), z = (1 - (py + 0.5f) / cm.h) * t.size_z();
                    const float d = std::sqrt((x - c.x) * (x - c.x) + (z - c.z) * (z - c.z));
                    if (d >= R) continue;
                    const size_t i = size_t(py) * cm.w + px;
                    const float wm = paint_cap_.touch(i, brush_falloff(d, R, brush_hardness_));
                    const uint32_t bs = paint_base_[i];
                    uint8_t ch[4] = {uint8_t(bs & 255), uint8_t((bs >> 8) & 255), uint8_t((bs >> 16) & 255), uint8_t(bs >> 24)};
                    if (!tex) {
                        for (int k = 0; k < 3; k++) ch[k] = ma2::paint_value(ch[k], tint[k], strength, wm);   // alpha is the detail blend mask: kept
                    } else {
                        if (neutral_on)
                            for (int k = 0; k < 3; k++) ch[k] = ma2::paint_value(ch[k], neutral, strength, wm);
                        if (!layers) ch[3] = ma2::paint_value(ch[3], alpha_to, strength, wm);
                    }
                    cm.px[i] = uint32_t(ch[3]) << 24 | uint32_t(ch[2]) << 16 | uint32_t(ch[1]) << 8 | uint32_t(ch[0]);
                }
            paint_rect_[0] = std::min(paint_rect_[0], qx0);
            paint_rect_[1] = std::min(paint_rect_[1], qy0);
            paint_rect_[2] = std::max(paint_rect_[2], qx1);
            paint_rect_[3] = std::max(paint_rect_[3], qy1);
            paint_pending_ = true;
        }
        if (layers) {
            // The texture brush on the weight map: the chosen layer rises toward 255 under the brush and the others fall in proportion; with
            // the reverse key it is rubbed out instead (its weight goes to the others in proportion). Earlier painting stays where the brush
            // did not go.
            GroundLayers& g = doc_->ground;
            const int layer = std::clamp(paint_texture_, 0, g.count() - 1);
            const uint8_t* bw = paint_base_w_->data();
            uint8_t* dw = g.mutable_weights();
            const int wx0 = std::max(0, int((c.x - R) / t.size_x() * g.w)), wx1 = std::min(g.w - 1, int((c.x + R) / t.size_x() * g.w) + 1);
            const int wy0 = std::max(0, int((1 - (c.z + R) / t.size_z()) * g.h)), wy1 = std::min(g.h - 1, int((1 - (c.z - R) / t.size_z()) * g.h) + 1);
            for (int y = wy0; y <= wy1; y++)
                for (int x = wx0; x <= wx1; x++) {
                    const float wx = (x + 0.5f) / g.w * t.size_x(), wz = (1 - (y + 0.5f) / g.h) * t.size_z();
                    const float dist = std::sqrt((wx - c.x) * (wx - c.x) + (wz - c.z) * (wz - c.z));
                    if (dist >= R) continue;
                    const size_t i = size_t(y) * g.w + x;
                    const float a = strength * paint_wcap_.touch(i, brush_falloff(dist, R, brush_hardness_));
                    if (stroke_rev_) {
                        std::memcpy(dw + i * 8, bw + i * 8, 8);
                        ma2::rub_out(dw + i * 8, layer, a);
                    } else
                        ma2::paint_layer_texel(bw + i * 8, dw + i * 8, layer, a);
                }
            paint_wrect_[0] = std::min(paint_wrect_[0], wx0);
            paint_wrect_[1] = std::min(paint_wrect_[1], wy0);
            paint_wrect_[2] = std::max(paint_wrect_[2], wx1);
            paint_wrect_[3] = std::max(paint_wrect_[3], wy1);
            paint_w_pending_ = true;
        }
        doc_->dirty = true;
        return;
    }
    if (tool_ == Tool::PaintType) {
        if (doc_->typemap.empty()) return;
        int W = doc_->typemap_w, H = doc_->typemap_h;
        // With the reverse key the brush paints Playable (value 0) over what is marked.
        uint16_t value = stroke_rev_ ? uint16_t(0) : uint16_t(std::clamp(paint_type_value_, 0, 0xFFFF));
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
        // game scatters that type's plants by the level (app_culture.cpp). Row 0 is z = 0. The reverse key flips Grow / Clear for the stroke.
        if (culture_.map.empty() || plant_pick_ < 0 || plant_pick_ >= int(culture_.types.size())) return;
        const CultureType& ct = culture_.types[size_t(plant_pick_)];
        const int top = (1 << ct.bits) - 1, W = culture_.w, H = culture_.h;
        const uint16_t mask = uint16_t(top << ct.shift);
        const bool erase = plant_erase_ != stroke_rev_;
        float k = std::min(1.f, S * 8.f * dt);
        int x0 = std::max(0, int((c.x - R) / t.size_x() * W)), x1 = std::min(W - 1, int((c.x + R) / t.size_x() * W) + 1);
        int y0 = std::max(0, int((c.z - R) / t.size_z() * H)), y1 = std::min(H - 1, int((c.z + R) / t.size_z() * H) + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float wx = (x + 0.5f) / W * t.size_x(), wz = (y + 0.5f) / H * t.size_z();
                float d = std::sqrt((wx - c.x) * (wx - c.x) + (wz - c.z) * (wz - c.z));
                if (d >= R) continue;
                float f = 1.f - d / R;
                // The first frame always applies one level step under the inner half of the ring, so a click shows; elsewhere the random
                // level steps are how the culture map works (one level step per hit, so Strength sets how fast it fills).
                const bool sure = first && d < 0.5f * R;
                if (!sure && rng.unit() >= k * (0.35f + 0.65f * f)) continue;
                uint16_t& v = culture_.map[size_t(y) * W + x];
                int level = (v & mask) >> ct.shift;
                level = erase ? std::max(0, level - 1) : std::min(top, level + 1);
                v = uint16_t((v & ~mask) | (level << ct.shift));
            }
        for (auto& ty : culture_.types) ty.inst.clear();  // regenerate the scatter around the eye
        culture_.at[0] = culture_.at[1] = 1e30f;
        doc_->dirty = true;
        overlay_dirty_ = true;
    }
}

// The painted rectangle of this frame (the colour map and the ground-layer weights) goes to the card, once, whatever the number of sub-steps.
void App::flush_paint() {
    if (!doc_) return;
    if (paint_pending_) {
        paint_pending_ = false;
        if (paint_full_upload_) r_.set_colormap(doc_->colormap);
        else r_.update_colormap_rect(doc_->colormap, paint_rect_[0], paint_rect_[1], paint_rect_[2], paint_rect_[3]);
        paint_rect_[0] = paint_rect_[1] = 1 << 30;
        paint_rect_[2] = paint_rect_[3] = -1;
    }
    if (paint_w_pending_) {
        paint_w_pending_ = false;
        if (paint_full_upload_) r_.set_ground_weights(doc_->ground);
        else r_.update_ground_weights_rect(doc_->ground, paint_wrect_[0], paint_wrect_[1], paint_wrect_[2], paint_wrect_[3]);
        paint_wrect_[0] = paint_wrect_[1] = 1 << 30;
        paint_wrect_[2] = paint_wrect_[3] = -1;
    }
}

// After the brush has been applied (once or in several sub-steps) this frame: show the touched ground, carry what stands on it, and mark the
// light map stale. Once per frame, whatever the number of sub-steps.
void App::flush_brush() {
    flush_paint();
    if (!sculpt_pending_ || !doc_) return;
    sculpt_pending_ = false;
    Terrain& t = doc_->terrain;
    r_.update_terrain_rect(t, sculpt_rect_[0], sculpt_rect_[1], sculpt_rect_[2], sculpt_rect_[3]);
    sculpt_rect_[0] = sculpt_rect_[1] = 1 << 30;
    sculpt_rect_[2] = sculpt_rect_[3] = -1;
    if (stroke_stepped_ && settings_.objects_follow_terrain) ground_changed(stroke_standing_);
    stroke_stepped_ = false;
    doc_->dirty = true;
    lightmap_stale_ = true;
    stale_sun_only_ = false;
}

// The brush from a to b: one dab every max(0.5, R / 8) m, the frame's time shared between them (Smooth and Flatten work by time), so
// neither drag speed nor frame rate leaves gaps or changes the result.
void App::brush_path(const Vec3& a, const Vec3& b, float dt) {
    const int n = ma2::substeps(a.x, a.z, b.x, b.z, brush_radius_);
    for (int k = 1; k <= n; k++) {
        const float f = float(k) / float(n);
        Vec3 p{a.x + (b.x - a.x) * f, 0, a.z + (b.z - a.z) * f};
        p.y = doc_->terrain.height_at(p.x, p.z);
        apply_brush(p, dt / float(n), false);
    }
}

void App::place_object(const Vec3& at) {
    // Sloping ground under a flat piece (app_slope.cpp): level the ground first (same undo step as the placing), or tilt the piece after.
    const SlopePlan sp = plan_slope(at);
    if (sp.active && sp.mode == SlopeMode::Level) apply_slope_ground(sp);
    const int slope_moved = slope_moved_;
    if (!place_road_.empty()) {  // a road piece (no template)
        uint32_t u = io_->place_road(*doc_, place_road_, at, place_heading_);
        if (!u) return;
        // Joined to the end of a tilted piece: it starts at that end's height (road_snap), not on the ground.
        bool pinned = road_snapped_ && !std::isnan(road_snap_y_);
        if (pinned)
            if (WObj* ro = object_by_uid(doc_->world, u))
                if (Vec3 q; get_pos(*ro, q)) set_pos(*ro, {q.x, road_snap_y_, q.z});
        if (sp.active && sp.mode == SlopeMode::Tilt) { apply_slope_pose(u, sp); pinned = true; }
        else if (sp.active && sp.mode == SlopeMode::Level && sp.pin_y && !(sp.joined && road_joint_ok_)) { lower_to_plate(u, sp.y); pinned = true; }  // 
        else if (sp.active && (sp.mode == SlopeMode::Level || sp.mode == SlopeMode::Float) && sp.joined && road_joint_ok_)  // levelled ground or floating, joined: the joint keeps its height
            if (WObj* ro = object_by_uid(doc_->world, u))
                if (Vec3 q; get_pos(*ro, q)) { set_pos(*ro, {q.x, road_joint_y_, q.z}); pinned = true; }
        sheet_dropped(u, pinned);  // the last piece placed goes on top of what it overlaps (app_sheets.cpp)
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
        SlopePlan sps = sp;
        if (sp.active && sp.mode == SlopeMode::Tilt && sp.fits_ground) fit_ground_under_placed(u, sps);  // lands on the click: no float left to fix
        if (sps.active) { status_ = slope_status_text_ = slope_status(sps, slope_moved); status_time_ = 8; }
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
            if (!any) {   // A level whose theme carries building templates (its culled set + the common set) has buildings of its own
                std::string key = lower(doc_->base);
                if (iends_with(key, ".mgf")) key.resize(key.size() - 4);
                std::set<uint32_t> have(cat_->culled_set(key).begin(), cat_->culled_set(key).end());
                for (uint32_t u : cat_->common_set(doc_->multiplayer)) have.insert(u);
                for (uint32_t u : have)
                    if (const TemplateInfo* bt = cat_->find(u); bt && bt->cls_name == "MABuilding" && !bt->model.empty() && !is_foundation(*bt)) { any = true; break; }
            }
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
    // copied from ("1stBase 1 Temporary Autocannon Turret": the source level's group prefix).
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
    // Clicked on a roof or bridge deck: up onto it by the click's height above the ground.
    if (const Vec3 po0 = place_origin_offset(); at.y - surface_at(at.x - po0.x, at.z - po0.z) > 0.25f && place_on_top_) {
        const float dy = at.y - surface_at(at.x - po0.x, at.z - po0.z);
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
    // A thin foundation is a sheet (app_sheets.cpp): the last one placed goes on top of what it overlaps; one joined edge to edge
    // takes its run's layer and the run is seated level (playtest 11-04: no steps between joined plates), so on rising ground the plates it
    // joins come up with it. Same undo step as the placing.
    if (WObj* ro = object_by_uid(doc_->world, root); ro && sheet_kind(*ro) == 2) {
        const bool tilted_now = sp.active && sp.mode == SlopeMode::Tilt;
        const bool plate_pinned = sp.active && sp.mode == SlopeMode::Level && sp.pin_y;  // Rests on the levelled plate, not on the ground left alone beside it
        if (tilted_now) apply_slope_pose(root, sp);
        if (plate_pinned) lower_to_plate(root, sp.y);
        sheet_dropped(root, tilted_now || plate_pinned);
        if (sp.active) { status_ = slope_status_text_ = slope_status(sp, slope_moved); status_time_ = 8; }
        if (piece_snapped_) {
            int raised = 0;
            float most = 0;
            for (auto& [uid, dy] : reseat_moves_)
                if (uid != root && dy > 0.01f) { raised++; most = std::max(most, dy); }
            if (raised) {
                char msg[200];
                snprintf(msg, sizeof msg, "The ground is higher here: raised %d joined foundation%s (up to %.1f m) so the sheet stays level. %s undoes it.",
                         raised, raised == 1 ? "" : "s", most, kt(A_UNDO).c_str());
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
            set_pos(o, {at.x, water_new_dig_ ? at.y - kShoreDrop : at.y + 1.f, at.z});
            tool_ = Tool::Select;
            if (water_new_dig_) {
                water_fixed_pending_ = root;
                water_fixed_tries_ = 0;
                status_ = "Water placed in a basin dug under it; players wade in it. " + kt(A_UNDO) + " undoes both.";
            } else
                status_ = "Water placed: its surface is 1 m above where you clicked. Drag the green arrow to raise or lower it.";
            status_time_ = 8;
        }
    world_changed();
}

void App::move_selection(const Vec3& d, bool snap) {
    WTree t;
    t.build(doc_->world);
    // Glued pieces (app_glue.cpp) move with the selection as one rigid body: following the ground piece by piece would
    // pull their joints apart.
    const std::vector<uint32_t> movers = glue_group(selection_);
    if (movers.size() > selection_.size()) snap = false;
    else for (uint32_t u : selection_) if (const WObj* o = object_by_uid(doc_->world, u); o && o->find(kGlueProp)) snap = false;
    // Sheets (roads, thin plates, app_sheets.cpp): a seated one is put at its height by the stack, not by "the same height above
    // the ground". What each moved sheet overlapped when the drag began is remembered, so a sheet dragged over another goes on
    // top of it (dropped on top, like a placed one) and one that was under it from the start keeps its place.
    std::set<uint32_t> sheet_movers;
    for (uint32_t u : movers)
        if (const WObj* mo = object_by_uid(doc_->world, u); mo && sheet_kind(*mo)) sheet_movers.insert(u);
    // The sheets as they are before anything moves: what an object keeps standing on, and (first frame) what each moved sheet overlapped.
    const SheetSet pre = sheets(false);
    const place::SurfaceIndex surf(pre.s);
    std::vector<Standing> stood;
    if (!sheet_movers.empty()) {
        stood = standing_now(pre);
        bool have = true;
        for (uint32_t u : sheet_movers) if (!move_overlap0_.count(u)) have = false;
        if (!have) {
            for (size_t i = 0; i < pre.s.size(); i++) {
                if (!sheet_movers.count(pre.s[i].uid) || move_overlap0_.count(pre.s[i].uid)) continue;
                std::set<uint32_t>& base = move_overlap0_[pre.s[i].uid];
                for (size_t j = 0; j < pre.s.size(); j++)
                    if (j != i && place::overlap(pre.s[j], pre.s[i])) base.insert(pre.s[j].uid);
            }
        }
    }
    const Terrain& terr = doc_->terrain;
    for (uint32_t u : movers) {
        int ri = t.find(u);
        if (ri < 0) continue;
        const bool is_sheet = sheet_movers.count(u) != 0;
        const bool seated = is_sheet && !sheet_is_fixed(doc_->world.objs[size_t(ri)]);
        std::vector<int> sub;
        t.subtree(ri, sub);
        // Objects stand on the top sheet under their origin, else the ground: it keeps its height over that surface and takes the one
        // at the new spot (limited to 1.5 m over its foot, so it climbs onto a sheet but not onto a ramp overhead). A sheet itself
        // follows the ground.
        Vec3 rp;
        const WObj& root = doc_->world.objs[size_t(ri)];
        const bool root_pos = get_pos(root, rp);
        auto surface = [&](float x, float z, float below) { return is_sheet ? terr.height_at(x, z) : surf.at(terr, x, z, below); };
        float root_dy = d.y;
        if (snap && root_pos && !seated) {
            const float foot0 = is_sheet ? rp.y : rp.y - stand_foot(root);
            float rx, rz;
            stand_xz(root, rp, rx, rz);  // under the middle of the model's base, where the click and the gizmo are
            const float above = rp.y - surface(rx, rz, foot0 + place::kStandTol);
            root_dy = surface(rx + d.x, rz + d.z, foot0 + 1.5f) + above - rp.y;
        }
        for (int i : sub) {
            WObj& o = doc_->world.objs[size_t(i)];
            Vec3 p;
            if (!get_pos(o, p)) continue;
            Vec3 q{p.x + d.x, p.y + d.y, p.z + d.z};
            if (root_pos) {  // the whole subtree moves rigidly with its root
                if (!(seated && i == ri)) q.y = p.y + root_dy;
            } else if (snap) {  // a group without a place of its own: each member by itself
                const float foot0 = is_sheet ? p.y : p.y - stand_foot(o);
                const float above = p.y - surface(p.x, p.z, foot0 + place::kStandTol);
                q.y = surface(q.x, q.z, foot0 + 1.5f) + above;
            }
            set_pos(o, q);
        }
    }
    if (!sheet_movers.empty()) {
        SheetSet ss = sheets(true);
        std::vector<int> was;
        for (const place::Sheet& s : ss.s) was.push_back(s.layer);
        bool relayered = false;
        for (size_t i = 0; i < ss.s.size(); i++) {
            if (!sheet_movers.count(ss.s[i].uid)) continue;
            const std::set<uint32_t>& base = move_overlap0_[ss.s[i].uid];
            std::vector<uint32_t> fresh;  // sheets it overlaps now that it did not when the drag began
            for (size_t j = 0; j < ss.s.size(); j++) {
                if (j == i || sheet_movers.count(ss.s[j].uid) || (ss.s[i].run >= 0 && ss.s[j].run == ss.s[i].run)) continue;
                if (!base.count(ss.s[j].uid) && place::overlap(ss.s[j], ss.s[i])) fresh.push_back(ss.s[j].uid);
            }
            if (fresh.empty()) continue;
            place::Sheet probe = ss.s[i];
            const int layer = place::drop_layer(ss.s, probe, &fresh);  // may renumber the others to make room
            for (size_t k = 0; k < ss.s.size(); k++)
                if (k == i || (ss.s[i].run >= 0 && ss.s[k].run == ss.s[i].run)) ss.s[k].layer = layer;
            relayered = true;
        }
        if (relayered) commit_layers(ss, was);
        reseat_sheets(&ss, &stood);
    }
    doc_->dirty = true;
    objects_dirty_ = true;
}

void App::rotate_selection(float delta) {
    if (selection_.empty()) return;
    push_undo_merge("rotate", "rotate");  // a run of Q / E presses is one step
    if (Vec3 gp; gizmo_pivot(gp) && glue_turn(0, delta, gp)) return;  // one glued piece: about its glued joint, the rest following (app_glue.cpp)
    // Heading readout: the first selected object's heading after the turn and the step, floated for a second.
    auto say = [&]() {
        for (uint32_t u : selection_)
            if (const WObj* fo = object_by_uid(doc_->world, u); fo && fo->find("CorporalHeading")) {
                char b[100];
                snprintf(b, sizeof b, "Heading %.0f deg (%+.0f)", std::fmod(std::fmod(world::heading(*fo) * 57.2957795f, 360.f) + 360.f, 360.f), delta * 57.2957795f);
                set_readout(b);
                break;
            }
    };
    if (selection_.size() > 1 && !turn_each_) {  // a group turns about its centre, like the ring does (the checkbox keeps the old turn in place)
        if (Vec3 gp; gizmo_pivot(gp)) {
            rotate_group(delta, gp);
            for (uint32_t u : selection_)
                if (const WObj* so = object_by_uid(doc_->world, u); so && sheet_kind(*so)) { reseat_sheets(); break; }
            say();
            return;
        }
    }
    WTree t;
    t.build(doc_->world);
    for (uint32_t u : selection_) {
        int ri = t.find(u);
        if (ri < 0) continue;
        WObj& root = doc_->world.objs[size_t(ri)];
        Vec3 c;
        if (!get_pos(root, c)) continue;
        // Turn about the middle of the footprint, where the gizmo sits (a road piece's origin is its
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
    for (uint32_t u : selection_)  // a turned sheet has another footprint: re-seat the stack
        if (const WObj* so = object_by_uid(doc_->world, u); so && sheet_kind(*so)) { reseat_sheets(); break; }
    doc_->dirty = true;
    objects_dirty_ = true;
}

// ---- Key nudges, the readout label

void App::set_readout(const std::string& text) {
    readout_label(text);
    status_ = text;
    status_time_ = 1.f;
}

void App::readout_label(const std::string& text) {
    readout_ = text;
    readout_until_ = ImGui::GetTime() + 1.0;
}

// Arrow keys: along the world axis nearest the camera's forward (fwd) / right (right), keeping the ground-following of a drag.
void App::nudge_selection(int fwd, int right) {
    if (selection_.empty() || !editable()) return;
    const float step = ImGui::GetIO().KeyShift ? 10.f : 1.f;
    const float fx = std::sin(cam_.yaw), fz = std::cos(cam_.yaw), rx = std::cos(cam_.yaw), rz = -std::sin(cam_.yaw);
    auto axis = [](float x, float z, float& ox, float& oz) {
        if (std::fabs(x) >= std::fabs(z)) { ox = x < 0 ? -1.f : 1.f; oz = 0; }
        else { ox = 0; oz = z < 0 ? -1.f : 1.f; }
    };
    float ax, az, bx, bz;
    axis(fx, fz, ax, az);
    axis(rx, rz, bx, bz);
    Vec3 d{(ax * fwd + bx * right) * step, 0, (az * fwd + bz * right) * step};
    // With the grid on a nudge is one cell and lands on the lattice (the first selected object's footprint centre; the group follows).
    // Shift stays the 10 m step (and the place-freely key, which turns the grid off).
    if (grid_active() && !ImGui::GetIO().KeyShift) {
        const float cell = settings_.snap.grid_m;
        for (uint32_t u : selection_) {
            const WObj* so = object_by_uid(doc_->world, u);
            Vec3 p;
            if (!so || !get_pos(*so, p)) continue;
            float cx, cz;
            stand_xz(*so, p, cx, cz);
            d = {0, 0, 0};
            const float nx = ax * fwd + bx * right, nz = az * fwd + bz * right;
            if (nx != 0) d.x = ma2::snap::grid_round(cx + (nx < 0 ? -cell : cell), cell) - cx;
            if (nz != 0) d.z = ma2::snap::grid_round(cz + (nz < 0 ? -cell : cell), cell) - cz;
            break;
        }
    }
    push_undo_merge("nudge", "nudge");
    move_selection(d, true);
    char b[100];
    snprintf(b, sizeof b, "Moved %.0f m %s", step, fwd > 0 ? "forward" : fwd < 0 ? "back" : right > 0 ? "right" : "left");
    set_readout(b);
}

// PgUp / PgDn: lifts the selection; lowering stops where the lowest object meets what it rests on (the same floor as the lift handle).
void App::raise_selection(float dy) {
    if (selection_.empty() || !editable()) return;
    lift_begin();
    float total = dy;
    bool stopped = false;
    if (dy < 0 && dy < lift_floor_) { total = lift_floor_; stopped = true; }
    lift_started_ = false;
    if (stopped) {
        status_ = "Stopped at the ground. To push it under, drag the lift knob holding " + std::string(mod_name(settings_.hold_free)) + ", or type a height.";
        status_time_ = 3;
        readout_label("Stopped at the ground");
    }
    if (std::fabs(total) < 1e-4f) return;
    push_undo_merge("raise", "raise");
    mark_selection_fixed();  // a lifted flat piece keeps that height
    move_selection({0, total, 0}, false);
    if (!stopped) set_readout(lift_readout(total));
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
    bool had_sheet = false;
    for (uint32_t u : selection_)
        if (const WObj* so = object_by_uid(doc_->world, u); so && sheet_kind(*so)) had_sheet = true;
    std::vector<Standing> stood;  // what rests on the flat pieces, as it is before they go: it comes down to what is under it
    if (had_sheet) stood = standing_now(sheets(false));
    // A water takes its basin with it: the ground it dug goes back.
    int basins_filled = 0;
    for (uint32_t u : selection_)
        if (WObj* so = object_by_uid(doc_->world, u); so && so->cls == K_WATER && so->find(place::kBasinProp)) { refit_basin(*so, true); basins_filled++; }
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
    // once the last piece is gone those two are empty containers the user never placed, so they go too.
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
    if (had_sheet) reseat_sheets(nullptr, &stood);  // what lay on the deleted piece comes down
    world_changed();
    if (mode_pieces || trigger_refs) {
        std::string w = "You deleted ";
        if (mode_pieces) w += "an object a game type needs (a power plant, flag or Snatch It piece)";
        if (mode_pieces && trigger_refs) w += ", and ";
        if (trigger_refs) w += "a unit that " + std::to_string(trigger_refs) + " trigger setting(s) use";
        w += ". Press " + kt(A_UNDO) + " to undo it, or see Check scenario for what to fix.";
        message("Deleted something important", w);
    } else {
        status_ = "Deleted." + std::string(basins_filled ? (basins_filled == 1 ? " Its basin was filled in." : " Their basins were filled in.") : "") + " " + kt(A_UNDO) + " undoes it.";
        status_time_ = basins_filled ? 6 : 4;
    }
}

// Ctrl+X: the clipboard gets the selection, then it is deleted (one undo step, "Undo cut").
void App::cut_selection() {
    if (!doc_ || selection_.empty() || !editable()) return;
    copy_selection();
    if (clipboard_.empty()) return;
    clipboard_cut_ = true;
    size_t n = clipboard_.size();
    delete_selection("cut");
    if (status_time_ > 0 && status_.rfind("Deleted", 0) == 0) {
        status_ = std::to_string(n) + " object(s) cut. " + kt(A_PASTE) + " pastes them under the mouse, " + kt(A_UNDO) + " puts them back.";
        status_time_ = 4;
    }
}

// The selected objects as clips: each a private copy of its subtree, its place, how high it stands over its surface and its box for the ghost.
std::vector<App::Clip> App::clips_of_selection() {
    std::vector<Clip> out;
    if (!doc_) return out;
    std::unordered_set<uint32_t> keep = cat_->ref_keys();
    keep.erase(key_of("ReferenceUID"));
    keep.insert(key_of("TemplateID"));
    uint32_t tmp = 0x40000000u;  // temporary ids, renumbered on paste
    for (uint32_t u : selection_) {
        Clip c;
        c.objs = wedit::clone_subtree(doc_->world, u, tmp, &keep);
        if (c.objs.empty() || !get_pos(c.objs.front(), c.pos)) continue;
        if (const WObj* so = object_by_uid(doc_->world, u)) { c.sheet = sheet_kind(*so) != 0; c.above = c.sheet ? 0.f : above_surface(*so); }
        c.heading = world::heading(c.objs.front());
        for (const ObjView& v : views_)
            if (v.uid == u && v.index == v.root) {
                for (int k = 0; k < 3; k++) { c.lo[k] = v.lo[k]; c.hi[k] = v.hi[k]; }
                break;
            }
        out.push_back(std::move(c));
    }
    return out;
}

void App::copy_selection() {
    if (!doc_ || selection_.empty()) return;
    clipboard_ = clips_of_selection();
    clipboard_cut_ = false;
    status_ = std::to_string(clipboard_.size()) + " object(s) copied";
    status_time_ = 3;
}

// Puts the clips down with their centre at `at`, the whole group turned by `heading` about that centre. One undo step (dup: it is named
// duplicate, else paste); name_copy: the copies are named "<name> copy".
void App::paste_clips(const std::vector<Clip>& clips, const Vec3& at, float heading, bool dup, bool name_copy) {
    if (clips.empty() || !editable()) return;
    push_undo(dup ? "duplicate" : "paste");
    Vec3 centre{0, 0, 0};
    for (auto& c : clips) { centre.x += c.pos.x; centre.z += c.pos.z; }
    centre.x /= float(clips.size());
    centre.z /= float(clips.size());
    const float cs = std::cos(heading), sn = std::sin(heading);
    uint32_t next = next_uid(doc_->world);
    uint32_t grp = ensure_group(doc_->world, terrain_uid(doc_->world), "Placed Objects", next);
    selection_.clear();
    for (auto& c : clips) {
        Wdf tmp;
        tmp.objs = c.objs;
        uint32_t root = io_->clone_from_world(*doc_, tmp, c.objs.front().uid(), grp);
        if (!root) continue;
        const float dx = c.pos.x - centre.x, dz = c.pos.z - centre.z;
        ScenarioIO::move_subtree_to(*doc_, root, {at.x + cs * dx + sn * dz, 0, at.z - sn * dx + cs * dz}, 0, false, nullptr);
        if (heading != 0) {  // the copy turns about its own origin with the group; its parts follow
            WTree t;
            t.build(doc_->world);
            const int ri = t.find(root);
            Vec3 rp;
            if (ri >= 0 && get_pos(doc_->world.objs[size_t(ri)], rp)) {
                std::vector<int> sub;
                t.subtree(ri, sub);
                for (int i : sub) {
                    WObj& o = doc_->world.objs[size_t(i)];
                    Vec3 p;
                    if (i != ri && get_pos(o, p)) {
                        const float px = p.x - rp.x, pz = p.z - rp.z;
                        set_pos(o, {rp.x + cs * px + sn * pz, p.y, rp.z - sn * px + cs * pz});
                    }
                    if (o.find("CorporalHeading") || i == ri) world::set_heading(o, world::heading(o) + heading);
                }
            }
        }
        selection_.insert(root);
        if (WObj* po = object_by_uid(doc_->world, root)) {
            if (name_copy && po->find("Description")) po->set_str("Description", copy_name(po->description()));
        }
        if (WObj* po = object_by_uid(doc_->world, root); po && sheet_kind(*po)) {  // a pasted flat piece is placed anew: on top
            po->erase(key_of(place::kLayerProp));
            sheet_dropped(root);
        } else if (!c.sheet) {
            if (WObj* wo = object_by_uid(doc_->world, root); wo && wo->cls == K_WATER) wo->erase(key_of(place::kBasinProp));  // a pasted lake digs nothing
            put_on_surface(root, c.above);  // the same height over the paving or ground it lands on
        }
    }
    world_changed();
    status_ = std::string(dup ? "Duplicated " : "Pasted ") + std::to_string(selection_.size()) + (selection_.size() == 1 ? " object" : " objects") + ". " + kt(A_UNDO) + " undoes it.";
    status_time_ = 4;
}

// "Paste at view centre" and the Properties Paste button: an immediate paste, no ghost.
void App::paste(const Vec3& at) {
    paste_clips(clipboard_, at, 0.f, false, !clipboard_cut_);
}

// Ctrl+V / Ctrl+D: the clips hang on the mouse as outlines until a click puts them down (handle_viewport_input).
void App::arm_paste(std::vector<Clip> src, bool dup) {
    if (src.empty() || !editable()) return;
    paste_src_ = std::move(src);
    paste_dup_ = dup;
    paste_heading_ = 0;
    paste_on_ = false;
    last_place_was_paste_ = true;
    place_drag_ = false;
    tool_ = Tool::PasteGhost;
    status_ = std::string(dup ? "Duplicate" : "Paste") + ": move the mouse over the map and click to put the " + (paste_src_.size() == 1 ? "copy" : "copies") + " down. " +
              turn_keys_text() + " turn, " + kt(A_CANCEL) + " stops.";
    status_time_ = 6;
}

// Ctrl+A: every object drawn on the map (the View filters and helper switches decide what that is).
void App::select_all() {
    selection_.clear();
    for (auto& v : views_)
        if (v.index == v.root) selection_.insert(v.uid);
    status_ = std::to_string(selection_.size()) + " objects selected. " + kt(A_CANCEL) + " clears the selection.";
    status_time_ = 4;
}

// Ctrl+I: what is selected becomes unselected and every other object drawn on the map is selected.
void App::invert_selection() {
    std::set<uint32_t> now;
    for (auto& v : views_)
        if (v.index == v.root && !selection_.count(v.uid)) now.insert(v.uid);
    selection_ = std::move(now);
    status_ = std::to_string(selection_.size()) + " objects selected (selection inverted).";
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
    if (!get_pos(r, p)) return;
    if (!has_ground_model(r)) {
        // No model of its own (a fence or barricade made of parts, a marker): it already stands on the ground under the click; on
        // paving it goes up onto the top of it (the same surface every other object stands on).
        if (r.cls == K_WATER || r.cls == key_of("MARoad")) return;
        uint32_t on = 0;
        const float sf = surface_at(p.x, p.z, 1e30f, &on), g = doc_->terrain.height_at(p.x, p.z);
        if (!on || p.y > sf + 0.5f || p.y < g - 0.5f) return;
        const float up = sf - g;
        std::vector<int> sub0;
        wt.subtree(ri, sub0);
        for (int i : sub0) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + up, q.z});
        }
        return;
    }
    // A placed copy keeps its source level's height above its own ground (cliffs and tree groups sunk on purpose,
    // aircraft in the air); a new object goes on the ground: its model's lowest point at the terrain height.
    // It stands on the top sheet under its origin, else the ground; a tree, rock or roof only when the placing key asks.
    uint32_t on = 0;
    float sx, sz;
    stand_xz(r, p, sx, sz);
    float dy = surface_at(sx, sz, 1e30f, &on) + model_foot(r) - p.y;
    // A foundation goes a little above the highest ground under it (playtest 11-01: plates clipped through flat ground).
    // A thick footing stands on a sheet like any block; a thin plate is a sheet and is seated by the stack.
    if (float fy; foundation_rest(r, fy) && (!on || sheet_kind(r) == 2)) dy = fy - p.y;
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
    const bool was_dirty = doc_->dirty;
    push_undo("snap to ground");
    WTree t;
    t.build(doc_->world);
    int total = 0, changed = 0;
    std::string on_name;
    for (uint32_t u : selection_) {
        int ri = t.find(u);
        if (ri < 0) continue;
        WObj& o = doc_->world.objs[size_t(ri)];
        Vec3 p;
        if (!get_pos(o, p)) continue;
        total++;
        if (sheet_kind(o)) {  // roads and thin plates: back into the stack (app_sheets.cpp)
            sheet_to_ground(u);
            Vec3 q;
            if (get_pos(o, q) && std::fabs(q.y - p.y) > 0.001f) changed++;
            continue;
        }
        uint32_t on = 0;
        float sx, sz;
        stand_xz(o, p, sx, sz);
        float g = surface_at(sx, sz, 1e30f, &on) + stand_foot(o);
        if (o.cls == key_of("MARoad") && io_) g = io_->road_rest_height(*doc_, o.str("RoadDefinition"), p, heading(o));
        if (float fy; !on && foundation_rest(o, fy)) g = fy;  // a thick foundation: just above the highest ground under it
        move_selection({0, 0, 0}, false);
        float dy = g - p.y;
        if (std::fabs(dy) > 0.001f) {
            changed++;
            if (on) if (const WObj* so = object_by_uid(doc_->world, on)) on_name = so->description();
        }
        std::vector<int> sub;
        t.subtree(ri, sub);
        for (int i : sub) {
            Vec3 q;
            if (get_pos(doc_->world.objs[size_t(i)], q)) set_pos(doc_->world.objs[size_t(i)], {q.x, q.y + dy, q.z});
        }
    }
    if (!changed) {  // nothing moved by more than 1 mm: no step, no dirty mark, the world as it was
        Snapshot pre = drop_last_undo();
        doc_->world = std::move(pre.world);
        doc_->dirty = was_dirty;
        world_changed();
        if (total) { status_ = "Already on the ground."; status_time_ = 4; }
        return;
    }
    reseat_sheets();
    world_changed();
    if (!total) return;
    if (!changed) status_ = total == 1 ? "It is already on the ground." : "They are already on the ground.";
    else if (changed == 1 && total == 1 && !on_name.empty()) status_ = "Put it on " + on_name + ".";
    else if (changed == 1 && total == 1) status_ = "Put it on the ground.";
    else status_ = "Put " + std::to_string(changed) + (changed == 1 ? " object" : " objects") + " on the ground.";
    status_time_ = 5;
}

}  // namespace ed

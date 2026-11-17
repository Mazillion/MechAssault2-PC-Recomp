// Snapping: the app side of the solver in core\snap.cpp. Every frame while a road piece, wall, bridge or foundation is being
// placed, the piece is turned into a Mover, the join points of the pieces drawn near the cursor into Sockets, and the solver picks one
// joint: the ghost's own end, edge or centre within reach of a placed piece's, measured on the screen and in metres. The result is the
// footprint centre and heading the ghost takes; the click places it there. Q / E step through the other ways to join at that joint.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.h"
#include "imgui.h"
#include "../core/crop.h"
#include "../core/json.h"
#include "../core/triggers.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;
namespace sn = ma2::snap;

// Join points of the drawn pieces of snap family `fam` (0: every family) whose pivot is near (x, z); open[i] = nothing else meets sockets[i].
void App::snap_sockets(float x, float z, float radius, int fam, const std::set<uint32_t>& skip, std::vector<sn::Socket>& out, std::vector<char>& open) {
    out.clear();
    open.clear();
    if (!doc_ || !cat_) return;
    const float pivr = radius + 150.f;
    std::vector<Joint> js;
    for (const ObjView& v : views_) {
        if (v.helper >= 0 || v.index < 0 || size_t(v.index) >= doc_->world.objs.size()) continue;
        if (std::fabs(v.pos.x - x) > pivr || std::fabs(v.pos.z - z) > pivr) continue;
        const WObj& o = doc_->world.objs[size_t(v.index)];
        const int f = glue_family(o);
        if (!f || (fam && f != fam) || skip.count(o.uid())) continue;
        Vec3 p;
        if (!get_pos(o, p) || !object_joints(o, js)) continue;
        float tp = 0, tr = 0;
        get_tilt(o, tp, tr);
        const bool tilted = std::fabs(tp) + std::fabs(tr) > 1e-4f;
        float half = 0, hx = 0, hz = 0;
        if (f == 1 || f == 2) {
            const TemplateInfo* t = cat_->find(o.template_id());
            SnapFoot sf;
            if (!t || !object_foot(o, sf)) continue;
            half = sf.half;
        } else if (f == 3) {
            GpuModel* gm = obj_model(o);
            if (!gm || gm->state != GpuModel::Ready) continue;
            hx = 0.5f * (gm->bmax[0] - gm->bmin[0]);
            hz = 0.5f * (gm->bmax[2] - gm->bmin[2]);
        }
        const float yaw = heading(o);
        for (size_t k = 0; k < js.size(); k++) {
            sn::Socket s;
            s.fam = f;
            s.uid = o.uid();
            s.index = int(k);
            s.obj = v.index;
            s.x = js[k].p.x; s.y = js[k].p.y; s.z = js[k].p.z;
            s.ox = js[k].ox; s.oz = js[k].oz;
            s.py = p.y;
            s.tilted = tilted;
            s.roll = tr;
            if (f == 1 || f == 2) {
                s.axis = std::atan2(s.ox, s.oz) + (k == 1 ? sn::kPi : 0.f);   // the heading of the piece's own +long axis
                s.half = half;
            } else if (f == 3) {
                s.axis = yaw;
                s.half = k < 2 ? hx : hz;   // across the edge, as ends_meet compares
                s.len = k < 2 ? hz : hx;    // along it
            }
            out.push_back(s);
        }
    }
    // An end is open when no end of another piece meets it (ends_meet's rule; roads: 1.5 m on the map, at any height).
    open.assign(out.size(), 1);
    for (size_t i = 0; i < out.size(); i++)
        for (size_t j = 0; j < out.size(); j++) {
            const sn::Socket &a = out[i], &b = out[j];
            if (i == j || a.uid == b.uid || a.fam != b.fam) continue;
            const float tol = a.fam == 4 ? 1.5f : std::max(1.5f, 0.2f * std::min(a.half, b.half));
            if (std::fabs(a.x - b.x) < tol && std::fabs(a.z - b.z) < tol && (a.fam == 4 || std::fabs(a.y - b.y) < std::max(tol, 3.f))) { open[i] = 0; break; }
        }
}

// The open join points of the piece's family within reach of (x, z).
void App::snap_targets(const sn::Mover& m, float x, float z, const std::set<uint32_t>& skip, std::vector<sn::Socket>& open) {
    open.clear();
    if (m.fam < 1 || m.fam > 4) return;
    const float R = 2.f * std::max(m.hx, m.hz) + 40.f;
    std::vector<sn::Socket> all;
    std::vector<char> is_open;
    snap_sockets(x, z, std::max(R, 300.f), m.fam, skip, all, is_open);
    for (size_t i = 0; i < all.size(); i++)
        if (is_open[i] && std::hypot(all[i].x - x, all[i].z - z) <= std::max(R, 300.f)) open.push_back(all[i]);
}

// A road piece as the solver sees it: its own frame, footprint and join points (cropped: the cut ends, as object_joints builds them).
sn::Mover App::mover_from_road(const RoadPiece& rp, const WObj* cropped) {
    sn::Mover m;
    std::vector<RoadPiece::Exit> ex{{0, 0, 0, 1}};   // the entry: local 0,0 facing +z (the piece runs along -z)
    if (rp.exits.empty()) ex.push_back({0, -rp.l, 0, -1});
    else ex.insert(ex.end(), rp.exits.begin(), rp.exits.end());
    crop::Rect cr;
    const bool is_cropped = cropped && crop::of(*cropped, cr);
    if (is_cropped)
        for (auto& e : ex) crop::clamp_point(cr, e.x, e.z);
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    for (auto& e : ex) { x0 = std::min(x0, e.x); x1 = std::max(x1, e.x); z0 = std::min(z0, e.z); z1 = std::max(z1, e.z); }
    GpuModel* gm = rp.model.empty() ? nullptr : models_.get(rp.model, "");
    if (!is_cropped && gm && gm->state == GpuModel::Ready && gm->bmax[0] > gm->bmin[0]) {   // the same middle as place_origin_offset
        x0 = gm->bmin[0]; x1 = gm->bmax[0]; z0 = gm->bmin[2]; z1 = gm->bmax[2];
    }
    m.fam = 4;
    m.cx = 0.5f * (x0 + x1); m.cz = 0.5f * (z0 + z1);
    m.hx = 0.5f * (x1 - x0); m.hz = 0.5f * (z1 - z0);
    // straight: no exits listed, or one exit straight ahead (retail's longStraight lists its far end)
    m.straight = rp.exits.empty() || (rp.exits.size() == 1 && std::fabs(rp.exits[0].x) < 0.5f && std::fabs(rp.exits[0].vx) < 0.1f && rp.exits[0].vz < -0.9f);
    m.road_w = rp.w; m.road_l = rp.l;
    for (size_t i = 0; i < ex.size(); i++) {
        sn::Socket s;
        s.fam = 4; s.index = int(i);
        s.x = ex[i].x; s.z = ex[i].z; s.ox = ex[i].vx; s.oz = ex[i].vz;
        m.own.push_back(s);
    }
    return m;
}

// A template's footprint and join points (walls and bridges: the two ends of the long axis; plates: the edge middles; buildings: centre, edges, corners).
sn::Mover App::mover_from_template(const TemplateInfo& tt, GpuModel* gm, const SnapFoot* foot) {
    sn::Mover m;
    const TemplateInfo* t = &tt;
    int fam = snap_family(*t);
    if (!fam && t->cls_name == "MABuilding") {   // a building sits on a plate (not walls, cinematic copies or husks)
        auto kit = palette_kind_.find(t->uid);
        const bool cine = kit != palette_kind_.end() && (kit->second == "cinematic_prop" || kit->second == "destroyed_husk");
        const std::string n = lower(t->name);
        if (!cine && n.find("set only") == std::string::npos && n.find("nearclip") == std::string::npos) fam = 5;
    }
    if (!fam) return m;
    if (!gm || gm->state != GpuModel::Ready) return m;
    m.fam = fam;
    m.cx = 0.5f * (gm->bmin[0] + gm->bmax[0]);
    m.cz = 0.5f * (gm->bmin[2] + gm->bmax[2]);
    m.hx = 0.5f * (gm->bmax[0] - gm->bmin[0]);
    m.hz = 0.5f * (gm->bmax[2] - gm->bmin[2]);
    if (fam == 5) {
        if (m.hx < 1.f || m.hz < 1.f) { m.fam = 0; return m; }
        // centre, the four edge middles (+x -x +z -z), the four corners (++ +- -+ --) of the footprint
        auto add = [&](float x, float z, float ox, float oz, int i) {
            sn::Socket s;
            s.fam = 5; s.index = i;
            s.x = x; s.z = z; s.ox = ox; s.oz = oz;
            m.own.push_back(s);
        };
        add(m.cx, m.cz, 0, 0, 0);
        add(m.cx + m.hx, m.cz, 1, 0, 1); add(m.cx - m.hx, m.cz, -1, 0, 2); add(m.cx, m.cz + m.hz, 0, 1, 3); add(m.cx, m.cz - m.hz, 0, -1, 4);
        add(m.cx + m.hx, m.cz + m.hz, 0, 0, 5); add(m.cx + m.hx, m.cz - m.hz, 0, 0, 6); add(m.cx - m.hx, m.cz + m.hz, 0, 0, 7); add(m.cx - m.hx, m.cz - m.hz, 0, 0, 8);
        return m;
    }
    if (fam == 3) {
        // The middle of each edge, on the plate's top: +x, -x, +z, -z (object_joints' order).
        m.top = gm->bmax[1];
        const float ex[4] = {m.cx + m.hx, m.cx - m.hx, m.cx, m.cx}, ez[4] = {m.cz, m.cz, m.cz + m.hz, m.cz - m.hz};
        const float ox[4] = {1, -1, 0, 0}, oz[4] = {0, 0, 1, -1};
        for (int i = 0; i < 4; i++) {
            sn::Socket s;
            s.fam = 3; s.index = i;
            s.x = ex[i]; s.z = ez[i]; s.ox = ox[i]; s.oz = oz[i];
            s.len = i < 2 ? m.hz : m.hx;
            m.own.push_back(s);
        }
        return m;
    }
    SnapFoot f;
    if (foot) f = *foot;
    else if (!snap_foot(*t, f)) { m.fam = 0; return m; }
    const std::string mp = lower(t->model);
    if (mp.find("largewall_span") != std::string::npos) m.ratio = 0.94f;
    else if (mp.find("tallwall_span") != std::string::npos) m.ratio = 0.91f;
    else if (mp.find("widebridge02") != std::string::npos || mp.find("widebridge04") != std::string::npos) m.ratio = 0.97f;
    for (float sg : {1.f, -1.f}) {   // the two ends of the long axis (object_joints' order: +axis first)
        const float ux = f.along_x ? sg : 0.f, uz = f.along_x ? 0.f : sg;
        sn::Socket s;
        s.fam = fam; s.index = sg > 0 ? 0 : 1;
        s.x = f.cx + ux * f.half; s.z = f.cz + uz * f.half; s.ox = ux; s.oz = uz;
        m.own.push_back(s);
    }
    return m;
}

// The piece being placed as the solver sees it.
sn::Mover App::mover_for_place() {
    if (!doc_ || !cat_) return {};
    if (!place_road_.empty()) {
        const RoadPiece* rp = find_road_piece(place_road_);
        return rp ? mover_from_road(*rp, nullptr) : sn::Mover{};
    }
    const TemplateInfo* t = cat_->find(place_template_);
    if (!t || t->model.empty()) return {};
    return mover_from_template(*t, models_.get(t->model, t->mtb));
}

// A placed or copied object as the solver sees it (flat ones only: a tilted piece has no table, the grid still moves it).
sn::Mover App::mover_for_wobj(const WObj& o) {
    static const uint32_t K_ROAD_M = key_of("MARoad");
    if (!doc_ || !cat_) return {};
    float tp = 0, tr = 0;
    get_tilt(o, tp, tr);
    if (std::fabs(tp) + std::fabs(tr) > 1e-4f) return {};
    if (o.cls == K_ROAD_M) {
        std::string def = o.str("RoadDefinition");
        for (char& ch : def) if (ch == 0x5C) ch = '/';
        const RoadPiece* rp = find_road_piece(def);
        return rp ? mover_from_road(*rp, &o) : sn::Mover{};
    }
    const TemplateInfo* t = cat_->find(o.template_id());
    if (!t || t->model.empty()) return {};
    GpuModel* gm = obj_model(o);
    if (!gm) gm = models_.get(t->model, t->mtb);
    const int fam = snap_family(*t);
    SnapFoot cut;
    crop::Rect cr;
    if ((fam == 1 || fam == 2) && crop::of(o, cr) && object_foot(o, cut)) return mover_from_template(*t, gm, &cut);  // a cut piece's ends
    return mover_from_template(*t, gm);
}

// Plates and buildings drawn near (x, z) that a piece being placed can sit on, lie beside or end at: flat ones only (no pitch or roll).
void App::snap_anchors(const sn::Mover& m, float x, float z, std::vector<sn::Anchor>& out) {
    out.clear();
    if (!doc_ || !cat_ || !(m.fam == 1 || m.fam == 4 || m.fam == 5)) return;
    static const uint32_t K_BUILDING = key_of("MABuilding");
    const float reach = 2.f * std::max(m.hx, m.hz) + 40.f;
    for (const ObjView& v : views_) {
        if (v.helper >= 0 || v.index < 0 || size_t(v.index) >= doc_->world.objs.size()) continue;
        if (std::fabs(v.pos.x - x) > reach + 250.f || std::fabs(v.pos.z - z) > reach + 250.f) continue;
        const WObj& o = doc_->world.objs[size_t(v.index)];
        if (snap_skip_.count(o.uid())) continue;
        int kind = 0;
        if (o.cls == K_BUILDING && !is_foundation_obj(o)) kind = 5;
        else if (glue_family(o) == 3) kind = 3;
        if (!kind || (kind == 5 && m.fam != 1)) continue;   // buildings anchor only walls; plates anchor buildings, roads and walls
        Vec3 p;
        float tp = 0, tr = 0;
        if (!get_pos(o, p)) continue;
        get_tilt(o, tp, tr);
        if (std::fabs(tp) + std::fabs(tr) > 1e-4f) continue;
        GpuModel* gm = obj_model(o);
        if (!gm) gm = v.model;
        if (!gm || gm->state != GpuModel::Ready) continue;
        const float hx = 0.5f * (gm->bmax[0] - gm->bmin[0]), hz = 0.5f * (gm->bmax[2] - gm->bmin[2]);
        if (hx < 1.f || hz < 1.f) continue;
        const float lx = 0.5f * (gm->bmin[0] + gm->bmax[0]), lz = 0.5f * (gm->bmin[2] + gm->bmax[2]);
        const float yaw = heading(o), c = std::cos(yaw), s = std::sin(yaw);
        sn::Anchor a;
        a.uid = o.uid();
        a.kind = kind;
        a.cx = p.x + c * lx + s * lz;
        a.cz = p.z - s * lx + c * lz;
        a.yaw = yaw;
        a.hx = hx; a.hz = hz;
        a.top = p.y + gm->bmax[1];
        a.py = p.y;
        a.obj = v.index;
        if (std::hypot(a.cx - x, a.cz - z) - std::hypot(hx, hz) > reach) continue;
        out.push_back(a);
    }
}

void App::snap_clear() {
    snap_.on = false;
    snap_.cands.clear();
    snap_.win = -1;
    snap_.prev = sn::Prev{};
    snap_.label.clear();
    road_snapped_ = false;
    piece_snapped_ = false;
    piece_snap_obj_ = -1;
    road_open_ends_.clear();
}

static const char* kind_name(sn::Kind k) {
    switch (k) {
        case sn::Kind::End: return "End";
        case sn::Kind::Edge: return "Edge";
        case sn::Kind::EdgeCorner: return "EdgeCorner";
        case sn::Kind::Corner: return "Corner";
        case sn::Kind::Centre: return "Centre";
        case sn::Kind::Inside: return "Inside";
        case sn::Kind::Beside: return "Beside";
        case sn::Kind::EndOnEdge: return "EndOnEdge";
    }
    return "";
}

// What the winning cross-family candidate does, for the cursor label and the hint ("On Building base 3: NE corner").
static std::string anchor_label(const sn::Cand& c, int fam, const std::string& nm) {
    static const char* edge[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    static const char* corner[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = int(std::lround(std::atan2(c.sock.ox, c.sock.oz) / (sn::kPi / 4)));
    i = ((i % 8) + 8) % 8;
    switch (c.kind) {
        case sn::Kind::Centre: return "On " + nm + ": centre";
        case sn::Kind::Inside: return "On " + nm + (c.socket <= 4 ? std::string(": ") + edge[i] + " edge" : std::string(": ") + corner[i] + " corner");
        case sn::Kind::Beside: return "Beside " + nm;
        case sn::Kind::EndOnEdge: case sn::Kind::Edge: return "Ends at " + nm;
        case sn::Kind::Corner: return fam == 1 ? "At the corner of " + nm : "Beside " + nm + ": " + corner[i] + " corner";
        default: return "Joins " + nm;
    }
}

// The joint for the piece whose free ghost is centred at `centre` and turned to `yaw`. True: it joins; centre / yaw become the joined
// ghost's footprint centre and heading and the compat members (road_snapped_, piece_snapped_, snap_origin_y_ ...) are set.
bool App::solve_snap(const sn::Mover& m, Vec3& centre, float& yaw) {
    const sn::Prev keep = snap_.prev;
    snap_.on = false;
    snap_.cands.clear();
    snap_.win = -1;
    snap_.label.clear();
    road_snapped_ = false;
    piece_snapped_ = false;
    piece_snap_obj_ = -1;
    road_open_ends_.clear();
    snap_.fcx = centre.x;
    snap_.fcz = centre.z;
    if (m.fam < 1 || m.fam > 5 || m.own.empty() || !settings_.snap.joints) { snap_.prev = sn::Prev{}; return false; }
    std::vector<sn::Socket> open;
    snap_targets(m, centre.x, centre.z, snap_skip_, open);
    for (const sn::Socket& s : open) road_open_ends_.push_back({s.x, s.z, s.ox, s.oz, false});
    sn::same_family(m, open, snap_.cands);
    // Across families: a building on a plate, a road beside or ending at a plate edge, a wall end at a building's or plate's corner.
    {
        std::vector<sn::Anchor> anchors;
        snap_anchors(m, centre.x, centre.z, anchors);
        for (const sn::Anchor& a : anchors) {
            if (m.fam == 5) sn::on_plate(m, a, centre.x, centre.z, snap_.cands);
            else if (m.fam == 4) sn::road_edge(m, a, centre.x, centre.z, yaw, snap_.cands);
            else if (m.fam == 1) sn::wall_corner(m, a, snap_.cands);
        }
    }
    if (snap_translate_only_) sn::filter_translation(snap_.cands, yaw);   // a group only slides
    const sn::ToPx cam_px = [&](float x, float y, float z, float& sx, float& sy) { return cam_.project({x, y, z}, vp_w, vp_h, sx, sy); };
    const sn::ToPx& px = snap_px_override_ ? *snap_px_override_ : cam_px;
    snap_.reach_px = settings_.snap.reach_px * dpi_;
    snap_.cap = sn::cap_m(m);
    const int w = sn::choose(snap_.cands, m, centre.x, centre.z, yaw, snap_.reach_px, keep, px);
    snap_.win = w;
    if (w < 0) { snap_.prev = sn::Prev{}; return false; }
    const sn::Cand& c = snap_.cands[size_t(w)];
    snap_.on = true;
    snap_.c = c;
    snap_.d_px = c.d_px;
    snap_.d_m = c.d_m;
    sn::Prev pv;
    pv.on = true; pv.uid = c.uid; pv.socket = c.socket; pv.own = c.own; pv.kind = c.kind; pv.yaw = c.yaw;
    if (keep.pinned && keep.puid == c.uid && keep.psock == c.socket) { pv.pinned = true; pv.puid = keep.puid; pv.psock = keep.psock; pv.pfx = keep.pfx; pv.pfz = keep.pfz; pv.pfy = keep.pfy; }
    snap_.prev = pv;
    // The own joint on the free ghost (the line from it to the target shows the jump before the click).
    {
        const sn::Socket o = sn::own_point(m, c.own);
        const float lx = o.x - m.cx, lz = o.z - m.cz, cs = std::cos(yaw), sw = std::sin(yaw);
        snap_.jx = centre.x + cs * lx + sw * lz;
        snap_.jz = centre.z - sw * lx + cs * lz;
    }
    float cx, cz;
    sn::centre_of(m, c.ox, c.oz, c.yaw, cx, cz);
    centre = {cx, m.fam == 5 ? surface_at(cx, cz) : doc_->terrain.height_at(cx, cz), cz};   // a building stands on the plate under it
    yaw = c.yaw;
    ghost_heading_ = c.yaw;
    // What R1 reads (heights): the same four values the old solvers set.
    if (c.anchor) {
        // across families: R1 seats the piece (surface under it); no joint height
    } else if (m.fam == 4) {
        road_snapped_ = true;
        road_snap_y_ = c.y;   // NaN unless the target is tilted
        road_joint_ok_ = true;
        road_joint_y_ = c.sock.y;
        road_joint_roll_ = c.sock.roll;
        road_joint_px_ = c.jx;
        road_joint_pz_ = c.jz;
    } else {
        piece_snapped_ = true;
        snap_origin_y_ = c.y;
        if (m.fam == 3) piece_snap_obj_ = c.sock.obj;
    }
    for (RoadOpenEnd& e : road_open_ends_)
        if (std::fabs(e.x - c.sock.x) < 0.01f && std::fabs(e.z - c.sock.z) < 0.01f) e.joined = true;
    const WObj* to = c.sock.obj >= 0 && size_t(c.sock.obj) < doc_->world.objs.size() ? &doc_->world.objs[size_t(c.sock.obj)] : nullptr;
    const std::string nm = to ? to->description() : std::string("the piece next to it");
    snap_.label = c.anchor ? anchor_label(c, m.fam, nm) : "Joins " + nm;
    return true;
}

// Q / E while joined: the next way the piece can join at this joint, in yaw order (a straight road flips ends, a wall turns its corner).
bool App::snap_turn(int dir) {
    if (!snap_.on || holding(settings_.hold_free)) return false;
    const float ny = sn::next_yaw(snap_.cands, snap_.win, ghost_heading_, dir);
    if (std::isnan(ny)) return false;
    place_heading_ = ny;
    sn::Prev& p = snap_.prev;
    p.pinned = true;
    p.puid = snap_.c.uid;
    p.psock = snap_.c.socket;
    p.pfx = snap_.fcx;
    p.pfz = snap_.fcz;
    p.pfy = snap_.c.sock.y;
    return true;
}

// Open join points near the cursor as dots, the chosen one ringed, a line from the ghost's own joint to it, and what it joins as a label.
void App::draw_snap_feedback(ImDrawList* dl) {
    const bool pasting = tool_ == Tool::PasteGhost && paste_on_;
    if (!doc_ || (!preview_on_ && !move_snap_.on && !pasting)) return;
    const bool marks = tool_ == Tool::PlaceObject || move_snap_.on || pasting;
    if (!marks && snap_.grid_label.empty()) return;
    dl->PushClipRect(ImVec2(vp_x, vp_y), ImVec2(vp_x + vp_w, vp_y + vp_h), true);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const float d = dpi_;
    auto label = [&](const char* txt) {
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        const ImVec2 at(mp.x + 14.f * d, mp.y + 34.f * d);  // below the ghost's own name tag
        dl->AddRectFilled(ImVec2(at.x - 4 * d, at.y - 2 * d), ImVec2(at.x + ts.x + 4 * d, at.y + ts.y + 2 * d), IM_COL32(0, 0, 0, 150), 3 * d);
        dl->AddText(at, IM_COL32(215, 235, 235, 235), txt);
    };
    if (!snap_.on && !snap_.grid_label.empty()) label(snap_.grid_label.c_str());   // the grid rounded the ghost (no joint)
    if (marks && (!snap_.cands.empty() || snap_.on)) {
        std::set<std::pair<int, int>> seen;
        for (const sn::Cand& k : snap_.cands) {
            if (k.kind == sn::Kind::Beside || k.kind == sn::Kind::EndOnEdge) continue;   // slide along an edge: no fixed point to mark
            if (!seen.insert({int(k.uid), k.socket}).second) continue;
            float sx, sy;
            if (!cam_.project({k.tx, k.ty, k.tz}, vp_w, vp_h, sx, sy)) continue;
            const ImVec2 p(vp_x + sx, vp_y + sy);
            if (std::hypot(p.x - mp.x, p.y - mp.y) > 300.f * d) continue;
            dl->AddCircleFilled(p, 3.f * d, IM_COL32(255, 220, 90, 230));
        }
    }
    if (snap_.on) {
        float ax, ay, bx, by;
        const sn::Cand& c = snap_.c;
        if (cam_.project({c.tx, c.ty, c.tz}, vp_w, vp_h, bx, by)) {
            const ImVec2 t(vp_x + bx, vp_y + by);
            dl->AddCircle(t, 8.f * d, IM_COL32(120, 255, 235, 255), 24, 2.f * d);
            if (cam_.project({snap_.jx, c.ty, snap_.jz}, vp_w, vp_h, ax, ay))
                dl->AddLine(ImVec2(vp_x + ax, vp_y + ay), t, IM_COL32(120, 255, 235, 200), 1.f * d);
            label(snap_.label.c_str());
        }
    }
    dl->PopClipRect();
}

// ---- Snapping while moving ----

// The anchor's footprint centre now (the mover's own centre when it has one, else the middle of the model's base).
bool App::move_snap_centre(const WObj& o, float& x, float& z) {
    Vec3 p;
    if (!get_pos(o, p)) return false;
    if (move_snap_.mover.fam) sn::centre_of(move_snap_.mover, p.x, p.z, heading(o), x, z);
    else stand_xz(o, p, x, z);
    return true;
}

// The press of a body drag or the plane square: the anchor, its centre and heading, and the grab offset from the ground under the press.
bool App::begin_move_snap(uint32_t uid, float press_x, float press_z) {
    move_snap_ = MoveSnap{};
    if (!doc_ || !editable()) return false;
    const WObj* o = object_by_uid(doc_->world, uid);
    Vec3 p;
    if (!o || !get_pos(*o, p)) return false;
    MoveSnap& ms = move_snap_;
    ms.uid = uid;
    ms.yaw0 = heading(*o);
    const std::vector<uint32_t> grp = glue_group(selection_);
    ms.skip.insert(selection_.begin(), selection_.end());
    ms.skip.insert(grp.begin(), grp.end());
    ms.single = selection_.size() == 1 && grp.size() <= 1 && !o->find(kGlueProp);
    ms.mover = mover_for_wobj(*o);
    float cx, cz;
    if (!move_snap_centre(*o, cx, cz)) { ms = MoveSnap{}; return false; }
    if (!ms.single && ms.mover.fam >= 1 && ms.mover.fam <= 3) build_group_members(*o, ms.skip, cx, cz);
    ms.gx = cx - press_x;
    ms.gz = cz - press_z;
    ms.on = true;
    return true;
}

// A glued pair (or several selected pieces) slides like one piece, so any free end of the group may join a free end elsewhere, not only
// the ends of the piece the user pressed. Every member becomes a mover of its own (walls, bridge pieces, plates); its centre keeps its
// offset from the anchor's centre. Ends that another member's end meets are inside the group and never join (`interior`).
void App::build_group_members(const WObj& anchor, const std::set<uint32_t>& members, float acx, float acz) {
    MoveSnap& ms = move_snap_;
    ms.members.clear();
    struct WS { float x, z; uint32_t uid; };
    struct Cand1 { uint32_t uid; sn::Mover m; float yaw, ox, oz; std::vector<WS> ends; };
    std::vector<Cand1> list;
    for (uint32_t u : members) {
        const WObj* o = object_by_uid(doc_->world, u);
        Vec3 p;
        if (!o || !get_pos(*o, p)) continue;
        sn::Mover mm = o == &anchor ? ms.mover : mover_for_wobj(*o);
        if (mm.fam < 1 || mm.fam > 3 || mm.own.empty()) continue;
        const float y = heading(*o), c = std::cos(y), sn_ = std::sin(y);
        float mx, mz;
        sn::centre_of(mm, p.x, p.z, y, mx, mz);
        Cand1 k{u, mm, y, mx - acx, mz - acz, {}};
        for (const sn::Socket& e : k.m.own) k.ends.push_back({p.x + c * e.x + sn_ * e.z, p.z - sn_ * e.x + c * e.z, u});
        list.push_back(std::move(k));
    }
    if (list.size() < 2) return;
    for (const Cand1& k : list) {
        MoveSnap::Member mem;
        mem.m = k.m;
        mem.yaw = k.yaw;
        mem.ox = k.ox;
        mem.oz = k.oz;
        bool any_open = false;
        for (const WS& e : k.ends) {
            bool met = false;
            for (const Cand1& o : list)
                for (const WS& f : o.ends) met |= f.uid != e.uid && std::fabs(e.x - f.x) < 2.f && std::fabs(e.z - f.z) < 2.f;
            mem.interior.push_back(met ? 1 : 0);
            any_open |= !met;
        }
        if (any_open) ms.members.push_back(std::move(mem));
    }
}

void App::end_move_snap() {
    move_snap_ = MoveSnap{};
    snap_clear();
    snap_.grid_label.clear();
}

// One frame of a snapped move. (ground_x, ground_z) = the ground under the cursor. Solves the anchor's footprint centre like a placed ghost
// (joints first, then the grid), moves the selection by the difference and, for one unglued object, turns it to the joint's heading.
bool App::move_snap_step(float ground_x, float ground_z) {
    MoveSnap& ms = move_snap_;
    if (!ms.on || !doc_) return false;
    WObj* o = object_by_uid(doc_->world, ms.uid);
    float cx, cz;
    if (!o || !move_snap_centre(*o, cx, cz)) return false;
    const float fx = ground_x + ms.gx, fz = ground_z + ms.gz;
    float tx = fx, tz = fz, tyaw = ms.yaw0;
    bool joined = false;
    snap_.grid_label.clear();
    if (!holding(settings_.hold_free) && ms.mover.fam && settings_.snap.joints) {
        snap_skip_ = ms.skip;
        snap_translate_only_ = !ms.single;
        if (!ms.single && !ms.members.empty()) {
            // A group: each member joins by its own free ends; the nearest joint on the screen wins.
            int best = -1;
            float best_d = 1e30f;
            for (size_t i = 0; i < ms.members.size(); i++) {
                const MoveSnap::Member& mem = ms.members[i];
                Vec3 c{fx + mem.ox, doc_->terrain.height_at(fx + mem.ox, fz + mem.oz), fz + mem.oz};
                float y = mem.yaw;
                if (!solve_snap(mem.m, c, y)) continue;
                const int own = snap_.c.own;
                if (own >= 0 && size_t(own) < mem.interior.size() && mem.interior[size_t(own)]) continue;
                if (snap_.d_px < best_d) { best_d = snap_.d_px; best = int(i); }
            }
            if (best >= 0) {
                const MoveSnap::Member& mem = ms.members[size_t(best)];
                Vec3 c{fx + mem.ox, doc_->terrain.height_at(fx + mem.ox, fz + mem.oz), fz + mem.oz};
                float y = mem.yaw;
                joined = solve_snap(mem.m, c, y);
                if (joined) { tx = c.x - mem.ox; tz = c.z - mem.oz; tyaw = ms.yaw0; }
            }
            if (!joined) snap_clear();
        } else {
            Vec3 c{fx, doc_->terrain.height_at(fx, fz), fz};
            float y = ms.yaw0;
            joined = solve_snap(ms.mover, c, y);
            if (joined) { tx = c.x; tz = c.z; tyaw = ms.single ? y : ms.yaw0; }
        }
        snap_skip_.clear();
        snap_translate_only_ = false;
    }
    if (!joined) {
        snap_clear();
        if (grid_active()) {
            const float cell = settings_.snap.grid_m;
            tx = sn::grid_round(fx, cell);
            tz = sn::grid_round(fz, cell);
            char b[32];
            std::snprintf(b, sizeof b, "Grid %g m", double(cell));
            snap_.grid_label = b;
        }
    }
    if (grid_active()) r_.params.snap_grid = {fx, fz, settings_.snap.grid_m, 12.f};
    bool moved = false;
    const Vec3 d{tx - cx, 0, tz - cz};
    if (std::fabs(d.x) + std::fabs(d.z) > 1e-4f) { move_selection(d, true); moved = true; }
    if (ms.single) {
        o = object_by_uid(doc_->world, ms.uid);
        if (o) {
            const float dy = sn::wrap_pi(tyaw - heading(*o));
            if (std::fabs(dy) > 1e-4f) { turn_root_about(ms.uid, dy, tx, tz); moved = true; }
        }
    }
    // Heights from a joint: a wall or bridge piece joined end to end takes its neighbour's height; a road joined to a tilted road takes the
    // joint's (and stays fixed). Everything else is (sheets re-seat after every step).
    if (joined && ms.single && !snap_.c.anchor) {
        o = object_by_uid(doc_->world, ms.uid);
        Vec3 p;
        if (o && get_pos(*o, p)) {
            const int fam = ms.mover.fam;
            if ((fam == 1 || fam == 2) && !std::isnan(snap_.c.y) && std::fabs(snap_.c.y - p.y) > 1e-4f) {
                move_selection({0, snap_.c.y - p.y, 0}, false);
                moved = true;
            } else if (fam == 4 && !std::isnan(road_snap_y_)) {
                if (std::fabs(road_snap_y_ - p.y) > 1e-4f) { move_selection({0, road_snap_y_ - p.y, 0}, false); moved = true; }
                if (!sheet_is_fixed(*o)) mark_selection_fixed();
            }
        }
    }
    return moved;
}

// An arrow drag (axis A through pivot pv, `total` metres along it): the grid on the pivot's coordinate along that axis only.
bool App::snap_move_axis(const Vec3& A, const Vec3& pv, float total, float& out) {
    if (!grid_active()) return false;
    const float cell = settings_.snap.grid_m;
    if (std::fabs(A.x) > 0.999f) out = (sn::grid_round(pv.x + A.x * total, cell) - pv.x) / A.x;
    else if (std::fabs(A.z) > 0.999f) out = (sn::grid_round(pv.z + A.z * total, cell) - pv.z) / A.z;
    else return false;
    r_.params.snap_grid = {pv.x + A.x * total, pv.z + A.z * total, cell, 12.f};
    char b[32];
    std::snprintf(b, sizeof b, "Grid %g m", double(cell));
    snap_.grid_label = b;
    return true;
}

// Turns one root and its parts by `delta` about (pvx, pvz): no undo step (a move drag already has its own).
void App::turn_root_about(uint32_t uid, float delta, float pvx, float pvz) {
    WTree t;
    t.build(doc_->world);
    const int ri = t.find(uid);
    if (ri < 0) return;
    std::vector<int> sub;
    t.subtree(ri, sub);
    const float cs = std::cos(delta), sn_ = std::sin(delta);
    for (int i : sub) {
        WObj& o = doc_->world.objs[size_t(i)];
        Vec3 p;
        if (get_pos(o, p)) {
            const float dx = p.x - pvx, dz = p.z - pvz;
            set_pos(o, {pvx + cs * dx + sn_ * dz, p.y, pvz - sn_ * dx + cs * dz});
        }
        if (i == ri || o.find("CorporalHeading")) set_heading(o, heading(o) + delta);
    }
    if (const WObj* so = object_by_uid(doc_->world, uid); so && sheet_kind(*so)) reseat_sheets();
    doc_->dirty = true;
    objects_dirty_ = true;
}

// The single-object paste ghost: joins like a placed piece (heading H = the clip's heading plus the user's turn); several objects: the grid on the
// group centre. `at` in: the free origin of the group centre (the cursor's ground point); out: where it lands. Sets paste_turn_ (the effective turn).
bool App::paste_solve(Vec3& at) {
    paste_turn_ = paste_heading_;
    if (paste_src_.empty() || !doc_) return false;
    bool joined = false;
    snap_.grid_label.clear();
    if (paste_src_.size() == 1 && !holding(settings_.hold_free) && settings_.snap.joints) {
        const Clip& c = paste_src_[0];
        const WObj& o = c.objs.front();
        const sn::Mover m = mover_for_wobj(o);
        if (m.fam) {
            const float h = c.heading + paste_heading_, ch = std::cos(h), sh = std::sin(h);
            Vec3 cen{at.x + ch * m.cx + sh * m.cz, 0, at.z - sh * m.cx + ch * m.cz};   // the free ghost's footprint centre
            cen.y = doc_->terrain.height_at(cen.x, cen.z);
            float yaw = h;
            snap_skip_.clear();
            snap_translate_only_ = false;
            if (solve_snap(m, cen, yaw)) {
                const float cy = std::cos(yaw), sy = std::sin(yaw);
                at.x = cen.x - (cy * m.cx + sy * m.cz);
                at.z = cen.z - (-sy * m.cx + cy * m.cz);
                paste_turn_ = yaw - c.heading;
                joined = true;
            }
        }
    }
    if (!joined) {
        snap_clear();
        if (grid_active()) {
            const float cell = settings_.snap.grid_m;
            at.x = sn::grid_round(at.x, cell);
            at.z = sn::grid_round(at.z, cell);
            r_.params.snap_grid = {at.x, at.z, cell, 12.f};
            char b[32];
            std::snprintf(b, sizeof b, "Grid %g m", double(cell));
            snap_.grid_label = b;
        }
    }
    return joined;
}

// ---- Grid, angle steps, the snap bar ----

bool App::grid_active() const { return settings_.snap.grid && !holding(settings_.hold_free); }

float App::angle_snapped(float yaw) const {
    if (!settings_.snap.angle || holding(settings_.hold_free)) return yaw;
    return sn::angle_round(yaw, angle_step_rad());
}

float App::turn_step_rad() const { return std::max(0.2617994f, angle_step_rad()); }

// Q / E on a free ghost: 15 degrees (or the angle step when that is bigger), then onto the step.
void App::turn_place_heading(int dir) {
    place_heading_ = angle_snapped(place_heading_ + float(dir) * turn_step_rad());
}

void App::grid_toggle() {
    settings_.snap.grid = !settings_.snap.grid;
    snap_prefs_save();
    char b[160];
    if (settings_.snap.grid) std::snprintf(b, sizeof b, "Grid on (%g m). %s turns it off.", double(settings_.snap.grid_m), kt(A_GRID_TOGGLE).c_str());
    else std::snprintf(b, sizeof b, "Grid off. %s turns it on.", kt(A_GRID_TOGGLE).c_str());
    status_ = b;
    status_time_ = 3;
}

void App::grid_step(int dir) {
    const float n = snap_grid_step(settings_.snap.grid_m, dir);
    char b[160];
    if (n == settings_.snap.grid_m) std::snprintf(b, sizeof b, "Grid %g m is the %s size.", double(n), dir > 0 ? "biggest" : "smallest");
    else {
        settings_.snap.grid_m = n;
        snap_prefs_save();
        if (settings_.snap.grid) std::snprintf(b, sizeof b, "Grid %g m.", double(n));
        else std::snprintf(b, sizeof b, "Grid %g m (the grid is off: %s turns it on).", double(n), kt(A_GRID_TOGGLE).c_str());
    }
    status_ = b;
    status_time_ = 3;
}

// The three toggles with their sizes and steps. compact: the snap bar and the placing panel; else the Edit menu's items.
void App::snap_controls_ui(bool compact) {
    SnapPrefs& sp = settings_.snap;
    const std::string fr = mod_name(settings_.hold_free);
    auto key_note = [&](int a) { const std::string k = kt(a); return k.empty() ? std::string() : " (" + k + ")"; };
    bool ch = false;
    if (compact) {
        ch |= ImGui::Checkbox("Joints##snapj", &sp.joints);
        ImGui::SetItemTooltip("Roads, walls, bridges, bases and buildings join the ends, edges and corners they come near.%s Hold %s to place freely.", key_note(A_SNAP_JOINTS).c_str(), fr.c_str());
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Grid##snapg", &sp.grid);
        ImGui::SetItemTooltip("Placing and moving round to a grid of the size beside it.%s %s / %s change the size while placing. Hold %s to place freely.",
                              key_note(A_GRID_TOGGLE).c_str(), kt(A_GRID_SMALLER).c_str(), kt(A_GRID_BIGGER).c_str(), fr.c_str());
        ImGui::SameLine();
        char gl[24];
        std::snprintf(gl, sizeof gl, "%g m", double(sp.grid_m));
        ImGui::SetNextItemWidth(62.f * dpi_);
        if (!sp.grid) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));   // The size is greyed while the grid is off
        const bool gopen = begin_combo("##snapgs", gl);
        if (!sp.grid) ImGui::PopStyleColor();
        if (gopen) {
            for (int i = 0; i < kGridSizeCount; i++) {
                char t[24];
                std::snprintf(t, sizeof t, "%g m", double(kGridSizes[i]));
                if (ImGui::Selectable(t, kGridSizes[i] == sp.grid_m)) { sp.grid_m = kGridSizes[i]; ch = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ch |= ImGui::Checkbox("Angle##snapa", &sp.angle);
        ImGui::SetItemTooltip("Headings from drags, new picks, the turn keys and the turn ring come in steps of the size beside it.%s Hold %s to turn freely.", key_note(A_ANGLE_SNAP).c_str(), fr.c_str());
        ImGui::SameLine();
        std::snprintf(gl, sizeof gl, "%g deg", double(sp.angle_deg));
        ImGui::SetNextItemWidth(70.f * dpi_);
        if (!sp.angle) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool aopen = begin_combo("##snapas", gl);
        if (!sp.angle) ImGui::PopStyleColor();
        if (aopen) {
            for (int i = 0; i < kAngleStepCount; i++) {
                char t[24];
                std::snprintf(t, sizeof t, "%g deg", double(kAngleSteps[i]));
                if (ImGui::Selectable(t, kAngleSteps[i] == sp.angle_deg)) { sp.angle_deg = kAngleSteps[i]; ch = true; }
            }
            ImGui::EndCombo();
        }
    } else {
        auto item = [&](const char* label, int act, bool* v) {
            const std::string k = kt(act);
            ch |= ImGui::MenuItem(label, k.empty() ? nullptr : k.c_str(), v);
        };
        item("Snap to joints", A_SNAP_JOINTS, &sp.joints);
        ImGui::SetItemTooltip("Roads, walls, bridges, bases and buildings join the ends, edges and corners they come near. Hold %s to place freely.", fr.c_str());
        item("Snap to grid", A_GRID_TOGGLE, &sp.grid);
        if (ImGui::BeginMenu("Grid size")) {
            for (int i = 0; i < kGridSizeCount; i++) {
                char t[24];
                std::snprintf(t, sizeof t, "%g m", double(kGridSizes[i]));
                if (ImGui::MenuItem(t, nullptr, kGridSizes[i] == sp.grid_m)) { sp.grid_m = kGridSizes[i]; ch = true; }
            }
            ImGui::EndMenu();
        }
        item("Angle steps", A_ANGLE_SNAP, &sp.angle);
        if (ImGui::BeginMenu("Angle step")) {
            for (int i = 0; i < kAngleStepCount; i++) {
                char t[24];
                std::snprintf(t, sizeof t, "%g degrees", double(kAngleSteps[i]));
                if (ImGui::MenuItem(t, nullptr, kAngleSteps[i] == sp.angle_deg)) { sp.angle_deg = kAngleSteps[i]; ch = true; }
            }
            ImGui::EndMenu();
        }
    }
    if (ch) snap_prefs_save();
}

// The snap bar: a small borderless window 8 px inside the viewport's top-left corner (the helper legend is bottom-left).
void App::snap_bar() {
    if (!doc_ || !editable() || vp_w < 100.f) return;
    // Not while a brush is in hand (the terrain brushes, painting, scatter, pond): joints, grid and angle do nothing there and the bar
    // covered a corner of the map. The placing panel keeps its own copy.
    if ((tool_ >= Tool::Raise && tool_ <= Tool::PaintCulture) || tool_ == Tool::ScatterTrees || tool_ == Tool::MakePond) return;
    ImGui::SetNextWindowPos(ImVec2(vp_x + 8.f * dpi_, vp_y + 8.f * dpi_));
    ImGui::SetNextWindowBgAlpha(0.72f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##snapbar", nullptr, f)) snap_controls_ui(true);
    ImGui::End();
}

// ---- automation reads (tools\AUTOMATION.md) ----

std::string App::automation_joints(float near_px, int limit) {
    Json out = Json::object();
    Json rows = Json::array();
    if (!doc_) return out.with("rows", rows).dump();
    const ImVec2 mp = ImGui::GetIO().MousePos;
    Vec3 g;
    float cx = cam_.tx, cz = cam_.tz;
    if (cursor_ground(g)) { cx = g.x; cz = g.z; }
    std::vector<sn::Socket> all;
    std::vector<char> open;
    snap_sockets(cx, cz, 600.f, 0, {}, all, open);
    struct Row { double d; size_t i; float sx, sy; };
    std::vector<Row> hits;
    for (size_t i = 0; i < all.size(); i++) {
        float sx, sy;
        if (!cam_.project({all[i].x, all[i].y, all[i].z}, vp_w, vp_h, sx, sy)) continue;
        const double d = std::hypot(double(vp_x + sx) - mp.x, double(vp_y + sy) - mp.y);
        if (d <= near_px) hits.push_back({d, i, vp_x + sx, vp_y + sy});
    }
    std::sort(hits.begin(), hits.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
    auto r2 = [](double v) { return std::round(v * 100.0) / 100.0; };
    for (const Row& h : hits) {
        if (int(rows.a.size()) >= limit) break;
        const sn::Socket& s = all[h.i];
        std::string name;
        if (s.obj >= 0 && size_t(s.obj) < doc_->world.objs.size()) name = doc_->world.objs[size_t(s.obj)].description();
        Json row = Json::object();
        row.with("uid", Json::number(s.uid)).with("name", Json::string(name)).with("fam", Json::number(s.fam)).with("index", Json::number(s.index));
        row.with("x", Json::number(r2(s.x))).with("y", Json::number(r2(s.y))).with("z", Json::number(r2(s.z)));
        row.with("ox", Json::number(r2(s.ox))).with("oz", Json::number(r2(s.oz))).with("open", Json::boolean(open[h.i] != 0));
        row.with("sx", Json::number(std::round(h.sx))).with("sy", Json::number(std::round(h.sy)));
        rows.push(std::move(row));
    }
    out.with("rows", rows).with("count", Json::number(double(hits.size())));
    return out.dump();
}

std::string App::automation_project(float x, float y, float z) const {
    Json out = Json::object();
    float sx = 0, sy = 0;
    const bool on = cam_.project({x, y, z}, vp_w, vp_h, sx, sy);
    out.with("sx", Json::number(std::round(vp_x + sx))).with("sy", Json::number(std::round(vp_y + sy)));
    out.with("on_screen", Json::boolean(on && sx >= 0 && sy >= 0 && sx < vp_w && sy < vp_h));
    return out.dump();
}

Json App::snap_state_json() const {
    Json j = Json::object();
    j.with("on", Json::boolean(snap_.on));
    if (snap_.on) {
        const sn::Cand& c = snap_.c;
        std::string target;
        if (c.sock.obj >= 0 && size_t(c.sock.obj) < doc_->world.objs.size()) target = doc_->world.objs[size_t(c.sock.obj)].description();
        j.with("kind", Json::string(kind_name(c.kind))).with("target_uid", Json::number(c.uid)).with("target", Json::string(target));
        j.with("socket", Json::number(c.socket)).with("own", Json::number(c.own));
        j.with("tx", Json::number(std::round(c.tx * 1000.f) / 1000.f)).with("tz", Json::number(std::round(c.tz * 1000.f) / 1000.f));
        j.with("d_px", Json::number(std::round(snap_.d_px * 10.f) / 10.f)).with("d_m", Json::number(std::round(snap_.d_m * 100.f) / 100.f));
        j.with("yaw_deg", Json::number(std::round(c.yaw * 5729.58f) / 100.f));
    }
    j.with("reach_px", Json::number(std::round(settings_.snap.reach_px * dpi_)));
    j.with("cap_m", Json::number(snap_.cap));
    j.with("free", Json::boolean(holding(settings_.hold_free)));
    j.with("joints", Json::boolean(settings_.snap.joints));
    j.with("grid", Json::boolean(settings_.snap.grid)).with("grid_m", Json::number(settings_.snap.grid_m));
    j.with("angle", Json::boolean(settings_.snap.angle)).with("angle_deg", Json::number(settings_.snap.angle_deg));
    j.with("grid_label", Json::string(snap_.grid_label));
    j.with("anchor", Json::boolean(snap_.on && snap_.c.anchor));
    j.with("moving", Json::boolean(move_snap_.on)).with("moving_fam", Json::number(move_snap_.mover.fam)).with("moving_single", Json::boolean(move_snap_.single));
    j.with("candidates", Json::number(double(snap_.cands.size())));
    j.with("label", Json::string(snap_.label));
    return j;
}

}  // namespace ed

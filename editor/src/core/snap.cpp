#include "snap.h"

#include <algorithm>

namespace ma2::snap {

namespace {

inline float ang(float x, float z) { return std::atan2(x, z); }
inline void rot(float yaw, float x, float z, float& wx, float& wz) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    wx = c * x + s * z;
    wz = -s * x + c * z;
}

int prio_of(Kind k) {
    switch (k) {
        case Kind::End: return 0;
        case Kind::Edge: case Kind::EdgeCorner: case Kind::Corner: case Kind::Inside: return 1;
        case Kind::Centre: return 2;
        default: return 3;
    }
}

// A candidate that puts own point `oi` of the mover (turned by yaw) on the target point.
void add_on_point(const Mover& m, const Socket& s, int oi, float yaw, Kind kind, float y, std::vector<Cand>& out) {
    const Socket& o = m.own[size_t(oi)];
    float wx, wz;
    rot(yaw, o.x, o.z, wx, wz);
    Cand k;
    k.yaw = wrap_pi(yaw);
    k.ox = s.x - wx;
    k.oz = s.z - wz;
    k.jx = o.x;
    k.jz = o.z;
    k.tx = s.x;
    k.tz = s.z;
    k.ty = s.y;
    k.y = y;
    k.kind = kind;
    k.uid = s.uid;
    k.socket = s.index;
    k.own = oi;
    k.sock = s;
    out.push_back(k);
}

// A wall or bridge piece: the long axis lies along the run, the piece's centre half a length past the join (less the overlap retail
// uses for a few models), or turned a quarter at the end of a wall (a corner: no overlap). Each way also comes mirrored (the piece
// flipped end for end), so turning the ghost always reaches the wanted one.
void along_run(const Mover& m, const Socket& s, std::vector<Cand>& out) {
    if (m.own.size() < 2) return;
    const Socket& e0 = m.own[0];  // the +axis end; own[1] is the -axis end
    const float A_l = ang(e0.ox, e0.oz);
    const float half = std::hypot(e0.x - m.cx, e0.z - m.cz);
    const float a = ang(s.ox, s.oz);
    const int turns = m.fam == 1 ? 3 : 1;
    for (int t = 0; t < turns; t++) {
        const float turn = t == 0 ? 0.f : t == 1 ? kPi / 2 : -kPi / 2;
        for (int flip = 0; flip < 2; flip++) {
            const float D = a + turn;                              // the way the piece runs away from the join
            const float W = s.axis + turn + (flip ? kPi : 0.f);    // where its own +axis points (the neighbour's heading, never flipped, unless mirrored)
            const float yaw = W - A_l;
            const int oi = std::fabs(wrap_pi(W - (D + kPi))) < 1e-3f ? 0 : 1;   // the end that faces the join
            const float reach = half * (turn == 0.f ? 1.f - 2.f * (1.f - m.ratio) : 1.f);
            const float cxw = s.x + std::sin(D) * reach, czw = s.z + std::cos(D) * reach;   // footprint centre
            float wx, wz;
            rot(yaw, m.cx, m.cz, wx, wz);
            Cand k;
            k.yaw = wrap_pi(yaw);
            k.ox = cxw - wx;
            k.oz = czw - wz;
            k.jx = m.own[size_t(oi)].x;
            k.jz = m.own[size_t(oi)].z;
            k.tx = s.x;
            k.tz = s.z;
            k.ty = s.y;
            k.y = s.py;   // retail runs share one height: the neighbour's origin height
            k.kind = Kind::End;
            k.uid = s.uid;
            k.socket = s.index;
            k.own = oi;
            k.sock = s;
            out.push_back(k);
        }
    }
}

}  // namespace

float wrap_pi(float a) {
    const float tp = 2.f * kPi;
    a = std::fmod(a, tp);
    if (a > kPi) a -= tp;
    else if (a <= -kPi) a += tp;
    return a;
}

float cap_m(const Mover& m) {
    const float side = 2.f * std::max(m.hx, m.hz);
    return std::clamp(kCapFrac * side, kCapMin, kCapMax);
}

void same_family(const Mover& m, const std::vector<Socket>& open, std::vector<Cand>& out) {
    if (m.fam < 1 || m.fam > 4 || m.own.empty()) return;
    for (const Socket& s : open) {
        if (s.fam != m.fam) continue;
        if (m.fam == 1 || m.fam == 2) {
            along_run(m, s, out);
        } else if (m.fam == 3) {
            // A plate's own edge on the target edge, facing it: its edge middle on the target's (three slide positions come with).
            for (size_t i = 0; i < m.own.size(); i++) {
                const float yaw = ang(-s.ox, -s.oz) - ang(m.own[i].ox, m.own[i].oz);
                add_on_point(m, s, int(i), yaw, Kind::Edge, s.y - m.top, out);
                // Edges of different lengths: also flush with either corner (the own middle lands that far along the target edge).
                const float d = s.len - m.own[i].len;
                if (std::fabs(d) > 0.1f)
                    for (float sg : {1.f, -1.f}) {
                        Socket t = s;
                        t.x += s.oz * d * sg;
                        t.z -= s.ox * d * sg;
                        add_on_point(m, t, int(i), yaw, Kind::EdgeCorner, s.y - m.top, out);
                    }
            }
        } else {
            // A road piece: one of its own ends (entry or an exit) on the open end, turned so that it runs on from it.
            for (size_t i = 0; i < m.own.size(); i++) {
                const float yaw = ang(-s.ox, -s.oz) - ang(m.own[i].ox, m.own[i].oz);
                add_on_point(m, s, int(i), yaw, Kind::End, s.tilted ? s.y : NAN, out);
            }
        }
    }
}


namespace {

struct V2 { float x, z; };
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.z + b.z}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.z - b.z}; }
inline V2 operator*(V2 a, float k) { return {a.x * k, a.z * k}; }
inline float dot(V2 a, V2 b) { return a.x * b.x + a.z * b.z; }
inline V2 uvec(float yaw) { return {std::cos(yaw), -std::sin(yaw)}; }   // a footprint's own +x in the world
inline V2 vvec(float yaw) { return {std::sin(yaw), std::cos(yaw)}; }    // its own +z

// The candidate that puts the piece's footprint centre at (wcx, wcz) turned by yaw, joined at own point oi to (tx, tz) (NaN: wherever that point lands).
Cand anchor_cand(const Mover& m, const Anchor& a, int oi, float yaw, float wcx, float wcz, float tx, float tz, Kind kind, int sock, V2 sdir) {
    Cand k;
    k.yaw = wrap_pi(yaw);
    float lx, lz;
    rot(yaw, m.cx, m.cz, lx, lz);
    k.ox = wcx - lx;
    k.oz = wcz - lz;
    const Socket o = own_point(m, oi);
    float dx, dz;
    rot(yaw, o.x - m.cx, o.z - m.cz, dx, dz);
    if (std::isnan(tx)) { tx = wcx + dx; tz = wcz + dz; }
    k.jx = o.x; k.jz = o.z;
    k.tx = tx; k.tz = tz; k.ty = a.top;
    k.kind = kind;
    k.uid = a.uid;
    k.socket = sock;
    k.own = oi;
    k.anchor = true;
    k.sock.fam = a.kind; k.sock.uid = a.uid; k.sock.obj = a.obj; k.sock.index = sock;
    k.sock.x = tx; k.sock.z = tz; k.sock.y = a.top; k.sock.py = a.py;
    k.sock.ox = sdir.x; k.sock.oz = sdir.z; k.sock.axis = a.yaw;
    return k;
}

// Centre of the piece when its own point oi (turned by yaw) lands on (tx, tz).
V2 centre_for(const Mover& m, int oi, float yaw, float tx, float tz) {
    const Socket o = own_point(m, oi);
    float dx, dz;
    rot(yaw, o.x - m.cx, o.z - m.cz, dx, dz);
    return {tx - dx, tz - dz};
}

int corner_id(int sx, int sz) { return sx > 0 ? (sz > 0 ? 5 : 6) : (sz > 0 ? 7 : 8); }

}  // namespace

void on_plate(const Mover& m, const Anchor& a, float fcx, float fcz, std::vector<Cand>& out) {
    if (m.fam != 5 || m.own.size() < 9) return;
    const V2 u = uvec(a.yaw), v = vvec(a.yaw), C{a.cx, a.cz};
    const V2 rel = V2{fcx, fcz} - C;
    if (std::fabs(dot(rel, u)) > a.hx + 2.f || std::fabs(dot(rel, v)) > a.hz + 2.f) return;   // the anchor rect grown by 2 m
    struct Pt { int id, sx, sz; };
    static const Pt pts[9] = {{0, 0, 0}, {1, 1, 0}, {2, -1, 0}, {3, 0, 1}, {4, 0, -1}, {5, 1, 1}, {6, 1, -1}, {7, -1, 1}, {8, -1, -1}};
    for (int q = 0; q < 4; q++) {
        const float yaw = a.yaw + float(q) * kPi / 2;
        const float bhx = q & 1 ? m.hz : m.hx, bhz = q & 1 ? m.hx : m.hz;
        const float rx = std::max(a.hx - bhx, 0.f), rz = std::max(a.hz - bhz, 0.f);   // a bigger piece stays centred on that axis
        std::vector<V2> seen;
        for (const Pt& p : pts) {
            const float px = float(p.sx) * rx, pz = float(p.sz) * rz;
            bool dup = false;
            for (const V2& s : seen) if (std::fabs(s.x - px) < 0.01f && std::fabs(s.z - pz) < 0.01f) dup = true;
            if (dup) continue;
            seen.push_back({px, pz});
            const V2 wc = C + u * px + v * pz;
            // which own point faces / lands on that edge or corner of the plate at this turn
            int oi = 0;
            if (p.id >= 1 && p.id <= 4) {
                for (int i = 1; i <= 4; i++) {
                    float wx, wz;
                    rot(yaw, m.own[size_t(i)].ox, m.own[size_t(i)].oz, wx, wz);
                    const V2 w{wx, wz};
                    if (std::fabs(dot(w, u) - float(p.sx)) < 0.1f && std::fabs(dot(w, v) - float(p.sz)) < 0.1f) oi = i;
                }
            } else if (p.id >= 5) {
                for (int i = 5; i <= 8; i++) {
                    float wx, wz;
                    rot(yaw, m.own[size_t(i)].x - m.cx, m.own[size_t(i)].z - m.cz, wx, wz);
                    const V2 w{wx, wz};
                    if (dot(w, u) * float(p.sx) > 0 && dot(w, v) * float(p.sz) > 0) oi = i;
                }
            }
            if (p.id && !oi) continue;
            const V2 sd = u * float(p.sx) + v * float(p.sz);
            out.push_back(anchor_cand(m, a, oi, yaw, wc.x, wc.z, p.id == 0 ? C.x : NAN, p.id == 0 ? C.z : NAN, p.id == 0 ? Kind::Centre : Kind::Inside, p.id, sd));
        }
    }
}

void road_edge(const Mover& m, const Anchor& a, float fcx, float fcz, float fyaw, std::vector<Cand>& out) {
    if (m.fam != 4 || m.own.size() < 2 || a.kind != 3) return;
    const V2 u = uvec(a.yaw), v = vvec(a.yaw), C{a.cx, a.cz}, fc{fcx, fcz};
    const float w = m.hx > 0.5f ? m.hx : m.road_w;   // half the road's width: its kerb
    struct Ed { int id; V2 n, e; float h, L; };
    const Ed eds[4] = {{1, u, v, a.hx, a.hz}, {2, u * -1.f, v, a.hx, a.hz}, {3, v, u, a.hz, a.hx}, {4, v * -1.f, u, a.hz, a.hx}};
    for (const Ed& E : eds) {
        const V2 M = C + E.n * E.h;
        const V2 rel = fc - M;
        const float perp = dot(rel, E.n), tf = dot(rel, E.e);
        if (std::fabs(perp) > 60.f || std::fabs(tf) > E.L + std::max(m.road_l, 40.f)) continue;
        const float side = perp >= 0 ? 1.f : -1.f;   // outside or inside: the side the free centre is on
        if (m.straight) {
            // Beside: the kerb on the edge line, the road parallel to it, sliding along the edge with the cursor. The distance that counts is
            // the free centre's to the edge line, so a cursor near the edge pulls the road alongside it.
            const float t = std::clamp(tf, -E.L, E.L);
            const V2 T = M + E.e * t;
            for (int dir = 1; dir >= -1; dir -= 2) {
                const V2 D = E.e * float(dir);
                const float yaw = ang(D.x, D.z);
                const V2 wc = T + E.n * (side * w);
                out.push_back(anchor_cand(m, a, kOwnCentre, yaw, wc.x, wc.z, T.x, T.z, Kind::Beside, E.id, E.n * side));
            }
            // The road's end flush with a plate corner (the road lies along the edge, either way from the corner).
            for (int cs = 1; cs >= -1; cs -= 2) {
                const V2 K = M + E.e * (float(cs) * E.L);
                const int sx = E.id <= 2 ? (E.id == 1 ? 1 : -1) : cs, sz = E.id <= 2 ? cs : (E.id == 3 ? 1 : -1);
                const V2 Tt = K + E.n * (side * w);
                for (int dir = 1; dir >= -1; dir -= 2) {
                    const V2 D = E.e * float(dir);
                    for (int i = 0; i < 2; i++) {
                        const float yaw = ang(-D.x, -D.z) - ang(m.own[size_t(i)].ox, m.own[size_t(i)].oz);
                        const V2 wc = centre_for(m, i, yaw, Tt.x, Tt.z);
                        out.push_back(anchor_cand(m, a, i, yaw, wc.x, wc.z, Tt.x, Tt.z, Kind::Corner, corner_id(sx, sz), E.n * side + E.e * float(cs)));
                    }
                }
            }
        }
        // An end of the road on the edge line, the road running out from it (perpendicular, outside).
        for (size_t i = 0; i < m.own.size(); i++) {
            const float yaw = ang(-E.n.x, -E.n.z) - ang(m.own[i].ox, m.own[i].oz);
            float lx, lz;
            rot(fyaw, m.own[i].x - m.cx, m.own[i].z - m.cz, lx, lz);
            const V2 Jf = fc + V2{lx, lz};
            const float t = std::clamp(dot(Jf - M, E.e), -E.L, E.L);
            const V2 T = M + E.e * t;
            V2 wc = centre_for(m, int(i), yaw, T.x, T.z);
            out.push_back(anchor_cand(m, a, int(i), yaw, wc.x, wc.z, T.x, T.z, Kind::EndOnEdge, E.id, E.n));
            wc = centre_for(m, int(i), yaw, M.x, M.z);
            out.push_back(anchor_cand(m, a, int(i), yaw, wc.x, wc.z, M.x, M.z, Kind::Edge, E.id, E.n));
        }
    }
}

void wall_corner(const Mover& m, const Anchor& a, std::vector<Cand>& out) {
    if (m.fam != 1 || m.own.size() < 2) return;
    const V2 u = uvec(a.yaw), v = vvec(a.yaw), C{a.cx, a.cz};
    const float thick = std::min(m.hx, m.hz);   // half the wall's thickness
    for (int sx = 1; sx >= -1; sx -= 2)
        for (int sz = 1; sz >= -1; sz -= 2) {
            const V2 K = C + u * (float(sx) * a.hx) + v * (float(sz) * a.hz);
            const V2 sdir = u * float(sx) + v * float(sz);
            struct Face { V2 n, in; };
            const Face fs[2] = {{u * float(sx), v * float(-sz)}, {v * float(sz), u * float(-sx)}};   // the two edges meeting at the corner
            for (const Face& f : fs)
                for (int over = 1; over >= 0; over--) {   // over the face (inner face on it) or on along the edge line away from the corner
                    const V2 D = over ? f.in : f.in * -1.f;
                    V2 T = K;
                    if (over) T = K + f.n * (a.kind == 5 ? thick : -thick);   // a building's wall stands outside it, a plate's fence inside its rim
                    for (size_t i = 0; i < 2; i++) {
                        const float yaw = ang(-D.x, -D.z) - ang(m.own[i].ox, m.own[i].oz);
                        const V2 wc = centre_for(m, int(i), yaw, T.x, T.z);
                        out.push_back(anchor_cand(m, a, int(i), yaw, wc.x, wc.z, T.x, T.z, Kind::Corner, corner_id(sx, sz), sdir));
                    }
                }
        }
}

float edge_overlap(float ax, float az, float aox, float aoz, float alen, float bx, float bz, float box, float boz, float blen) {
    if (aox * box + aoz * boz > -0.99f) return 0;   // facing each other
    const float perp = (bx - ax) * aox + (bz - az) * aoz;
    if (std::fabs(perp) > 0.1f) return 0;           // on one line
    const float ex = aoz, ez = -aox;
    const float ta = ax * ex + az * ez, tb = bx * ex + bz * ez;
    const float lo = std::max(ta - alen, tb - blen), hi = std::min(ta + alen, tb + blen);
    return std::max(0.f, hi - lo);
}

int choose(std::vector<Cand>& c, const Mover& m, float fcx, float fcz, float fyaw, float reach_px, const Prev& prev, const ToPx& to_px) {
    const float cap = cap_m(m);
    const float cs = std::cos(fyaw), sn = std::sin(fyaw);
    auto same_as_prev = [&](const Cand& k) {
        return prev.on && k.uid == prev.uid && k.socket == prev.socket && k.own == prev.own && k.kind == prev.kind &&
               std::fabs(wrap_pi(k.yaw - prev.yaw)) < 1e-3f;
    };
    for (Cand& k : c) {
        k.ok = false;
        k.d_m = k.d_px = k.dj_px = 1e9f;
        k.prio = prio_of(k.kind);
        if (k.own < 0 || (k.own != kOwnCentre && size_t(k.own) >= m.own.size())) continue;
        const Socket o = own_point(m, k.own);
        const float lx = o.x - m.cx, lz = o.z - m.cz;
        const float jx = fcx + cs * lx + sn * lz, jz = fcz - sn * lx + cs * lz;   // the joint on the free ghost
        k.d_m = std::hypot(jx - k.tx, jz - k.tz);
        float ax, ay, bx, by;
        if (!to_px(jx, k.ty, jz, ax, ay) || !to_px(k.tx, k.ty, k.tz, bx, by)) continue;
        k.d_px = k.dj_px = std::hypot(ax - bx, ay - by);
        if (std::fabs(wrap_pi(k.yaw - fyaw)) > kYawTol) continue;
        const bool same = same_as_prev(k);
        const float h = same ? kHyst : 1.f;
        bool ok = k.d_px <= reach_px * h && k.d_m <= cap * h;
        // Aiming (design decision): a ghost whose footprint centre (the cursor) is near an open end joins at that end as well, by
        // its own end meeting it: users point at the end, not with the far end of the piece. Same reach and cap; roads, walls, bridges.
        if (!k.anchor && k.kind == Kind::End) {
            float cx2, cy2;
            if (to_px(fcx, k.ty, fcz, cx2, cy2)) {
                const float dc_px = std::hypot(cx2 - bx, cy2 - by), dc_m = std::hypot(fcx - k.tx, fcz - k.tz);
                if (dc_px <= reach_px * h && dc_m <= cap * h) {
                    if (!ok || dc_px < k.d_px) { k.d_px = dc_px; k.d_m = dc_m; }
                    ok = true;
                }
            }
        }
        if (prev.pinned && k.uid == prev.puid && k.socket == prev.psock) {
            // The turn key was pressed here: the joint stays the target while the cursor stays near where it was.
            const float pm = std::hypot(fcx - prev.pfx, fcz - prev.pfz);
            float px0, py0, px1, py1;
            if (to_px(fcx, prev.pfy, fcz, px0, py0) && to_px(prev.pfx, prev.pfy, prev.pfz, px1, py1)) {
                const float pp = std::hypot(px0 - px1, py0 - py1);
                if (pp <= reach_px && pm <= cap) {
                    ok = true;
                    k.d_px = std::min(k.d_px, pp);
                    k.d_m = std::min(k.d_m, pm);
                }
            }
        }
        k.ok = ok;
    }
    float best = 1e9f;
    for (const Cand& k : c)
        if (k.ok) best = std::min(best, k.d_px);
    if (best >= 1e8f) return -1;
    int win = -1;
    for (size_t i = 0; i < c.size(); i++)
        if (c[i].ok && same_as_prev(c[i]) && c[i].d_px <= best + kTiePx) return int(i);   // last frame's choice is kept
    for (size_t i = 0; i < c.size(); i++) {
        const Cand& k = c[i];
        if (!k.ok || k.d_px > best + kTiePx) continue;
        if (win < 0) { win = int(i); continue; }
        const Cand& w = c[size_t(win)];
        if (k.prio != w.prio ? k.prio < w.prio : k.uid != w.uid ? k.uid < w.uid : k.dj_px < w.dj_px) win = int(i);
    }
    return win;
}

float next_yaw(const std::vector<Cand>& c, int winner, float cur_yaw, int dir) {
    if (winner < 0 || size_t(winner) >= c.size() || dir == 0) return NAN;
    const Cand& w = c[size_t(winner)];
    const float tp = 2.f * kPi;
    float best = 1e9f, yaw = NAN;
    for (const Cand& k : c) {
        if (k.uid != w.uid || k.socket != w.socket) continue;
        float off = std::fmod(k.yaw - cur_yaw, tp);
        if (off < 0) off += tp;                        // [0, 2 pi)
        if (off < 1e-3f || off > tp - 1e-3f) continue; // the same heading
        const float step = dir > 0 ? off : tp - off;   // how far it is in the pressed direction
        if (step < best) { best = step; yaw = k.yaw; }
    }
    return yaw;
}

float grid_round(float v, float step) {
    if (step <= 0) return v;
    return std::round(v / step) * step;
}

// A group (or glued set) only slides: drop every candidate that would turn the anchor by more than one degree.
void filter_translation(std::vector<Cand>& c, float yaw) {
    constexpr float kOneDeg = 0.0174533f;
    c.erase(std::remove_if(c.begin(), c.end(), [&](const Cand& k) { return std::fabs(wrap_pi(k.yaw - yaw)) > kOneDeg; }), c.end());
}

float size_step(float kept, float step, float min_side) {
    if (step <= 0) return std::max(kept, min_side);
    return std::max(min_side, float(std::round(double(kept) / double(step)) * double(step)));
}

namespace {
// The offset of a target from the dragged side: ok when it applies at all (lateral / parallel tests).
bool edge_offset(const EdgeDrag& e, const EdgeTarget& t, float& s) {
    if (t.kind == 0) {
        const float dx = t.x - e.ex, dz = t.z - e.ez;
        const float lat = std::fabs(dx * e.tx + dz * e.tz);
        if (lat > std::max(kEdgeLateralMin, e.half_t)) return false;
        s = dx * e.nx + dz * e.nz;
        return true;
    }
    const float l = std::hypot(t.dx, t.dz);
    if (l < 1e-6f) return false;
    const float cosang = std::fabs(t.dx * e.tx + t.dz * e.tz) / l;
    if (cosang < std::cos(kEdgeParallelDeg * kPi / 180.f)) return false;
    s = (t.x - e.ex) * e.nx + (t.z - e.ez) * e.nz;
    return true;
}
}  // namespace

bool edge_snap(const EdgeDrag& e, const std::vector<EdgeTarget>& t, float reach_px, float px_per_m, const EdgeHit* prev, EdgeHit& out) {
    const float reach_m = px_per_m > 1e-6f ? reach_px / px_per_m : 0.f;
    const float lim = std::min(reach_m, kCapMax);
    int best = -1;
    float best_s = 0;
    for (size_t i = 0; i < t.size(); i++) {
        float s;
        if (t[i].uid == e.uid || !edge_offset(e, t[i], s)) continue;
        if (std::fabs(s) > lim) continue;
        if (best < 0 || std::fabs(s) < std::fabs(best_s)) { best = int(i); best_s = s; }
    }
    // The previous hit stays while it is within kHyst x the limits, unless a fresh one is clearly closer (kTiePx).
    if (prev && prev->kind >= 0) {
        for (size_t i = 0; i < t.size(); i++) {
            if (t[i].kind != prev->kind || t[i].uid != prev->uid || t[i].index != prev->index) continue;
            float s;
            if (!edge_offset(e, t[i], s)) break;
            if (std::fabs(s) <= kHyst * lim && (best < 0 || best == int(i) || (std::fabs(best_s) + kTiePx / std::max(px_per_m, 1e-6f)) >= std::fabs(s))) { best = int(i); best_s = s; }
            break;
        }
    }
    if (best < 0) return false;
    out.s = best_s;
    out.kind = t[size_t(best)].kind;
    out.uid = t[size_t(best)].uid;
    out.index = t[size_t(best)].index;
    out.d_px = std::fabs(best_s) * px_per_m;
    return true;
}

float angle_round(float yaw, float step) {
    if (step <= 0) return wrap_pi(yaw);
    double v = std::round(double(yaw) / double(step)) * double(step);
    const double pi = 3.14159265358979323846;
    while (v > pi + 1e-4) v -= 2 * pi;
    while (v <= -pi + 1e-4) v += 2 * pi;
    return float(v);
}

}  // namespace ma2::snap

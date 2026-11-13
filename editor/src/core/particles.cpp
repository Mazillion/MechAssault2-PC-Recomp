#include "particles.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ma2 {

namespace {

struct Xml {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Xml> kids;
    const std::string* attr(const char* n) const {
        for (auto& a : attrs) if (a.first == n) return &a.second;
        return nullptr;
    }
    const Xml* child(const char* t) const {
        for (auto& k : kids) if (k.tag == t) return &k;
        return nullptr;
    }
    float num(const char* n, float def = 0) const {
        const std::string* a = attr(n);
        return a ? float(std::atof(a->c_str())) : def;
    }
    std::string str(const char* n) const {
        const std::string* a = attr(n);
        return a ? *a : std::string();
    }
};

// Elements and attributes only (the .mgparticle files have no text nodes that matter).
bool parse_xml(const std::string& s, Xml& root) {
    size_t i = 0;
    std::vector<Xml*> stack;
    Xml top;
    stack.push_back(&top);
    auto skip_ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++; };
    while (i < s.size()) {
        size_t lt = s.find('<', i);
        if (lt == std::string::npos) break;
        i = lt + 1;
        if (i >= s.size()) break;
        if (s[i] == '?') { size_t e = s.find("?>", i); if (e == std::string::npos) return false; i = e + 2; continue; }
        if (s.compare(i, 3, "!--") == 0) { size_t e = s.find("-->", i); if (e == std::string::npos) return false; i = e + 3; continue; }
        if (s[i] == '/') {
            size_t e = s.find('>', i);
            if (e == std::string::npos || stack.size() < 2) return false;
            stack.pop_back();
            i = e + 1;
            continue;
        }
        Xml el;
        size_t t0 = i;
        while (i < s.size() && !strchr(" \t\r\n/>", s[i])) i++;
        el.tag = s.substr(t0, i - t0);
        bool self_close = false;
        for (;;) {
            skip_ws();
            if (i >= s.size()) return false;
            if (s[i] == '/') { self_close = true; i++; continue; }
            if (s[i] == '>') { i++; break; }
            size_t a0 = i;
            while (i < s.size() && s[i] != '=' && s[i] != '>' && s[i] != '/' && s[i] != ' ') i++;
            std::string name = s.substr(a0, i - a0);
            skip_ws();
            std::string val;
            if (i < s.size() && s[i] == '=') {
                i++;
                skip_ws();
                if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
                    char q = s[i++];
                    size_t v0 = i;
                    while (i < s.size() && s[i] != q) i++;
                    val = s.substr(v0, i - v0);
                    i++;
                }
            }
            if (!name.empty()) el.attrs.emplace_back(std::move(name), std::move(val));
        }
        stack.back()->kids.push_back(std::move(el));
        if (!self_close) stack.push_back(&stack.back()->kids.back());
    }
    if (top.kids.empty()) return false;
    root = std::move(top.kids[0]);
    return true;
}

int parse_floats(const std::string& s, float* out, int max_n) {
    int n = 0;
    const char* p = s.c_str();
    while (n < max_n && *p) {
        char* e = nullptr;
        float v = std::strtof(p, &e);
        if (e == p) { p++; continue; }
        out[n++] = v;
        p = e;
        while (*p == ',' || *p == ' ') p++;
    }
    return n;
}

PCurve read_curve(const Xml* x, int comps) {
    PCurve c;
    if (!x) return c;
    c.variance = x->num("variance");
    for (auto& k : x->kids) {
        if (k.tag != "key") continue;
        PCurve::Key key;
        key.t = k.num("time");
        std::string v = k.str("value");
        float f[4] = {0, 0, 0, 0};
        int n = parse_floats(v, f, comps);
        for (int i = 0; i < 4; i++) key.v[i] = i < n ? f[i] : (n ? f[n - 1] : 0);
        c.keys.push_back(key);
    }
    return c;
}

bool is_true(const std::string& s) { return s == "true" || s == "1" || s == "yes"; }

}  // namespace

float PCurve::eval(float t, int comp) const {
    if (keys.empty()) return 0;
    if (t <= keys.front().t) return keys.front().v[comp];
    for (size_t i = 1; i < keys.size(); i++)
        if (t <= keys[i].t) {
            float span = keys[i].t - keys[i - 1].t;
            float u = span > 1e-6f ? (t - keys[i - 1].t) / span : 1.f;
            return keys[i - 1].v[comp] + (keys[i].v[comp] - keys[i - 1].v[comp]) * u;
        }
    return keys.back().v[comp];
}

namespace {

bool emitter_draws(const PEmitter& e, bool retrigger) {
    if (!e.looped && !retrigger) return false;
    for (auto& s : e.systems) {
        if (s.has_sprite) return true;
        for (auto& sub : s.subs)
            if (emitter_draws(sub, retrigger)) return true;
    }
    return false;
}

void parse_emitter(const Xml& e, PEmitter& em, float& max_life, int depth) {
    em.looped = e.str("cycle_mode") == "MODE_LOOPED";
    em.cycle_length = std::max(0.05f, e.num("cycle_length", 1));
    em.worldspace = is_true(e.str("emit_worldspace"));
    if (const Xml* src = e.child("source")) {
        std::string ty = src->str("type");
        em.source = ty == "mgBoxSource" ? PEmitter::Box : ty == "mgSphereSource" ? PEmitter::Sphere : ty == "mgDiskSource" ? PEmitter::Disk : PEmitter::Point;
        em.length[0] = read_curve(src->child("length_x"), 1);
        em.length[1] = read_curve(src->child("length_y"), 1);
        em.length[2] = read_curve(src->child("length_z"), 1);
        em.pitch_start = read_curve(src->child("pitch_start"), 1);
        em.pitch_width = read_curve(src->child("pitch_width"), 1);
        em.radius_start = read_curve(src->child("radius_start"), 1);
        em.radius_width = read_curve(src->child("radius_width"), 1);
    }
    for (auto& s : e.kids) {
        if (s.tag != "system") continue;
        PSystem sy;
        sy.kinematic = s.str("type") == "mgKinematicSystem";
        sy.gravity = s.num("gravity");
        sy.drag = s.num("drag");
        sy.inherit = s.num("inherit_velocity");
        float off[3] = {0, 0, 0};
        parse_floats(s.str("offset"), off, 3);
        for (int i = 0; i < 3; i++) sy.offset[i] = off[i];
        sy.rate = read_curve(s.child("rate"), 1);
        sy.lifespan = read_curve(s.child("lifespan"), 1);
        sy.velocity = read_curve(s.child("initial_velocity"), 1);
        for (auto& p : s.kids) {
            if (p.tag != "prototype") continue;
            std::string pt = p.str("type");
            if (pt == "mgEmitterType" && depth < 3) {
                for (auto& se : p.kids)
                    if (se.tag == "emitter") {
                        PEmitter sub;
                        parse_emitter(se, sub, max_life, depth + 1);
                        sy.subs.push_back(std::move(sub));
                    }
            }
            if (pt != "mgSpriteType" || sy.has_sprite) continue;
            PSprite sp;
            sp.size = read_curve(p.child("size"), 1);
            sp.aspect = read_curve(p.child("aspect_ratio"), 1);
            sp.spin = read_curve(p.child("spin"), 1);
            sp.color = read_curve(p.child("color"), 4);
            sp.spin_offset = p.num("spin_offset");
            float pv[2] = {0, 0};
            parse_floats(p.str("pivot"), pv, 2);
            sp.pivot[0] = pv[0];
            sp.pivot[1] = pv[1];
            sp.orient_to_velocity = is_true(p.str("orient_to_velocity"));
            sp.stretch = p.num("stretch_to_velocity", 1);
            if (const Xml* m = p.child("material")) {
                std::string bm = m->str("blend_mode");
                sp.blend = bm == "additive" ? 1 : bm == "subtractive" ? 2 : 0;
                if (const Xml* bt = m->child("base_texture")) sp.texture = bt->str("name");
            }
            sy.has_sprite = !sp.texture.empty() && !sp.size.empty() && !sp.color.empty();
            sy.sprite = std::move(sp);
        }
        if (sy.rate.empty() || sy.lifespan.empty()) continue;
        float life_max = 0;
        for (auto& k : sy.lifespan.keys) life_max = std::max(life_max, k.v[0]);
        max_life = std::max(max_life, life_max * (1.f + sy.lifespan.variance));
        em.systems.push_back(std::move(sy));
    }
}

}  // namespace

bool ParticleFx::loops() const {
    for (auto& e : emitters)
        if (emitter_draws(e, false)) return true;
    return false;
}

bool ParticleFx::has_sprites() const {
    for (auto& e : emitters)
        if (emitter_draws(e, true)) return true;
    return false;
}

bool parse_particle(const std::string& xml, ParticleFx& out) {
    out = ParticleFx{};
    Xml root;
    if (!parse_xml(xml, root) || root.tag != "mgParticle") return false;
    out.lod_start = root.num("lod_start", 250);
    out.lod_range = root.num("lod_range", 100);
    for (auto& e : root.kids) {
        if (e.tag != "emitter") continue;
        PEmitter em;
        parse_emitter(e, em, out.max_life, 0);
        out.emitters.push_back(std::move(em));
    }
    return !out.emitters.empty();
}

namespace {

uint32_t hash32(uint32_t h) {
    h += 0x9E3779B9u;
    h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}
// Uniform in [0, 1) for (seed, slot, salt).
float rnd(uint32_t seed, int32_t k, uint32_t salt) {
    return float(hash32(seed * 0x9E3779B1u ^ hash32(uint32_t(k) * 0x85EBCA6Bu + salt * 0xC2B2AE35u)) >> 8) * (1.f / 16777216.f);
}
float vary(float v, float variance, float r) { return v * (1.f + variance * (2.f * r - 1.f)); }

}  // namespace

namespace {

// One emitter at `origin`, clock t seconds since it started (a sub-emitter: since its parent particle was born).
void emit(const PEmitter& em, const float origin[3], float t, uint32_t seed, float fade, float on_time, float off_time, bool retrigger,
          std::vector<PSpriteOut>& out, int depth, const float* rot) {
    if (!em.looped && !retrigger && depth == 0) return;
    const float period = on_time + off_time > 0.05f ? on_time + off_time : em.cycle_length * 2.f;
    uint32_t ss = 0;
    for (const PSystem& sy : em.systems) {
        ss++;
        if (!sy.has_sprite && sy.subs.empty()) continue;
        float R = 0;
        for (auto& k : sy.rate.keys) R = std::max(R, k.v[0] * (1.f + sy.rate.variance));
        if (R <= 0.01f) continue;
        float life_max = 0;
        for (auto& k : sy.lifespan.keys) life_max = std::max(life_max, k.v[0]);
        life_max *= 1.f + sy.lifespan.variance;
        if (life_max <= 0.01f) continue;
        uint32_t sd = hash32(seed + ss * 31u);
        int32_t k1 = int32_t(std::floor(t * R)), k0 = int32_t(std::ceil((t - life_max) * R));
        if (k1 - k0 > 900) k0 = k1 - 900;
        for (int32_t k = k0; k <= k1; k++) {
            float ts = float(k) / R, age = t - ts;
            if (age < 0) continue;
            float phase = std::fmod(ts, em.cycle_length) / em.cycle_length;
            if (phase < 0) phase += 1.f;
            if (!em.looped && depth == 0) {  // restarted every `period` seconds, emitting for cycle_length of each
                float local = std::fmod(ts + float(sd & 1023) * 0.0173f, period);
                if (local < 0) local += period;
                if (local >= em.cycle_length) continue;
                phase = local / em.cycle_length;
            } else if (!em.looped && ts > em.cycle_length) continue;  // a one-way sub-emitter runs once from its parent's birth
            if (rnd(sd, k, 1) * R >= sy.rate.eval(phase)) continue;  // thinning by the (possibly varying) rate
            if (depth == 0 && em.looped && on_time > 0 && off_time > 0) {  // intermittent effect: spawn only while "alive"
                float ph = std::fmod(ts + float(sd & 1023) * 0.0173f, on_time + off_time);
                if (ph < 0) ph += on_time + off_time;
                if (ph > on_time) continue;
            }
            float life = std::max(0.05f, vary(sy.lifespan.eval(phase), sy.lifespan.variance, rnd(sd, k, 2)));
            if (age >= life) continue;
            float f = age / life;
            // Spawn point in the source volume.
            float sp[3] = {0, 0, 0}, dir[3] = {0, 1, 0};
            float r1 = rnd(sd, k, 3), r2 = rnd(sd, k, 4), r3 = rnd(sd, k, 5), r4 = rnd(sd, k, 6), r5 = rnd(sd, k, 7);
            if (em.source == PEmitter::Box) {
                sp[0] = (r1 - 0.5f) * em.length[0].eval(phase);
                sp[1] = (r2 - 0.5f) * em.length[1].eval(phase);
                sp[2] = (r3 - 0.5f) * em.length[2].eval(phase);
            } else if (em.source == PEmitter::Sphere || em.source == PEmitter::Disk) {
                float rad = em.radius_start.eval(phase) + em.radius_width.eval(phase) * r1;
                float az = r2 * 6.2831853f;
                if (em.source == PEmitter::Sphere) {
                    float cz = r3 * 2.f - 1.f, sz = std::sqrt(std::max(0.f, 1.f - cz * cz));
                    sp[0] = std::cos(az) * sz * rad; sp[1] = cz * rad; sp[2] = std::sin(az) * sz * rad;
                    if (rad > 1e-3f) { dir[0] = sp[0] / rad; dir[1] = sp[1] / rad; dir[2] = sp[2] / rad; }
                } else {
                    sp[0] = std::cos(az) * rad; sp[2] = std::sin(az) * rad;
                }
            }
            if (em.source != PEmitter::Sphere) {
                // Cone about +Y: degrees from the axis, pitch_start .. pitch_start + pitch_width (180 = every direction), even over the solid angle.
                float c0 = std::cos(em.pitch_start.eval(phase) * 0.0174533f), c1 = std::cos((em.pitch_start.eval(phase) + em.pitch_width.eval(phase)) * 0.0174533f);
                float th = std::acos(std::clamp(c0 + (c1 - c0) * r4, -1.f, 1.f));
                float az = r5 * 6.2831853f;
                dir[0] = std::sin(th) * std::cos(az); dir[1] = std::cos(th); dir[2] = std::sin(th) * std::sin(az);
            }
            float speed = vary(sy.velocity.eval(phase), sy.velocity.variance, rnd(sd, k, 8));
            float travel = sy.drag > 1e-3f ? speed * (1.f - std::exp(-sy.drag * age)) / sy.drag : speed * age;
            float vcur = speed * (sy.drag > 1e-3f ? std::exp(-sy.drag * age) : 1.f);
            PSpriteOut o;
            float lp[3], lv[3];
            for (int i = 0; i < 3; i++) { lp[i] = sp[i] + sy.offset[i] + dir[i] * travel; lv[i] = dir[i] * vcur; }
            if (rot) {
                float rp[3], rv[3];
                for (int i = 0; i < 3; i++) {
                    rp[i] = rot[i * 3] * lp[0] + rot[i * 3 + 1] * lp[1] + rot[i * 3 + 2] * lp[2];
                    rv[i] = rot[i * 3] * lv[0] + rot[i * 3 + 1] * lv[1] + rot[i * 3 + 2] * lv[2];
                }
                for (int i = 0; i < 3; i++) { lp[i] = rp[i]; lv[i] = rv[i]; }
            }
            for (int i = 0; i < 3; i++) { o.pos[i] = origin[i] + lp[i]; o.vel[i] = lv[i]; }
            o.pos[1] -= 0.5f * sy.gravity * age * age;
            o.vel[1] -= sy.gravity * age;
            // Sub-emitters ride on this particle, their clock starting at its birth.
            for (size_t si = 0; si < sy.subs.size() && depth < 3; si++)
                emit(sy.subs[si], o.pos, age, hash32(sd + uint32_t(k) * 7919u + uint32_t(si)), fade, 0, 0, false, out, depth + 1, rot);
            if (!sy.has_sprite) continue;
            const PSprite& spr = sy.sprite;
            float size = std::max(0.f, vary(spr.size.eval(f), spr.size.variance, rnd(sd, k, 9)));
            float asp = spr.aspect.empty() ? 1.f : std::max(0.01f, vary(spr.aspect.eval(f), spr.aspect.variance, rnd(sd, k, 10)));
            o.h = size;
            o.w = size * asp;
            // Spin: random start angle, then the integral of the spin curve over the life so far.
            float ang = spr.spin_offset * rnd(sd, k, 11);
            if (!spr.spin.empty()) {
                const int N = 6;
                float acc = 0;
                for (int i = 0; i < N; i++) acc += spr.spin.eval(f * (float(i) + 0.5f) / N);
                ang += acc / N * age;
            }
            o.angle = ang;
            for (int c2 = 0; c2 < 4; c2++) o.rgba[c2] = std::clamp(spr.color.eval(f, c2) / 255.f, 0.f, 1.f);
            o.rgba[3] *= fade;
            o.sprite = &spr;
            if (o.rgba[3] <= 0.004f || o.w <= 0 || o.h <= 0) continue;
            out.push_back(o);
        }
    }
}

}  // namespace

void simulate_particle(const ParticleFx& fx, const float origin[3], float t, uint32_t seed, float fade,
                       float on_time, float off_time, bool retrigger, std::vector<PSpriteOut>& out, const float* rot) {
    if (fade <= 0) return;
    uint32_t es = 0;
    for (const PEmitter& em : fx.emitters) {
        es++;
        emit(em, origin, t, seed + es * 977u, fade, on_time, off_time, retrigger, out, 0, rot);
    }
}

}  // namespace ma2

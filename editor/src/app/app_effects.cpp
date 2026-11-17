// View > Effects: the game's particle effects over the level (of the render-parity list).
//
// Each placed object whose model's .mtb carries MAModSubObjectParticleEffect entries (the fire on
// a wrecked tower's floors, the oil fires of the crash scar, smoke stacks, energy pickups) starts
// those effects when the level loads. The editor reads the same .mgparticle files and draws the
// looped sprite emitters as camera-facing quads (core/particles.cpp). One-shot emitters (debris,
// dust bursts, window shatter) belong to destruction and damage, so they are not drawn.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../core/weather.h"
#include "app.h"
#include "imgui.h"

namespace ed {

using namespace ma2;
using namespace ma2::world;

namespace {
// Effects the game starts only on units in a state the editor does not show: damage fires and
// sparks on 'Mechs and people, foot splashes, rain-only effects.
bool wanted(const FxRef& r) { return !r.damage_state && !r.terrain_only && !r.rain_only; }
// 'Mechs, vehicles, turrets and people carry damage fire / smoke / sparks in their .mtb: those
// effects switch on with their damage level, which a placed unit does not have at the start.
bool is_unit(uint32_t cls) {
    static const uint32_t K[] = {key_of("MAMech"), key_of("MATank"), key_of("MAFlyer"), key_of("MAVehicle"), key_of("MATurret"), key_of("MAPerson")};
    for (uint32_t k : K) if (cls == k) return true;
    return false;
}
}  // namespace

void App::add_effects() {
    r_.sprites.clear();
    if (!doc_ || !show_effects_ || !show_models_) return;
    const float t = float(ImGui::GetTime());
    const XMFLOAT3 eye = cam_.eye;
    FILE* dump = dump_effects_.empty() ? nullptr : fopen(dump_effects_.c_str(), "w");
    std::vector<PSpriteOut> out;
    size_t budget = 40000;  // sprites per frame
    for (const ObjView& v : views_) {
        if (v.index < 0 || v.fx_model.empty()) continue;
        const std::vector<FxRef>* refs = models_.effects(v.fx_model, v.fx_mtb);
        if (!refs || refs->empty()) continue;
        const WObj& o = doc_->world.objs[size_t(v.index)];
        if (is_unit(o.cls)) continue;
        BoxInstance mi;
        orient_instance(mi, v.heading, v.pitch, v.roll, v.pos);
        for (const FxRef& r : *refs) {
            const ParticleFx* fx = models_.particle(r.file);
            // A placed MAEffect whose effect is a one-way burst (Mission_15 Nat Shield, Arena_Dais_Shield: 4 s cycle) is still running in the game
            // seconds after the level starts (trace t=8: ~300 steam quads over the facade), so it repeats here once per cycle.
            const bool one_way_effect = fx && o.cls == key_of("MAEffect") && !fx->loops() && fx->has_sprites() && !r.retrigger && !r.damage_state;
            bool shown = fx && (fx->loops() || one_way_effect || (r.retrigger && fx->has_sprites())) && wanted(r);
            if (dump) {
                const DictClass* dc = dict_.cls(o.cls);
                fprintf(dump, "%u %s %s -> %s loops %d damage %d terrain %d rain %d beh %d%s\n", o.uid(), dc ? dc->name.c_str() : "?", v.fx_model.c_str(),
                        r.file.c_str(), fx && fx->loops() ? 1 : 0, r.damage_state, r.terrain_only, r.rain_only, r.intermittent ? 1 : 0, shown ? " SHOWN" : "");
            }
            if (dump && fx)
                for (const ma2::PEmitter& em : fx->emitters) {
                    fprintf(dump, "   emitter looped %d cycle %.2f src %d systems %zu\n", em.looped ? 1 : 0, em.cycle_length, int(em.source), em.systems.size());
                    for (const ma2::PSystem& sy : em.systems)
                        fprintf(dump, "     system sprite %d blend %d rate %.1f life %.2f vel %.2f size %.2f alpha0 %.0f tex %s\n", sy.has_sprite ? 1 : 0, sy.sprite.blend, sy.rate.first(), sy.lifespan.first(), sy.velocity.first(), sy.sprite.size.first(), sy.sprite.color.keys.empty() ? 0.f : sy.sprite.color.keys[0].v[3], sy.sprite.texture.c_str());
                }
            if (!shown) continue;
            float org[3] = {mi.r0.x * r.pos[0] + mi.r0.y * r.pos[1] + mi.r0.z * r.pos[2] + mi.r0.w,
                            mi.r1.x * r.pos[0] + mi.r1.y * r.pos[1] + mi.r1.z * r.pos[2] + mi.r1.w,
                            mi.r2.x * r.pos[0] + mi.r2.y * r.pos[1] + mi.r2.z * r.pos[2] + mi.r2.w};
            float dx = org[0] - eye.x, dy = org[1] - eye.y, dz = org[2] - eye.z;
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            float reach = fx->lod_start + fx->lod_range;
            if (d > reach) continue;
            float fade = d <= fx->lod_start ? 1.f : 1.f - (d - fx->lod_start) / std::max(1.f, fx->lod_range);
            out.clear();
            // emitter frame = object orientation x node frame (mi rows are the column-vector matrix: world = mi * model)
            const float M[9] = {mi.r0.x, mi.r0.y, mi.r0.z, mi.r1.x, mi.r1.y, mi.r1.z, mi.r2.x, mi.r2.y, mi.r2.z};
            float rot[9];
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) rot[a * 3 + b] = M[a * 3] * r.rot[b] + M[a * 3 + 1] * r.rot[3 + b] + M[a * 3 + 2] * r.rot[6 + b];
            simulate_particle(*fx, org, t, o.uid() * 2654435761u + uint32_t(&r - refs->data()) * 40503u, fade, r.intermittent ? r.on_s : one_way_effect ? fx->emitters[0].cycle_length : 0.f,
                              r.intermittent ? r.off_s : 0.f, r.retrigger || one_way_effect, out, rot);
            for (const PSpriteOut& s : out) {
                if (budget == 0) break;
                ID3D11ShaderResourceView* tex = models_.texture(s.sprite->texture);
                if (!tex) continue;
                Renderer::Sprite sp;
                sp.p = {s.pos[0], s.pos[1], s.pos[2]};
                sp.vel = {s.vel[0], s.vel[1], s.vel[2]};
                sp.w = s.w;
                sp.h = s.h;
                sp.angle = s.angle;
                sp.pivot_x = s.sprite->pivot[0];
                sp.pivot_y = s.sprite->pivot[1];
                sp.orient = s.sprite->orient_to_velocity;
                for (int k = 0; k < 4; k++) sp.rgba[k] = s.rgba[k];
                sp.tex = tex;
                sp.blend = s.sprite->blend;
                r_.sprites.push_back(sp);
                budget--;
            }
        }
    }
    add_precip_effect(out, budget, dump);
    if (dump) fclose(dump);
}

// View > Weather with Snow: the game's PrecipEffectID 31780 (Env_Snow_Blowing) follows the camera;
// its model's .mtb starts the snow particle effects. The editor places those effects at the eye.
// Rain is the same effect with the snow particle turned into rain for the map's rain level (playtest: the
// editor's rain must look like the game's), exactly as the save writes it.
void App::add_precip_effect(std::vector<PSpriteOut>& out, size_t& budget, FILE* dump) {
    precip_3d_ = false;
    precip_stats_ = {};
    if (!show_weather_ || !show_effects_) return;
    int ei = find_class(doc_->world, K_ENV);
    if (ei < 0) return;
    const WObj& env = doc_->world.objs[size_t(ei)];
    const weather::Precip p = weather::precip(env);
    if (p != weather::Precip::Snow && p != weather::Precip::Rain && p != weather::Precip::Dust) return;  // dust: the snow effect turned brown
    const int rain = p == weather::Precip::Rain ? int(weather::rain_level(env)) : -1;
    static const char* kModel = "SpecialEffects/Environment/Env_Precip_Snow_01/Env_Precip_Snow_01.MGMODEL";
    static const char* kMtb = "SpecialEffects/Environment/Env_Precip_Snow_01/Env_Precip_Snow_01.mtb";
    models_.get(kModel, kMtb);  // queued until loaded
    const std::vector<FxRef>* refs = models_.effects(kModel, kMtb);
    if (!refs) return;
    const float t = float(ImGui::GetTime());
    for (const FxRef& r : *refs) {
        const bool snow_part = _stricmp(r.file.c_str(), "SpecialEffects/Environment/Env_Snow_01.mgparticle") == 0 ||
                               _stricmp(r.file.c_str(), "SpecialEffects\Environment\Env_Snow_01.mgparticle") == 0;
        const ParticleFx* fx = p == weather::Precip::Dust ? models_.dust_particle(r.file, !snow_part)
                               : rain >= 0 && snow_part ? models_.rain_particle(r.file, rain) : models_.particle(r.file);
        if (!fx || !(fx->loops() || (r.retrigger && fx->has_sprites()))) continue;
        precip_3d_ = true;
        // The precipitation object follows the camera turned with it: the snow box (offset 0, 10, 10) sits 10 m
        // in front of the eye whichever way the camera faces. With world axes it sat 10 m to the side when the camera faced
        // east, half out of view: the L9 pair had about half the game's flake coverage.
        const float cy = std::cos(cam_.yaw), sy = std::sin(cam_.yaw);
        const float Y[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};  // local x right, y up, z forward -> world
        float org[3], rot[9];
        for (int a = 0; a < 3; a++) {
            org[a] = (a == 0 ? cam_.eye.x : a == 1 ? cam_.eye.y : cam_.eye.z) + Y[a * 3] * r.pos[0] + Y[a * 3 + 1] * r.pos[1] + Y[a * 3 + 2] * r.pos[2];
            for (int b = 0; b < 3; b++) rot[a * 3 + b] = Y[a * 3] * r.rot[b] + Y[a * 3 + 1] * r.rot[3 + b] + Y[a * 3 + 2] * r.rot[6 + b];
        }
        out.clear();
        simulate_particle(*fx, org, t, 0x51A0u + uint32_t(&r - refs->data()) * 40503u, 1.f, r.intermittent ? r.on_s : 0.f,
                          r.intermittent ? r.off_s : 0.f, r.retrigger, out, rot);
        precip_stats_.sprites += out.size();
        if (snow_part) {
            precip_stats_.rate = fx->emitters.empty() || fx->emitters[0].systems.empty() ? 0.f : fx->emitters[0].systems[0].rate.first();
            precip_stats_.life = fx->emitters.empty() || fx->emitters[0].systems.empty() ? 0.f : fx->emitters[0].systems[0].lifespan.first();
            precip_stats_.size = fx->emitters.empty() || fx->emitters[0].systems.empty() ? 0.f : fx->emitters[0].systems[0].sprite.size.first();
        }
        if (dump) {
            size_t textured = 0;
            for (const PSpriteOut& s : out) textured += models_.texture(s.sprite->texture) ? 1 : 0;
            float lo = 1e9f, hi = -1e9f;
            for (const PSpriteOut& s : out) { lo = std::min(lo, s.pos[1] - cam_.eye.y); hi = std::max(hi, s.pos[1] - cam_.eye.y); }
            fprintf(dump, "PRECIP %s pos %g %g %g sprites %zu textured %zu loops %d height above eye %.1f..%.1f m\n", r.file.c_str(), r.pos[0], r.pos[1],
                    r.pos[2], out.size(), textured, fx->loops() ? 1 : 0, lo, hi);
        }
        if (rain >= 0 && snow_part) {
            // The rain particle's sprite is the snow flake texture stretched along the velocity. The game shows it as a
            // long thin see-through streak; the editor's sprite path shrinks that texture to dots, so each particle of the
            // same simulation is drawn as a line of the sprite's length along its velocity, in the sprite's colour.
            // Tuned on the same-pose pairs (ux\shots\rain-levels-pair-*): the game's Light rain is many very faint streaks,
            // mostly below the eye; its Heavy rain a denser faint curtain. Each particle stands for `copies` streaks spread
            // across the 22 m box (the game's soft sprites read as more, finer streaks than one crisp line each).
            const bool light = rain == int(weather::RainLevel::Light);
            const int copies = light ? 3 : 2;
            const float alpha_k = light ? 0.16f : 0.30f;
            auto u8 = [](float x) { return uint32_t(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
            for (const PSpriteOut& s : out) {
                const float vl = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
                if (vl < 1e-3f) continue;
                // The game's streaks are about twice the sprite size long (same-pose pair): size is a half-length there.
                const float hx = s.vel[0] / vl * s.h, hy = s.vel[1] / vl * s.h, hz = s.vel[2] / vl * s.h;
                const uint32_t c = u8(s.rgba[0]) | u8(s.rgba[1]) << 8 | u8(s.rgba[2]) << 16 | u8(s.rgba[3] * alpha_k) << 24;
                const uint32_t seed = uint32_t(&s - out.data()) * 2654435761u;
                for (int k = 0; k < copies; k++) {
                    const uint32_t h = (seed + uint32_t(k) * 40503u) * 2246822519u;
                    const float ox = k ? (float(h & 1023) / 1023.f - 0.5f) * 22.f : 0.f, oz = k ? (float((h >> 10) & 1023) / 1023.f - 0.5f) * 22.f : 0.f;
                    const float y = s.pos[1] - (k ? float((h >> 20) & 1023) / 1023.f * 12.f : 0.f);
                    // Light: the game's streaks sit mostly in the lower half of the view; keep 1 in 4 above the eye.
                    if (light && y > cam_.eye.y && (h >> 30) != 0) continue;
                    r_.lines.push_back({{s.pos[0] + ox - hx, y - hy, s.pos[2] + oz - hz}, c});
                    r_.lines.push_back({{s.pos[0] + ox + hx, y + hy, s.pos[2] + oz + hz}, c});
                }
            }
            continue;
        }
        for (const PSpriteOut& s : out) {
            if (budget == 0) break;
            ID3D11ShaderResourceView* tex = models_.texture(s.sprite->texture);
            if (!tex) continue;
            Renderer::Sprite sp;
            sp.p = {s.pos[0], s.pos[1], s.pos[2]};
            sp.vel = {s.vel[0], s.vel[1], s.vel[2]};
            sp.w = s.w;
            sp.h = s.h;
            sp.angle = s.angle;
            sp.pivot_x = s.sprite->pivot[0];
            sp.pivot_y = s.sprite->pivot[1];
            sp.orient = s.sprite->orient_to_velocity;
            for (int k = 0; k < 4; k++) sp.rgba[k] = s.rgba[k];
            sp.tex = tex;
            sp.blend = s.sprite->blend;
            r_.sprites.push_back(sp);
            budget--;
        }
    }
}

}  // namespace ed

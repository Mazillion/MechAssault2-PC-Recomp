// Weather and time of day (see weather.h for what the game does with each field).
#include "weather.h"

#include <algorithm>
#include <cmath>

#include "mgf.h"
#include "mgic.h"
#include "scenario.h"
#include "templates.h"

namespace ma2 {
namespace weather {

using namespace world;

// ----------------------------------------------------------------------------- precipitation

Precip precip(const WObj& env) {
    if (env.i32("EnableRain") == 0) return Precip::None;
    switch (env.u32("PrecipEffectID")) {
        case kRainTemplate:
        case kLightRainTemplate: return Precip::Rain;
        case kSnowTemplate: return Precip::Snow;
        case kDustTemplate: return Precip::Dust;
        case 0: return Precip::None;  // EnableRain without an effect shows nothing in the game
        default: return Precip::Other;
    }
}

void set_precip(WObj& env, Precip p) {
    uint32_t id = p == Precip::Rain ? kRainTemplate : p == Precip::Snow ? kSnowTemplate : p == Precip::Dust ? kDustTemplate : 0;
    if (p == Precip::Other) return;
    env.set_i32("EnableRain", id ? 1 : 0);
    env.set_u32("PrecipEffectID", id);
    // RainEffect (only Mission 11a sets it) is read but not stored by the game; leave it.
}

// Heavy = the af298f3 streaks (600/s, alpha 190 -> 160) about 17% fewer and fainter; Light = 40% of Heavy's
// particles, smaller and fainter (design note: heavy toned down slightly, light a lot).
const RainParams& rain_params(RainLevel level) {
    static const RainParams heavy{500.f, 3.0f, 0.15f, 160, 135, 225, 232, 245};
    static const RainParams light{200.f, 3.0f, 0.14f, 145, 120, 225, 232, 245};
    return level == RainLevel::Light ? light : heavy;
}

RainLevel rain_level(const WObj& env) { return env.u32("PrecipEffectID") == kLightRainTemplate ? RainLevel::Light : RainLevel::Heavy; }

void set_rain_level(WObj& env, RainLevel level) {
    env.set_i32("EnableRain", 1);
    env.set_u32("PrecipEffectID", level == RainLevel::Light ? kLightRainTemplate : kRainTemplate);
}

const char* rain_label(RainLevel level) { return level == RainLevel::Light ? "Light rain" : "Heavy rain"; }

const char* precip_label(Precip p) {
    switch (p) {
        case Precip::None: return "Clear";
        case Precip::Rain: return "Rain";
        case Precip::Snow: return "Snow";
        case Precip::Dust: return "Dust storm";
        default: return "Other";
    }
}

bool lightning(const WObj& env) { return env.i32("LightFlash") != 0; }

// As the four retail worlds that flash (Mission_08, Bogged Down, Ill Wind, Mountain High) have it: LightFlash 1, SkyFlash 0,
// EnableLightning 0, MaxFlashes 4, white light (research 2026-10-26). Only LightFlash is read (env+0x18D gates
// sub_001263F8); SkyFlash is only copied and EnableLightning only stored, so both stay 0 like retail.
void set_lightning(WObj& env, bool on) {
    env.set_i32("LightFlash", on ? 1 : 0);
    env.set_i32("SkyFlash", 0);
    env.set_i32("EnableLightning", 0);
    if (!on) return;
    if (!env.find("MaxFlashes") || env.u32("MaxFlashes") == 0) env.set_u32("MaxFlashes", 4);
    if (!env.find("FlashTimeMin")) env.set_u32("FlashTimeMin", 3000);
    if (!env.find("FlashTimeMax") || env.u32("FlashTimeMax") <= env.u32("FlashTimeMin")) env.set_u32("FlashTimeMax", std::max(15000u, env.u32("FlashTimeMin") + 1000));
    for (const char* c : {"LightningColorRed", "LightningColorGreen", "LightningColorBlue"})
        if (!env.find(c)) env.set_u32(c, 255);
}

// ----------------------------------------------------------------------------- looks

namespace {
// Values from the retail worlds (MAEnvironment and MASky2), so a look needs no world loads.
struct LookData {
    Look look;
    uint32_t ambient, sun, fog;
    float fog_start, fog_end;
    uint32_t shadow;
    const char* env_map;
    const char* sky;  // ScenarioIO sky name
};
const std::vector<LookData>& data() {
    static const std::vector<LookData> v = {
        {{"Warm sun", "UnderCover", false, "Bright, sunny day with a blue sky (Under Cover, Mission 6b)."},
         0xFF313326u, 0xFF7B7159u, 0xFFB4D0DEu, 200, 475, 0x1E646464u, "\\EnvMaps\\EnvMap_Sky_WarmSunMP.tif", "Env_Sky_WarmSun"},
        {{"Bright overcast", "OnTheRocks", false, "White cloudy sky, even light (On The Rocks)."},
         0xFF4B4B4Bu, 0xFF828282u, 0xFFBAB9C6u, 100, 450, 0xFF0A0A0Au, "\\EnvMaps\\EnvMap_Sky_56_stormFront.tif", "Env_SkyNOR_02"},
        {{"Dusk", "BoggedDown", false, "Hazy evening light, low golden sun (Bogged Down).", true, true},
         0xFF3C4738u, 0xFF63533Au, 0xFFBABBA6u, 175, 450, 0x81333333u, "\\EnvMaps\\EnvMap_BabylonDam_MP.tif", "Env_SkyDusk02"},
        {{"Cold dusk", "Mission_10", false, "Blue winter evening (Mission 10); good with snow.", true},
         0xFF3F4356u, 0xFF727274u, 0xFF243558u, 75, 500, 0x7F595959u, "\\EnvMaps\\EnvMap_Sky_55_DuskSM.tif", "Env_SkyNOR_01"},
        {{"Red planet", "Mission_14", false, "Hesperus II's red sky and dust (Mission 14)."},
         0xFF342529u, 0xFF6F5940u, 0xFF561618u, 200, 475, 0x1E785E5Cu, "\\EnvMaps\\EnvMap_Sky_060_Hesperusmp.tif", "Env_Sky_Hesperus_01"},
        {{"City night", "GoCommando", true, "Dark city night with an orange glow (Go Commando, Missions 1-3).", false, true},
         0xFF1B213Bu, 0xFF4E3F34u, 0xFF000000u, 300, 450, 0x7F595959u, "\\EnvMaps\\EnvMap_Sky_CityNight.tif", "Env_Sky_CityNight"},
        {{"Stormy night", "IllWind", true, "Dark blue storm clouds (Ill Wind).", false, true},
         0xFF0F0F0Fu, 0xFF4D4D4Fu, 0xFF243558u, 150, 500, 0x81515153u, "\\EnvMaps\\EnvMap_Sky_56_stormFrontMP.tif", "Env_SkyNOR_01"},
        {{"Aurora night", "NewRiverCity", true, "Night sky with green aurora (New River City)."},
         0xFF262632u, 0xFF5C474Fu, 0xFF030309u, 300, 450, 0x7F595959u, "\\EnvMaps\\EnvMap_Sky_57_AuroraSM.tif", "Env_SkyAurora_01"},
        {{"Thunderstorm night", "WitchCauldron", true, "Black storm clouds over a city (Witch Cauldron, Mission 12); good with rain and lightning."},
         0xFF22282Du, 0xFF545236u, 0xFF040409u, 300, 400, 0x7F676767u, "\\EnvMaps\\EnvMap_Sky_CityNight_sm.tif", "Env_Sky_Stormy"},
    };
    return v;
}
const LookData* data_of(const Look& l) {
    for (auto& d : data()) if (&d.look == &l || iequals(d.look.label, l.label)) return &d;
    return nullptr;
}
}  // namespace

const std::vector<Look>& looks() {
    static std::vector<Look> v;
    if (v.empty()) for (auto& d : data()) v.push_back(d.look);
    return v;
}

const Look* default_look(bool night) {
    for (auto& l : looks()) if (l.night == night) return &l;
    return nullptr;
}

const Look* current_look(ScenarioIO& io, const Scenario& s) {
    int ei = find_class(s.world, K_ENV);
    if (ei < 0) return nullptr;
    const WObj& env = s.world.objs[size_t(ei)];
    std::string sky = io.sky_name(s);
    for (auto& l : looks()) {
        const LookData* d = data_of(l);
        if (d && env.u32("LightAmbientColor") == d->ambient && env.u32("DirectionalLightColor") == d->sun &&
            env.u32("FogColor") == d->fog && iequals(sky, d->sky))
            return &l;
    }
    return nullptr;
}

const char* look_sky(const Look& look) {
    const LookData* d = data_of(look);
    return d ? d->sky : "";
}

bool apply_look(ScenarioIO& io, Scenario& s, const Look& look, bool night, std::string* err) {
    const LookData* d = data_of(look);
    int ei = find_class(s.world, K_ENV);
    if (!d || ei < 0) { if (err) *err = "this world has no environment object"; return false; }
    if (!io.set_sky(s, d->sky, false, err)) return false;
    WObj& env = s.world.objs[size_t(ei)];
    env.set_u32("LightAmbientColor", d->ambient);
    env.set_u32("DirectionalLightColor", d->sun);
    env.set_u32("FogColor", d->fog);
    env.set_f32("FogStart", d->fog_start);
    env.set_f32("FogEnd", d->fog_end);
    env.set_u32("ShadowColor", d->shadow);
    env.set_str("EnvironmentMapName", d->env_map);
    env.set_i32("IsNightTime", look.lit ? 1 : 0);  // as the retail level does: Go Commando, Ill Wind and Bogged Down are 1; New River City, Witch Cauldron and Mission_10 are 0
    s.dirty = true;
    return true;
}

bool apply_own_light(ScenarioIO& io, Scenario& s, std::string* err) {
    const Wdf* w = io.base_world(s);
    const int bi = w ? find_class(*w, K_ENV) : -1;
    const int ei = find_class(s.world, K_ENV);
    if (bi < 0 || ei < 0) { if (err) *err = "the base level's light is not available"; return false; }
    const std::string sky = io.base_sky_name(s);
    if (!sky.empty() && !io.set_sky(s, sky, false, err)) return false;
    const WObj& src = w->objs[size_t(bi)];
    WObj& env = s.world.objs[size_t(ei)];
    for (const char* k : {"LightAmbientColor", "DirectionalLightColor", "FogColor", "ShadowColor"}) env.set_u32(k, src.u32(k));
    env.set_f32("FogStart", src.f32("FogStart"));
    env.set_f32("FogEnd", src.f32("FogEnd"));
    env.set_str("EnvironmentMapName", src.str("EnvironmentMapName"));
    env.set_i32("IsNightTime", src.i32("IsNightTime"));
    s.dirty = true;
    return true;
}

// ----------------------------------------------------------------------------- rain files

// The game only knows the retail templates (it never loads the scenario's templates.wdf, see
// ma2_scenario.c build_index), so rain is the snow template 31780 drawing the scenario's own
// files: the scenario archive carries the snow effect's model, material table, dust layer and
// texture (copied from Mission 10/11) and, at the snow particle's path, a rain particle (the
// snow file with fast fall, no spin and the sprite turned along its velocity, which stretches
// it into a streak). Tested in game 2026-10-23 (runs T1-T5): sizes below ~1 are invisible, and
// a sprite not oriented to its velocity stays a flake.
namespace {
const char* kSnowParticle = "SpecialEffects/Environment/Env_Snow_01.mgparticle";
const char* kRainName = "<mgParticle name=\"MA2SE Rain\"";
const char* kSnowFiles[] = {"SpecialEffects/Environment/Env_Precip_Snow_01/Env_Precip_Snow_01.MGMODEL",
                            "SpecialEffects/Environment/Env_Precip_Snow_01/Env_Precip_Snow_01.mtb",
                            "SpecialEffects/Environment/Env_SnowDust_01.mgparticle", "Texture_Dump/SpecialEffects/ENV_snow.tif"};

bool replace_once(std::string& s, const std::string& from, const std::string& to) {
    size_t i = s.find(from);
    if (i == std::string::npos) return false;
    s.replace(i, from.size(), to);
    return true;
}

// The first key of a channel ("<rate variance=...>\r\n<key time="0.00000000" value="40"/>").
bool set_channel(std::string& s, const char* name, const char* value) {
    size_t i = s.find(std::string("<") + name + " variance=");
    if (i == std::string::npos) return false;
    size_t v = s.find("value=\"", i);
    if (v == std::string::npos) return false;
    v += 7;
    size_t e = s.find('"', v);
    s.replace(v, e - v, value);
    return true;
}

std::string fnum(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.8f", v);
    return b;
}

bool rain_particle_impl(std::string& s, RainLevel level) {
    const RainParams& rp = rain_params(level);
    auto rgba = [&](int a) { return std::to_string(rp.r) + ", " + std::to_string(rp.g) + ", " + std::to_string(rp.b) + ", " + std::to_string(a); };
    // E1: the particle keeps the snow effect's own name. Renamed ("MA2SE Rain") the game drew it as big magenta quads
    // (missing material); rain is told apart by orient_to_velocity="true" (snow is "false").
    if (s.rfind("<mgParticle name=\"", 0) != 0) return false;
    bool ok = replace_once(s, "offset=\"0.00000000, 3.00000000, 0.00000000\"", "offset=\"0.00000000, 12.00000000, 0.00000000\"") &&
              replace_once(s, "gravity=\"1.00000000\"", "gravity=\"10.00000000\"") &&
              replace_once(s, "orient_to_velocity=\"false\"", "orient_to_velocity=\"true\"") &&
              replace_once(s, "spin_offset=\"6.28318548\"", "spin_offset=\"0.00000000\"") &&
              set_channel(s, "rate", fnum(rp.rate).c_str()) && set_channel(s, "lifespan", "1.10000000") &&
              set_channel(s, "initial_velocity", "-12.00000000") &&
              set_channel(s, "size", fnum(rp.size).c_str()) && set_channel(s, "aspect_ratio", fnum(rp.aspect).c_str()) &&
              replace_once(s, "limit_screen_size=\"true\"", "limit_screen_size=\"false\"") &&
              replace_once(s, "255, 255, 255, 217", rgba(rp.alpha_start)) && replace_once(s, "255, 255, 255, 203", rgba(rp.alpha_end));
    // No spin (one key at 0).
    size_t a = s.find("<spin variance="), b = s.find("</spin>");
    if (!ok || a == std::string::npos || b == std::string::npos) return false;
    s.replace(a, b - a, "<spin variance=\"0.00000000\">\r\n<key time=\"0.00000000\" value=\"0.00000000\"/>\r\n");
    return true;
}
}  // namespace

bool rain_particle_xml(std::string& snow_xml, RainLevel level) { return rain_particle_impl(snow_xml, level); }

bool add_rain(GameData& gd, MgfWriter& mgf, RainLevel level, std::string* err) {
    for (auto& an : gd.level_archives()) {
        MgfReader* a = gd.archive(an);
        if (!a || a->find(kSnowParticle) < 0 || a->find(kSnowFiles[0]) < 0) continue;
        Bytes p;
        std::vector<Bytes> files(std::size(kSnowFiles));
        bool ok = a->read_file(kSnowParticle, p);
        for (size_t i = 0; ok && i < files.size(); i++) ok = a->read_file(kSnowFiles[i], files[i]);
        if (!ok) continue;
        std::string s(p.begin(), p.end());
        if (!rain_particle_xml(s, level)) break;
        for (size_t i = 0; i < files.size(); i++) mgf.add(kSnowFiles[i], files[i]);
        mgf.add(kSnowParticle, Bytes(s.begin(), s.end()));
        return true;
    }
    if (err) *err = "Rain: the snow effect's files were not found on the disc (Missions 10 and 11).";
    return false;
}

bool base_has_snow(GameData& gd, const std::string& base) {
    MgfReader* a = gd.archive(base);
    return a && a->find(kSnowParticle) >= 0;
}

void to_game(Wdf& world) {
    int ei = find_class(world, K_ENV);
    if (ei < 0) return;
    WObj& env = world.objs[size_t(ei)];
    if (precip(env) == Precip::Rain) env.set_u32("PrecipEffectID", kSnowTemplate);
    // FlashTimeMax == FlashTimeMin crashes the game (integer division by zero, seen 2026-10-23).
    if (env.i32("LightFlash") != 0 && env.u32("FlashTimeMax") <= env.u32("FlashTimeMin"))
        env.set_u32("FlashTimeMax", env.u32("FlashTimeMin") + 1000);
}

void from_game(Wdf& world, MgfReader& scenario) {
    int ei = find_class(world, K_ENV);
    if (ei < 0 || precip(world.objs[size_t(ei)]) != Precip::Snow) return;
    Bytes b;
    std::string rn = kRainName;
    if (scenario.find(kSnowParticle) >= 0 && scenario.read_file(kSnowParticle, b) &&
        ((b.size() >= rn.size() && std::equal(rn.begin(), rn.end(), b.begin())) ||
         std::string(b.begin(), b.end()).find("orient_to_velocity=\"true\"") != std::string::npos))
        world.objs[size_t(ei)].set_u32("PrecipEffectID",
                                       std::string(b.begin(), b.end()).find("value=\"" + fnum(rain_params(RainLevel::Light).rate) + "\"") != std::string::npos
                                           ? kLightRainTemplate : kRainTemplate);
}

void extra_template_seeds(const Wdf& world, std::vector<uint32_t>& out) {
    int ei = find_class(world, K_ENV);
    if (ei < 0) return;
    const WObj& env = world.objs[size_t(ei)];
    uint32_t id = env.u32("PrecipEffectID");
    if (!id || env.i32("EnableRain") == 0) return;
    out.push_back(id == kRainTemplate || id == kLightRainTemplate ? kSnowTemplate : id);  // rain is made from the snow template
}

}  // namespace weather
}  // namespace ma2

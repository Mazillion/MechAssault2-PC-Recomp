// Weather and time of day: what the game really does with MAEnvironment (research 2026-10-23,
// recomp sources game tree, reader sub_00124CC1, update sub_00126060).
//
//   precipitation  Every frame the environment starts its precipitation object when EnableRain
//                  is set (sub_0012693E: creates the template PrecipEffectID and follows the
//                  camera with it) and removes it when EnableRain is cleared. With
//                  PrecipEffectID 0 nothing appears. The disc has only two precipitation
//                  templates (MAPrecipitation): 31780 Env_Snow_Blowing (Missions 10, 11a, 11b)
//                  and 31516 Env_DustStorm_Wst; the Rain/Snow presets' 741 and 903 do not exist.
//                  Rain is therefore the snow template drawing a streak particle the editor
//                  writes into the scenario (.mgparticle is XML); see add_rain.
//   lightning      LightFlash makes the flashes (sub_001263F8: bursts of up to MaxFlashes, the
//                  next burst FlashTimeMin..FlashTimeMax ms later) and plays ThunderCueID (0 in
//                  every retail world: no thunder). EnableLightning is only stored (and set by
//                  trigger actions); nothing in the game reads it.
//   night          IsNightTime does not darken anything: Mission 06a's warm sun is flagged as
//                  night and Mission 12's dark city is not. It only switches building window lights
//                  (MABuilding light groups; lamp posts do not read it; wiki isnighttime-buildings-only). Darkness comes from the ambient, sun and fog colours,
//                  the sky model and the reflection (environment) map, so the Night switch
//                  applies a whole "look" copied from a retail level.
#pragma once
#include <string>
#include <vector>

#include "util.h"
#include "wdf.h"

namespace ma2 {

struct Scenario;
class ScenarioIO;
class GameData;
class TemplateCatalog;
class MgfWriter;
class MgfReader;

namespace weather {

constexpr uint32_t kSnowTemplate = 31780, kDustTemplate = 31516;
// The editor's in-memory id for rain. Saved worlds use kSnowTemplate (the game only knows the
// retail templates) and the scenario's own snow-effect files make it rain (to_game/from_game).
constexpr uint32_t kRainTemplate = 990001;       // Heavy rain (maps saved before the two levels load as this)
constexpr uint32_t kLightRainTemplate = 990002;  // Light rain

// The two rain levels (design note 2026-10-26). ONE table for the game export (add_rain) and the editor preview:
// rate = particles per second, size = sprite height (m), aspect = width / height, alpha_start / alpha_end = the colour
// curve's alpha (0-255) after the fade-in and at the end of life; colour = r, g, b.
struct RainParams {
    float rate, size, aspect;
    int alpha_start, alpha_end;
    int r, g, b;
};
enum class RainLevel { Heavy, Light };
const RainParams& rain_params(RainLevel level);
RainLevel rain_level(const WObj& env);                 // Heavy unless the world is Light rain
void set_rain_level(WObj& env, RainLevel level);       // also switches the weather to rain
const char* rain_label(RainLevel level);               // "Heavy rain" / "Light rain"

enum class Precip { None, Rain, Snow, Dust, Other };
Precip precip(const WObj& env);
void set_precip(WObj& env, Precip p);
const char* precip_label(Precip p);

bool lightning(const WObj& env);           // LightFlash
void set_lightning(WObj& env, bool on);     // LightFlash, SkyFlash and EnableLightning together

// A lighting look copied from a retail level: sun, ambient, fog and shadow colours, fog
// distances, reflection map and sky model.
struct Look {
    const char* label;    // "Warm sun"
    const char* world;    // retail map the values come from ("UnderCover")
    bool night;           // the Night switch picks night looks
    const char* tip;
    bool evening = false; // dusk: neither day nor night (shown as a mixed Night checkbox)
    bool lit = false;     // IsNightTime as the retail level has it (1: Go Commando, Ill Wind, Bogged Down; 0: the other looks, New River City and Witch Cauldron included)
};
const std::vector<Look>& looks();
const Look* default_look(bool night);
// The sky (ScenarioIO::SkyInfo name) the look puts on the world.
const char* look_sky(const Look& look);
// The look the world matches (same ambient, sun and fog colours and sky), or null.
const Look* current_look(ScenarioIO& io, const Scenario& s);
// Copies the look's values into the world's MAEnvironment and points MASky2 at its sky.
// Sets IsNightTime to the look's retail value (look.lit; `night` is unused). Returns false with err when the retail
// world or its sky is missing.
bool apply_look(ScenarioIO& io, Scenario& s, const Look& look, bool night, std::string* err);
// The base level's own light back (review E2: a level whose light is none of the looks had no way back after a look but
// Undo): its sky and the MAEnvironment values a look changes. False with err when the retail world is missing.
bool apply_own_light(ScenarioIO& io, Scenario& s, std::string* err);

// The rain particle: rewrites Env_Snow_01.mgparticle's XML (as read from the disc) in place with rain_params(level).
// The same function makes the game file (add_rain) and the editor preview, so the two cannot drift. False when the
// XML is not the snow particle the editor expects.
bool rain_particle_xml(std::string& snow_xml, RainLevel level);
// Save: the snow effect's files with a rain particle added to the scenario archive.
bool add_rain(GameData& gd, MgfWriter& mgf, RainLevel level, std::string* err);
// The base level ships the snow effect itself (Missions 10, 11a, 11b): its own files may win
// over the scenario's, so rain may show as snow there.
bool base_has_snow(GameData& gd, const std::string& base);
// Save: rain written as the snow template, and a flash interval the game can use. Load: a snow world whose archive carries the
// editor's rain particle is rain again.
void to_game(Wdf& world);
void from_game(Wdf& world, MgfReader& scenario);
// Extra template seeds the world needs that the catalog's learned reference keys miss
// (PrecipEffectID).
void extra_template_seeds(const Wdf& world, std::vector<uint32_t>& out);

}  // namespace weather
}  // namespace ma2

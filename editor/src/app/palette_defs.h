#pragma once
// The palette's group tables, shared by the object lists (app_tabs.cpp) and the palette browser (app_palette.cpp)..
#include <string>

namespace ma2 { struct TemplateInfo; }

namespace ed {

// Palette groups (the chips of the Place page). Each object has one home group (matched on the class, not the template folder;
// Special = the cutscene props / wrecks / decoration / skies categories), and the view groups Defenses, Bridges, Lights and Signs
// also list objects whose home is elsewhere (playtest 10-26: a turret is in Turrets and Defenses, a bridge in Buildings and Bridges).
enum PaletteGroup {
    PG_MECHS, PG_VEHICLES, PG_SOLDIERS, PG_CIVILIANS, PG_TURRETS, PG_DEFENSES, PG_BUILDINGS, PG_BRIDGES, PG_ROADS, PG_WATER,
    PG_LIGHTS, PG_SIGNS, PG_SCENERY, PG_PICKUPS, PG_EFFECTS, PG_SPECIAL, PG_ALL, PG_COUNT
};
inline const char* const kPaletteGroups[PG_COUNT][2] = {
    {"'Mechs", "BattleMechs: the big walking war machines."},
    {"Vehicles", "Tanks, hover vehicles and aircraft."},
    {"Soldiers", "People who fight: troopers, Elementals, battle armour, pilots."},
    {"Civilians", "People who don't fight: townspeople, workers, scientists."},
    {"Turrets", "Guns fixed to the ground."},
    {"Defenses", "Walls, gates, fences, barriers, bunkers and the turrets: everything that guards a base. Turrets are in Turrets too."},
    {"Buildings", "Buildings, towers, bridges and docks. Most can be destroyed."},
    {"Bridges", "Bridges, docks and piers. They are in Buildings too."},
    {"Roads", "Road pieces: straights, curves and junctions that join end to end. On a slope a piece tilts to lie on it; Properties > Fit ground to road lets vehicles drive up it."},
    {"Water", "Lakes, rivers and seas: place it like a unit, then size it."},
    {"Lights", "Street lights, floodlights, search lights, lamps and light columns. They are in Scenery or Buildings too."},
    {"Signs", "Signs, banners and billboards. They are in Scenery or Buildings too."},
    {"Scenery", "Trees, rocks, plants, street furniture and other props. Walls and fences are under Defenses."},
    {"Pickups", "Power-ups and crates the player can pick up."},
    {"Effects", "Fires, smoke, sparks and other effects."},
    {"Special", "Cutscene props, wrecks, decoration and skies."},
    {"All", "Everything at once."}};

inline constexpr const char* kGroupNatural = "Foliage & Natural";
inline constexpr const char* kGroupCity = "City & Man-made";
inline constexpr const char* kFxHere = "Effects in this level";

// app_tabs.cpp
const char* special_category_tip(const std::string& c);            // non-null for the special categories
bool in_group(const ma2::TemplateInfo& t, const std::string& g);   // the cross-cutting groups Natural / City
int palette_group_of(const ma2::TemplateInfo& t, bool special);    // the object's home PaletteGroup
std::string kind_label(const std::string& cls);                    // "building", "'Mech"...

}  // namespace ed

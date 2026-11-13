// Template catalog: common.mgf Templates/GlobalEntities.wdf (3,449 templates), the per-level
// culled subsets in Templates.mgf, and which level archives hold each template's model files.
//
// Culled template files (verified byte-identical by fmt_test): the selected GlobalEntities
// objects in their original order, with `child` re-pointed to the first selected child and
// `sibling` to the next selected sibling. Everything else is unchanged.
//
// Closure (what a world needs): TemplateID of every object, plus integer properties whose key is
// a learned template-reference key (90%+ of its non-zero values are template UIDs: WeaponLoadout,
// CorporalDestroyed, DestroyedEffect, ProjectileTemplate, ...), followed recursively; children
// of non-group templates; MAPlayerConfiguration whose CorporealTemplateUID is selected and
// MoeEntityReference whose ReferenceUID is selected; then every ancestor. The editor unions that
// with the Base level's retail culled set and the set common to all retail levels of the type.
#pragma once
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gamedata.h"
#include "wdf.h"

namespace ma2 {

struct TemplateInfo {
    uint32_t uid = 0;
    uint32_t cls = 0;
    std::string cls_name;
    std::string name;       // Description
    std::string category;   // group path below the root: "Environment/Trees"
    std::string model;      // Corporal3DObj (as stored, "\Models\...")
    std::string mtb;        // CorporalMTB
    std::vector<int> archives;  // indices into GameData::level_archives() holding the model
    int index = -1;         // in global().objs
    bool placeable = false; // offered in the object palette
};

class TemplateCatalog {
public:
    bool load(GameData& gd, std::string* err = nullptr);

    size_t count() const { return infos_.size(); }
    const std::vector<TemplateInfo>& all() const { return infos_; }
    const TemplateInfo* find(uint32_t uid) const;
    const Wdf& global() const { return ge_; }
    const WObj* object(uint32_t uid) const;

    std::vector<std::string> culled_levels() const;
    bool has_culled(const std::string& level) const { return culled_.count(lower(level)) != 0; }
    const std::unordered_set<uint32_t>& culled_set(const std::string& level) const;
    size_t culled_set_count() const { return culled_.size(); }
    const std::unordered_set<uint32_t>& common_set(bool multiplayer) const { return multiplayer ? common_mp_ : common_mission_; }

    const std::unordered_set<uint32_t>& ref_keys() const { return ref_keys_; }
    size_t model_template_count() const;

    // Pure closure of the world's references (no base set). with_mtb (default) also follows the
    // template UIDs that a template's .mtb names (mtb_refs); false gives the pre-11-09 closure.
    std::unordered_set<uint32_t> closure(const std::unordered_set<uint32_t>& seed, bool with_mtb = true) const;
    // Templates a template's .mtb (CorporalMTB, else "<model>.mtb") names: the entity objects the
    // .mtb carries (TemplateID) and their reference properties (DestroyedEffect, MAEffectID, ...).
    // The game creates them when it builds the object; if one's files are not mounted the object
    // fails to build and draws as the pink pillar-and-cube placeholder (sub_D8221, 2026-11-09).
    const std::vector<uint32_t>& mtb_refs(uint32_t uid) const;
    // Files the game's code (not the template data) needs to build an object of this template's
    // class; a missing one fails the build like a missing model. MABuilding resolve (sub_129490)
    // reads the four strings at 0x4B0030..0x4B003C for every building.
    std::vector<std::string> code_required(uint32_t uid) const;
    std::unordered_set<uint32_t> seed_of(const Wdf& world) const;
    // base culled set U common set U closure(world).
    std::unordered_set<uint32_t> closure_for_world(const Wdf& world, const std::string& base_level, bool multiplayer) const;
    Bytes write_culled(const std::unordered_set<uint32_t>& uids) const;
    Wdf culled(const std::unordered_set<uint32_t>& uids) const;

    // Archive (by level_archives index) that holds a file path; -1 if none.
    int archive_of(const std::string& path) const;
    std::vector<int> archives_of(const std::string& path) const;
    // All file paths (normalized) of a level archive.
    const std::vector<std::string>* archive_files(int archive) const;
    bool archive_has(int archive, const std::string& path) const;

private:
    Wdf ge_;
    WTree tree_;
    std::vector<TemplateInfo> infos_;
    std::unordered_map<uint32_t, int> by_uid_;
    std::map<std::string, std::unordered_set<uint32_t>> culled_;
    std::unordered_set<uint32_t> common_mp_, common_mission_, empty_;
    std::unordered_set<uint32_t> ref_keys_;
    std::vector<int> player_configs_, entity_refs_;
    std::vector<std::vector<std::string>> archive_files_;
    std::vector<std::unordered_set<std::string>> archive_sets_;
    std::unordered_map<std::string, std::vector<int>> file_archives_;
    GameData* gd_ = nullptr;
    mutable std::unordered_map<uint32_t, std::vector<uint32_t>> mtb_refs_;
};

}  // namespace ma2

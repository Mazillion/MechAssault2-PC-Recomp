// Model loader state shared by model.cpp (geometry) and deps.cpp (closure).
#pragma once

#include "internal.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ma2m {
namespace detail {

// The game's index data winds front faces clockwise when seen from the front
// with cross(p1-p0, p2-p0) as the reference; kFlipWinding swaps to the API
// convention (see NOTES.md, "Winding").
static const bool kFlipWinding = false;

class Loader {
public:
    Loader(const ArchiveSet& a, const ModelLoadOptions& o, Model& m);
    bool Run(const std::string& path);

    const ArchiveSet& ar;
    const ModelLoadOptions& opt;
    Model& model;
    TextureCache* opt_cache = nullptr;
    std::string error;
    bool skip_geometry = false;   // deps mode: walk references only
    bool collect_all = false;     // deps mode: follow shadow meshes and every LOD
    int failed_meshes = 0;
    std::vector<Dependency> deps;
    std::vector<std::string> model_files;  // expression files etc.

    void Ref(const std::string& path, const char* kind, const std::string& by);

private:
    std::set<std::string> seen;
    std::map<std::string, bool> alpha_cache;
    std::map<std::string, int> legacy_materials;
    std::string current_file;
    std::vector<std::string> pending_child;
    int pending_anim_node = -1;
    int node_budget = 0;
    int file_depth = 0;

    std::string FindTexture(const std::string& base, const std::string& rel);
    void ResolveTexture(const std::string& base, const std::string& rel, TextureSlot& ts);
    bool TextureAlpha(const std::string& path, bool& has_alpha);
    void FinishMaterial(Material& mat, int alpha_kill);
    int NewNode(const std::string& name, const std::string& type, int parent, const float local[16], bool on, int lod);
    void AddSubMesh(const std::string& name, int node, int material, const std::vector<uint8_t>& vdata,
                    const std::vector<uint8_t>& idata, bool strip, bool shadow, const std::string& where);
    bool LoadMgmodel(const std::string& path, int parent, int lod);
    void AddXmlNode(const XmlNode& el, const std::string& file, int parent, int lod,
                    const std::map<std::string, int>& materials, const std::map<std::string, const XmlNode*>& meshes);
    void ReadAnimationSet(const std::string& path);
    int LegacyMaterial(const std::string& path);
    void LegacyMesh(const std::string& mesh_path, int node);
    void LegacySection(const std::vector<IniSection>& secs, size_t& cursor, const std::string& file, int parent, int lod);
    void AnimNode(const IniSection& s, const std::string& file, int node);
    bool LoadNodeFile(const std::string& path, int parent, int lod);
    bool LoadAny(const std::string& path, int parent, int lod);
    std::string GuessMtb() const;
    void ApplyMtb(const std::string& path);
    void Finish();
};

}  // namespace detail
}  // namespace ma2m

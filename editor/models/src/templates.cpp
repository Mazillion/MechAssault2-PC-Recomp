// Template scan: which objects in a .wdf carry models, and where they point.
#include "internal.h"

namespace ma2m {

bool ReadTemplateModels(const uint8_t* wdf, size_t size, std::vector<TemplateModel>& out, std::string* error) {
    out.clear();
    std::vector<detail::WdfObject> objs;
    if (!detail::ParseWdf(wdf, size, objs, error)) return false;
    static const uint32_t kUid = WdfKey("UID"), kTid = WdfKey("TemplateID"), kDesc = WdfKey("Description"),
                          kModel = WdfKey("Corporal3DObj"), kMtb = WdfKey("CorporalMTB");
    static const char* kFileProps[] = {"AbandonedModel", "AbandonedMTB", "TransitionModel", "TransitionMTB",
                                       "CamouflageTexture", "HUDIconImage", "HUDEnemyIconImage",
                                       "HUDFriendIconImage", "MechFootprintDecal", "MechClawDecal",
                                       "EnvironmentMapName"};
    static const char* kLinkProps[] = {"DestroyedBuilding", "CorporalDestroyed", "DestroyedEffect", "HitEffect",
                                       "HoleEdgeEffect", "HoleTopEffect"};
    for (const auto& o : objs) {
        TemplateModel t;
        bool any = false;
        for (const auto& p : o.props) {
            if (p.key == kModel && p.type == 9) {
                t.model = NormalizePath(p.AsString());
                if (!t.model.empty()) any = true;
            } else if (p.key == kMtb && p.type == 9) {
                t.mtb = NormalizePath(p.AsString());
            } else if (p.key == kUid) {
                t.uid = p.AsU32();
            } else if (p.key == kTid) {
                t.template_id = p.AsU32();
            } else if (p.key == kDesc && p.type == 9) {
                t.name = p.AsString();
            } else if (p.type == 9) {
                for (const char* fp : kFileProps) {
                    if (p.key == WdfKey(fp)) {
                        std::string v = NormalizePath(p.AsString());
                        if (!v.empty()) {
                            t.extra_files.emplace_back(fp, v);
                            any = true;
                        }
                    }
                }
            } else if (p.type == 6) {
                for (const char* lp : kLinkProps)
                    if (p.key == WdfKey(lp) && p.AsU32()) t.links.emplace_back(lp, p.AsU32());
            }
        }
        if (!any) continue;
        t.class_key = o.class_key;
        const char* cn = detail::KnownWdfName(o.class_key);
        t.class_name = cn ? cn : std::string();
        out.push_back(std::move(t));
    }
    return true;
}

bool ReadTemplateModels(const ArchiveSet& archives, const std::string& wdf_path, std::vector<TemplateModel>& out,
                        std::string* error) {
    std::vector<uint8_t> data;
    if (!archives.Read(wdf_path, data, nullptr, error)) return false;
    return ReadTemplateModels(data.data(), data.size(), out, error);
}

}  // namespace ma2m

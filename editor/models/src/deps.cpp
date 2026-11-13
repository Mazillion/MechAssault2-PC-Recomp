// Dependency closure: every archive file the game reads to show a model.
#include "model_loader.h"

#include <set>

namespace ma2m {

namespace {

bool IsFileValue(const std::string& v) {
    static const char* exts[] = {"mgmodel", "node", "tif", "mgparticle", "mat", "mtb", "gr2", "anm", "lbp", "mesh"};
    std::string e = Extension(v);
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

const char* KindOf(const std::string& path) {
    std::string e = Extension(path);
    if (e == "mgmodel") return "model";
    if (e == "node") return "node";
    if (e == "tif") return "texture";
    if (e == "mgparticle") return "particle";
    if (e == "mat") return "material";
    if (e == "mtb") return "mtb";
    if (e == "gr2") return "animation";
    if (e == "mesh") return "mesh";
    return "other";
}

struct Closure {
    const ArchiveSet& ar;
    std::vector<Dependency> deps;
    std::set<std::string> walked;  // models / particles already expanded
    int budget = 0;

    explicit Closure(const ArchiveSet& a) : ar(a) {}

    void Merge(const std::vector<Dependency>& more) {
        for (const auto& d : more) deps.push_back(d);
    }

    void Add(const std::string& path, const std::string& by) {
        Dependency d;
        std::string canon = ar.CanonicalPath(path);
        d.found = !canon.empty();
        d.path = d.found ? canon : NormalizePath(path);
        int ai = ar.Find(path);
        d.archive = ai >= 0 ? ar.ArchiveName((size_t)ai) : std::string();
        d.kind = KindOf(path);
        d.required_by = by;
        deps.push_back(d);
    }

    bool Model(const std::string& path, const std::string& mtb, bool use_mtb, std::string* error) {
        std::string key = detail::Lower(NormalizePath(path)) + "|" + detail::Lower(NormalizePath(mtb));
        if (!walked.insert(key).second || ++budget > 4000) return true;
        ModelLoadOptions o;
        o.include_shadow_meshes = true;
        o.include_invisible = true;
        o.all_lods = true;
        o.apply_mtb = use_mtb;
        o.mtb_path = mtb;
        ma2m::Model m;
        detail::Loader l(ar, o, m);
        l.skip_geometry = true;
        l.collect_all = true;
        if (!l.Run(path)) {
            if (error) *error = l.error;
            return false;
        }
        Merge(l.deps);
        if (!m.mtb_path.empty()) Mtb(m.mtb_path);
        return true;
    }

    void Mtb(const std::string& path) {
        std::vector<uint8_t> data;
        if (!ar.Read(path, data)) return;
        std::vector<detail::WdfObject> objs;
        if (!detail::ParseWdf(data.data(), data.size(), objs, nullptr)) return;
        std::string canon = ar.CanonicalPath(path);
        for (const auto& o : objs)
            for (const auto& p : o.props) {
                if (p.type != 9) continue;
                std::string v = p.AsString();
                if (!IsFileValue(v)) continue;
                std::string fp = ResolveRelative(canon, v);
                if (Extension(fp) == "tif") Texture(fp, canon);
                else File(fp, canon);
            }
    }

    void File(const std::string& path, const std::string& by) {
        std::string e = Extension(path);
        if (e == "mgparticle") {
            Add(path, by);
            Particle(path);
        } else if (e == "mgmodel" || e == "node") {
            // The loader records the resolved file (it may be a _low variant).
            if (!Model(path, std::string(), false, nullptr)) Add(path, by);
        } else if (e == "mtb") {
            Add(path, by);
            Mtb(path);
        } else if (e == "mat") {
            Add(path, by);
            Mat(path);
        } else {
            Add(path, by);
        }
    }

    // Legacy material file named directly (decals): its textures.
    void Mat(const std::string& path) {
        std::vector<uint8_t> data;
        if (!ar.Read(path, data)) return;
        std::vector<detail::IniSection> secs;
        detail::ParseIni((const char*)data.data(), data.size(), secs);
        std::string canon = ar.CanonicalPath(path);
        for (const auto& s : secs)
            for (const auto& kv : s.values)
                if (Extension(kv.second) == "tif") Texture(ResolveRelative(canon, kv.second), canon);
    }

    // A texture, or the frames of an animated one ("X 01.tif", ...).
    void Texture(const std::string& path, const std::string& by) {
        if (ar.Exists(path)) {
            Add(path, by);
            return;
        }
        std::string ext = Extension(path);
        std::string stem = path.substr(0, path.size() - (ext.empty() ? 0 : ext.size() + 1));
        bool any = false;
        for (int k = 1; k < 100; k++) {
            char num[8];
            snprintf(num, sizeof(num), " %02d", k);
            std::string f = stem + num + (ext.empty() ? std::string() : "." + ext);
            if (!ar.Exists(f)) break;
            Add(f, by);
            any = true;
        }
        if (!any) Add(path, by);
    }

    void Particle(const std::string& path) {
        std::string key = detail::Lower(NormalizePath(path));
        if (!walked.insert(key).second || ++budget > 4000) return;
        std::vector<uint8_t> data;
        if (!ar.Read(path, data)) return;
        detail::XmlNode root;
        if (!detail::ParseXml((const char*)data.data(), data.size(), root, nullptr)) return;
        std::string canon = ar.CanonicalPath(path);
        std::vector<const detail::XmlNode*> stack{&root};
        while (!stack.empty()) {
            const detail::XmlNode* n = stack.back();
            stack.pop_back();
            for (const auto& kv : n->attrs) {
                if (!IsFileValue(kv.second)) continue;
                std::string p = ResolveRelative(canon, kv.second);
                if (Extension(p) == "tif") Texture(p, canon);
                else File(p, canon);
            }
            for (const auto& c : n->children) stack.push_back(&c);
        }
    }
};

}  // namespace

namespace {
void Finish(const Closure& c, std::vector<Dependency>& out) {
    // Unique by path (first reference wins), sorted.
    std::map<std::string, Dependency> uniq;
    for (const auto& d : c.deps) {
        std::string key = detail::Lower(d.path);
        if (!uniq.count(key)) uniq[key] = d;
    }
    out.clear();
    for (auto& kv : uniq) out.push_back(kv.second);
}
}  // namespace

bool CollectDependencies(const ArchiveSet& archives, const std::string& model_path, const std::string& mtb_path,
                         std::vector<Dependency>& out, std::string* error) {
    out.clear();
    Closure c(archives);
    if (!c.Model(model_path, mtb_path, true, error)) return false;
    Finish(c, out);
    return true;
}

bool CollectTemplateDependencies(const ArchiveSet& archives, const TemplateModel& t, std::vector<Dependency>& out,
                                 std::string* error) {
    out.clear();
    Closure c(archives);
    bool ok = true;
    std::string by = "template " + std::to_string(t.uid) + (t.name.empty() ? std::string() : " (" + t.name + ")");
    if (!t.model.empty() && !c.Model(t.model, t.mtb, true, error)) ok = false;
    if (t.model.empty() && !t.mtb.empty()) c.File(t.mtb, by);
    auto extra = [&](const char* key) -> std::string {
        for (const auto& kv : t.extra_files)
            if (kv.first == key) return kv.second;
        return std::string();
    };
    std::string am = extra("AbandonedModel"), amtb = extra("AbandonedMTB");
    std::string tm = extra("TransitionModel"), tmtb = extra("TransitionMTB");
    if (!am.empty() && !c.Model(am, amtb, true, error)) ok = false;
    if (!tm.empty() && !c.Model(tm, tmtb, true, error)) ok = false;
    for (const auto& kv : t.extra_files) {
        if (kv.second == am || kv.second == amtb || kv.second == tm || kv.second == tmtb) continue;
        if (Extension(kv.second) == "tif") c.Texture(kv.second, by);
        else c.File(kv.second, by);
    }
    Finish(c, out);
    return ok;
}

}  // namespace ma2m

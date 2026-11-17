// Palette sweep (developer): what the retail data holds that the object palette could offer.
// --dump-sweep DIR; see palette-notes in round-1023/palette.
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

#include "app.h"

namespace ed {
using namespace ma2;

// --dump-sweep DIR: what the retail data holds that the object palette could offer.
//   files.tsv    archive, extension, count, one example path (every archive on the disc)
//   classes.tsv  per map: class, count, how many have a TemplateID that is not a palette template
//   roads.tsv    per map: every distinct RoadDefinition used
//   roadfiles.tsv every .road file on the disc and the archives holding it
//   orphans.tsv  per map and class: TemplateIDs that are not in GlobalEntities, or whose template is not placeable
void App::dump_sweep(const std::string& dir) {
    auto open = [&](const char* n) { return fopen((dir + "/" + n).c_str(), "w"); };
    FILE* ff = open("files.tsv");
    FILE* fc = open("classes.tsv");
    FILE* fr = open("roads.tsv");
    FILE* fo = open("orphans.tsv");
    FILE* frf = open("roadfiles.tsv");
    if (!ff || !fc || !fr || !fo || !frf || !cat_) return;
    auto ext_of = [](const std::string& p) {
        size_t d = p.rfind('.');
        return d == std::string::npos ? std::string("(none)") : lower(p.substr(d));
    };
    std::map<std::string, std::set<std::string>> road_archives;
    auto scan = [&](const std::string& name, MgfReader* r) {
        if (!r) return;
        std::map<std::string, std::pair<int, std::string>> ex;
        for (auto& f : r->files()) {
            auto& e = ex[ext_of(f.path)];
            if (!e.first++) e.second = f.path;
            if (ext_of(f.path) == ".road") road_archives[lower(f.path)].insert(name);
        }
        for (auto& [e, v] : ex) fprintf(ff, "%s\t%s\t%d\t%s\n", name.c_str(), e.c_str(), v.first, v.second.c_str());
    };
    scan("common.mgf", &gd_.common());
    if (gd_.templates_archive()) scan("templates.mgf", gd_.templates_archive());
    for (auto& a : gd_.level_archives()) scan(a, gd_.archive(a));
    for (auto& [p, s] : road_archives) {
        fprintf(frf, "%s\t", p.c_str());
        for (auto& a : s) fprintf(frf, "%s ", a.c_str());
        fprintf(frf, "\n");
    }
    std::map<uint32_t, std::set<std::string>> used_in;  // template uid -> maps
    std::map<uint32_t, int> used_n;
    for (auto& m : gd_.maps()) {
        Wdf w;
        if (!gd_.load_world(m, w)) continue;
        std::map<uint32_t, int> cnt, orphan, unplaceable, notid;
        std::map<std::string, int> roads;
        std::map<std::pair<uint32_t, uint32_t>, int> detail;  // (class, template uid) -> uses
        for (auto& o : w.objs) {
            cnt[o.cls]++; if (!o.template_id()) notid[o.cls]++;
            uint32_t tid = o.template_id();
            if (tid) {
                used_in[tid].insert(m.name);
                used_n[tid]++;
                const TemplateInfo* t = cat_->find(tid);
                if (!t) { orphan[o.cls]++; detail[{o.cls, tid}]++; }
                else if (!t->placeable) { unplaceable[o.cls]++; detail[{o.cls, tid}]++; }
            }
            if (o.cls == key_of("MARoad")) roads[norm_path(o.str("RoadDefinition"))]++;
        }
        for (auto& [c, n] : cnt)
            fprintf(fc, "%s\t%s\t%d\t%d\t%d\t%d\n", m.name.c_str(), gd_.names().label(c).c_str(), n, orphan[c], unplaceable[c], notid[c]);
        for (auto& [p, n] : roads) fprintf(fr, "%s\t%s\t%d\n", m.name.c_str(), p.c_str(), n);
        for (auto& [k, n] : detail) {
            const TemplateInfo* t = cat_->find(k.second);
            fprintf(fo, "%s\t%s\t%u\t%s\t%s\t%s\t%d\n", m.name.c_str(), gd_.names().label(k.first).c_str(), k.second,
                    t ? "not-placeable-template" : "not-in-GlobalEntities", t ? t->name.c_str() : "", t ? t->category.c_str() : "", n);
        }
    }
    // Every template a retail world places that the palette does not offer.
    if (FILE* fh = open("hidden_templates.tsv")) {
        fprintf(fh, "uid\tclass\tname\tcategory\tuses\tmaps\n");
        for (auto& [uid, maps] : used_in) {
            const TemplateInfo* t = cat_->find(uid);
            if (!t || t->placeable) continue;
            std::string ms;
            for (auto& mn : maps) ms += mn + " ";
            fprintf(fh, "%u\t%s\t%s\t%s\t%d\t%s\n", uid, t->cls_name.c_str(), t->name.c_str(), t->category.c_str(), used_n[uid], ms.c_str());
        }
        fclose(fh);
    }
    fclose(ff); fclose(fc); fclose(fr); fclose(fo); fclose(frf);
}

}  // namespace ed

namespace ed {
// --dump-search "words|FILE": every palette entry the words find (all themes), with its groups.
void App::dump_search(const std::string& words, const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f || !cat_) { if (f) fclose(f); return; }
    std::string w = lower(words);
    for (auto& t : cat_->all()) {
        if (!t.placeable) continue;
        std::string shown = palette_name(t);
        bool in_group_word = false;
        for (auto& g : palette_extra_groups()) if (lower(g.first) == w) in_group_word = palette_group_has(t, g.first);
        bool hit = in_group_word || palette_rank_of(w, t, palette_plain_name(t)) >= 0;
        if (!hit) continue;
        std::string gs;
        for (auto& g : palette_extra_groups()) if (palette_group_has(t, g.first)) gs += std::string(g.first) + "; ";
        fprintf(f, "%s\t%s\t%s\n", shown.c_str(), palette_category(t).c_str(), gs.c_str());
    }
    for (auto& p : road_pieces())
        if (p.search.find(w) != std::string::npos) fprintf(f, "ROAD %s\t%s\n", p.friendly.c_str(), p.path.c_str());
    fclose(f);
}
}  // namespace ed

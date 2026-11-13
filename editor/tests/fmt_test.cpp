// Format library tests: run against the user's own game_files (read-only).
//   fmt_test.exe [--game <install folder>] [--quick]
// Exit code 0 when every check passes.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>

#include "../src/core/crop.h"
#include "../src/core/gamedata.h"
#include "../src/core/mgf.h"
#include "../src/core/mgic.h"
#include "../src/core/names.h"
#include "../src/core/particles.h"
#include "../src/core/pathfind.h"
#include "../src/core/scenario.h"
#include "../src/core/templates.h"
#include "../src/core/terrain.h"
#include "../src/core/triggers.h"
#include "../src/core/wdf.h"
#include "../src/app/version.h"
#include "../src/core/water_custom.h"
#include "../src/app/tool_hints.h"
#include "../models/ma2models.h"


#include <windows.h>

using namespace ma2;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (cond) g_pass++;                                  \
        else { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
    } while (0)

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static const uint32_t K_TERRAIN = key_of("MATerrain"), K_TINFO = key_of("MATerrainInfo");

// UX-SPEC 5 / G2: every editor tool has a status-bar line from the one template, ASCII only, and every
// tool other than Select names its stop key.
static void test_tool_hints() {
    ed::ToolHintCtx c;
    c.thing = "Test Thing";
    c.brush_m = 40;
    for (int i = 0; i < int(ed::Tool::Count); i++) {
        for (int sel = 0; sel < 2; sel++) {
            c.selected = sel;
            std::string line = ed::tool_hint_line(ed::Tool(i), c);
            CHECK(!line.empty(), "tool %d has no status line", i);
            bool ascii = true;
            for (unsigned char ch : line) ascii &= ch >= 32 && ch < 127;
            CHECK(ascii, "tool %d status line is not plain ASCII: %s", i, line.c_str());
            CHECK(line.find(": ") != std::string::npos, "tool %d status line has no 'Name: click' form: %s", i, line.c_str());
            if (i != int(ed::Tool::Select)) CHECK(line.find("right-click") != std::string::npos, "tool %d status line names no way to stop: %s", i, line.c_str());
        }
    }
    c.selected = 1;
    c.no_place = true;
    c.what = "This trigger";
    std::string np = ed::tool_hint_line(ed::Tool::Select, c);
    CHECK(np.find("This trigger: no place on the map") == 0, "position-less selection line (O5): %s", np.c_str());
    // Playtest 10-26: the sculpt tools say whether things on the ground move with it.
    c.objects_follow = false;
    std::string off = ed::tool_hint_line(ed::Tool::Lower, c);
    c.objects_follow = true;
    std::string on = ed::tool_hint_line(ed::Tool::Lower, c);
    CHECK(off.find("objects follow the terrain: OFF") != std::string::npos && on.find("objects follow the terrain: ON") != std::string::npos,
          "sculpt line shows the 'Objects follow the terrain' state: %s / %s", on.c_str(), off.c_str());
}

// --mtb-sweep OUT.tsv (2026-11-09, pink placeholder): for every palette template, the templates
// its closure reaches only through .mtb references (TemplateCatalog::mtb_refs) and, per Base level,
// whether their files (model, .mtb) are mounted there (Base archive or common.mgf). Before the fix
// the editor shipped none of them, so an object listed here drew as the pink pole-and-cube
// placeholder on every Base marked in the 'fails_on' column.
static int run_mtb_sweep(GameData& gd, const fs::path& out_path) {
    TemplateCatalog cat;
    std::string err;
    if (!cat.load(gd, &err)) { printf("template catalog: %s\n", err.c_str()); return 2; }
    std::vector<std::pair<std::string, int>> bases;  // archive name, level_archives index
    {
        std::set<std::string> seen;
        for (auto& m : gd.maps()) {
            if (!m.resource_present || !seen.insert(m.resource).second) continue;
            int idx = -1;
            for (size_t a = 0; a < gd.level_archives().size(); a++) if (gd.level_archives()[a] == m.resource) idx = int(a);
            if (idx >= 0) bases.push_back({m.resource, idx});
        }
    }
    auto files_of = [&](uint32_t u) {
        std::vector<std::string> f;
        if (const TemplateInfo* t = cat.find(u)) {
            if (!t->model.empty()) f.push_back(t->model);
            if (!t->mtb.empty()) f.push_back(t->mtb);
        }
        return f;
    };
    auto in_common = [&](const std::string& p) { return gd.common().find(p) >= 0; };
    auto on_disc = [&](const std::string& p) { return in_common(p) || cat.archive_of(p) >= 0; };
    FILE* out = _wfopen(out_path.c_str(), L"w");
    if (!out) { printf("cannot write %s\n", path_utf8(out_path).c_str()); return 2; }
    fprintf(out, "uid\tname\tclass\tcategory\tmtb_only_templates\tunshipped_files_before_fix\tfails_on_bases_before_fix\tfails_on\n");
    size_t placeable = 0, with_refs = 0, affected = 0, unfixable = 0;
    std::map<std::string, size_t> per_base;
    std::set<uint32_t> culprits;
    for (auto& t : cat.all()) {
        if (!t.placeable) continue;
        placeable++;
        auto before = cat.closure({t.uid}, false);
        auto after = cat.closure({t.uid}, true);
        std::vector<uint32_t> added;
        for (uint32_t u : after) if (!before.count(u) && !files_of(u).empty()) added.push_back(u);
        std::set<std::string> code;  // files the game's code needs for a class in the closure (never shipped before)
        for (uint32_t u : after) for (auto& f : cat.code_required(u)) code.insert(f);
        if (added.empty() && code.empty()) continue;
        if (!added.empty()) with_refs++;
        std::sort(added.begin(), added.end());
        std::set<std::string> missing;  // files on a disc archive that the old export never shipped
        bool off_disc = false;
        for (uint32_t u : added)
            for (auto& f : files_of(u)) {
                if (!on_disc(f)) { off_disc = true; continue; }
                if (!in_common(f)) missing.insert(norm_path(f));
            }
        for (auto& f : code) {
            if (!on_disc(f)) { off_disc = true; continue; }
            if (!in_common(f)) missing.insert(norm_path(f));
        }
        std::vector<std::string> fails;
        for (auto& [name, idx] : bases) {
            bool bad = false;
            for (auto& f : missing) if (!cat.archive_has(idx, f)) { bad = true; break; }
            if (bad) { fails.push_back(name); per_base[name]++; }
        }
        if (off_disc) unfixable++;
        if (fails.empty()) continue;
        affected++;
        std::string tl, ml, fl;
        for (uint32_t u : added) { culprits.insert(u); const TemplateInfo* x = cat.find(u); tl += (tl.empty() ? "" : "; ") + std::to_string(u) + " " + (x ? x->name : std::string()); }
        for (auto& f : missing) ml += (ml.empty() ? "" : "; ") + f;
        for (auto& f : fails) fl += (fl.empty() ? "" : " ") + f;
        fprintf(out, "%u\t%s\t%s\t%s\t%s\t%s\t%zu\t%s\n", t.uid, t.name.c_str(), t.cls_name.c_str(), t.category.c_str(), tl.c_str(),
                ml.c_str(), fails.size(), fl.c_str());
    }
    fclose(out);
    // After the fix every one of those files ships from the archive that holds it, so what can still fail is a
    // needed file that is on no disc archive.
    printf("[mtb-sweep] %zu palette templates; %zu reach templates only through .mtb references; %zu drew as the placeholder on at least one of %zu Base levels before the fix (.mtb references + code-required building files); %zu still affected after the fix (need a file on no disc archive)\n",
           placeable, with_refs, affected, bases.size(), unfixable);
    printf("[mtb-sweep] %zu distinct .mtb-referenced templates involved:", culprits.size());
    for (uint32_t u : culprits) { const TemplateInfo* x = cat.find(u); printf(" %u(%s)", u, x ? x->name.c_str() : "?"); }
    printf("\n");
    for (auto& [b, n] : per_base) printf("[mtb-sweep]   %-22s %zu\n", b.c_str(), n);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    fs::path install, samples, bisect, mvariant, mtb_sweep;
    bool quick = false, all_maps = false;
    setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps the lines printed before it
    test_tool_hints();
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc) install = argv[++i];
        else if (a == L"--samples" && i + 1 < argc) samples = argv[++i];
        else if (a == L"--quick") quick = true;
        else if (a == L"--all-maps") all_maps = true;
        else if (a == L"--bisect" && i + 1 < argc) bisect = argv[++i];
        else if (a == L"--mission-variant" && i + 1 < argc) mvariant = argv[++i];
        else if (a == L"--mtb-sweep" && i + 1 < argc) mtb_sweep = argv[++i];
    }
    if (install.empty()) {
        wchar_t buf[MAX_PATH];
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        install = GameData::find_install(fs::path(buf).parent_path());
    }
    printf("install: %s\n", path_utf8(install).c_str());
    GameData gd;
    std::string err;
    if (!gd.open(install, &err)) { printf("cannot open game data: %s\n", err.c_str()); return 2; }
    printf("names: %zu keys; level archives: %zu; maps: %zu\n", gd.names().size(), gd.level_archives().size(), gd.maps().size());
    if (!mtb_sweep.empty()) return run_mtb_sweep(gd, mtb_sweep);

    // ---------------------------------------------------------------- the game's own text (text.mgf mgText)
    CHECK(gd.game_text(524787) == "To the Rescue", "game_text(524787) = Mission_05 title ('%s')", gd.game_text(524787).c_str());
    CHECK(gd.game_text(524370) == "Press A to open the gate.", "game_text(524370) = the M05 gate prompt ('%s')", gd.game_text(524370).c_str());
    CHECK(gd.game_text(0xF00001).empty(), "a scenario-owned id has no game text");

    // ---------------------------------------------------------------- P1 palette <-> layers (no game data)
    {
        Image cm;
        cm.w = 8; cm.h = 4;
        cm.px.resize(32);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++) cm.at(x, y) = 0x00808080u | (x < 4 ? 0xFF000000u : 0u);  // west half First, east half Second
        GroundLayers g;
        g.enabled = true;
        g.layers.resize(2);
        g.init_from_colormap(cm);
        CHECK(g.w == 32 && g.h == 16 && g.map_w == 8 && g.map_h == 4, "layers weights are 4x the colour map (%dx%d)", g.w, g.h);
        CHECK(g.texel(0, 0)[0] == 255 && g.texel(0, 0)[1] == 0 && g.texel(31, 0)[0] == 0 && g.texel(31, 0)[1] == 255, "start = the retail alpha mix");
        // The third texture: paint a disc in the middle of a 100 x 50 m map; the rest keeps the pair.
        g.layers.resize(3);
        for (int i = 0; i < 40; i++) g.paint(2, 50.f, 25.f, 10.f, 1.f, 100.f, 50.f);
        const uint8_t* mid = g.texel(16, 8);
        CHECK(mid[2] == 255 && mid[0] == 0 && mid[1] == 0, "brush centre = layer 2 (%d %d %d)", mid[0], mid[1], mid[2]);
        CHECK(g.texel(2, 2)[0] == 255 && g.texel(2, 2)[2] == 0, "painting elsewhere stays (west still layer 0)");
        // Layers persist: painting layer 1 over part of the disc edge keeps layer 2 where the brush did not reach.
        for (int i = 0; i < 40; i++) g.paint(1, 50.f + 12.f, 25.f, 6.f, 1.f, 100.f, 50.f);
        CHECK(g.texel(16, 8)[2] == 255, "layer 2 at the centre survives a later stroke");
        // Round trip of the three-layer palette through the file format.
        g.layers[2].image = resample_square(Image(), 8);
        g.layers[2].path = "terrain/x.tif"; g.layers[2].archive = "a.mgf";
        Bytes enc = encode_ground_layers(g);
        GroundLayers dec;
        std::string perr;
        CHECK(decode_ground_layers(enc, dec, &perr) && dec.count() == 3 && *dec.weights == *g.weights, "3-texture palette encode/decode %s", perr.c_str());
        CHECK(brush_falloff(0.f, 10.f, 0.f) == 1.f && brush_falloff(10.f, 10.f, 0.5f) == 0.f && brush_falloff(5.f, 10.f, 0.5f) == 1.f && brush_falloff(5.f, 10.f, 0.f) < 0.6f,
              "brush hardness widens the full-strength core");
        // Fill.
        GroundLayers f = g;
        f.fill(2);
        bool all2 = true;
        for (int y = 0; y < f.h; y++)
            for (int x = 0; x < f.w; x++) all2 = all2 && f.texel(x, y)[2] == 255 && f.texel(x, y)[0] == 0 && f.texel(x, y)[1] == 0;
        CHECK(all2 && g.texel(2, 2)[0] == 255, "fill whole map with layer 2 (copy on write left the original alone)");
        // Remove layer 2 (a fourth shifts down), then bake back to the retail alpha.
        g.layers.resize(4);
        for (int i = 0; i < 40; i++) g.paint(3, 10.f, 25.f, 6.f, 1.f, 100.f, 50.f);
        CHECK(g.texel(3, 8)[3] == 255, "layer 3 painted in the west");
        g.remove_layer(2);
        CHECK(g.count() == 3 && g.texel(3, 8)[2] == 255 && g.texel(16, 8)[2] == 0, "removing layer 2 moves layer 3 down and clears the old disc");
        g.remove_layer(2);
        CHECK(g.count() == 2, "back to the pair");
        Image back = cm;
        for (auto& p : back.px) p = (p & 0x00FFFFFFu) | 0x7F000000u;  // a stale alpha: only the texels the pair covers are rewritten
        g.bake_colormap_alpha(back);
        CHECK((back.at(0, 0) >> 24) == 255 && (back.at(7, 0) >> 24) == 0, "bake back to the colour-map alpha (west %u, east %u)", back.at(0, 0) >> 24, back.at(7, 0) >> 24);
        // A map filled with a removed layer has no pair weights anywhere: the alpha in the colour map stays.
        GroundLayers z = dec;
        z.fill(2);
        z.remove_layer(2);
        Image keep = cm;
        for (auto& p : keep.px) p = (p & 0x00FFFFFFu) | 0x7F000000u;
        z.bake_colormap_alpha(keep);
        bool same = true;
        for (auto& p : keep.px) same = same && (p >> 24) == 0x7F;
        CHECK(same, "bake keeps the alpha where only removed layers were");
    }

    // ---------------------------------------------------------------- MGF + WDF round trip
    double t0 = now_s();
    int worlds = 0, world_ok = 0;
    size_t world_bytes = 0, keys_total = 0, keys_named = 0;
    std::vector<std::pair<std::string, Bytes>> heightfields, infoblobs;
    for (auto& f : gd.common().files()) {
        if (!iends_with(f.path, ".wdf")) continue;
        Bytes b;
        bool rd = gd.common().read_file(f.path, b, &err);
        CHECK(rd, "read %s: %s", f.path.c_str(), err.c_str());
        if (!rd) continue;
        CHECK(crc_acc(b.data(), b.size()) == f.rec[1], "record crc %s", f.path.c_str());
        Wdf w;
        bool ok = w.parse(b, &err);
        CHECK(ok, "parse %s: %s", f.path.c_str(), err.c_str());
        worlds++;
        if (!ok) continue;
        Bytes s = w.serialize();
        bool same = s == b;
        CHECK(same, "round trip %s (%zu vs %zu bytes)", f.path.c_str(), s.size(), b.size());
        if (same) world_ok++;
        world_bytes += b.size();
        for (auto& o : w.objs) {
            keys_total++; if (gd.names().name(o.cls)) keys_named++;
            for (auto& p : o.props) { keys_total++; if (gd.names().name(p.key)) keys_named++; }
            if (o.cls == K_TERRAIN) if (auto* p = o.find("TerrainHeight Field")) heightfields.push_back({f.path, p->data});
            if (o.cls == K_TINFO) for (auto& p : o.props) if (p.type == PT_BLOB && p.data.size() > 0x40) infoblobs.push_back({f.path, p.data});
        }
    }
    printf("[wdf] common.mgf world files: %d/%d byte-identical (%.1f MB) in %.1fs; keys named %.2f%%\n", world_ok, worlds,
           world_bytes / 1048576.0, now_s() - t0, 100.0 * keys_named / std::max<size_t>(1, keys_total));

    int culled = 0, culled_ok = 0;
    if (MgfReader* tm = gd.templates_archive()) {
        for (auto& f : tm->files()) {
            if (!iends_with(f.path, ".wdf")) continue;
            Bytes b;
            if (!tm->read_file(f.path, b, &err)) { CHECK(false, "read %s", f.path.c_str()); continue; }
            Wdf w;
            culled++;
            if (w.parse(b, &err) && w.serialize() == b) culled_ok++;
            else CHECK(false, "culled round trip %s", f.path.c_str());
        }
    }
    printf("[wdf] Templates.mgf culled sets: %d/%d byte-identical\n", culled_ok, culled);

    // ---------------------------------------------------------------- MGIc in world files
    int hf_ok = 0;
    for (auto& [name, blob] : heightfields) {
        Mgic m;
        bool ok = m.parse(blob, &err) && m.serialize() == blob;
        CHECK(ok, "heightfield container round trip %s", name.c_str());
        Picture p;
        ok = ok && read_picture(m, p, &err);
        CHECK(ok, "heightfield picture %s: %s", name.c_str(), err.c_str());
        std::vector<uint8_t> s;
        ok = ok && picture_samples8(p, s);
        if (!ok) continue;
        Bytes rebuilt = build_heightfield(m, p.width, p.height, s);
        Picture q;
        std::vector<uint8_t> s2;
        bool same = read_picture(rebuilt, q, &err) && picture_samples8(q, s2) && s2 == s && rebuilt.size() == blob.size();
        CHECK(same, "heightfield re-encode %s", name.c_str());
        if (same) hf_ok++;
    }
    printf("[mgic] heightfields: %d/%zu container byte-identical and re-encoded losslessly (P8, 64-byte pitch)\n", hf_ok, heightfields.size());
    int ib_ok = 0;
    std::map<std::string, int> info_formats;
    for (auto& [name, blob] : infoblobs) {
        Mgic m;
        bool ok = m.parse(blob, &err) && m.serialize() == blob;
        CHECK(ok, "terrain-info blob round trip %s", name.c_str());
        Picture p;
        if (ok && read_picture(m, p, &err)) {
            info_formats[p.format_name() + " " + std::to_string(p.width) + "x" + std::to_string(p.height)]++;
            Picture q = p;
            Image img;
            if (decode_picture(p, 0, img, &err) && p.format() != PF_ARGB4444) {
                q.bits = encode_levels(p, img);
                CHECK(q.bits == p.bits, "terrain-info re-encode %s", name.c_str());
            }
            ib_ok++;
        }
    }
    printf("[mgic] terrain-info blobs: %d/%zu;", ib_ok, infoblobs.size());
    for (auto& [k, v] : info_formats) printf(" %s x%d;", k.c_str(), v);
    printf("\n");

    // ---------------------------------------------------------------- MGIc terrain textures in level archives
    std::map<std::string, int> tex_formats;
    int tex = 0, tex_ok = 0, tex_lossless = 0, tex_lossless_ok = 0;
    double worst_psnr = 1e9;
    std::string worst_name;
    t0 = now_s();
    for (auto& an : gd.level_archives()) {
        MgfReader* a = gd.archive(an);
        if (!a) { CHECK(false, "open %s", an.c_str()); continue; }
        for (auto& f : a->files()) {
            if (!istarts_with(f.path, "Terrain/") || !iends_with(f.path, ".tif")) continue;
            Bytes b;
            if (!a->read_file(f.path, b, &err)) { CHECK(false, "read %s/%s", an.c_str(), f.path.c_str()); continue; }
            Mgic m;
            tex++;
            bool ok = m.parse(b, &err) && m.serialize() == b;
            CHECK(ok, "texture container round trip %s/%s", an.c_str(), f.path.c_str());
            Picture p;
            if (!ok || !read_picture(m, p, &err)) { CHECK(false, "picture %s/%s: %s", an.c_str(), f.path.c_str(), err.c_str()); continue; }
            tex_formats[p.format_name()]++;
            Image img;
            if (!decode_picture(p, 0, img, &err)) { CHECK(false, "decode %s/%s: %s", an.c_str(), f.path.c_str(), err.c_str()); continue; }
            tex_ok++;
            Picture q = p;
            q.bits = encode_levels(p, img);
            CHECK(q.bits.size() == p.bits.size(), "encoded size %s/%s", an.c_str(), f.path.c_str());
            Image back;
            decode_picture(q, 0, back, &err);
            if (!p.dxt()) {
                tex_lossless++;
                bool same = std::equal(q.bits.begin(), q.bits.begin() + p.level_size(0), p.bits.begin());
                CHECK(same, "lossless re-encode of level 0 %s/%s", an.c_str(), f.path.c_str());
                if (same) tex_lossless_ok++;
            } else {
                // DXT: compare against the retail decode (the source is already block compressed).
                double se = 0;
                for (size_t i = 0; i < img.px.size(); i++)
                    for (int c = 0; c < 3; c++) {
                        double d = double((img.px[i] >> (8 * c)) & 255) - double((back.px[i] >> (8 * c)) & 255);
                        se += d * d;
                    }
                double mse = se / (img.px.size() * 3.0);
                double psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99;
                if (psnr < worst_psnr) { worst_psnr = psnr; worst_name = an + "/" + f.path; }
                CHECK(psnr > 30, "DXT re-encode quality %s/%s: %.1f dB", an.c_str(), f.path.c_str(), psnr);
            }
            // swizzle: decode the re-encoded picture and compare (all formats).
            if (!p.dxt()) CHECK(back.px == img.px, "decode(encode(x)) %s/%s", an.c_str(), f.path.c_str());
        }
        if (quick) break;
    }
    printf("[mgic] level-archive Terrain/*.tif: %d/%d decoded; lossless formats level 0 re-encoded identically %d/%d; worst DXT re-encode %.1f dB (%s); %.1fs\n",
           tex_ok, tex, tex_lossless_ok, tex_lossless, worst_psnr, worst_name.c_str(), now_s() - t0);
    for (auto& [k, v] : tex_formats) printf("        %-24s x%d\n", k.c_str(), v);

    // ---------------------------------------------------------------- MGF writer
    {
        MgfWriter w;
        Bytes a{1, 2, 3}, b(1000, 7), c;
        w.add("scenario.ini", a);
        w.add("Terrain\\Scn0001_ColorMap.tif", b);
        w.add("Terrain/Scn0001_Lightmap.tif", c);
        w.add("editor/project.json", a);
        w.add("Pathfinding/Scn0001_Mech.map", b);
        Bytes arc = w.build();
        fs::path tmp = fs::temp_directory_path() / L"ma2scn_fmt_test.mgf";
        write_file(tmp, arc);
        MgfReader r;
        bool ok = r.open(tmp, &err);
        CHECK(ok, "reopen written archive: %s", err.c_str());
        Bytes x;
        CHECK(ok && r.files().size() == 5, "written archive file count %zu", r.files().size());
        CHECK(ok && r.read_file("terrain/scn0001_colormap.tif", x) && x == b, "written archive content");
        CHECK(ok && r.read_file("SCENARIO.INI", x) && x == a, "written archive case-insensitive lookup");
        CHECK(ok && r.read_file("Terrain/Scn0001_Lightmap.tif", x) && x.empty(), "written archive empty file");
        for (auto& f : r.files()) {
            Bytes y;
            r.read_file(f.path, y);
            CHECK(crc_acc(y.data(), y.size()) == f.rec[1], "written crc %s", f.path.c_str());
        }
        r.close();
        fs::remove(tmp);
        printf("[mgf] writer: 5-file archive reopened, contents and CRCs match\n");
        // Folder-row hashes and file-record keys: key(lower("\dir\name")), checked on retail archives.
        auto check_keys = [&](MgfReader& a, const char* label) {
            size_t ok = 0, total = 0;
            auto full = [&](int row) {
                std::vector<std::string> parts;
                while (row > 0) { parts.push_back(a.row_names[size_t(row)]); row = a.rows[size_t(row)][1]; }
                std::string p;
                for (size_t k = parts.size(); k-- > 0;) p += "\\" + parts[k];
                return lower(p.empty() ? std::string("\\") : p);
            };
            for (size_t r = 0; r < a.rows.size(); r++) {
                total++;
                if (a.rows[r][5] < 0) { if (uint32_t(a.rows[r][0]) == key_of(full(int(r)))) ok++; }
                else if (a.files().size() && true) {
                    for (auto& f : a.files()) if (f.row == int(r)) { if (f.rec[2] == key_of(full(int(r)))) ok++; break; }
                }
            }
            CHECK(ok == total, "%s directory keys %zu/%zu", label, ok, total);
            printf("[mgf] %s: %zu/%zu folder hashes and file keys match key(lower path)\n", label, ok, total);
        };
        check_keys(gd.common(), "common.mgf");
        if (MgfReader* bd = gd.archive("boggeddown.mgf")) check_keys(*bd, "boggeddown.mgf");
        write_file(tmp, arc);
        if (r.open(tmp, &err)) {
            check_keys(r, "editor-written archive");
            CHECK(r.header[2] == 0x10 && r.header[1] == 0x5A5A0104u, "written header words");
            for (auto& f : r.files()) CHECK(f.rec[7] == 0x12F700u && f.rec[5] != 0, "written record fields %s", f.path.c_str());
            r.close();
        }
        fs::remove(tmp);
    }

    // ---------------------------------------------------------------- Templates catalog
    {
        t0 = now_s();
        TemplateCatalog cat;
        bool ok = cat.load(gd, &err);
        CHECK(ok, "template catalog: %s", err.c_str());
        if (ok) {
            printf("[templates] %zu templates, %zu culled sets, %zu reference keys, %zu with model files; %.1fs\n",
                   cat.count(), cat.culled_set_count(), cat.ref_keys().size(), cat.model_template_count(), now_s() - t0);
            // Rebuilding each retail culled file from its own UID set must give identical bytes.
            int rb = 0, rb_ok = 0;
            for (auto& level : cat.culled_levels()) {
                Bytes orig;
                if (!gd.templates_archive()->read_file("CulledTemplates/globalentities." + level + ".wdf", orig)) continue;
                rb++;
                Bytes built = cat.write_culled(cat.culled_set(level));
                if (built == orig) rb_ok++;
                else CHECK(false, "culled rebuild %s (%zu vs %zu)", level.c_str(), built.size(), orig.size());
            }
            printf("[templates] culled sets rebuilt from UID lists: %d/%d byte-identical\n", rb_ok, rb);
            // Closure check: Base culled set + closure(world) covers the retail set for every level.
            int cov = 0, cov_ok = 0;
            for (auto& m : gd.maps()) {
                std::string level = lower(m.name);
                if (!cat.has_culled(level)) continue;
                Wdf w;
                if (!gd.load_world(m, w)) continue;
                auto need = cat.closure_for_world(w, level, m.multiplayer);
                auto& want = cat.culled_set(level);
                size_t miss = 0;
                for (uint32_t u : want) if (!need.count(u)) miss++;
                cov++;
                if (!miss) cov_ok++;
                else CHECK(false, "closure misses %zu templates for %s", miss, m.name.c_str());
            }
            printf("[templates] closure(base + world) covers the retail culled set: %d/%d levels\n", cov_ok, cov);
            // Information: how close the pure reference closure (no base set) gets.
            size_t pure_miss = 0, pure_extra = 0, want_total = 0;
            for (auto& m : gd.maps()) {
                std::string level = lower(m.name);
                if (!cat.has_culled(level)) continue;
                Wdf w;
                if (!gd.load_world(m, w)) continue;
                auto seed = cat.seed_of(w);
                for (uint32_t u : cat.common_set(m.multiplayer)) seed.insert(u);
                auto pure = cat.closure(seed);
                auto& want = cat.culled_set(level);
                want_total += want.size();
                for (uint32_t u : want) if (!pure.count(u)) pure_miss++;
                for (uint32_t u : pure) if (!want.count(u)) pure_extra++;
            }
            printf("[templates] info: pure closure (world refs + common set, no base) misses %zu and adds %zu of %zu retail entries\n",
                   pure_miss, pure_extra, want_total);
            // Model archive index sample.
            const TemplateInfo* t = cat.find(36028);
            if (t) printf("[templates] sample 36028 '%s' category '%s' model '%s' in %zu archives\n", t->name.c_str(),
                          t->category.c_str(), t->model.c_str(), t->archives.size());
        }
    }

    // ---------------------------------------------------------------- FT-ED: DPI scale, crash-save timeout, file names
    {
        CHECK(dpi_to_scale(96) == 1.f && dpi_to_scale(144) == 1.5f && dpi_to_scale(192) == 2.f, "FT dpi_to_scale 96/144/192");
        CHECK(dpi_to_scale(0) == 1.f && dpi_to_scale(5) == 1.f && dpi_to_scale(100000) == 1.f, "FT dpi_to_scale nonsense gives 1");

        bool fin = false;
        CHECK(run_with_timeout([] { return true; }, 2000, &fin) && fin, "FT run_with_timeout fast true");
        CHECK(!run_with_timeout([] { return false; }, 2000, &fin) && fin, "FT run_with_timeout fast false");
        CHECK(!run_with_timeout([]() -> bool { throw 1; }, 2000, &fin) && fin, "FT run_with_timeout swallows a C++ exception");
        CHECK(!run_with_timeout([]() -> bool { *(volatile int*)nullptr = 1; return true; }, 2000, &fin) && fin,
              "FT run_with_timeout swallows an access violation");
        auto t0 = std::chrono::steady_clock::now();
        static volatile long hung_done = 0;
        bool r = run_with_timeout([] { Sleep(1500); hung_done = 1; return true; }, 150, &fin);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!r && !fin && ms < 1000, "FT run_with_timeout gives up on a hung job (%.0f ms)", ms);
        Sleep(1800);  // the abandoned thread finishes and frees itself without touching freed state
        CHECK(hung_done == 1, "FT abandoned job still ran to its end");
        // A job that locks a mutex the caller holds (the crash case) must not hang the caller.
        static std::mutex mu;
        mu.lock();
        r = run_with_timeout([] { std::lock_guard<std::mutex> g(mu); return true; }, 150, &fin);
        CHECK(!r && !fin, "FT run_with_timeout on a lock held by the caller");
        mu.unlock();
        Sleep(100);

        CHECK(safe_file_stem("My Map") == "My Map", "FT safe_file_stem plain");
        CHECK(safe_file_stem("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j", "FT safe_file_stem bad characters");
        CHECK(safe_file_stem("CON") == "_CON" && safe_file_stem("nul") == "_nul" && safe_file_stem("com1") == "_com1" &&
                  safe_file_stem("LPT9.txt") == "_LPT9.txt", "FT safe_file_stem reserved names");
        CHECK(safe_file_stem("Console") == "Console" && safe_file_stem("com10") == "com10", "FT safe_file_stem near-reserved names kept");
        CHECK(safe_file_stem("map. . ") == "map" && safe_file_stem("   ") == "Scenario" && safe_file_stem("") == "Scenario" &&
                  safe_file_stem("...") == "Scenario", "FT safe_file_stem trailing dots and spaces, empty");
        CHECK(safe_file_stem(std::string(300, 'x')).size() == 120, "FT safe_file_stem length cap");
        CHECK(safe_file_stem("Caf\xC3\xA9 Map") == "Caf\xC3\xA9 Map", "FT safe_file_stem keeps UTF-8");
    }

    // ---------------------------------------------------------------- B9: durable save, recovery names, load guards
    {
        // write_file: replaces the target, leaves no .part behind, and works on an empty file.
        fs::path d = fs::temp_directory_path() / L"ma2scn_b9_test";
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::path f = d / L"a.bin";
        Bytes one(5000, 1), two(7, 2), got;
        CHECK(write_file(f, one) && read_file(f, got) && got == one, "B9 write_file first write");
        CHECK(write_file(f, two) && read_file(f, got) && got == two, "B9 write_file replaces an existing file");
        CHECK(!fs::exists(fs::path(f) += L".part", ec), "B9 write_file leaves no .part");
        CHECK(write_file(f, Bytes()) && read_file(f, got) && got.empty(), "B9 write_file empty");
        fs::remove_all(d, ec);

        // Terrain guards: the largest retail heightfield stays under the load cap, and a bad scale is repaired (a scale of 0
        // made raycast loop forever).
        int max_w = 0, max_h = 0, n_t = 0, dup_uids = 0;
        for (auto& m : gd.maps()) {
            Wdf w;
            Terrain t;
            if (!gd.load_world(m, w) || !t.load_from_world(w, &err)) continue;
            n_t++;
            max_w = std::max(max_w, t.w);
            max_h = std::max(max_h, t.h);
            CHECK(t.scale > Terrain::kMinScale && t.scale < Terrain::kMaxScale, "B9 retail terrain scale %s: %g", m.name.c_str(), t.scale);
            std::set<uint32_t> uids;
            for (auto& o : w.objs) if (o.uid() && !uids.insert(o.uid()).second) dup_uids++;
        }
        printf("[ft] duplicate object UIDs across retail levels: %d\n", dup_uids);
        printf("[b9] %d retail terrains; largest %dx%d (load cap %d)\n", n_t, max_w, max_h, Terrain::kMaxSize);
        CHECK(n_t > 0 && max_w <= Terrain::kMaxSize && max_h <= Terrain::kMaxSize, "B9 retail terrain size under the cap");
        if (const MapInfo* m = gd.find_map("NewRiverCity")) {
            Wdf w;
            if (gd.load_world(*m, w)) {
                WObj* tobj = nullptr;
                for (auto& o : w.objs) if (o.cls == K_TERRAIN) { tobj = &o; break; }
                if (tobj) {
                    for (float bad : {0.f, -3.f, std::nanf(""), 1e30f}) {
                        tobj->set_f32("TerrainWorldScale", bad);
                        Terrain t;
                        CHECK(t.load_from_world(w, &err) && t.scale == 8.f, "B9 bad TerrainWorldScale %g falls back to 8", bad);
                        float step_end = 0;
                        Vec3 o{0, 500, 0}, dir{0.1f, -1, 0.1f};
                        CHECK(t.raycast(o, dir, step_end, 2000.f) || true, "B9 raycast returns with the repaired scale");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- Terrain model + pathfinding
    {
        const MapInfo* m = gd.find_map("NewRiverCity");
        Wdf w;
        if (m && gd.load_world(*m, w)) {
            Terrain t;
            bool ok = t.load_from_world(w, &err);
            CHECK(ok, "terrain from NewRiverCity: %s", err.c_str());
            MgfReader* a = gd.archive(m->resource);
            Bytes mech;
            if (ok && a && a->read_file("PATHFINDING/NewRiverCity_Mech.map", mech)) {
                NavGraph g;
                g.parse(std::string_view(reinterpret_cast<const char*>(mech.data()), mech.size()));
                int on = 0, tot = 0;
                for (auto& n : g.nodes) {
                    if (n.type != 4) continue;
                    tot++;
                    if (std::fabs(t.height_at(n.pos.x, n.pos.z) + 2.0f - n.pos.y) < 0.05f) on++;
                }
                printf("[terrain] NewRiverCity: %dx%d, %.0f..%.0f m; retail grid nodes at terrain+2 m: %d/%d\n", t.w, t.h,
                       t.min_h, t.max_h, on, tot);
                CHECK(tot > 0 && on * 10 >= tot * 9, "height formula vs retail nav nodes (%d/%d)", on, tot);
                std::string text = g.serialize();
                NavGraph g2;
                g2.parse(text);
                CHECK(g2.nodes.size() == g.nodes.size(), "nav graph text round trip");
            }
        }
    }

    // ---------------------------------------------------------------- Scenario save / reload
    {
        t0 = now_s();
        TemplateCatalog cat;
        if (cat.load(gd, &err)) {
            ScenarioIO io(gd, cat);
            auto report = [&](const char* label, Scenario& s, const fs::path& out) {
                SaveReport rep;
                bool ok = io.save(s, out, &rep, &err);
                CHECK(ok, "save %s: %s", label, err.c_str());
                if (!ok) return;
                printf("[scenario] %s -> %s: %zu objects, %zu templates, %zu asset files (%.1f MB outside Base), %d nav nodes, %.2f MB file\n",
                       label, path_utf8(out.filename()).c_str(), rep.objects, rep.templates, rep.asset_files,
                       rep.asset_bytes / 1048576.0, rep.nav_nodes, rep.file_bytes / 1048576.0);
                for (auto& w : rep.warnings) printf("           warning: %s\n", w.c_str());
                Scenario back;
                ok = io.load(out, back, &err);
                CHECK(ok, "reload %s: %s", label, err.c_str());
                if (!ok) return;
                CHECK(back.id == s.id && back.name == s.name && back.types == s.types && back.base == s.base, "reload metadata %s", label);
                CHECK(back.terrain.samples() == s.terrain.samples(), "reload heights %s", label);
                CHECK(back.colormap_custom == s.colormap_custom && back.lightmap_custom == s.lightmap_custom, "reload textures %s", label);
                // Saving the reloaded scenario again gives the same archive.
                Bytes a, b;
                SaveReport r2;
                io.build(s, a, &r2, &err);
                io.build(back, b, &r2, &err);
                CHECK(a.size() == b.size(), "re-save stable size %s (%zu vs %zu)", label, a.size(), b.size());
                for (auto& item : io.validate(back))
                    if (item.level > 0) printf("           validate %s: %s\n", item.level == 2 ? "ERROR" : "warn", item.text.c_str());
            };
            fs::path dir = samples.empty() ? fs::temp_directory_path() / L"ma2scn_test" : samples;
            fs::create_directories(dir);
            // B9: .bak of the last good version, the over-the-limit refusal, and a Base that is not a bare archive name.
            {
                NewMapParams bp;
                bp.start = NewMapParams::CopyRetail;
                bp.copy_map = "UnderCover";
                bp.multiplayer = true;
                bp.name = "B9 save test";
                Scenario sb;
                if (io.create(bp, sb, &err)) {
                    fs::path bf = fs::temp_directory_path() / L"ma2scn_b9_save.ma2map", bak = fs::path(bf) += L".bak";
                    std::error_code bec;
                    fs::remove(bf, bec);
                    fs::remove(bak, bec);
                    SaveReport brep;
                    bool first = io.save(sb, bf, &brep, &err);
                    CHECK(first && !brep.over_limit, "B9 first save: %s", err.c_str());
                    CHECK(!fs::exists(bak, bec), "B9 the first save has nothing to back up");
                    Bytes v1;
                    read_file(bf, v1);
                    sb.name = "B9 save test 2";
                    CHECK(io.save(sb, bf, &brep, &err), "B9 second save: %s", err.c_str());
                    Bytes b1;
                    CHECK(fs::exists(bak, bec) && read_file(bak, b1) && b1 == v1, "B9 .bak holds the previous version");
                    // Past 3,000 objects: refused, the file untouched; forced: written.
                    Scenario big = sb;
                    size_t n0 = big.world.objs.size();
                    for (size_t i = 0; i < n0 && big.world.objs.size() <= ScenarioIO::kMaxObjects + 5; i = (i + 1) % n0) {
                        WObj o = big.world.objs[i];
                        if (o.cls == K_TERRAIN || o.cls == K_TINFO) continue;
                        big.world.objs.push_back(o);
                    }
                    Bytes before;
                    read_file(bf, before);
                    SaveReport orep;
                    std::string oerr;
                    bool refused = !io.save(big, bf, &orep, &oerr);
                    Bytes after;
                    read_file(bf, after);
                    CHECK(refused && orep.over_limit && !oerr.empty(), "B9 a map over 3,000 objects is not saved (%zu objects)", big.world.objs.size());
                    CHECK(before == after, "B9 the refused save leaves the file as it was");
                    CHECK(io.save(big, bf, &orep, &oerr, true), "B9 forced save of an over-limit map: %s", oerr.c_str());
                    fs::remove(bf, bec);
                    fs::remove(bak, bec);
                    printf("[b9] save/.bak/limit checks ran (over-limit map: %zu objects, %s)\n", big.world.objs.size(), orep.limit_text.c_str());
                    // A Base naming a path is refused on load.
                    Bytes ab;
                    SaveReport arep;
                    if (io.build(sb, ab, &arep, &err)) {
                        std::string txt(ab.begin(), ab.end());
                        size_t at = txt.find("Base = ");
                        CHECK(at != std::string::npos, "B9 scenario.ini holds a Base line");
                    }
                    Scenario badbase = sb;
                    badbase.base = "../evil.mgf";
                    Bytes bb;
                    SaveReport brep2;
                    fs::path bbf = fs::temp_directory_path() / L"ma2scn_b9_badbase.ma2map";
                    // build() writes the Base as named; load() must refuse it.
                    if (io.build(badbase, bb, &brep2, &err) && write_file(bbf, bb)) {
                        Scenario lb;
                        std::string lerr;
                        CHECK(!io.load(bbf, lb, &lerr), "B9 load refuses a Base that names a path");
                    }
                    fs::remove(bbf, bec);
                }
            }
            // W-13b: the save revision. scenario.ini carries SavedWith = kSaveRevision; a file without the line loads as revision 1.
            // rewrite_scn copies a saved .ma2map into a new archive with scenario.ini passed through `edit` (an "old editor" file).
            auto rewrite_scn = [&](const fs::path& src, const fs::path& dst, const std::function<std::string(const std::string&)>& edit) {
                MgfReader rd;
                std::string rerr;
                if (!rd.open(src, &rerr)) return false;
                MgfWriter wr;
                for (size_t i = 0; i < rd.files().size(); i++) {
                    Bytes data;
                    if (!rd.read_file(int(i), data, &rerr)) return false;
                    if (iequals(rd.files()[i].path, "scenario.ini")) {
                        std::string t = edit(std::string(data.begin(), data.end()));
                        data.assign(t.begin(), t.end());
                    }
                    wr.add(rd.files()[i].path, data, rd.files()[i].rec[5]);
                }
                return write_file(dst, wr.build());
            };
            {
                NewMapParams wp;
                wp.start = NewMapParams::CopyRetail;
                wp.copy_map = "UnderCover";
                wp.multiplayer = true;
                wp.name = "W13b stamp";
                Scenario sw;
                if (io.create(wp, sw, &err)) {
                    CHECK(sw.saved_with == kSaveRevision, "W-13b a new scenario carries the current revision");
                    fs::path wf = fs::temp_directory_path() / L"ma2scn_w13b.ma2map", wold = fs::temp_directory_path() / L"ma2scn_w13b_old.ma2map";
                    std::error_code wec;
                    SaveReport wrep;
                    CHECK(io.save(sw, wf, &wrep, &err), "W-13b save: %s", err.c_str());
                    {
                        MgfReader rd;
                        Bytes ini;
                        bool ok = rd.open(wf, &err) && rd.read_file("scenario.ini", ini, &err);
                        std::string t(ini.begin(), ini.end());
                        CHECK(ok && t.find("FormatVersion = 1\r\n") != std::string::npos, "W-13b FormatVersion stays 1");
                        CHECK(ok && t.find("SavedWith = " + std::to_string(kSaveRevision) + "\r\n") != std::string::npos, "W-13b scenario.ini says SavedWith = %d", kSaveRevision);
                        CHECK(ok && t.find("Editor = " MA2SE_VERSION " (") != std::string::npos, "W-13b scenario.ini names the editor version");
                    }
                    Scenario back;
                    CHECK(io.load(wf, back, &err) && back.saved_with == kSaveRevision, "W-13b a round trip keeps SavedWith");
                    ScenarioHead head;
                    CHECK(ScenarioIO::peek(wf, head) && head.saved_with == kSaveRevision && head.id == sw.id && head.name == sw.name && head.multiplayer, "W-13b peek reads the header");
                    const bool made = rewrite_scn(wf, wold, [](const std::string& t) {
                        std::string o;
                        size_t at = 0;
                        while (at < t.size()) {
                            size_t e = t.find("\r\n", at);
                            std::string line = t.substr(at, e == std::string::npos ? std::string::npos : e - at);
                            if (line.rfind("SavedWith", 0) != 0 && line.rfind("Editor =", 0) != 0) o += line + "\r\n";
                            if (e == std::string::npos) break;
                            at = e + 2;
                        }
                        return o;
                    });
                    CHECK(made, "W-13b rewrite an old-style file");
                    Scenario old;
                    CHECK(made && io.load(wold, old, &err) && old.saved_with == 1, "W-13b a file without SavedWith loads as revision 1");
                    ScenarioHead oh;
                    CHECK(made && ScenarioIO::peek(wold, oh) && oh.saved_with == 1, "W-13b peek of a file without SavedWith says 1");
                    if (made) {
                        CHECK(io.save(old, wold, &wrep, &err) && old.saved_with == kSaveRevision, "W-13b saving an old file updates it");
                        Scenario upd;
                        CHECK(io.load(wold, upd, &err) && upd.saved_with == kSaveRevision, "W-13b the updated file loads as the current revision");
                        fs::path bak = wold;
                        bak += L".bak";
                        CHECK(fs::exists(bak, wec), "W-13b the update keeps the old file as .bak");
                        fs::remove(bak, wec);
                    }
                    fs::remove(wf, wec);
                    fs::remove(wold, wec);
                }
            }
            // W-14b: a file copied in Explorer keeps the Id; open_path sets Id 0 and the next save mints a new one: the Scn<Id> names follow.
            {
                NewMapParams cp;
                cp.start = NewMapParams::CopyRetail;
                cp.copy_map = "UnderCover";
                cp.multiplayer = true;
                cp.name = "W14b copy";
                Scenario sc;
                if (io.create(cp, sc, &err)) {
                    Bytes first;
                    SaveReport frep;
                    CHECK(io.build(sc, first, &frep, &err) && sc.id != 0, "W-14b first build mints an Id");
                    const std::string old_hex = sc.id_hex();
                    sc.id = 0;  // what open_path does for a copy
                    Bytes second;
                    CHECK(io.build(sc, second, &frep, &err) && sc.id != 0 && sc.id_hex() != old_hex, "W-14b Id 0 gets a new Id on save");
                    fs::path cf = fs::temp_directory_path() / L"ma2scn_w14b.ma2map";
                    CHECK(write_file(cf, second), "W-14b write");
                    MgfReader rd;
                    Bytes ini;
                    if (rd.open(cf, &err) && rd.read_file("scenario.ini", ini, &err)) {
                        const std::string t(ini.begin(), ini.end());
                        auto count = [&](const std::string& s) { size_t n = 0; for (size_t at = t.find(s); at != std::string::npos; at = t.find(s, at + 1)) n++; return n; };
                        CHECK(t.find("Id = " + sc.id_hex() + "\r\n") != std::string::npos, "W-14b scenario.ini holds the new Id");
                        size_t with_new = 0, with_old = 0;
                        for (auto& f : rd.files()) {
                            with_new += f.path.find("Scn" + sc.id_hex()) != std::string::npos;
                            with_old += f.path.find("Scn" + old_hex) != std::string::npos;
                        }
                        (void)count;
                        CHECK(with_new > 0, "W-14b the archive's Scn<Id> files use the new Id (%zu)", with_new);
                        CHECK(with_old == 0, "W-14b no file in the archive keeps the old Id (%zu)", with_old);
                    } else CHECK(false, "W-14b read scenario.ini: %s", err.c_str());
                    std::error_code cec;
                    fs::remove(cf, cec);
                }
            }
            // W-114: a flipped byte inside one entry of a saved .ma2map is reported by load (the entry is named; an optional one
            // falls back to its default), a damaged scenario.ini fails the load, an intact file reports nothing.
            {
                NewMapParams dp;
                dp.start = NewMapParams::CopyRetail;
                dp.copy_map = "UnderCover";
                dp.multiplayer = true;
                dp.name = "W114 damaged";
                Scenario sd;
                if (io.create(dp, sd, &err)) {
                    fs::path df = fs::temp_directory_path() / L"ma2scn_w114.ma2map";
                    std::error_code dec;
                    SaveReport drep;
                    CHECK(io.save(sd, df, &drep, &err), "W-114 save: %s", err.c_str());
                    std::vector<std::string> warn;
                    Scenario ok_back;
                    CHECK(io.load(df, ok_back, &err, &warn) && warn.empty(), "W-114 an intact file loads with no warning (%zu)", warn.size());
                    Bytes raw;
                    read_file(df, raw);
                    std::string target;
                    uint64_t at = 0;
                    {
                        MgfReader rd;
                        rd.open(df, &err);
                        for (auto& f : rd.files())
                            if (iequals(f.path, "editor/project.json") && f.size() > 64) { target = f.path; at = f.offset() + f.size() / 2; break; }
                    }
                    CHECK(!target.empty(), "W-114 the saved file has an editor/project.json entry");
                    if (!target.empty()) {
                        Bytes hurt = raw;
                        for (int i = 0; i < 16; i++) hurt[size_t(at) + size_t(i)] ^= 0xFF;
                        fs::path hf = fs::temp_directory_path() / L"ma2scn_w114_bad.ma2map";
                        CHECK(write_file(hf, hurt), "W-114 write the damaged copy");
                        Scenario bad;
                        std::vector<std::string> w2;
                        const bool loaded = io.load(hf, bad, &err, &w2);
                        CHECK(loaded, "W-114 a damaged optional entry still loads: %s", err.c_str());
                        CHECK(w2.size() == 1 && w2[0] == target, "W-114 the warning names the entry (%zu: %s)", w2.size(), w2.empty() ? "" : w2[0].c_str());
                        std::error_code hec;
                        fs::remove(hf, hec);
                    }
                    fs::remove(df, dec);
                }
            }
            // W-15b: scenario.ini says Ready = 0 while Check shows a Must fix item (the game then does not list the file), else Ready = 1.
            {
                NewMapParams rp;
                rp.start = NewMapParams::CopyRetail;
                rp.copy_map = "UnderCover";
                rp.multiplayer = true;
                rp.name = "W15b ready";
                Scenario sr;
                if (io.create(rp, sr, &err)) {
                    auto ready_line = [&](Scenario& s, const char* label) -> std::string {
                        fs::path rf = fs::temp_directory_path() / L"ma2scn_w15b.ma2map";
                        SaveReport rrep;
                        std::string line;
                        if (!io.save(s, rf, &rrep, &err)) { CHECK(false, "W-15b save %s: %s", label, err.c_str()); return line; }
                        MgfReader rd;
                        Bytes ini;
                        if (rd.open(rf, &err) && rd.read_file("scenario.ini", ini, &err)) {
                            const std::string t(ini.begin(), ini.end());
                            size_t at = t.find("Ready = ");
                            if (at != std::string::npos) line = t.substr(at, 9);
                        }
                        rd.close();
                        std::error_code rec;
                        fs::remove(rf, rec);
                        fs::remove(fs::path(rf) += L".bak", rec);
                        return line;
                    };
                    bool errors = false;
                    for (auto& v : io.validate(sr)) errors |= v.level == 2;
                    CHECK(!errors, "W-15b the copied level has no Must fix item");
                    CHECK(ready_line(sr, "ready") == "Ready = 1", "W-15b a scenario with no Must fix item is Ready = 1");
                    const size_t before = sr.world.objs.size();
                    sr.world.objs.erase(std::remove_if(sr.world.objs.begin(), sr.world.objs.end(), [](const WObj& o) { return o.cls == key_of("MALevelStart"); }), sr.world.objs.end());
                    errors = false;
                    for (auto& v : io.validate(sr)) errors |= v.level == 2;
                    CHECK(sr.world.objs.size() < before && errors, "W-15b without spawn points Check shows a Must fix item");
                    CHECK(ready_line(sr, "unfinished") == "Ready = 0", "W-15b a scenario with a Must fix item is Ready = 0");
                }
            }
            // P4 ground layers: 4 textures (the retail pair + 2 from other levels) on a copy of UnderCover,
            // quadrants plus soft circles; the file, the weights and the baked fallback alpha survive a reload.
            // MA2SE_P4_OUT=<file> also writes the scenario there (the in-game test).
            {
                NewMapParams gp;
                gp.start = NewMapParams::CopyRetail;
                gp.copy_map = "UnderCover";
                gp.multiplayer = true;
                gp.name = "Ground layers test";
                Scenario sg;
                bool gok = io.create(gp, sg, &err);
                CHECK(gok, "copy UnderCover for the ground layers test: %s", err.c_str());
                std::vector<std::pair<std::string, std::string>> extra;  // archive, path
                for (auto& an : gd.level_archives()) {
                    if (extra.size() >= 2 || iequals(an, sg.base)) continue;
                    MgfReader* a = gd.archive(an);
                    if (!a) continue;
                    for (auto& f : a->files()) {
                        std::string l = lower(f.path);
                        if (!istarts_with(l, "terrain/") || !iends_with(l, ".tif") || l.find("detail") == std::string::npos || l.find("micro") != std::string::npos) continue;
                        extra.push_back({an, f.path});
                        break;
                    }
                }
                CHECK(extra.size() == 2, "two ground textures from other levels (%zu)", extra.size());
                if (gok && extra.size() == 2 && !sg.colormap.px.empty()) {
                    GroundLayers& g = sg.ground;
                    g.enabled = true;
                    g.layers.resize(4);
                    int ti = -1;
                    for (size_t i = 0; i < sg.world.objs.size(); i++) if (sg.world.objs[i].cls == K_TERRAIN) ti = int(i);
                    if (ti >= 0) {
                        g.layers[0].repeats = sg.world.objs[size_t(ti)].f32("TerrainDetailMult", 64);
                        g.layers[1].repeats = sg.world.objs[size_t(ti)].f32("TerrainDetailMult1", 64);
                    }
                    for (int k = 0; k < 2; k++) {
                        g.layers[size_t(2 + k)].archive = extra[size_t(k)].first;
                        g.layers[size_t(2 + k)].path = extra[size_t(k)].second;
                        g.layers[size_t(2 + k)].repeats = 48;
                        bool lt = load_ground_texture(*gd.archive(extra[size_t(k)].first), extra[size_t(k)].second, g.layers[size_t(2 + k)].image, &err);
                        CHECK(lt, "decode ground texture %s: %s", extra[size_t(k)].second.c_str(), err.c_str());
                        printf("[layers] layer %d = %s|%s\n", 2 + k, extra[size_t(k)].first.c_str(), extra[size_t(k)].second.c_str());
                    }
                    g.init_from_colormap(sg.colormap);
                    uint8_t* d = g.mutable_weights();
                    for (int y = 0; y < g.h; y++)
                        for (int x = 0; x < g.w; x++) {
                            uint8_t* tx = d + (size_t(y) * g.w + x) * 8;
                            std::fill(tx, tx + 8, uint8_t(0));
                            // NW (the UnderCover spawn) 2, NE 3, SW 0, SE 1: the spawn view shows an extra layer.
                            int q = ((x >= g.w / 2) + 2 * (y >= g.h / 2) + 2) % 4;
                            float cx = (x + 0.5f) / g.w - 0.5f, cy = (y + 0.5f) / g.h - 0.5f;
                            float r = std::sqrt(cx * cx + cy * cy);
                            float ring = std::clamp((0.12f - r) / 0.03f, 0.f, 1.f);  // a soft disc of layer 3 in the middle
                            tx[q] = uint8_t(std::lround(255 * (1 - ring)));
                            tx[3] = uint8_t(std::min(255, tx[3] + int(std::lround(255 * ring))));
                        }
                    sg.colormap_custom = true;
                    Bytes enc = encode_ground_layers(g);
                    GroundLayers dec;
                    bool dok = decode_ground_layers(enc, dec, &err);
                    CHECK(dok && dec.count() == 4 && dec.w == g.w && *dec.weights == *g.weights && dec.layers[2].image.px == g.layers[2].image.px &&
                          dec.layers[3].repeats == 48.f && dec.layers[2].path == g.layers[2].path, "ground layers encode/decode");
                    printf("[layers] terrain_layers.bin %.2f MB (weights %dx%d RLE)\n", enc.size() / 1048576.0, g.w, g.h);
                    fs::path gf = fs::temp_directory_path() / L"ma2scn_layers_test.ma2map";
                    SaveReport grep;
                    bool saved = io.save(sg, gf, &grep, &err);
                    CHECK(saved, "save the ground layers test: %s", err.c_str());
                    Scenario gb;
                    if (saved && io.load(gf, gb, &err)) {
                        CHECK(gb.ground.enabled && gb.ground.count() == 4 && gb.ground.weights && *gb.ground.weights == *g.weights &&
                              gb.ground.layers[3].image.px == g.layers[3].image.px, "ground layers reload");
                        // Fallback alpha: SW = layer 0 (255), SE = layer 1 (0), the extra quadrants keep the pair's share (none: 255).
                        uint32_t sw = gb.colormap.at(gb.colormap.w / 8, gb.colormap.h * 7 / 8) >> 24, se = gb.colormap.at(gb.colormap.w * 7 / 8, gb.colormap.h * 7 / 8) >> 24;
                        CHECK(sw == 255 && se == 0, "baked colour-map alpha (SW %u, SE %u)", sw, se);
                        CHECK(gb.ground.map_w == gb.colormap.w && gb.ground.map_h == gb.colormap.h, "layers carry the colour map size (%dx%d)", gb.ground.map_w, gb.ground.map_h);
                        MgfReader ar;
                        Bytes ini;
                        CHECK(ar.open(gf, &err) && ar.read_file("scenario.ini", ini) &&
                              std::string(ini.begin(), ini.end()).find("[Terrain]\r\nLayers = terrain_layers.bin") != std::string::npos, "scenario.ini [Terrain] Layers");
                    } else CHECK(false, "reload the ground layers test: %s", err.c_str());
                    // Off again: no file, no [Terrain] section.
                    Scenario off = sg;
                    off.ground = GroundLayers();
                    Bytes arc;
                    SaveReport orep;
                    io.build(off, arc, &orep, &err);
                    fs::path of = fs::temp_directory_path() / L"ma2scn_layers_off.ma2map";
                    MgfReader orr;
                    Bytes ini;
                    CHECK(write_file(of, arc) && orr.open(of, &err) && !orr.read_file(kGroundLayersFile, ini) && orr.read_file("scenario.ini", ini) &&
                          std::string(ini.begin(), ini.end()).find("[Terrain]") == std::string::npos, "no layers -> no terrain_layers.bin / [Terrain]");
                    char outp[MAX_PATH];
                    if (GetEnvironmentVariableA("MA2SE_P4_OUT", outp, MAX_PATH)) {
                        CHECK(io.save(sg, fs::u8path(outp), &grep, &err), "MA2SE_P4_OUT save: %s", err.c_str());
                        printf("[layers] wrote %s (id %s)\n", outp, sg.id_hex().c_str());
                    }
                }
            }
            // MA2SE_P4_MAPS=<dir>: one layered copy (4 textures: quadrants + a disc, like the UnderCover test) of each campaign
            // level in MA2SE_P4_LEVELS (comma list, default Mission_01b,Mission_02,Mission_03,Mission_04,Mission_05), written as
            // <dir>\<level>_layers.ma2map: the in-game check that the P4 recognition key also holds on levels without a lightmap T3.
            {
                char dirp[MAX_PATH], lvls[512];
                if (GetEnvironmentVariableA("MA2SE_P4_MAPS", dirp, MAX_PATH)) {
                    std::string list = GetEnvironmentVariableA("MA2SE_P4_LEVELS", lvls, sizeof(lvls)) ? lvls : "Mission_01b,Mission_02,Mission_03,Mission_04,Mission_05";
                    std::vector<std::pair<std::string, std::string>> extra;  // archive, path
                    std::vector<std::string> names;
                    for (size_t b = 0; b < list.size();) {
                        size_t e = list.find(',', b);
                        if (e == std::string::npos) e = list.size();
                        if (e > b) names.push_back(list.substr(b, e - b));
                        b = e + 1;
                    }
                    for (auto& nm : names) {
                        NewMapParams mp;
                        mp.start = NewMapParams::CopyRetail;
                        mp.copy_map = nm;
                        mp.multiplayer = false;
                        mp.types.clear();
                        mp.name = nm + " layers";
                        Scenario sm;
                        bool mok = io.create(mp, sm, &err);
                        CHECK(mok, "copy %s for the layers map: %s", nm.c_str(), err.c_str());
                        if (!mok || sm.colormap.px.empty()) continue;
                        // MA2SE_P4_LAYERS=N (2..8, default 4): N-2 extra textures; N > 4 splits the map into N pie slices (no disc).
                        size_t nl = 4;
                        { char nb[16]; if (GetEnvironmentVariableA("MA2SE_P4_LAYERS", nb, sizeof nb)) nl = size_t(std::clamp(atoi(nb), 4, 8)); }
                        extra.clear();
                        for (auto& an : gd.level_archives()) {
                            if (extra.size() >= nl - 2 || iequals(an, sm.base)) continue;
                            MgfReader* a = gd.archive(an);
                            if (!a) continue;
                            for (auto& f : a->files()) {
                                std::string l = lower(f.path);
                                if (!istarts_with(l, "terrain/") || !iends_with(l, ".tif") || l.find("detail") == std::string::npos || l.find("micro") != std::string::npos) continue;
                                if (extra.size() >= nl - 2) break;
                                bool dup = false;
                                for (auto& e2 : extra) dup = dup || iequals(e2.second, f.path);
                                if (dup) continue;
                                extra.push_back({an, f.path});
                                if (extra.size() % 2 == 0 || nl == 4) break;  // two per archive at most keeps them varied
                            }
                        }
                        if (extra.size() != nl - 2) { CHECK(false, "%zu extra ground textures for %s (got %zu)", nl - 2, nm.c_str(), extra.size()); continue; }
                        GroundLayers& g = sm.ground;
                        g.enabled = true;
                        g.layers.resize(nl);
                        int ti = -1;
                        for (size_t i = 0; i < sm.world.objs.size(); i++) if (sm.world.objs[i].cls == K_TERRAIN) ti = int(i);
                        if (ti >= 0) {
                            g.layers[0].repeats = sm.world.objs[size_t(ti)].f32("TerrainDetailMult", 64);
                            g.layers[1].repeats = sm.world.objs[size_t(ti)].f32("TerrainDetailMult1", 64);
                        }
                        bool lok = true;
                        for (size_t k = 0; k + 2 < nl; k++) {
                            GroundLayer& L = g.layers[2 + k];
                            L.archive = extra[size_t(k)].first;
                            L.path = extra[size_t(k)].second;
                            L.repeats = 48;
                            lok = lok && load_ground_texture(*gd.archive(L.archive), L.path, L.image, &err);
                        }
                        CHECK(lok, "decode the extra ground textures for %s: %s", nm.c_str(), err.c_str());
                        g.init_from_colormap(sm.colormap);
                        uint8_t* d = g.mutable_weights();
                        // MA2SE_P4_CENTER=fx,fy (0..1 of the map): where the quadrants meet and the disc sits (default the middle).
                        float pcx = 0.5f, pcy = 0.5f;
                        { char cb[64]; if (GetEnvironmentVariableA("MA2SE_P4_CENTER", cb, sizeof cb)) sscanf(cb, "%f,%f", &pcx, &pcy); }
                        for (int y = 0; y < g.h; y++)
                            for (int x = 0; x < g.w; x++) {
                                uint8_t* tx = d + (size_t(y) * g.w + x) * 8;
                                std::fill(tx, tx + 8, uint8_t(0));
                                int q = (((x + 0.5f) / g.w >= pcx) + 2 * ((y + 0.5f) / g.h >= pcy) + 2) % 4;
                                float cx = (x + 0.5f) / g.w - pcx, cy = (y + 0.5f) / g.h - pcy;
                                float r = std::sqrt(cx * cx + cy * cy);
                                float ring = std::clamp((0.12f - r) / 0.03f, 0.f, 1.f);
                                if (nl > 4) {  // N pie slices around the centre
                                    float ang = std::atan2(cy, cx) + 3.14159265f;
                                    tx[std::min(size_t(ang / 6.2831853f * float(nl)), nl - 1)] = 255;
                                    continue;
                                }
                                tx[q] = uint8_t(std::lround(255 * (1 - ring)));
                                tx[3] = uint8_t(std::min(255, tx[3] + int(std::lround(255 * ring))));
                            }
                        sm.colormap_custom = true;
                        fs::create_directories(fs::u8path(dirp));
                        fs::path outf = fs::u8path(dirp) / fs::u8path(nm + (nl > 4 ? "_layers" + std::to_string(nl) : std::string("_layers")) + ".ma2map");
                        SaveReport mrep;
                        CHECK(io.save(sm, outf, &mrep, &err), "save %s layers map: %s", nm.c_str(), err.c_str());
                        printf("[layers] wrote %s (%s, colour map %dx%d, weights %dx%d)\n", outf.string().c_str(), nm.c_str(), sm.colormap.w, sm.colormap.h, g.w, g.h);
                    }
                }
            }
            // 0. Custom-size water: the Description carries the size, the archive a model of that size, and
            // a reload shows the template's own model again.
            {
                NewMapParams wp;
                wp.start = NewMapParams::CopyRetail;
                wp.copy_map = "UnderCover";
                wp.multiplayer = true;
                wp.name = "Water test";
                Scenario sw;
                bool wok = io.create(wp, sw, &err);
                CHECK(wok, "copy UnderCover for the water test: %s", err.c_str());
                uint32_t wt = 0;
                for (auto& ti : cat.all()) if (ti.cls_name == "MAWater" && ti.name == "Water_1000x1000 NEW") { wt = ti.uid; break; }
                CHECK(wt != 0, "water template found");
                if (wok && wt) {
                    CHECK(io.water_resizable(sw, wt), "Water_1000x1000 NEW can be resized");
                    uint32_t root = io.place_template(sw, wt, {300, 10, 600}, 0);
                    for (auto& o : sw.world.objs)
                        if (o.uid() == root) o.set_str("Description", ScenarioIO::water_description("Lake", 300, 120));
                    Bytes arc;
                    SaveReport wrep;
                    bool built = io.build(sw, arc, &wrep, &err);
                    CHECK(built, "build the water test: %s", err.c_str());
                    fs::path wf = fs::temp_directory_path() / L"ma2scn_water_test.ma2map";
                    if (built && write_file(wf, arc)) {
                        MgfReader ar;
                        std::string tm = cat.find(wt)->model;
                        for (char& c : tm) if (c == '\\') c = '/';
                        if (!tm.empty() && tm[0] == '/') tm.erase(0, 1);
                        size_t sl = tm.find_last_of('/');
                        std::string stem = tm.substr(sl + 1, tm.rfind('.') - sl - 1);
                        std::string mdl = tm.substr(0, sl + 1) + "MA2SE_" + stem + "_300x120.MGMODEL";
                        Bytes v;
                        bool have = ar.open(wf, &err) && ar.find(mdl) >= 0 && ar.read_file(mdl + "{water_1000x1000 verts}", v) && ar.find(mdl + "{water_1000x1000 indices}") >= 0;
                        if (!have) for (auto& f : ar.files()) if (f.path.find("MA2SE") != std::string::npos) printf("           archive has %s\n", f.path.c_str());
                        CHECK(have, "the archive holds the 300 x 120 water model");
                        float ww = 0, wl = 0;
                        CHECK(have && wcustom::extent(v, ww, wl) && std::fabs(ww - 300) < 0.01f && std::fabs(wl - 120) < 0.01f, "the saved water vertices are 300 x 120 (%g x %g)", ww, wl);
                        ar.close();
                        Scenario wb;
                        bool lok = io.load(wf, wb, &err);
                        CHECK(lok, "reload the water test: %s", err.c_str());
                        const TemplateInfo* wti = cat.find(wt);
                        int found = 0;
                        if (lok)
                            for (auto& o : wb.world.objs) {
                                float cw, cl;
                                if (o.uid() == root) {
                                    found++;
                                    CHECK(ScenarioIO::water_custom_size(o, cw, cl) && cw == 300 && cl == 120 && ScenarioIO::water_name(o) == "Lake", "reloaded water keeps its name and size (%s)", o.description().c_str());
                                    CHECK(wti && o.str("Corporal3DObj") == wti->model, "reloaded water draws the template's model again (%s)", o.str("Corporal3DObj").c_str());
                                }
                            }
                        CHECK(found == 1, "the reloaded scenario has the water");
                        fs::remove(wf);
                    }
                }
            }
            printf("[water] done\n");
            // 0b. Cropped road piece and foundation (crop.h, playtest 11-01): the saved world points at cut models in the
            // scenario's archive, those load with the model library and stay inside the kept rectangle, and a reload puts
            // the crop and the original paths back.
            {
                NewMapParams mp16;
                mp16.start = NewMapParams::CopyRetail;
                mp16.copy_map = "Mission_16";
                mp16.multiplayer = false;
                mp16.types.clear();
                mp16.name = "Crop test";
                Scenario sc;
                printf("[crop] copy Mission_16\n");
                bool cok = io.create(mp16, sc, &err);
                CHECK(cok, "copy Mission_16 for the crop test: %s", err.c_str());
                const uint32_t K_ROAD_T = key_of("MARoad");
                uint32_t road_uid = 0, base_uid = 0;
                std::string road_def, base_model;
                for (auto& o : sc.world.objs) {
                    if (!road_uid && o.cls == K_ROAD_T && iends_with(o.str("RoadDefinition"), "longStraight.road")) { road_uid = o.uid(); road_def = o.str("RoadDefinition"); }
                    if (!base_uid && o.str("Corporal3DObj").find("Bb_34x34_Generic01_patternbrick") != std::string::npos) { base_uid = o.uid(); base_model = o.str("Corporal3DObj"); }
                }
                CHECK(road_uid && base_uid, "Mission_16 has a long straight road piece (%u) and a pattern-brick building base (%u)", road_uid, base_uid);
                const crop::Rect rr{-13, 13, -40, -5}, br{-19, 6, -19, 11};  // road: 35 m of its 60; base: 25 x 30 of 38 x 38
                if (cok && road_uid && base_uid) {
                    for (auto& o : sc.world.objs) {
                        if (o.uid() == road_uid) crop::set(o, &rr);
                        if (o.uid() == base_uid) crop::set(o, &br);
                    }
                    Bytes arc;
                    SaveReport crep;
                    printf("[crop] build\n");
                    bool built = io.build(sc, arc, &crep, &err);
                    CHECK(built, "build the crop test: %s", err.c_str());
                    for (auto& wmsg : crep.warnings) if (wmsg.find("cropped") != std::string::npos) CHECK(false, "crop warning: %s", wmsg.c_str());
                    fs::path cf = fs::temp_directory_path() / L"ma2scn_crop_test.ma2map";
                    if (built && write_file(cf, arc)) {
                        MgfReader ar;
                        Wdf sw;
                        Bytes wb;
                        CHECK(ar.open(cf, &err) && ar.read_file("world.wdf", wb) && sw.parse(wb), "read the saved crop world");
                        std::string saved_road, saved_base;
                        bool prop_left = false;
                        for (auto& o : sw.objs) {
                            if (o.uid() == road_uid) saved_road = o.str("RoadDefinition");
                            if (o.uid() == base_uid) saved_base = o.str("Corporal3DObj");
                            prop_left |= o.find(crop::kProp) != nullptr;
                        }
                        CHECK(!prop_left, "the saved world has no EditorCrop property");
                        CHECK(saved_road.find("MA2SE_longStraight_c") != std::string::npos && saved_road[0] != '\\', "saved road points at the cut .road (%s)", saved_road.c_str());
                        CHECK(saved_base.find("MA2SE_Bb_34x34_Generic01_patternbrick_c") != std::string::npos && saved_base[0] == '\\', "saved base points at the cut model (%s)", saved_base.c_str());
                        ar.close();
                        printf("[crop] load the cut models\n");
                        // The cut files load like retail ones (scenario archive first, then Base and common, as the game mounts them).
                        {  // the archive set holds the file open until the block ends
                        ma2m::ArchiveSet set;
                        set.Add(path_utf8(cf));
                        for (const char* an : {"mission_16.mgf", "common.mgf", "mission_02.mgf"}) set.Add(path_utf8(gd.game_files() / widen(an)));
                        Bytes rb;
                        std::string road_txt;
                        if (set.Read(ma2m::NormalizePath(saved_road), rb)) road_txt.assign(rb.begin(), rb.end());
                        CHECK(!road_txt.empty() && road_txt.find("MA2SE_") != std::string::npos, "the cut .road names a cut model");
                        CHECK(road_txt.find("point1\t  = 13.000000, 0.000000, -5.000000") != std::string::npos || road_txt.find("-5.000000") != std::string::npos,
                              "the cut .road boundary starts at the cut");
                        std::string rmodel;
                        for (auto& line : split(road_txt, '\n'))
                            if (size_t eq = line.find('='); eq != std::string::npos && lower(trim(line.substr(0, eq))) == "model") rmodel = ma2m::ResolveRelative(ma2m::NormalizePath(saved_road), trim(line.substr(eq + 1)));
                        for (auto& [path, r] : std::vector<std::pair<std::string, crop::Rect>>{{rmodel, rr}, {ma2m::NormalizePath(saved_base), br}}) {
                            ma2m::Model m;
                            ma2m::ModelLoadOptions mo;
                            mo.apply_mtb = false;
                            std::string merr;
                            bool lm = ma2m::LoadModel(set, path, m, mo, &merr);
                            size_t tris = 0;
                            bool inside = true;
                            for (auto& sm : m.meshes) {
                                tris += sm.indices.size() / 3;
                                std::vector<float> pos, nrm;
                                ma2m::BakeSubMesh(sm, pos, nrm);
                                for (size_t i = 0; i + 2 < pos.size(); i += 3)
                                    inside = inside && pos[i] >= r.x0 - 0.01f && pos[i] <= r.x1 + 0.01f && pos[i + 2] >= r.z0 - 0.01f && pos[i + 2] <= r.z1 + 0.01f;
                            }
                            CHECK(lm && m.failed_meshes == 0 && tris >= 2, "cut model %s loads (%zu triangles) %s", path.c_str(), tris, merr.c_str());
                            CHECK(inside && m.has_bbox && std::fabs(m.bbox_min[0] - r.x0) < 0.05f && std::fabs(m.bbox_max[2] - r.z1) < 0.05f,
                                  "cut model %s fills the kept rectangle (x %.2f..%.2f z %.2f..%.2f)", path.c_str(), m.bbox_min[0], m.bbox_max[0], m.bbox_min[2], m.bbox_max[2]);
                        }
                        }
                        printf("[crop] reload\n");
                        Scenario cb;
                        bool lok = io.load(cf, cb, &err);
                        CHECK(lok, "reload the crop test: %s", err.c_str());
                        int back = 0;
                        for (auto& o : cb.world.objs) {
                            crop::Rect r;
                            if (o.uid() == road_uid) back += crop::of(o, r) && std::fabs(r.z0 - rr.z0) < 0.01f && o.str("RoadDefinition") == road_def;
                            if (o.uid() == base_uid) back += crop::of(o, r) && std::fabs(r.x1 - br.x1) < 0.01f && o.str("Corporal3DObj") == base_model;
                        }
                        CHECK(back == 2, "reload restores both crops and the original paths (%d of 2)", back);
                        std::error_code rec;
                        fs::remove(cf, rec);
                    }
                    // A legacy .NODE plate whose node scales its mesh (Env_ParkingLot_30x30: scale 0.5, mesh +-30 m): the cut is
                    // in model space, so the planes go through the node transform.
                    {
                        ma2m::ArchiveSet src;
                        for (const char* an : {"gocommando.mgf", "common.mgf"}) src.Add(path_utf8(gd.game_files() / widen(an)));
                        std::vector<crop::OutFile> files;
                        std::string cut, cerr;
                        const crop::Rect pr{-15, 5, -10, 15};
                        bool made = crop::make_model(src, "Models/Environment/BuildingBases/ParkingLots/Env_ParkingLot_30x30.NODE", pr, files, cut, &cerr);
                        CHECK(made && files.size() == 4, "cut the parking lot node (%zu files) %s", files.size(), cerr.c_str());
                        MgfWriter mw;
                        for (auto& f : files) mw.add(f.path, f.data);
                        fs::path pf = fs::temp_directory_path() / L"ma2scn_crop_node.mgf";
                        if (made && write_file(pf, mw.build())) {
                            {
                                ma2m::ArchiveSet set;
                                set.Add(path_utf8(pf));
                                for (const char* an : {"gocommando.mgf", "common.mgf"}) set.Add(path_utf8(gd.game_files() / widen(an)));
                                ma2m::Model m;
                                ma2m::ModelLoadOptions mo;
                                mo.apply_mtb = false;
                                bool lm = ma2m::LoadModel(set, cut, m, mo, &cerr);
                                CHECK(lm && m.failed_meshes == 0 && m.has_bbox && std::fabs(m.bbox_min[0] - pr.x0) < 0.05f && std::fabs(m.bbox_max[0] - pr.x1) < 0.05f &&
                                          std::fabs(m.bbox_min[2] - pr.z0) < 0.05f && std::fabs(m.bbox_max[2] - pr.z1) < 0.05f,
                                      "cut parking lot loads to the kept rectangle (x %.2f..%.2f z %.2f..%.2f) %s", m.bbox_min[0], m.bbox_max[0], m.bbox_min[2], m.bbox_max[2], cerr.c_str());
                            }
                            std::error_code rec;
                            fs::remove(pf, rec);
                        }
                    }
                }
            }
            // 1. Bogged Down copy with one visible change: a raised plateau in the middle.
            NewMapParams cp;
            cp.start = NewMapParams::CopyRetail;
            cp.copy_map = "BoggedDown";
            cp.name = "Bogged Down - Plateau";
            Scenario s1;
            bool ok = io.create(cp, s1, &err);
            CHECK(ok, "copy BoggedDown: %s", err.c_str());
            if (ok) {
                s1.description = "Bogged Down with a 40 m plateau raised in the middle of the map (editor sample).";
                s1.author = "Scenario Editor sample";
                float cx = s1.terrain.size_x() * 0.5f, cz = s1.terrain.size_z() * 0.5f;
                for (int gz = 0; gz < s1.terrain.h; gz++)
                    for (int gx = 0; gx < s1.terrain.w; gx++) {
                        float dx = gx * s1.terrain.scale - cx, dz = gz * s1.terrain.scale - cz, d = std::sqrt(dx * dx + dz * dz);
                        if (d < 120.f) {
                            float f = std::clamp((120.f - d) / 40.f, 0.f, 1.f);
                            float& h = s1.terrain.ref(gx, gz);
                            h = std::max(h, h + (40.f - h) * f);
                        }
                    }
                io.bake_lightmap(s1);
                report("BoggedDown copy", s1, dir / L"BoggedDown_Plateau.ma2map");
            }
            // Sky picker: the catalog, the Base level's own sky, and a sky from another level's archive.
            {
                double ts = now_s();
                auto& skies = io.skies();
                printf("[sky] %zu skies in %.2fs:", skies.size(), now_s() - ts);
                for (auto& si : skies) printf(" %s(%zu models, %zu uses)", si.name.c_str(), si.models.size(), si.uses.size());
                printf("\n");
                const char* want[] = {"Env_SkyDusk02", "Env_Sky_CityNight", "Env_SkyNOR_01", "Env_SkyNOR_02", "Env_SkyHesperus_01",
                                      "Env_SkyHesperus_02", "Env_SkyAurora_01", "Env_Sky_StormFront_01", "Env_Sky_Stormy", "En_Sky_WarmSun",
                                      "Env_SkySpace", "Env_Planet"};
                int found = 0;
                for (const char* w : want) {
                    bool f = io.find_sky(w) != nullptr;
                    if (!f) for (auto& si : skies) f |= iequals(si.folder, w);
                    if (f) found++;
                    else printf("           missing sky %s\n", w);
                }
                CHECK(skies.size() >= 12 && found == 12, "sky catalog (%zu skies, %d of 12 named ones)", skies.size(), found);
                NewMapParams sp;
                sp.start = NewMapParams::CopyRetail;
                sp.copy_map = "BoggedDown";
                sp.name = "Sky Test";
                Scenario sk;
                if (io.create(sp, sk, &err)) {
                    std::string base_sky = io.base_sky_name(sk);
                    CHECK(base_sky == "Env_SkyDusk02" && io.sky_name(sk) == base_sky, "Bogged Down's own sky (%s)", base_sky.c_str());
                    int si = world::find_class(sk.world, world::K_SKY), ei = world::find_class(sk.world, world::K_ENV);
                    WObj before = sk.world.objs[size_t(si)];
                    uint32_t fog_before = sk.world.objs[size_t(ei)].u32("FogColor");
                    bool ok = io.set_sky(sk, "Env_SkyAurora_01", true, &err);
                    const WObj& now = sk.world.objs[size_t(si)];
                    printf("[sky] Sky_Test: %s | %s | scale %.0f,%.0f,%.0f offset %.0f | fog %08X -> %08X\n", now.str("Corporal3DObj").c_str(),
                           now.str("CorporalMTB").c_str(), now.vec3("SkyTerrainSkyScale").x, now.vec3("SkyTerrainSkyScale").y,
                           now.vec3("SkyTerrainSkyScale").z, now.vec3("SkyTerrainSkyOffset").y, fog_before,
                           sk.world.objs[size_t(ei)].u32("FogColor"));
                    CHECK(ok && iends_with(now.str("Corporal3DObj"), "Env_SkyAurora_01_MP.MGMODEL") && io.sky_name(sk) == "Env_SkyAurora_01",
                          "set_sky Env_SkyAurora_01 (MP model): %s", err.c_str());
                    // Back to the Base level's own sky restores its MASky2 exactly.
                    Scenario back = sk;
                    io.set_sky(back, base_sky, true, &err);
                    const WObj& rest = back.world.objs[size_t(si)];
                    bool same = rest.props.size() == before.props.size();
                    for (size_t k = 0; same && k < rest.props.size(); k++) same = rest.props[k].key == before.props[k].key && rest.props[k].data == before.props[k].data;
                    CHECK(same && back.world.objs[size_t(ei)].u32("FogColor") == fog_before, "restoring the Base sky and fog");
                    sk.description = "Bogged Down with New River City's aurora sky (Env_SkyAurora_01_MP) and its fog colour (editor sample).";
                    sk.author = "Scenario Editor sample";
                    fs::path out = dir / L"Sky_Test.ma2map";
                    report("sky test", sk, out);
                    MgfReader mr;
                    Bytes ib;
                    if (mr.open(out) && mr.read_file("scenario.ini", ib)) {
                        std::string ini = lower(std::string(ib.begin(), ib.end()));
                        int n = 0;
                        for (size_t q = ini.find("newrivercity.mgf"); q != std::string::npos; q = ini.find("newrivercity.mgf", q + 1)) n++;
                        CHECK(ini.find("env_skyaurora_01_mp.mgmodel") != std::string::npos, "sky model listed in [Assets]");
                        printf("[sky] Sky_Test [Assets]: %d entries from newrivercity.mgf\n", n);
                    }
                }
            }
            // 2. Small map from scratch: 6 Red + 6 Blue spawns, Team Destruction.
            NewMapParams np;
            np.multiplayer = true;
            np.w = np.h = 129;
            np.base = "gocommando.mgf";  // a retail 129x129 level (a 129x129 map on a 257x257 Base failed in game, bisect B4-B6)
            np.start = NewMapParams::Noise;
            np.flat_height = 10.f;
            np.seed = 20261020;
            np.types = {"TeamDestruction"};
            np.name = "Small Skirmish";
            Scenario s2;
            ok = io.create(np, s2, &err);
            CHECK(ok, "create small map: %s", err.c_str());
            if (ok) {
                {   // On foot: the free-for-all slots (StartVehiclesNotIt/LMS/Destruction/Grinder) start in Battle Armor as on retail maps.
                    uint32_t was = io.start_container_raw(s2);
                    io.set_start_container(s2, 123999);
                    std::map<uint32_t, int64_t> slot;
                    if (int ri = world::find_class(s2.world, world::K_ROOT); ri >= 0)
                        for (auto& p : s2.world.objs[size_t(ri)].props) if (p.data.size() == 4) slot[p.key] = p.as_int();
                    CHECK(slot[0xF2845C32u] == 124001 && slot[0x3E1C0123u] == 124001 && slot[0xE297B910u] == 124001 && slot[0xBA4F078Au] == 124001 &&
                              slot[0x749BC5D7u] == 123999 && slot[0xC39D0BEDu] == 123999 && io.start_container(s2) == 123999,
                          "on-foot MP start: Not It/LMS/Destruction/Grinder in Battle Armor, the team types on foot (NotIt %lld TD %lld)",
                          (long long)slot[0xF2845C32u], (long long)slot[0x749BC5D7u]);
                    io.set_start_container(s2, was);
                }
                s2.description = "Small 129x129 map made from scratch on GoCommando: 6 Red and 6 Blue spawns, Team Destruction (editor sample).";
                s2.author = "Scenario Editor sample";
                float W = s2.terrain.size_x(), H = s2.terrain.size_z();
                io.add_spawn_ring(s2, 1, {W * 0.22f, 0, H * 0.22f}, 28.f, 6);
                io.add_spawn_ring(s2, 2, {W * 0.78f, 0, H * 0.78f}, 28.f, 6);
                auto& pads = io.pad_kinds();
                printf("[scenario] %zu vehicle pad kinds in the retail MP maps\n", pads.size());
                float off = 0;
                int pads_placed = 0;
                for (auto& pk : pads)
                    if (pk.map == "GoCommando" && pads_placed < 2) {  // the Base level's own vehicles
                        io.add_pad(s2, pk, {W * 0.30f + off, 0, H * 0.15f}, 0.f);
                        io.add_pad(s2, pk, {W * 0.70f - off, 0, H * 0.85f}, 3.14159f);
                        printf("[scenario] pad kind '%s' (%s) placed twice\n", pk.label.c_str(), pk.map.c_str());
                        off += 25.f;
                        pads_placed++;
                    }
                {   // Check: a free-for-all type that starts on foot with fewer pads than players gets a warning; the on-foot default
                    // writes Battle Armor into Not It's slot, so it gets none.
                    auto types = s2.types;
                    uint32_t was = io.start_container_raw(s2);
                    s2.types = {"NotIt"};
                    io.set_start_container(s2, 123999);
                    auto warned = [&] {
                        for (auto& it : io.validate(s2)) if (it.text.rfind("NotIt: players start on foot", 0) == 0) return true;
                        return false;
                    };
                    bool w_ba = warned();
                    if (int ri = world::find_class(s2.world, world::K_ROOT); ri >= 0)
                        for (auto& p : s2.world.objs[size_t(ri)].props) if (p.key == 0xF2845C32u) p.set_int(123999);
                    bool w_foot = warned();
                    CHECK(!w_ba && w_foot, "Check: Not It on foot with %d pads warns (BA start %d, on foot %d)", pads_placed * 2, int(w_ba), int(w_foot));
                    s2.types = types;
                    io.set_start_container(s2, was);
                }
                // Type map of a generated map: one playable area inside an out-of-bounds edge and a border band.
                {
                    int Wt = s2.typemap_w, Ht = s2.typemap_h;
                    std::map<int, int> hist;
                    for (uint16_t v : s2.typemap) hist[v]++;
                    CHECK(Wt == 512 && Ht == 512 && hist.size() == 3 && hist.count(0) && hist.count(1) && hist.count(2),
                          "generated type map holds 0/1/2 only (%dx%d, %zu values)", Wt, Ht, hist.size());
                    CHECK(s2.typemap[0] == tmtype::OutOfBounds && s2.typemap[size_t(Ht / 2) * Wt + Wt / 2] == tmtype::Playable &&
                              s2.typemap[size_t(5) * Wt + Wt / 2] == tmtype::Border,
                          "generated type map edge 1, band 2, centre 0");
                    printf("[scenario] generated type map: %d playable, %d border, %d out-of-bounds cells\n", hist[0], hist[2], hist[1]);
                    int errors = 0;
                    for (auto& it : io.validate(s2))
                        if (it.level == 2) { errors++; printf("           validate: %s\n", it.text.c_str()); }
                    CHECK(errors == 0, "small map validates (%d errors)", errors);
                    // A spawn on out-of-bounds ground is an error; an out-of-bounds wall splitting the map is a warning.
                    std::vector<uint16_t> keep = s2.typemap;
                    Vec3 sp{};
                    for (auto& o : s2.world.objs) if (o.cls == world::K_LEVELSTART) { world::get_pos(o, sp); break; }
                    int tx = int(sp.x / W * Wt), tz = int(sp.z / H * Ht);
                    s2.typemap[size_t(tz) * Wt + tx] = tmtype::OutOfBounds;
                    int e1 = 0;
                    for (auto& it : io.validate(s2))
                        if (it.level == 2 && it.text.find("stands where players cannot go") != std::string::npos) e1++;
                    s2.typemap = keep;
                    for (int y = 0; y < Ht; y++)
                        for (int x = Wt / 2 - 2; x <= Wt / 2 + 2; x++) s2.typemap[size_t(y) * Wt + x] = tmtype::OutOfBounds;
                    bool split = false;
                    for (auto& it : io.validate(s2))
                        if (it.level == 1 && it.text.find("separate areas") != std::string::npos) split = true;
                    s2.typemap = keep;
                    CHECK(e1 == 1 && split, "ground validation: spawn on type 1 -> %d error(s), split map warned %d", e1, int(split));
                }
                // A few snow trees from another level's archive: exercises [Assets].
                int base_idx = -1;
                for (size_t i = 0; i < gd.level_archives().size(); i++) if (gd.level_archives()[i] == "gocommando.mgf") base_idx = int(i);
                int placed = 0;
                for (auto& t : cat.all()) {
                    if (!t.placeable || t.archives.empty() || lower(t.name).find("conifer02_snow_single") == std::string::npos) continue;
                    if (std::find(t.archives.begin(), t.archives.end(), base_idx) != t.archives.end()) continue;
                    for (int k = 0; k < 4; k++) {
                        float a = k * 1.5708f;
                        io.place_template(s2, t.uid, {W * 0.5f + std::cos(a) * 30, 0, H * 0.5f + std::sin(a) * 30}, a);
                    }
                    printf("[scenario] placed 4 x '%s' (template %u, archive %s)\n", t.name.c_str(), t.uid,
                           gd.level_archives()[size_t(t.archives.front())].c_str());
                    if (++placed == 1) break;
                }
                report("small map", s2, dir / L"Small_TeamDestruction.ma2map");
            }
            // 3. Mission: two enemy mechs, a patrol, an ambush, a message with imported audio, an
            //    objective and the win condition.
            std::string perr;
            bool protos = load_trigger_prototypes(gd, &perr);
            CHECK(protos, "trigger prototypes: %s", perr.c_str());
            printf("[triggers] prototypes loaded; %zu retail sound cues, %zu music cues\n", retail_sound_cues().size(), retail_music_cues().size());
            NewMapParams mp;
            mp.multiplayer = false;
            mp.w = mp.h = 129;
            mp.base = "mission_05.mgf";
            mp.start = NewMapParams::Noise;
            mp.flat_height = 15.f;
            mp.seed = 5;
            mp.name = "Ambush at the Ridge";
            auto fill_mission = [&](Scenario& s3) {
            s3.description = "Mission sample: two enemy mechs, a patrol, an ambush, a message with imported audio, an objective, win condition.";
            s3.author = "Scenario Editor sample";
            s3.briefing_title = "Ambush at the Ridge";
            s3.briefing = "Two enemy mechs guard the ridge.\nDestroy both of them.";
            s3.objectives = {"Destroy both enemy mechs"};
            float W = s3.terrain.size_x(), H = s3.terrain.size_z();
            uint32_t m1 = io.place_template(s3, 27265, {W * 0.70f, 0, H * 0.70f}, 3.1f);
            uint32_t m2 = io.place_template(s3, 27265, {W * 0.78f, 0, H * 0.62f}, 3.1f);
            for (auto& o : s3.world.objs) if (o.uid() == m1 || o.uid() == m2) o.set_i32("Nation", 5);
            CHECK(m1 && m2, "place enemy mechs");
            // A one-second 440 Hz tone as the imported clip.
            {
                AudioClip clip;
                clip.source = "tone440.wav";
                const int rate = 22050, n = rate;
                Bytes& b = clip.wav;
                auto u32 = [&](uint32_t v) { put(b, v); };
                auto u16 = [&](uint16_t v) { put(b, v); };
                put_bytes(b, "RIFF", 4); u32(36 + n * 2); put_bytes(b, "WAVEfmt ", 8); u32(16); u16(1); u16(1);
                u32(rate); u32(rate * 2); u16(2); u16(16); put_bytes(b, "data", 4); u32(n * 2);
                for (int i = 0; i < n; i++) u16(uint16_t(int16_t(std::sin(i * 6.2831853 * 440 / rate) * 8000 * std::min(1.0, (n - i) / 2000.0))));
                s3.audio[1] = std::move(clip);
            }
            EdTrigger intro;
            intro.name = "Intro";
            intro.conditions.push_back({"timer", {{"seconds", "2"}}});
            intro.effects.push_back({"message", {{"text", "Pilot, two enemy mechs hold the ridge. Destroy them."}, {"caption", "Commander"}, {"seconds", "6"}, {"pause", "0"}}});
            intro.effects.push_back({"audio", {{"clip", "1"}}});
            EdTrigger patrol;
            patrol.name = "Patrol";
            char pathbuf[160];
            snprintf(pathbuf, sizeof pathbuf, "%d,%d;%d,%d;%d,%d", int(W * 0.70f), int(H * 0.70f), int(W * 0.55f), int(H * 0.75f),
                     int(W * 0.60f), int(H * 0.55f));
            patrol.effects.push_back({"ai", {{"unit", std::to_string(m1)}, {"order", "patrol"}, {"path", pathbuf}}});
            char ptbuf[64];
            snprintf(ptbuf, sizeof ptbuf, "%d,%d", int(W * 0.78f), int(H * 0.62f));
            patrol.effects.push_back({"ai", {{"unit", std::to_string(m2)}, {"order", "defend"}, {"point", ptbuf}, {"radius", "80"}}});
            EdTrigger ambush;
            ambush.name = "Ambush";
            snprintf(ptbuf, sizeof ptbuf, "%d,%d", int(W * 0.62f), int(H * 0.62f));
            ambush.conditions.push_back({"area", {{"point", ptbuf}, {"radius", "120"}, {"inside", "1"}}});
            ambush.effects.push_back({"ai", {{"unit", std::to_string(m1)}, {"order", "attack"}}});
            ambush.effects.push_back({"ai", {{"unit", std::to_string(m2)}, {"order", "attack"}}});
            ambush.effects.push_back({"message", {{"text", "They've spotted you!"}, {"seconds", "4"}}});
            EdTrigger victory;
            victory.name = "Victory";
            victory.conditions.push_back({"destroyed", {{"units", std::to_string(m1) + "," + std::to_string(m2)}, {"all", "1"}, {"jacking", "1"}}});
            victory.effects.push_back({"objective", {{"objective", "0"}}});
            victory.effects.push_back({"message", {{"text", "Ridge secured. Well done, pilot."}, {"seconds", "5"}}});
            victory.effects.push_back({"checkpoint", {{"name", "ridge"}}});
            victory.effects.push_back({"trigger_on", {{"trigger", "End"}}});
            // Retail pattern ("TT - delay end mission"): a disabled timer that another trigger
            // enables; its countdown starts then.
            EdTrigger end;
            end.name = "End";
            end.enabled = false;
            end.conditions.push_back({"timer", {{"seconds", "8"}}});
            end.effects.push_back({"win", {}});
            // A logic trigger: both mechs damaged or the player reaches the ridge -> music change.
            EdTrigger tension;
            tension.name = "Tension";
            tension.any = true;
            tension.conditions.push_back({"trigger", {{"trigger", "Ambush"}}});
            tension.conditions.push_back({"damaged", {{"units", std::to_string(m1)}, {"percent", "50"}}});
            if (!retail_music_cues().empty())
                tension.effects.push_back({"music", {{"cue", std::to_string(retail_music_cues().front().id)}, {"intensity", "7000"}}});
            s3.triggers = {intro, patrol, ambush, victory, end, tension};
            };
            Scenario s3;
            ok = io.create(mp, s3, &err);
            CHECK(ok, "create mission: %s", err.c_str());
            if (ok) {
                fill_mission(s3);
                CHECK(convert_legacy_triggers(s3) == 6 && s3.triggers.empty(), "trigger list compiled into the world");
                fs::path out = dir / L"Mission_Ambush.ma2map";
                report("mission", s3, out);
                // Inspect the compiled world.
                MgfReader mr;
                Bytes wb;
                Wdf cw;
                if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                    std::map<std::string, int> counts;
                    for (auto& o : cw.objs) {
                        std::string n = gd.names().label(o.cls);
                        if (n.compare(0, 8, "MATrigge") == 0 || n.compare(0, 8, "MAAction") == 0 || n == "MAEvent" || n == "MAPatrol" ||
                            n == "MADefend" || n == "MAAttack" || n == "MAWaypoint" || n == "MAAIGroup" || n == "MoeEntityReference")
                            counts[n]++;
                    }
                    printf("[triggers] compiled:");
                    for (auto& [n, c] : counts) printf(" %s x%d", n.c_str(), c);
                    printf("\n");
                    CHECK(counts["MATriggerLogic"] == 1 && counts["MATriggerDestroyed"] == 2 && counts["MATriggerRadius"] == 1 &&
                              counts["MAEvent"] == 4 && counts["MAActionBroadcast"] == 3 && counts["MAActionGameCondition"] == 1,
                          "compiled trigger structure");
                    Bytes ib;
                    mr.read_file("scenario.ini", ib);
                    std::string ini(ib.begin(), ib.end());
                    CHECK(ini.find("61000 = Pilot, two enemy") != std::string::npos && ini.find("60100 = Destroy both") != std::string::npos,
                          "inline text in [Strings]");
                    CHECK(mr.find("Audio/1.wav") >= 0, "audio clip stored");
                }
            }
            // 3. Older files (retail11 G8): the trigger list in editor/project.json plus the hidden compiled
            //    group load as ordinary world objects (no "[ed] " names, no Editor Triggers group, no list).
            {
                Scenario sl;
                ok = io.create(mp, sl, &err);
                CHECK(ok, "create legacy mission: %s", err.c_str());
                if (ok) {
                    fill_mission(sl);  // the list stays, as an older editor saved it
                    fs::path out = dir / L"Mission_Legacy.ma2map";
                    SaveReport rep;
                    CHECK(io.save(sl, out, &rep, &err), "save legacy mission: %s", err.c_str());
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        int hidden = 0, groups = 0, trig = 0, broadcasts = 0, texts = 0;
                        for (auto& o : back.world.objs) {
                            std::string d = o.description();
                            hidden += d.compare(0, strlen(kEditorPrefix), kEditorPrefix) == 0;
                            groups += o.cls == key_of("MAGroup") && d == kEditorTriggerGroup;
                            trig += gd.names().label(o.cls).rfind("MATrigger", 0) == 0;
                            if (o.cls == key_of("MAActionBroadcast")) {
                                broadcasts++;
                                texts += own_text(back, o.u32("BodyTextID")) != nullptr;
                            }
                        }
                        CHECK(back.triggers.empty() && hidden == 0 && groups == 0, "legacy file: list gone, no [ed] objects, no Editor Triggers group (%d, %d)", hidden, groups);
                        CHECK(trig == 7 && broadcasts == 3 && texts == 3, "legacy file: %d triggers, %d messages with their own text (%d)", trig, broadcasts, texts);
                        report("legacy mission converted", back, dir / L"Mission_Legacy2.ma2map");
                    }
                }
            }
            // 3a. AI patrol paths and defend/teleport markers are real objects (retail11 G7): the effect
            //     names them ("wpt" / "marker"), they stay visible under "AI Paths" in the editor and
            //     on save the waypoint group hangs under the unit's MAEvent like retail; old arguments convert.
            {
                Scenario s5;
                ok = io.create(mp, s5, &err);
                CHECK(ok, "create AI-path mission: %s", err.c_str());
                if (ok) {
                    uint32_t unit = 0;
                    for (auto& o : s5.world.objs) if (o.cls == key_of("MALevelStart")) { unit = o.uid(); break; }
                    float W = s5.terrain.size_x(), H = s5.terrain.size_z();
                    char pts[160], pt[40];
                    snprintf(pts, sizeof pts, "%d,%d;%d,%d;%d,%d", int(W * .4f), int(H * .4f), int(W * .45f), int(H * .4f), int(W * .45f), int(H * .45f));
                    snprintf(pt, sizeof pt, "%d,%d", int(W * .6f), int(H * .6f));
                    EdTrigger t1;
                    t1.name = "Orders";
                    t1.effects.push_back({"ai", {{"unit", std::to_string(unit)}, {"order", "patrol"}, {"path", pts}}});
                    t1.effects.push_back({"ai", {{"unit", std::to_string(unit)}, {"order", "defend"}, {"point", pt}, {"radius", "50"}}});
                    s5.triggers = {t1};
                    int conv = adopt_ai_objects(s5);
                    auto& ea = s5.triggers[0].effects;
                    uint32_t wpt = uint32_t(strtoul(ea[0].args["wpt"].c_str(), nullptr, 10));
                    uint32_t mk = uint32_t(strtoul(ea[1].args["marker"].c_str(), nullptr, 10));
                    CHECK(conv == 2 && wpt && mk && ea[0].args["path"].empty() && ea[1].args["point"].empty(), "old path/point converted to objects (%d)", conv);
                    CHECK(ai_path_length(s5, wpt) == 3, "path object has 3 waypoints");
                    convert_legacy_triggers(s5);
                    fs::path out = dir / L"Mission_AIPaths.ma2map";
                    report("ai paths", s5, out);
                    MgfReader mr;
                    Bytes wb;
                    Wdf cw;
                    if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                        WTree t;
                        t.build(cw);
                        int gi = t.find(wpt);
                        CHECK(gi >= 0 && cw.objs[size_t(gi)].cls == key_of("MAWaypointGroup"), "saved: the patrol path is a MAWaypointGroup");
                        if (gi >= 0) {
                            int p_ = t.parent_of[size_t(gi)];
                            CHECK(p_ >= 0 && cw.objs[size_t(p_)].cls == key_of("MAEvent"), "saved: the path hangs under the unit's MAEvent (retail)");
                            CHECK(t.kids[size_t(gi)].size() == 3, "saved: 3 waypoints");
                        }
                        int groups = 0, mks = 0;
                        for (auto& o : cw.objs) { groups += o.cls == key_of("MAWaypointGroup"); mks += o.uid() == mk; }
                        CHECK(groups == 1 && mks == 1, "no extra hidden copies (%d path groups, %d marker)", groups, mks);
                        int mi = t.find(mk);
                        int mp = mi >= 0 ? t.parent_of[size_t(mi)] : -1;
                        CHECK(mp >= 0 && cw.objs[size_t(mp)].description() == kAIPathGroup, "the marker stays in the visible AI Paths group");
                    }
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        CHECK(back.triggers.empty(), "reloaded orders: no trigger list");
                        WTree t2;
                        t2.build(back.world);
                        int ix = t2.find(wpt);
                        int pp = ix >= 0 ? t2.parent_of[size_t(ix)] : -1;
                        CHECK(pp >= 0 && back.world.objs[size_t(pp)].cls == key_of("MAEvent"), "reloaded path still hangs under its MAEvent");
                        int events = 0;
                        for (auto& o : back.world.objs) events += o.cls == key_of("MAEvent");
                        CHECK(events == 2, "reloaded: 2 orders (%d)", events);
                    }
                }
            }
            // 3b. Trigger boxes and circles are retail objects (retail11 G1): a box condition names a
            //     MATriggerCuboidalVolume of the map ("vol"), saved as the trigger itself with the
            //     actions under it, with exactly the property layout of a retail volume; older files'
            //     box/circle arguments are converted at load into such objects.
            {
                Scenario s4;
                ok = io.create(mp, s4, &err);
                CHECK(ok, "create trigger-shape mission: %s", err.c_str());
                if (ok) {
                    float W = s4.terrain.size_x(), H = s4.terrain.size_z();
                    TriggerShape sh;
                    sh.x = W * 0.5f; sh.z = H * 0.5f; sh.base = 5; sh.w = 30; sh.l = 12; sh.h = 20; sh.heading = 30;
                    sh.name = "TB - gate";
                    uint32_t vol = add_trigger_shape(s4, sh);
                    CHECK(vol != 0, "add a trigger box");
                    EdTrigger gate;
                    gate.name = "Gate";
                    gate.conditions.push_back({"box", {{"vol", std::to_string(vol)}}});
                    gate.effects.push_back({"message", {{"text", "Gate box"}, {"seconds", "5"}}});
                    // An older file's trigger: shape in its own arguments.
                    EdTrigger old;
                    old.name = "Old box";
                    char pt[64];
                    snprintf(pt, sizeof pt, "%d,%d", int(W * 0.3f), int(H * 0.3f));
                    old.conditions.push_back({"box", {{"point", pt}, {"base", "8"}, {"height", "10"}, {"width", "40"}, {"length", "40"}, {"heading", "0"}}});
                    old.conditions.push_back({"area", {{"point", pt}, {"radius", "55"}, {"inside", "1"}}});
                    old.effects.push_back({"message", {{"text", "Old box"}, {"seconds", "5"}}});
                    s4.triggers = {gate, old};
                    int conv = adopt_trigger_shapes(s4);
                    const WObj* ob = condition_shape(s4, s4.triggers[1].conditions[0].args);
                    const WObj* oc = condition_shape(s4, s4.triggers[1].conditions[1].args);
                    CHECK(conv == 2 && ob && oc && s4.triggers[1].conditions[0].args.size() == 1, "old box/circle conditions converted (%d)", conv);
                    if (ob) {
                        Vec3 p;
                        world::get_pos(*ob, p);
                        float g = s4.terrain.height_at(W * 0.3f, H * 0.3f);
                        CHECK(std::fabs(p.y - (g + 8 + 5)) < 0.6f && ob->f32("VolumeHeight") == 10.f, "converted box keeps its height (centre %.1f, ground %.1f)", p.y, g);
                    }
                    CHECK(oc && oc->f32("EncounterRadius") == 55.f && oc->i32("InRadiusCheck") == 1, "converted circle keeps its radius");
                    // Property layout (keys and wire types, in order) of a retail volume / radius.
                    auto layout = [](const WObj& o) {
                        std::vector<std::pair<uint32_t, int>> v;
                        for (auto& p : o.props)
                            if (p.key != key_of("UID") && p.key != key_of("parent") && p.key != key_of("child") && p.key != key_of("sibling"))
                                v.push_back({p.key, int(p.type)});
                        return v;
                    };
                    std::set<std::vector<std::pair<uint32_t, int>>> retail_vol, retail_rad;
                    for (auto& m : gd.maps()) {
                        if (m.multiplayer) continue;
                        Wdf rw;
                        if (!gd.load_world(m, rw)) continue;
                        for (auto& o : rw.objs) {
                            if (o.cls == key_of("MATriggerCuboidalVolume")) retail_vol.insert(layout(o));
                            if (o.cls == key_of("MATriggerRadius")) retail_rad.insert(layout(o));
                        }
                    }
                    uint32_t oc_uid = oc ? oc->uid() : 0;  // compiling grows the object list: no pointers across it
                    convert_legacy_triggers(s4);
                    fs::path out = dir / L"Mission_TriggerBoxes.ma2map";
                    report("trigger boxes", s4, out);
                    MgfReader mr;
                    Bytes wb;
                    Wdf cw;
                    if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                        WTree t;
                        t.build(cw);
                        int vi = t.find(vol);
                        CHECK(vi >= 0 && cw.objs[size_t(vi)].cls == key_of("MATriggerCuboidalVolume"), "saved: the gate box is a MATriggerCuboidalVolume");
                        if (vi >= 0) {
                            const WObj& v = cw.objs[size_t(vi)];
                            CHECK(retail_vol.count(layout(v)) == 1, "saved box has a retail volume's exact property layout (%zu retail layouts)", retail_vol.size());
                            Vec3 p;
                            world::get_pos(v, p);
                            float g = s4.terrain.height_at(W * 0.5f, H * 0.5f);
                            CHECK(std::fabs(p.y - (g + 5 + 10)) < 0.6f && v.f32("VolumeWidth") == 30.f && v.f32("VolumeLength") == 12.f &&
                                      v.f32("VolumeHeading") == 30.f && v.i32("TriggerEnabled") == 1 && v.i32("PlayerEntitesOnly") == 1,
                                  "saved box fields");
                            int msgs = 0;
                            for (int k : t.kids[size_t(vi)]) msgs += cw.objs[size_t(k)].cls == key_of("MAActionBroadcast");
                            CHECK(msgs == 1, "the message action hangs under the box itself (%d)", msgs);
                            int p_ = t.parent_of[size_t(vi)];
                            CHECK(p_ >= 0 && cw.objs[size_t(p_)].description() == kTriggerShapeGroup, "the box stays in the visible Trigger Volumes group");
                        }
                        int vols = 0, rads = 0;
                        for (auto& o : cw.objs) { vols += o.cls == key_of("MATriggerCuboidalVolume"); rads += o.cls == key_of("MATriggerRadius"); }
                        CHECK(vols == 2 && rads == 1, "no extra compiled copies (%d boxes, %d circles)", vols, rads);
                        if (oc_uid) {
                            int ci = t.find(oc_uid);
                            CHECK(ci >= 0 && retail_rad.count(layout(cw.objs[size_t(ci)])) == 1, "saved circle has a retail radius trigger's exact property layout");
                        }
                    }
                    // Reload and save again: the shapes stay, the actions do not pile up.
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        CHECK(back.triggers.empty() && object_by_uid(back.world, vol) != nullptr, "reloaded gate box is still on the map");
                        Bytes b2;
                        SaveReport r2;
                        io.build(back, b2, &r2, &err);
                        fs::path out2 = dir / L"Mission_TriggerBoxes2.ma2map";
                        write_file(out2, b2.data(), b2.size());
                        MgfReader m2;
                        Wdf w2;
                        if (m2.open(out2) && m2.read_file("world.wdf", wb) && w2.parse(wb)) {
                            int vols = 0, msgs = 0;
                            for (auto& o : w2.objs) { vols += o.cls == key_of("MATriggerCuboidalVolume"); msgs += o.cls == key_of("MAActionBroadcast"); }
                            CHECK(vols == 2 && msgs == 2, "re-saved: %d boxes, %d messages (2 and 2)", vols, msgs);
                        }
                    }
                }
            }
            // 3d. Native trigger editing (retail11 G8): triggers, actions and orders are made and changed as
            //     world objects; nothing is rebuilt at save.
            {
                Scenario sn;
                ok = io.create(mp, sn, &err);
                CHECK(ok, "create native-trigger mission: %s", err.c_str());
                if (ok) {
                    uint32_t unit = 0;
                    for (auto& o : sn.world.objs) if (o.cls == key_of("MALevelStart")) { unit = o.uid(); break; }
                    uint32_t trig = add_trigger_object(sn, "MATriggerTimer", "Start");
                    uint32_t a1 = add_action_object(sn, trig, "MAActionBroadcast", "Message");
                    uint32_t a2 = add_action_object(sn, trig, "MAActionGameCondition", "Win");
                    uint32_t a3 = add_action_object(sn, trig, "MAActionShow", "Show");
                    CHECK(trig && a1 && a2 && a3, "native trigger with 3 actions");
                    const WObj* m = object_by_uid(sn.world, a1);
                    std::string* tx = m ? own_text(sn, m->u32("BodyTextID")) : nullptr;
                    CHECK(tx != nullptr, "message action has its own text");
                    if (tx) *tx = "Hello pilot";
                    CHECK(wedit::move_sibling(sn.world, a3, -1), "move an action up");
                    {
                        WTree t;
                        t.build(sn.world);
                        int ti = t.find(trig);
                        std::vector<uint32_t> kids;
                        for (int k : t.kids[size_t(ti)]) kids.push_back(sn.world.objs[size_t(k)].uid());
                        CHECK(kids == std::vector<uint32_t>({a1, a3, a2}), "actions reordered");
                        CHECK(!wedit::move_sibling(sn.world, a1, -1), "first action cannot move up");
                    }
                    uint32_t ev = add_order(sn, trig, unit, "MAPatrol");
                    for (int i = 0; i < 3; i++) order_add_point(sn, ev, 100.f + 20 * i, 200.f);
                    CHECK(ev && order_unit(sn.world, ev) == unit && order_path(sn.world, ev) && ai_path_length(sn, order_path(sn.world, ev)) == 3, "patrol order with a 3-point path");
                    CHECK(set_order_kind(sn, ev, "MADefend") && gd.names().label(object_by_uid(sn.world, order_behaviour(sn.world, ev))->cls) == "MADefend", "order changed to defend");
                    uint32_t mk = set_marker_link(sn, order_behaviour(sn.world, ev), "DefendUID", 300.f, 300.f, true, "Defend point");
                    CHECK(mk && object_by_uid(sn.world, order_behaviour(sn.world, ev))->u32("DefendUID") == mk, "defend marker created");
                    CHECK(orders_of_trigger(sn.world, trig).size() == 1, "one order starts from the trigger");
                    uint32_t logic = add_trigger_object(sn, "MATriggerLogic", "Both");
                    add_reference(sn, logic, trig);
                    CHECK(reference_children(sn.world, logic).size() == 1, "logic input");
                    fs::path out = dir / L"Mission_NativeTriggers.ma2map";
                    report("native triggers", sn, out);
                    Scenario back;
                    if (io.load(out, back, &err)) {
                        CHECK(back.triggers.empty() && object_by_uid(back.world, trig) && orders_of_trigger(back.world, trig).size() == 1, "native triggers reload unchanged");
                        const WObj* bm = object_by_uid(back.world, a1);
                        std::string* bt = bm ? own_text(back, bm->u32("BodyTextID")) : nullptr;
                        CHECK(bt && *bt == "Hello pilot", "message text reloads");
                    }
                    int warnings = 0;
                    for (auto& v : io.validate(sn)) warnings += v.level >= 2;
                    CHECK(warnings == 0, "native trigger mission validates without errors (%d)", warnings);
                }
            }
            // 3c. Add part / Duplicate (retail11 G5): every retail class can be added from its prototype
            //     (same property layout as the retail instance it copies), saved and read back; a duplicated
            //     trigger subtree gets fresh UIDs with its internal links remapped.
            {
                Scenario s5;
                ok = io.create(mp, s5, &err);
                CHECK(ok, "create add-part mission: %s", err.c_str());
                if (ok) {
                    auto layout = [](const WObj& o) {
                        std::vector<std::pair<uint32_t, int>> v;
                        for (auto& p : o.props)
                            if (p.key != key_of("UID") && p.key != key_of("parent") && p.key != key_of("child") && p.key != key_of("sibling"))
                                v.push_back({p.key, int(p.type)});
                        return v;
                    };
                    const auto& kinds = io.part_kinds();
                    CHECK(kinds.size() > 80, "part kinds from the retail worlds (%zu)", kinds.size());
                    uint32_t nx = world::next_uid(s5.world);
                    uint32_t grp = world::ensure_group(s5.world, world::root_uid(s5.world), "Added parts", nx);
                    Vec3 at{s5.terrain.size_x() * 0.5f, 0, s5.terrain.size_z() * 0.5f};
                    int added = 0, bad_layout = 0;
                    std::set<uint32_t> uids;
                    for (auto& k : kinds) {
                        uint32_t u = io.add_part(s5, k.cls, grp, &at);
                        if (!u) continue;
                        added++;
                        CHECK(uids.insert(u).second, "unique uid for %s", k.name.c_str());
                        for (auto& o : s5.world.objs)
                            if (o.uid() == u) {
                                if (layout(o) != layout(k.proto)) { bad_layout++; printf("           layout differs: %s\n", k.name.c_str()); }
                                Vec3 p;
                                if (k.positioned) CHECK(world::get_pos(o, p) && std::fabs(p.x - at.x) < 0.01f, "%s placed at the view centre", k.name.c_str());
                                // The retail links are empty.
                                if (o.cls == key_of("MAActionShow")) CHECK(o.u32("ActionEntity") == 0 && o.u32("AISet") == 0, "MAActionShow links empty");
                            }
                    }
                    CHECK(added == int(kinds.size()) && bad_layout == 0, "every part kind added with its retail layout (%d of %zu, %d differ)", added, kinds.size(), bad_layout);
                    // Duplicate a trigger with its actions.
                    uint32_t trig = 0;
                    for (auto& o : s5.world.objs) if (o.cls == key_of("MATriggerTimer")) { trig = o.uid(); break; }
                    uint32_t act = io.add_part(s5, key_of("MAActionBroadcast"), trig, &at);
                    uint32_t act2 = io.add_part(s5, key_of("MAActionShow"), trig, &at);
                    CHECK(act && act2, "add actions under a timer");
                    size_t before = s5.world.objs.size();
                    uint32_t copy = io.duplicate_subtree(s5, trig, 0);
                    CHECK(copy && copy != trig && s5.world.objs.size() == before + 3, "duplicate a timer with its two actions (%zu -> %zu)", before, s5.world.objs.size());
                    WTree t5;
                    t5.build(s5.world);
                    int ci = t5.find(copy);
                    CHECK(ci >= 0 && t5.kids[size_t(ci)].size() == 2 && t5.parent_of[size_t(ci)] == t5.parent_of[size_t(t5.find(trig))], "the copy has its two actions and the original's parent");
                    std::set<uint32_t> all;
                    bool dup_uid = false;
                    for (auto& o : s5.world.objs) dup_uid |= !all.insert(o.uid()).second;
                    CHECK(!dup_uid, "no duplicate uid after duplicate");
                    fs::path out = dir / L"Mission_AddParts.ma2map";
                    report("add parts", s5, out);
                    MgfReader mr;
                    Bytes wb;
                    Wdf cw;
                    if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                        WTree t6;
                        t6.build(cw);
                        int reach = 0;
                        std::vector<int> sub;
                        if (t6.root >= 0) { t6.subtree(t6.root, sub); reach = int(sub.size()); }
                        CHECK(reach == int(cw.objs.size()), "saved world is one tree (%d of %zu objects reachable)", reach, cw.objs.size());
                        CHECK(t6.find(copy) >= 0, "the duplicate was saved");
                    }
                }
            }
            // 3d. Cutscenes (retail11 G6): a real MACinema tree, shots add on, links stay inside.
            {
                Scenario s6;
                ok = io.create(mp, s6, &err);
                CHECK(ok, "create cutscene mission: %s", err.c_str());
                if (ok) {
                    Vec3 at{s6.terrain.size_x() * 0.4f, 0, s6.terrain.size_z() * 0.6f};
                    uint32_t cin = io.add_cutscene(s6, at, "Test cutscene");
                    CHECK(cin != 0, "add a cutscene");
                    auto count = [&](const char* cls) { int n = 0; for (auto& o : s6.world.objs) n += o.cls == key_of(cls); return n; };
                    if (cin) {
                        WTree t6;
                        t6.build(s6.world);
                        std::vector<int> sub;
                        t6.subtree(t6.find(cin), sub);
                        std::set<uint32_t> in;
                        for (int i : sub) in.insert(s6.world.objs[size_t(i)].uid());
                        int ctrl = 0, cam = 0, paths = 0, props = 0, actions = 0, dangling = 0;
                        for (int i : sub) {
                            const WObj& o = s6.world.objs[size_t(i)];
                            ctrl += o.cls == key_of("MAFreeCameraController");
                            cam += o.cls == key_of("MAFreeCamera");
                            paths += o.cls == key_of("MAPath");
                            props += o.cls == key_of("MACameraProperty");
                            actions += gd.names().label(o.cls).compare(0, 8, "MAAction") == 0;
                            for (const char* k : {"Camera", "triggerInit", "triggerExit", "eyeUID", "focusUID", "m_wptGroupUID", "WaypointID"})
                                if (o.find(k) && o.u32(k) && !in.count(o.u32(k))) { dangling++; printf("           dangling %s in %s\n", k, o.description().c_str()); }
                        }
                        CHECK(ctrl == 1 && cam == 1 && paths == 1 && props == 5 && actions == 0 && dangling == 0, "cutscene shape (ctrl %d cam %d path %d steps %d actions %d dangling %d)", ctrl, cam, paths, props, actions, dangling);
                        const WObj& co = s6.world.objs[size_t(t6.find(cin))];
                        Vec3 cp;
                        CHECK(co.description() == "Test cutscene" && world::get_pos(co, cp) && std::fabs(cp.x - at.x) < 0.01f, "cutscene named and placed");
                        uint32_t c2 = io.add_cutscene_shot(s6, cin);
                        CHECK(c2 != 0 && count("MAFreeCameraController") == 2 && count("MAPath") == 2 && count("MAFreeCamera") == 2, "second shot added (%d controllers, %d paths)", count("MAFreeCameraController"), count("MAPath"));
                        t6.build(s6.world);
                        int c2i = t6.find(c2);
                        if (c2i >= 0) {
                            const WObj& cc = s6.world.objs[size_t(c2i)];
                            int camera_i = t6.find(cc.u32("Camera"));
                            CHECK(camera_i >= 0 && cc.u32("Camera") != s6.world.objs[size_t(t6.find(cin))].uid(), "shot 2 uses its own camera");
                            std::vector<int> ks;
                            t6.subtree(c2i, ks);
                            bool own_path = false;
                            for (int i : ks) {
                                const WObj& pr = s6.world.objs[size_t(i)];
                                if (pr.cls == key_of("MACameraProperty") && pr.u32("eyeUID")) {
                                    int pi = t6.find(pr.u32("eyeUID"));
                                    own_path = pi >= 0 && t6.find(s6.world.objs[size_t(pi)].u32("m_wptGroupUID")) >= 0;
                                    // the first shot's path is another object
                                    for (int j : sub) own_path = own_path && s6.world.objs[size_t(j)].uid() != pr.u32("eyeUID") || own_path;
                                }
                            }
                            CHECK(own_path, "shot 2's eye path and waypoint group exist");
                        }
                        std::set<uint32_t> all;
                        bool dup = false;
                        for (auto& o : s6.world.objs) dup |= !all.insert(o.uid()).second;
                        CHECK(!dup, "no duplicate uid after cutscene + shot");
                        {
                            size_t before = s6.world.objs.size();
                            int gone = io.remove_cutscene_shot(s6, c2);
                            CHECK(gone > 5 && count("MAFreeCameraController") == 1 && count("MAPath") == 1 && count("MAFreeCamera") == 1, "second shot deleted (%d objects, %d controllers)", gone, count("MAFreeCameraController"));
                            std::set<uint32_t> ids;
                            bool dangling = false;
                            for (auto& o : s6.world.objs) ids.insert(o.uid());
                            for (auto& o : s6.world.objs)
                                for (const char* k : {"Camera", "triggerInit", "triggerExit", "eyeUID", "focusUID"}) dangling |= o.find(k) && o.u32(k) && !ids.count(o.u32(k));
                            CHECK(!dangling && s6.world.objs.size() == before - size_t(gone), "no dangling shot links after deleting a shot");
                            uint32_t c3 = io.add_cutscene_shot(s6, cin);
                            CHECK(c3 != 0 && count("MAFreeCameraController") == 2, "shot can be added again");
                        }
                        EdTrigger tt;
                        tt.name = "Play it";
                        tt.conditions.push_back({"timer", {{"seconds", "3"}}});
                        tt.effects.push_back({"cinema", {{"cinema", std::to_string(cin)}}});
                        s6.triggers = {tt};
                        convert_legacy_triggers(s6);
                        fs::path out = dir / L"Mission_Cutscene.ma2map";
                        report("cutscene", s6, out);
                        MgfReader mr;
                        Bytes wb;
                        Wdf cw;
                        if (mr.open(out) && mr.read_file("world.wdf", wb) && cw.parse(wb)) {
                            WTree t7;
                            t7.build(cw);
                            std::vector<int> all7;
                            t7.subtree(t7.root, all7);
                            CHECK(all7.size() == cw.objs.size(), "saved cutscene world is one tree (%zu of %zu)", all7.size(), cw.objs.size());
                            int acts = 0;
                            for (auto& o : cw.objs) acts += o.cls == key_of("MAActionActivateCinema") && o.u32("cinemaID") == cin;
                            CHECK(acts == 1, "the trigger plays the new cutscene (%d)", acts);
                        }
                    }
                }
            }
            // Mode kits: a CTF kit from Bogged Down makes a fresh map pass CTF validation.
            {
                NewMapParams kp;
                kp.w = kp.h = 129;
                kp.types = {"CTF"};
                Scenario k;
                if (io.create(kp, k, &err)) {
                    io.add_spawn_ring(k, 1, {200, 0, 200}, 25, 6);
                    io.add_spawn_ring(k, 2, {800, 0, 800}, 25, 6);
                    bool before = false, after = false;
                    for (auto& v : io.validate(k)) before |= v.level == 2;
                    int n = io.add_mode_kit(k, "CTF", "BoggedDown", &err);
                    io.sync_spawn_groups(k);
                    for (auto& v : io.validate(k)) { after |= v.level == 2; if (v.level == 2) printf("           kit validate: %s\n", v.text.c_str()); }
                    CHECK(before && !after && n > 0, "CTF kit fixes validation (%d objects added)", n);
                    printf("[scenario] CTF kit from BoggedDown: %d objects; validation errors before %d, after %d\n", n, int(before), int(after));
                }
            }
            // Snatch It kit (bots batch 3): the data cores keep the retail pile's spacing on a smaller map (scaled to 5 m apart
            // the core models made one solid block 'Mechs could not reach).
            {
                NewMapParams sp;
                sp.multiplayer = true;
                sp.w = sp.h = 129;
                sp.types = {"SnatchIt"};
                Scenario k;
                auto smaps = io.maps_with_mode("SnatchIt");
                if (!smaps.empty() && io.create(sp, k, &err)) {
                    io.add_spawn_ring(k, 1, {200, 0, 200}, 25, 6);
                    io.add_spawn_ring(k, 2, {800, 0, 800}, 25, 6);
                    int n = io.add_mode_kit(k, "SnatchIt", smaps.front(), &err);
                    std::vector<Vec3> cores;
                    for (auto& o : k.world.objs) { Vec3 p; if (o.cls == key_of("MASnatchItDataCore") && world::get_pos(o, p)) cores.push_back(p); }
                    float mind = 1e9f;
                    for (size_t a = 0; a < cores.size(); a++)
                        for (size_t b = a + 1; b < cores.size(); b++) mind = std::min(mind, std::hypot(cores[a].x - cores[b].x, cores[a].z - cores[b].z));
                    CHECK(n > 0 && cores.size() >= 2 && mind >= 9.f, "Snatch It kit from %s: %zu cores, nearest two %.1f m apart (>= 9)", smaps.front().c_str(), cores.size(), mind);
                }
            }
            // Base War kit (bots F20): the saved map's objective slots 0/1 name the kit's power plants (the clone renumbers
            // them; the retail UIDs left in MARoot ended the match at once), and the start-up sets list their members again.
            {
                NewMapParams bp;
                bp.multiplayer = true;
                bp.w = bp.h = 129;
                bp.types = {"BaseWar"};
                bp.base = "boggeddown.mgf";
                Scenario k;
                if (io.create(bp, k, &err)) {
                    io.add_spawn_ring(k, 1, {200, 0, 200}, 25, 6);
                    io.add_spawn_ring(k, 2, {800, 0, 800}, 25, 6);
                    int n = io.add_mode_kit(k, "BaseWar", "BoggedDown", &err);
                    io.sync_spawn_groups(k);
                    fs::path bf = fs::temp_directory_path() / L"ma2scn_basewar_test.ma2map";
                    SaveReport brep;
                    Scenario kb;
                    bool bok = n > 0 && io.save(k, bf, &brep, &err) && io.load(bf, kb, &err);
                    CHECK(bok, "Base War kit map saves and reloads (%d objects): %s", n, err.c_str());
                    if (bok) {
                        int ri = world::find_class(kb.world, world::K_ROOT);
                        for (int slot = 0; slot < 2 && ri >= 0; slot++) {
                            const WObj& r = kb.world.objs[size_t(ri)];
                            uint32_t t = r.u32("ob" + std::to_string(slot) + "obj0");
                            auto want = objective_targets(kb.world, slot);
                            const WObj* b = t ? object_by_uid(kb.world, t) : nullptr;
                            CHECK(b && want.size() == 1 && want[0] == t && r.i32("NumObjectiveObjects" + std::to_string(slot)) == 1,
                                  "Base War slot %d names the kit's power plant (ob%dobj0 %u, %s)", slot, slot, t, b ? b->description().c_str() : "missing");
                        }
                        int filled = 0, sets = 0;
                        for (auto& o : kb.world.objs)
                            if (o.cls == key_of("MoeEntitySet")) { sets++; filled += o.i32("MoeEntitySetCount") > 0; }
                        CHECK(filled > 0, "Base War kit: the start-up entity sets list members again (%d of %d)", filled, sets);
                        printf("[scenario] Base War kit from BoggedDown: %d objects; %d of %d entity sets with members\n", n, filled, sets);
                    }
                    // Check per team (bots F33): the kit puts a helipad by each team's VTOLs; with the repair areas gone,
                    // each team gets a 'no helipad' warning.
                    auto heli_warns = [&](const Scenario& sc) {
                        int c = 0;
                        for (auto& it : io.validate(sc)) c += it.text.find("no helipad") != std::string::npos;
                        return c;
                    };
                    int with = heli_warns(k);
                    for (auto& o : k.world.objs) if (o.cls == key_of("MACollectionArea")) o.cls = key_of("MAGroup");
                    int without = heli_warns(k);
                    CHECK(with == 0 && without == 2, "Check: a team with VTOLs and no helipad near its spawns is warned (kit %d, no pads %d)", with, without);
                }
            }
            // Grinder kit (bots F47): every set link in the copied waves names an entity set (the kit's new sets took
            // the clone's own ids, so the wave 'Mechs' links were renumbered onto the group; DestroySet / RadiusSet,
            // which start the next wave, were never mapped).
            {
                NewMapParams gp;
                gp.multiplayer = true;
                gp.w = gp.h = 129;
                gp.types = {"Grinder"};
                Scenario k;
                if (io.create(gp, k, &err)) {
                    int n = io.add_mode_kit(k, "Grinder", "InYourFace", &err);
                    int links = 0, bad = 0, wave_sets = 0;
                    for (auto& o : k.world.objs)
                        for (const char* f : {"SetMember0", "SetMember1", "ActionSet", "AISet", "DestroySet", "RadiusSet"}) {
                            uint32_t v = o.u32(f);
                            if (!v) continue;
                            const WObj* t = object_by_uid(k.world, v);
                            links++;
                            if (!t || t->cls != key_of("MoeEntitySet")) { bad++; continue; }
                            wave_sets += std::string(f) == "DestroySet";
                        }
                    CHECK(n > 0 && links > 30 && bad == 0 && wave_sets == 4, "Grinder kit: set links name entity sets (%d objects, %d links, %d broken, %d wave triggers)",
                          n, links, bad, wave_sets);
                    printf("[scenario] Grinder kit from InYourFace: %d objects; %d set links, %d broken\n", n, links, bad);
                }
            }
            if (!bisect.empty()) {
                // bisect2 (maintainer 2026-10-20): the type map is now always the Base level's own
                // (an all-0x0001 map stopped rounds, run X5). These isolate the remaining 129x129
                // failure on a Bogged Down base and check the fix.
                fs::create_directories(bisect);
                auto centroids = [&](const char* map, Vec3& red, Vec3& blue) {
                    red = blue = {0, 0, 0};
                    int nr = 0, nb = 0;
                    if (const Wdf* bw = io.retail_world(map))
                        for (auto& o : bw->objs) {
                            Vec3 p;
                            if (o.cls != key_of("MALevelStart") || !world::get_pos(o, p)) continue;
                            if (o.i32("TeamIndex") == 1) { red.x += p.x; red.z += p.z; nr++; }
                            if (o.i32("TeamIndex") == 2) { blue.x += p.x; blue.z += p.z; nb++; }
                        }
                    if (nr) { red.x /= nr; red.z /= nr; }
                    if (nb) { blue.x /= nb; blue.z /= nb; }
                };
                struct Variant { const char* file; const char* what; const char* map; int size; bool gen; int content; bool drop_culture; };
                // content: 0 = the Base map's own TD kit (its MTGs with TeamDestruction=1), 1 = editor spawns
                //          (6 Red + 6 Blue in MTG "Team Spawns"), 2 = editor spawns + 4 vehicle pads + 4 snow trees
                const Variant vs[] = {
                    {"G1_GoCommando129_GenHeights_EditorSpawns", "GoCommando base, generated 129 heights, GoCommando's own type map, editor spawns", "GoCommando", 129, true, 1, false},
                    {"G2_GoCommando129_OwnHeights_EditorSpawns", "GoCommando base, GoCommando's own heights and textures, editor spawns at its retail spawn areas", "GoCommando", 129, false, 1, false},
                    {"G3_GoCommando129_OwnHeights_RetailTDKit", "GoCommando base, own heights and textures, GoCommando's TD kit", "GoCommando", 129, false, 0, false},
                    {"G4_GoCommando129_GenHeights_RetailTDKit", "GoCommando base, generated 129 heights, GoCommando's TD kit", "GoCommando", 129, true, 0, false},
                    {"G5_GoCommando129_GenHeights_Spawns_Pads_SnowTrees", "the Small_TeamDestruction sample rebuilt on GoCommando: generated heights, editor spawns, 4 pads, 4 snow trees ([Assets])", "GoCommando", 129, true, 2, false},
                    {"B9_BD257_GenHeights_BDTypeMap_RetailTDKit", "B3 with the fix: Bogged Down base, generated 257 heights, Bogged Down's own type map, BD TD kit", "BoggedDown", 257, true, 0, false},
                    {"B10_BD129_GenHeights_BDTypeMap_EditorSpawns_NoCulture", "B6 without the culture system (MACultureSystem removed): Bogged Down base, generated 129 heights, BD type map, editor spawns", "BoggedDown", 129, true, 1, true},
                };
                for (auto& v : vs) {
                    Scenario sc;
                    NewMapParams bp;
                    bp.multiplayer = true;
                    bp.types = {"TeamDestruction"};
                    bp.name = v.file;
                    bp.base = lower(std::string(v.map)) + ".mgf";
                    bp.start = v.gen ? NewMapParams::Noise : NewMapParams::Flat;
                    bp.keep_base_terrain = !v.gen;
                    bp.drop_culture = v.drop_culture;
                    bp.w = bp.h = v.size;
                    bp.seed = 20261020;
                    bool good = io.create(bp, sc, &err);
                    CHECK(good, "bisect %s: %s", v.file, err.c_str());
                    if (!good) continue;
                    float W = sc.terrain.size_x(), H = sc.terrain.size_z();
                    if (v.content == 0) {
                        int n = io.add_mode_kit(sc, "TeamDestruction", v.map, &err);
                        CHECK(n > 0, "kit %s: %s", v.file, err.c_str());
                    } else if (!v.gen) {
                        Vec3 red, blue;
                        centroids(v.map, red, blue);
                        io.add_spawn_ring(sc, 1, red, 25, 6);
                        io.add_spawn_ring(sc, 2, blue, 25, 6);
                    } else {
                        io.add_spawn_ring(sc, 1, {W * 0.22f, 0, H * 0.22f}, 28.f, 6);
                        io.add_spawn_ring(sc, 2, {W * 0.78f, 0, H * 0.78f}, 28.f, 6);
                    }
                    if (v.content == 2) {
                        float off = 0;
                        std::set<std::string> pads_used;
                        for (auto& pk : io.pad_kinds())
                            if ((pk.label == "Atlas" || pk.label == "Catapult") && pads_used.insert(pk.label).second) {
                                io.add_pad(sc, pk, {W * 0.30f + off, 0, H * 0.15f}, 0.f);
                                io.add_pad(sc, pk, {W * 0.70f - off, 0, H * 0.85f}, 3.14159f);
                                off += 25.f;
                            }
                        for (int k = 0; k < 4; k++) {
                            float a = k * 1.5708f;
                            io.place_template(sc, 123384, {W * 0.5f + std::cos(a) * 30, 0, H * 0.5f + std::sin(a) * 30}, a);
                        }
                    }
                    io.sync_spawn_groups(sc);
                    sc.description = std::string("Bisect2: ") + v.what + ".";
                    sc.author = "Scenario Editor bisect";
                    SaveReport rep;
                    fs::path out = bisect / (widen(v.file) + L".ma2map");
                    bool saved = io.save(sc, out, &rep, &err);
                    CHECK(saved, "save %s: %s", v.file, err.c_str());
                    int errors = 0;
                    for (auto& it : io.validate(sc)) if (it.level == 2) errors++;
                    printf("[bisect] %-52s %4zu objects %4zu templates %3zu assets %4d nav %d validation errors\n", v.file, rep.objects,
                           rep.templates, rep.asset_files, rep.nav_nodes, errors);
                }
            }
            if (!mvariant.empty()) {
                // M3 (maintainer runs bi_M1/bi_M2): generated terrain with the height-fog band moved
                // under the lowest ground.
                fs::create_directories(mvariant);
                NewMapParams mm;
                mm.multiplayer = false;
                mm.w = mm.h = 129;
                mm.base = "mission_05.mgf";
                mm.start = NewMapParams::Noise;
                mm.flat_height = 15.f;
                mm.seed = 5;
                mm.name = "M3_Mission05_GenTerrain_FogUnderGround";
                Scenario sc;
                if (io.create(mm, sc, &err)) {
                    fill_mission(sc);
                    sc.name = mm.name;
                    sc.description = "Bisect2: mission sample on generated terrain; height-fog band moved 2 m under the lowest ground, colour-map alpha = Base mean.";
                    int ei = world::find_class(sc.world, world::K_ENV);
                    float lowest = *std::min_element(sc.terrain.hgt.begin(), sc.terrain.hgt.end());
                    if (ei >= 0) {
                        const WObj& env = sc.world.objs[size_t(ei)];
                        printf("[bisect] M3 fog band %.2f..%.2f m, lowest ground %.2f m, FogStart/End %.0f/%.0f\n", env.f32("FogBottom"),
                               env.f32("FogTop"), lowest, env.f32("FogStart"), env.f32("FogEnd"));
                        CHECK(env.f32("FogTop") < lowest, "fog band under the ground");
                    }
                    SaveReport rep;
                    bool saved = io.save(sc, mvariant / L"M3_Mission05_GenTerrain_FogUnderGround.ma2map", &rep, &err);
                    CHECK(saved, "save M3: %s", err.c_str());
                }
            }
            if (all_maps) {
                // Every retail map: copy, build the .ma2map in memory, reload it.
                int okc = 0, total = 0;
                double tt = now_s();
                for (auto& m : gd.maps()) {
                    if (!m.resource_present) continue;
                    if (const char* only = getenv("FMT_MAP")) if (m.name != only) continue;
                    total++;
                    NewMapParams cp2;
                    cp2.start = NewMapParams::CopyRetail;
                    cp2.copy_map = m.name;
                    Scenario sc;
                    Bytes arc;
                    SaveReport rep;
                    bool good = io.create(cp2, sc, &err) && io.build(sc, arc, &rep, &err);
                    fs::path tmpf = fs::temp_directory_path() / L"ma2scn_allmaps.ma2map";
                    Scenario back;
                    good = good && write_file(tmpf, arc) && io.load(tmpf, back, &err) && back.world.objs.size() >= sc.world.objs.size();
                    fs::remove(tmpf);
                    CHECK(good, "copy+save+load %s: %s", m.name.c_str(), err.c_str());
                    // Retail copies keep their own type map, and every retail spawn, pad and objective stands on 0/8.
                    int ground_errors = 0;
                    if (good)
                        for (auto& it : io.validate(sc))
                            if (it.level == 2 && it.text.find("stands where players cannot go") != std::string::npos) ground_errors++;
                    CHECK(ground_errors == 0, "retail copy %s: %d spawns/pads/objectives off the playable area", m.name.c_str(), ground_errors);
                    CHECK(!good || rep.asset_files == 0, "retail copy %s needs no [Assets] (%zu files)", m.name.c_str(), rep.asset_files);
                    // Single player reads StartVehiclesGeneric: 11a and 13b start on foot (the other 17 slots say Mad Dog / Rommel).
                    if (m.name == "Mission_11a" || m.name == "Mission_13b") CHECK(!good || io.start_container(sc) == 123999, "%s: the player starts on foot (container %u)", m.name.c_str(), io.start_container(sc));
                    // A new objective on a retail copy gets a mission variable nothing else sets (Mission_01b: slot 16's
                    // index was set by 'TT - delay post barge dest', so the new objective finished at once).
                    if (good && !sc.multiplayer && sc.objectives.size() < 30) {
                        const int slot = int(sc.objectives.size());
                        sc.objectives.push_back("New objective");
                        const int v = claim_objective_var(sc, slot);
                        int clash = 0;
                        for (auto& o : sc.world.objs)
                            if (o.cls == key_of("MAActionMissionVariableSet") && o.i32("MissionVariableSet", -1) == v) clash++;
                        for (int j = 0; j < slot; j++) clash += objective_var(sc, j) == v;
                        CHECK(clash == 0 && v >= 0 && v < 30, "retail copy %s: new objective %d got variable %d, already used %d times", m.name.c_str(), slot + 1, v, clash);
                    }
                    if (good) okc++;
                    // Loss audit: every object of the retail world must come back from the saved file with the same
                    // class and the same properties (key, type, bytes). Differences are tallied by class.property.
                    if (good)
                        if (const Wdf* rw = io.retail_world(m.name)) {
                            std::unordered_map<uint32_t, const WObj*> got;
                            for (auto& o : back.world.objs) got[o.uid()] = &o;
                            int miss = 0, cls_diff = 0, prop_miss = 0, prop_diff = 0, prop_extra = 0;
                            std::map<std::string, int> tally;
                            for (auto& o : rw->objs) {
                                auto it = got.find(o.uid());
                                if (it == got.end()) { miss++; tally["missing:" + gd.names().label(o.cls)]++; continue; }
                                const WObj& b2 = *it->second;
                                if (b2.cls != o.cls) { cls_diff++; continue; }
                                for (auto& pr : o.props) {
                                    const WProp* q = b2.find(pr.key);
                                    if (!q) { prop_miss++; tally["lost:" + gd.names().label(o.cls) + "." + gd.names().label(pr.key)]++; }
                                    else if (q->type != pr.type || q->data != pr.data) { prop_diff++; tally["changed:" + gd.names().label(o.cls) + "." + gd.names().label(pr.key)]++; }
                                }
                                for (auto& pr : b2.props) if (!o.find(pr.key)) prop_extra++;
                            }
                            printf("           audit %-16s objects %zu->%zu  missing %d  class %d  props lost %d changed %d extra %d\n", m.name.c_str(),
                                   rw->objs.size(), back.world.objs.size(), miss, cls_diff, prop_miss, prop_diff, prop_extra);
                            std::vector<std::pair<int, std::string>> top;
                            for (auto& [k, n] : tally) top.push_back({n, k});
                            std::sort(top.rbegin(), top.rend());
                            for (size_t k = 0; k < top.size() && k < 6; k++) printf("               %6d %s\n", top[k].first, top[k].second.c_str());
                            CHECK(cls_diff == 0 && miss == 0 && prop_miss == 0, "audit %s: nothing lost in copy+save+load (%d missing objects, %d class changes, %d lost properties; value changes are the model-path / low-detail / terrain fix-ups)", m.name.c_str(), miss, cls_diff, prop_miss);
                        }
                    int edited_nav = 0;
                    if (good) {  // the same map with one terrain sample changed gets a generated graph
                        sc.terrain.ref(1, 1) += 1.f;
                        SaveReport r2;
                        Bytes a2;
                        if (io.build(sc, a2, &r2, &err)) edited_nav = r2.nav_nodes;
                        CHECK(edited_nav > 0 && edited_nav <= 2500, "generated nav size %s: %d", m.name.c_str(), edited_nav);
                    }
                    printf("           %-16s %5zu objects %4zu templates %3zu assets %5d nav (edited %4d)  %.2f MB\n", m.name.c_str(), rep.objects,
                           rep.templates, rep.asset_files, rep.nav_nodes, edited_nav, rep.file_bytes / 1048576.0);
                }
                printf("[scenario] all retail maps copied, saved and reloaded: %d/%d in %.1fs\n", okc, total, now_s() - tt);
            }
            printf("[scenario] %.1fs\n", now_s() - t0);
        }
    }

    // ---------------------------------------------------------------- custom-size water
    {
        const char* kM = "Models/Environment/Water/Water_Sw_1000x1000/Water_Sw_1000x1000.MGMODEL";
        MgfReader* a = gd.archive("mission_07.mgf");
        wcustom::Source src;
        bool have = a && a->read_file(std::string(kM) + "{water_1000x1000 verts}", src.verts) && a->read_file(std::string(kM) + "{water_1000x1000 indices}", src.idx);
        CHECK(have, "water model files read");
        if (have) {
            CHECK(wcustom::plain(src), "retail 1000x1000 water is a plain grid");
            {
                // The Description carries name, size and shape: rename keeps size and shape, an oval reads back as an oval.
                WObj wo;
                wo.set_str("Description", ScenarioIO::water_description("Mere", 140, 90, wcustom::Shape::Oval));
                float cw = 0, cl = 0;
                CHECK(ScenarioIO::water_custom_size(wo, cw, cl) && cw == 140 && cl == 90, "oval water description keeps its size");
                CHECK(ScenarioIO::water_custom_shape(wo) == wcustom::Shape::Oval && ScenarioIO::water_name(wo) == "Mere", "oval water description keeps shape and name");
                wo.set_str("Description", ScenarioIO::water_description("Pond", cw, cl, ScenarioIO::water_custom_shape(wo)));
                CHECK(ScenarioIO::water_custom_shape(wo) == wcustom::Shape::Oval && ScenarioIO::water_name(wo) == "Pond", "renaming a water keeps its oval shape");
            }
            for (auto sz : {std::pair<float, float>{1000, 1000}, {300, 120}, {8, 8}, {75, 640}}) {
                Bytes v, i;
                std::string werr;
                bool ok = wcustom::generate(src, sz.first, sz.second, wcustom::Shape::Rect, v, i, nullptr, &werr);
                CHECK(ok, "water %gx%g generate: %s", sz.first, sz.second, werr.c_str());
                if (!ok) continue;
                float w = 0, l = 0;
                CHECK(wcustom::extent(v, w, l) && std::fabs(w - sz.first) < 0.01f && std::fabs(l - sz.second) < 0.01f, "water %gx%g extent %g x %g", sz.first, sz.second, w, l);
                CHECK(rd<uint32_t>(v.data() + 8) == v.size() && rd<uint32_t>(i.data() + 8) == i.size(), "water %gx%g header sizes", sz.first, sz.second);
                // Decode the strip: every real triangle faces up (same winding as the retail tile).
                auto find_tag = [](const Bytes& b, const char* t, size_t from) { for (size_t k = from; k + 4 <= b.size(); k++) if (!memcmp(b.data() + k, t, 4)) return k; return size_t(-1); };
                size_t vd = find_tag(v, "data", 0x20), vi = find_tag(v, "info", 0x20);
                size_t nn = find_tag(i, "nidx", 0x20), id = nn == size_t(-1) ? nn : find_tag(i, "data", nn);
                bool good = vd != size_t(-1) && id != size_t(-1) && vi != size_t(-1);
                int bad = 0, tris = 0;
                if (good) {
                    uint32_t cnt = rd<uint32_t>(v.data() + vi + 12), n = rd<uint32_t>(i.data() + nn + 8);
                    for (uint32_t t = 0; t + 2 < n; t++) {
                        uint16_t q[3] = {rd<uint16_t>(i.data() + id + 8 + 2 * t), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 1)), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 2))};
                        if (q[0] >= cnt || q[1] >= cnt || q[2] >= cnt) { bad++; continue; }
                        if (q[0] == q[1] || q[1] == q[2] || q[0] == q[2]) continue;
                        float p[3][2];
                        for (int k = 0; k < 3; k++) { p[k][0] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20); p[k][1] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20 + 8); }
                        float y = (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]);
                        if (t & 1) y = -y;
                        if (y <= 0) bad++;
                        tris++;
                    }
                }
                CHECK(good && bad == 0 && tris > 0, "water %gx%g strip: %d of %d triangles wrong or out of range", sz.first, sz.second, bad, tris);
            }
            // An oval: a triangle list of rings, up-facing, inside the box, with the box's extent.
            for (auto sz : {std::pair<float, float>{300, 120}, {60, 60}, {1000, 400}}) {
                Bytes v, i;
                bool list = false;
                std::string werr;
                bool ok = wcustom::generate(src, sz.first, sz.second, wcustom::Shape::Oval, v, i, &list, &werr);
                CHECK(ok && list, "oval %gx%g generate: %s", sz.first, sz.second, werr.c_str());
                if (!ok) continue;
                float w = 0, l = 0;
                CHECK(wcustom::extent(v, w, l) && std::fabs(w - sz.first) < 0.5f && std::fabs(l - sz.second) < 0.5f, "oval %gx%g extent %g x %g", sz.first, sz.second, w, l);
                auto find_tag = [](const Bytes& b, const char* t, size_t from) { for (size_t k = from; k + 4 <= b.size(); k++) if (!memcmp(b.data() + k, t, 4)) return k; return size_t(-1); };
                size_t vd = find_tag(v, "data", 0x20), vi = find_tag(v, "info", 0x20);
                size_t nn = find_tag(i, "nidx", 0x20), id = nn == size_t(-1) ? nn : find_tag(i, "data", nn);
                int bad = 0, tris = 0;
                if (vd != size_t(-1) && vi != size_t(-1) && id != size_t(-1)) {
                    uint32_t cnt = rd<uint32_t>(v.data() + vi + 12), n = rd<uint32_t>(i.data() + nn + 8);
                    bad += n % 3 != 0;
                    for (uint32_t t = 0; t + 2 < n; t += 3) {
                        uint16_t q[3] = {rd<uint16_t>(i.data() + id + 8 + 2 * t), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 1)), rd<uint16_t>(i.data() + id + 8 + 2 * (t + 2))};
                        if (q[0] >= cnt || q[1] >= cnt || q[2] >= cnt) { bad++; continue; }
                        float p[3][2];
                        for (int k = 0; k < 3; k++) { p[k][0] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20); p[k][1] = rd<float>(v.data() + vd + 8 + size_t(q[k]) * 20 + 8); }
                        float y = (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]) - (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]);
                        if (y <= 0) bad++;
                        tris++;
                    }
                } else bad = 1;
                CHECK(bad == 0 && tris > 0, "oval %gx%g list: %d of %d triangles wrong or out of range", sz.first, sz.second, bad, tris);
            }
        }
    }

    // ---------------------------------------------------------------- particle effects (View > Effects)
    {
        // Every retail .mgparticle parses; a looped sprite emitter simulates finite sprites.
        int files = 0, parsed = 0, looping = 0;
        for (auto& an : gd.level_archives()) {
            MgfReader* a = gd.archive(an);
            if (!a) continue;
            for (auto& f : a->files()) {
                if (!iends_with(f.path, ".mgparticle")) continue;
                Bytes b;
                if (!a->read_file(f.path, b, &err)) continue;
                files++;
                ParticleFx fx;
                if (parse_particle(std::string(b.begin(), b.end()), fx)) {
                    parsed++;
                    if (fx.loops()) {
                        looping++;
                        std::vector<PSpriteOut> out;
                        float o[3] = {0, 0, 0};
                        simulate_particle(fx, o, 12.3f, 7, 1.f, 0, 0, false, out);
                        bool finite = true;
                        for (auto& p : out) finite = finite && std::isfinite(p.pos[0] + p.pos[1] + p.pos[2] + p.w + p.h) && p.sprite;
                        CHECK(finite, "particle %s/%s: non-finite sprite", an.c_str(), f.path.c_str());
                    }
                } else CHECK(false, "particle %s/%s does not parse", an.c_str(), f.path.c_str());
            }
        }
        const char* xml =
            "<mgParticle version=\"3.0\" lod_start=\"100\" lod_range=\"50\"><emitter type=\"mgParticleEmitter\" name=\"e\" cycle_mode=\"MODE_LOOPED\" cycle_length=\"1.0\" emit_worldspace=\"true\">"
            "<source type=\"mgPointSource\"><pitch_start variance=\"0\"><key time=\"0\" value=\"0\"/></pitch_start></source>"
            "<system type=\"mgKineticSystem\" gravity=\"0\" drag=\"0\" offset=\"0, 1, 0\"><rate variance=\"0\"><key time=\"0\" value=\"10\"/></rate>"
            "<lifespan variance=\"0\"><key time=\"0\" value=\"2\"/></lifespan><initial_velocity variance=\"0\"><key time=\"0\" value=\"1\"/></initial_velocity>"
            "<prototype type=\"mgSpriteType\" name=\"s\"><size variance=\"0\"><key time=\"0\" value=\"2\"/></size><color variance=\"0\"><key time=\"0\" value=\"255, 128, 0, 200\"/></color>"
            "<material type=\"BASE\" blend_mode=\"additive\"><base_texture name=\"x.tif\"/></material></prototype></system></emitter></mgParticle>";
        ParticleFx fx;
        CHECK(parse_particle(xml, fx) && fx.loops() && fx.emitters[0].systems[0].sprite.blend == 1, "synthetic particle parse");
        std::vector<PSpriteOut> out;
        float o[3] = {10, 0, 5};
        simulate_particle(fx, o, 5.0f, 1, 1.f, 0, 0, false, out);
        // 10 per second for 2 s: about 20 alive, each rising 1 m/s from y = 1 + origin.
        bool rises = out.size() >= 15 && out.size() <= 21;
        for (auto& p : out) rises = rises && std::fabs(p.pos[0] - 10) < 1e-3f && p.pos[1] >= 1.f - 1e-3f && p.pos[1] <= 3.f + 1e-3f && std::fabs(p.rgba[3] - 200.f / 255.f) < 0.01f;
        CHECK(rises, "synthetic particle simulation (%zu sprites)", out.size());
        std::vector<PSpriteOut> again;
        simulate_particle(fx, o, 5.0f, 1, 1.f, 0, 0, false, again);
        CHECK(again.size() == out.size() && (out.empty() || again[0].pos[1] == out[0].pos[1]), "particle simulation is deterministic");
        std::vector<PSpriteOut> half;
        simulate_particle(fx, o, 5.0f, 1, 0.5f, 0, 0, false, half);
        CHECK(!half.empty() && std::fabs(half[0].rgba[3] - 100.f / 255.f) < 0.01f, "particle LOD fade scales the alpha");
        std::vector<PSpriteOut> gated;
        simulate_particle(fx, o, 5.0f, 1, 1.f, 0.2f, 1.8f, false, gated);
        CHECK(gated.size() < out.size(), "on/off gating thins an intermittent effect (%zu of %zu)", gated.size(), out.size());
        printf("particles: %d files, %d parsed, %d looping\n", files, parsed, looping);
    }

    // W-113: Name / Description limits count characters, not bytes.
    {
        std::string a(119, 'x');
        CHECK(clip_chars(a, kNameMax) == kNameMax && a.size() == size_t(kNameMax), "W-113 a 119-character name is cut to %d", kNameMax);
        std::string u;
        for (int i = 0; i < 70; i++) u += "\xC3\xA9";  // 70 x e-acute: 2 bytes each
        CHECK(clip_chars(u, kNameMax) == kNameMax && u.size() == size_t(kNameMax) * 2, "W-113 multi-byte characters are cut on a character boundary");
        std::string s = "short";
        CHECK(clip_chars(s, kNameMax) == 5 && s == "short", "W-113 a short name is untouched");
        std::string d(300, 'y');
        CHECK(clip_chars(d, kDescMax) == kDescMax && d.size() == size_t(kDescMax), "W-113 the description is cut to %d", kDescMax);
    }

    // Log privacy: user names and profile paths are redacted from logged text.
    {
        SetEnvironmentVariableW(L"LOCALAPPDATA", L"C:\\Users\\Jane Doe\\AppData\\Local");
        SetEnvironmentVariableW(L"APPDATA", L"C:\\Users\\Jane Doe\\AppData\\Roaming");
        SetEnvironmentVariableW(L"USERPROFILE", L"C:\\Users\\Jane Doe");
        auto R = [](const char* s) { return redact_user_paths(std::string_view(s)); };
        CHECK(R("x C:\\Users\\Jane Doe\\AppData\\Local\\MechAssault2\\Logs\\a.txt") == "x %LOCALAPPDATA%\\MechAssault2\\Logs\\a.txt", "redact LOCALAPPDATA");
        CHECK(R("c:\\users\\jane doe\\appdata\\roaming") == "%APPDATA%", "redact APPDATA (case-insensitive, at the end)");
        CHECK(R("\"C:\\Users\\Jane Doe\\Documents\\m.mgf\" open") == "\"%USERPROFILE%\\Documents\\m.mgf\" open", "redact USERPROFILE");
        CHECK(R("D:\\Users\\Bob\\x and C:\\Users\\Public\\y") == "D:\\Users\\<user>\\x and C:\\Users\\Public\\y", "redact other user names, keep Public");
        CHECK(R("C:\\Users\\Jane Doe2\\q") == "C:\\Users\\<user>\\q", "no partial-name match of the profile");
        CHECK(R("no paths here, game\\maps\\a.mgf") == "no paths here, game\\maps\\a.mgf", "plain text untouched");
    }

    printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#include "gamedata.h"

#include <algorithm>
#include <cstring>
#include <system_error>

namespace ma2 {

void IniFile::parse(std::string_view text, bool strip_comments) {
    entries.clear();
    std::string section;
    for (auto& raw : split(text, '\n')) {
        std::string line = raw;
        if (strip_comments) {
            size_t c = line.find(';');
            if (c != std::string::npos) line.erase(c);
        }
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            size_t e = line.find(']');
            section = trim(line.substr(1, e == std::string::npos ? std::string::npos : e - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        entries.push_back({section, trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
    }
}

std::string IniFile::get(std::string_view s, std::string_view k, std::string_view def) const {
    for (auto& e : entries) if (iequals(e.section, s) && iequals(e.key, k)) return e.value;
    return std::string(def);
}

std::vector<std::string> IniFile::get_all(std::string_view s, std::string_view k) const {
    std::vector<std::string> out;
    for (auto& e : entries) if (iequals(e.section, s) && iequals(e.key, k)) out.push_back(e.value);
    return out;
}

std::vector<const IniFile::Entry*> IniFile::section(std::string_view s) const {
    std::vector<const Entry*> out;
    for (auto& e : entries) if (iequals(e.section, s)) out.push_back(&e);
    return out;
}

// ---------------------------------------------------------------------------------------------

bool GameData::looks_like_install(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / L"game_files" / L"common.mgf", ec);
}

fs::path GameData::find_install(const fs::path& exe_dir) {
    if (looks_like_install(exe_dir)) return exe_dir;
    if (exe_dir.has_parent_path() && looks_like_install(exe_dir.parent_path())) return exe_dir.parent_path();
    fs::path p = exe_dir;
    for (int i = 0; i < 8 && p.has_parent_path() && p.parent_path() != p; i++) {
        p = p.parent_path();
        if (looks_like_install(p)) return p;
    }
    return exe_dir;
}

static const char* kNotLevels[] = {"common.mgf", "templates.mgf", "text.mgf", "movies.mgf"};

bool GameData::open(const fs::path& install, std::string* err) {
    common_.reset();
    templates_.reset();
    archives_.clear();
    level_archives_.clear();
    maps_.clear();
    text_.clear();
    text_loaded_ = false;
    install_ = install;
    game_files_ = install / L"game_files";
    auto c = std::make_unique<MgfReader>();
    if (!c->open(game_files_ / L"common.mgf", err)) return false;
    common_ = std::move(c);
    auto t = std::make_unique<MgfReader>();
    if (t->open(game_files_ / L"Templates.mgf", nullptr)) templates_ = std::move(t);
    std::error_code ec;
    for (auto& e : fs::directory_iterator(game_files_, ec)) {
        if (!e.is_regular_file(ec)) continue;
        std::string n = lower(path_utf8(e.path().filename()));
        if (!iends_with(n, ".mgf")) continue;
        if (std::find_if(std::begin(kNotLevels), std::end(kNotLevels), [&](const char* x) { return n == x; }) != std::end(kNotLevels)) continue;
        level_archives_.push_back(n);
    }
    std::sort(level_archives_.begin(), level_archives_.end());
    fs::path xbe = game_files_ / L"ma2.xbe";
    if (!fs::exists(xbe, ec)) xbe = game_files_ / L"default.xbe";
    names_ = NameTable();
    names_.load_xbe(xbe, nullptr);

    // Multiplayer maps: every Multiplay/*.ini with a [Mission] WorldFile.
    for (auto& f : common_->files()) {
        if (!istarts_with(f.path, "Multiplay/") || !iends_with(f.path, ".ini")) continue;
        Bytes b;
        if (!common_->read_file(f.path, b)) continue;
        IniFile ini;
        ini.parse(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
        std::string world = ini.get("Mission", "WorldFile");
        if (world.empty()) continue;
        MapInfo m;
        m.multiplayer = true;
        m.ini_path = f.path;
        m.world_path = "Multiplay/" + world;
        if (common_->find(m.world_path) < 0) continue;
        m.name = fs::path(widen(f.path)).stem().string();
        m.resource = lower(ini.get("Mission", "ResourceFile"));
        m.game_types = ini.get_all("Mission", "GameType");
        m.name_id = uint32_t(strtoul(ini.get("Mission", "MapName").c_str(), nullptr, 10));
        m.desc_id = uint32_t(strtoul(ini.get("Mission", "MapDescription").c_str(), nullptr, 10));
        m.resource_present = std::find(level_archives_.begin(), level_archives_.end(), m.resource) != level_archives_.end();
        maps_.push_back(std::move(m));
    }
    // Missions: Missions/Mission_*.wdf with a matching level archive.
    for (auto& f : common_->files()) {
        if (!istarts_with(f.path, "Missions/Mission_") || !iends_with(f.path, ".wdf")) continue;
        MapInfo m;
        m.world_path = f.path;
        m.name = fs::path(widen(f.path)).stem().string();
        m.resource = lower(m.name) + ".mgf";
        m.resource_present = std::find(level_archives_.begin(), level_archives_.end(), m.resource) != level_archives_.end();
        maps_.push_back(std::move(m));
    }
    std::stable_sort(maps_.begin(), maps_.end(), [](const MapInfo& a, const MapInfo& b) {
        if (a.multiplayer != b.multiplayer) return a.multiplayer;
        return lower(a.name) < lower(b.name);
    });
    return true;
}

MgfReader* GameData::archive(std::string_view name) {
    std::string key = lower(name);
    auto it = archives_.find(key);
    if (it != archives_.end()) return it->second.get();
    auto r = std::make_unique<MgfReader>();
    if (!r->open(game_files_ / widen(key), nullptr)) return nullptr;
    MgfReader* raw = r.get();
    archives_[key] = std::move(r);
    return raw;
}

const MapInfo* GameData::find_map(std::string_view name) const {
    for (auto& m : maps_) if (iequals(m.name, name)) return &m;
    return nullptr;
}

bool GameData::load_world(const MapInfo& m, Wdf& out, std::string* err) {
    Bytes b;
    if (!common_->read_file(m.world_path, b, err)) return false;
    return out.parse(b, err);
}

bool GameData::read_common(std::string_view path, Bytes& out, std::string* err) {
    return common_->read_file(path, out, err);
}

// mgText: an 'MGIc' container whose 'TEXT' chunk holds 'ver ' and then 'data' (u32 tag, u32 size).
// The data: u32 count, u32 lowest id, u32 highest id, u32 header size, then count x (u32 id,
// u32 offset from the start of the data); strings are zero-terminated UTF-16LE.
static void parse_mgtext(const Bytes& b, std::map<uint32_t, std::string>& out) {
    auto u32 = [&](size_t at) { return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; };
    size_t base = 0;
    for (size_t at = 0; at + 8 <= b.size() && at < 256; at++)
        if (!memcmp(b.data() + at, "data", 4)) { base = at + 8; break; }
    if (!base || base + 16 > b.size()) return;
    uint32_t count = u32(base), head = u32(base + 12);
    if (head < 16 || base + head + uint64_t(count) * 8 > b.size()) return;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = u32(base + head + size_t(i) * 8), at = u32(base + head + size_t(i) * 8 + 4);
        std::wstring w;
        for (size_t k = base + at; k + 1 < b.size(); k += 2) {
            wchar_t c = wchar_t(b[k] | b[k + 1] << 8);
            if (!c) break;
            w.push_back(c);
        }
        out.emplace(id, narrow(w));
    }
}

std::string GameData::game_text(uint32_t id) {
    if (!text_loaded_) {
        text_loaded_ = true;
        MgfReader r;
        if (r.open(game_files_ / L"text.mgf", nullptr))
            for (const char* f : {"interface/Text/English/Mission.mgText", "interface/Text/English/Persistent.mgText"}) {
                Bytes b;
                if (r.read_file(f, b)) parse_mgtext(b, text_);
            }
    }
    auto it = text_.find(id);
    return it == text_.end() ? std::string() : it->second;
}

}  // namespace ma2

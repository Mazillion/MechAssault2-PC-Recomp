// Access to the user's own retail data (game_files) at run time. Nothing here is ever written.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mgf.h"
#include "names.h"
#include "wdf.h"

namespace ma2 {

struct MapInfo {
    std::string name;          // "BoggedDown", "Mission_02"
    bool multiplayer = false;
    std::string ini_path;      // "Multiplay/BoggedDown.ini" (MP only)
    std::string world_path;    // "Multiplay/BoggedDown.wdf" in common.mgf
    std::string resource;      // "boggeddown.mgf" (lower case)
    std::vector<std::string> game_types;
    uint32_t name_id = 0, desc_id = 0;
    bool resource_present = false;  // DLC maps have no resource archive on the disc
};

class GameData {
public:
    // install = folder holding "MechAssault 2.exe" and/or game_files.
    bool open(const fs::path& install, std::string* err = nullptr);
    bool is_open() const { return common_ != nullptr; }
    const fs::path& install() const { return install_; }
    const fs::path& game_files() const { return game_files_; }

    MgfReader& common() { return *common_; }
    MgfReader* templates_archive() { return templates_.get(); }
    // Level/resource archive by file name ("boggeddown.mgf"); opened lazily and cached.
    MgfReader* archive(std::string_view name);
    // Every *.mgf in game_files that is a level resource archive (lower-case file names).
    const std::vector<std::string>& level_archives() const { return level_archives_; }

    const NameTable& names() const { return names_; }
    const std::vector<MapInfo>& maps() const { return maps_; }
    const MapInfo* find_map(std::string_view name) const;

    bool load_world(const MapInfo& m, Wdf& out, std::string* err = nullptr);
    bool read_common(std::string_view path, Bytes& out, std::string* err = nullptr);
    // The game's own words for a text id (mission titles, prompts, objective lines), from text.mgf
    // interface/Text/English/{Mission,Persistent}.mgText; "" when the id is not there.
    std::string game_text(uint32_t id);

    // Search order for a default install: the exe folder, its parent, then ancestors that hold
    // game_files/common.mgf (developer tree).
    static fs::path find_install(const fs::path& exe_dir);
    static bool looks_like_install(const fs::path& dir);

private:
    fs::path install_, game_files_;
    std::unique_ptr<MgfReader> common_, templates_;
    std::map<std::string, std::unique_ptr<MgfReader>> archives_;
    std::vector<std::string> level_archives_;
    std::vector<MapInfo> maps_;
    NameTable names_;
    bool text_loaded_ = false;
    std::map<uint32_t, std::string> text_;
};

// Minimal INI reader that keeps repeated keys ("GameType = CTF" twice) and strips ';' comments.
struct IniFile {
    struct Entry { std::string section, key, value; };
    std::vector<Entry> entries;
    void parse(std::string_view text, bool strip_comments = true);
    std::string get(std::string_view section, std::string_view key, std::string_view def = {}) const;
    std::vector<std::string> get_all(std::string_view section, std::string_view key) const;
    std::vector<const Entry*> section(std::string_view section) const;
};

}  // namespace ma2

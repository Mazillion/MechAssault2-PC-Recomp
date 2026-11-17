#pragma once
// Palette search ranking. Pure (no ImGui), so the format tests check it like tool_hints.h.
//
// Every typed word must match; an entry's tier is the worst of its words (lower = better):
//   0  the word starts a word of the SHOWN name or its kind label ("fence" -> "Security fence", "mech" -> "'Mech")
//   1  it sits inside a word of the shown name and is 5+ letters ("light" -> "Streetlight"); shorter words never match inside
//      a word, so "tree" does not find "Streetlight" or "street"
//   2  it starts a word of the OFFICIAL name or model file name (and their camelCase parts)
//   3  it starts a word of the aliases (synonyms, class words, group and category names)
//   4  5+ letters and inside the official name glued ("parkinglot" -> "ParkingLot_01")
//  -1  none of these: the entry is not listed
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ed {

struct RankText {
    std::string visible;   // the shown name + its kind label ("'Mech", "Building", "road piece")
    std::string official;  // the official name and the model stem
    std::string aliases;   // synonyms, class words, group and category names
};

// Words of a text: lower case; split on anything that is not a letter or digit (so "'" "_" "-" and spaces split), and also
// the camelCase / letter-digit parts. The unsplit words are kept too: "ParkingLot_01" -> parkinglot, parking, lot, 01,
// so "mk2" finds "Mk2" and "lot" finds "ParkingLot".
inline std::vector<std::string> rank_words(const std::string& s) {
    std::vector<std::string> out;
    std::string raw, cur;
    auto flush_raw = [&] { if (!raw.empty()) out.push_back(raw); raw.clear(); };
    auto flush = [&] { if (!cur.empty()) out.push_back(cur); cur.clear(); };
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (!std::isalnum(c)) { flush_raw(); flush(); continue; }
        const bool up = std::isupper(c) != 0, dig = std::isdigit(c) != 0;
        if (!cur.empty()) {
            const unsigned char p = (unsigned char)cur.back();
            const bool pdig = std::isdigit(p) != 0;
            const bool prev_lower = std::islower((unsigned char)s[i - 1]) != 0;
            const bool prev_upper = std::isupper((unsigned char)s[i - 1]) != 0;
            const bool next_lower = i + 1 < s.size() && std::islower((unsigned char)s[i + 1]) != 0;
            if (dig != pdig || (up && prev_lower) || (up && prev_upper && next_lower)) flush();  // "APCLaser" -> apc, laser
        }
        raw += char(std::tolower(c));
        cur += char(std::tolower(c));
    }
    flush_raw();
    flush();
    return out;
}

// The words of a query: lower case, split on anything that is not a letter or digit.
inline std::vector<std::string> query_words(const std::string& q) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : q) {
        if (std::isalnum(c)) cur += char(std::tolower(c));
        else { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

inline bool rank_starts_word(const std::vector<std::string>& words, const std::string& w) {
    for (auto& t : words) if (t.size() >= w.size() && t.compare(0, w.size(), w) == 0) return true;
    return false;
}
inline bool rank_inside_word(const std::vector<std::string>& words, const std::string& w) {
    for (auto& t : words) if (t.find(w) != std::string::npos) return true;
    return false;
}

// Lower-case letters and digits only, no separators: "Env_ParkingLot 01" -> "envparkinglot01".
inline std::string rank_glue(const std::string& s) {
    std::string g;
    for (unsigned char c : s) if (std::isalnum(c)) g += char(std::tolower(c));
    return g;
}

// A pre-split RankText, so a search over hundreds of entries does not split the same strings on every keystroke.
struct RankWords {
    std::vector<std::string> visible, official, aliases;
    std::string glued;  // the official name glued, and the model stem glued, separated by a space
};
inline RankWords rank_prepare(const RankText& t) {
    RankWords r;
    r.visible = rank_words(t.visible);
    r.official = rank_words(t.official);
    r.aliases = rank_words(t.aliases);
    // Each whitespace-separated official part ("Name stem") glued on its own, so a hit never straddles two parts.
    std::string part;
    auto flush = [&] { if (!part.empty()) { r.glued += rank_glue(part) + " "; part.clear(); } };
    for (char c : t.official) { if (c == '\n') flush(); else part += c; }
    flush();
    return r;
}

// The tier of one query word against the entry; -1 = no match.
inline int rank_word_tier(const std::string& w, const RankWords& r) {
    if (rank_starts_word(r.visible, w)) return 0;
    if (w.size() >= 5 && rank_inside_word(r.visible, w)) return 1;
    if (rank_starts_word(r.official, w)) return 2;
    if (rank_starts_word(r.aliases, w)) return 3;
    if (w.size() >= 5 && r.glued.find(w) != std::string::npos) return 4;
    return -1;
}

// An empty query ranks everything 0. Official-name parts are separated by '\n' in RankText::official for the glued test.
inline int palette_rank(const std::string& query, const RankWords& r) {
    int tier = 0;
    for (const std::string& w : query_words(query)) {
        const int t = rank_word_tier(w, r);
        if (t < 0) return -1;
        tier = std::max(tier, t);
    }
    return tier;
}
inline int palette_rank(const std::string& query, const RankText& t) { return palette_rank(query, rank_prepare(t)); }

}  // namespace ed

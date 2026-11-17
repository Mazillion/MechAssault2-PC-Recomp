#pragma once
// Packs the palette's entries into display rows: a heading owns a row, a row of cells holds at most `cols` entries and
// never runs over a heading. Pure (no ImGui), so the format tests check it. The browser draws the rows through ImGuiListClipper.
#include <algorithm>
#include <utility>
#include <vector>

namespace ed {

struct PackRow {
    int heading = -1;    // >= 0: a heading row (the heading's id); no cells
    int first = 0;       // cells: entries [first, first + count)
    int count = 0;
};

// `headings`: (index of the entry the heading sits before, heading id), in any order. A heading before an index past the last entry
// is dropped (a heading with nothing under it says nothing).
inline std::vector<PackRow> pack_rows(int n_entries, std::vector<std::pair<int, int>> headings, int cols) {
    std::vector<PackRow> rows;
    cols = std::max(1, cols);
    std::stable_sort(headings.begin(), headings.end(), [](auto& a, auto& b) { return a.first < b.first; });
    size_t h = 0;
    int i = 0;
    while (i < n_entries) {
        while (h < headings.size() && headings[h].first <= i) {
            PackRow r;
            r.heading = headings[h].second;
            rows.push_back(r);
            h++;
        }
        const int next_heading = h < headings.size() ? headings[h].first : n_entries;
        PackRow r;
        r.first = i;
        r.count = std::min({cols, next_heading - i, n_entries - i});
        rows.push_back(r);
        i += r.count;
    }
    return rows;
}

}  // namespace ed

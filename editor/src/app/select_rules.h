// Selecting with the mouse: what a click, a box, a double-click and a list row do to the selection set. No ImGui and no App
// dependency, so tests\fmt_test.cpp can check the rules.
#pragma once
#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

namespace ed {

struct SelMods {
    bool shift = false, ctrl = false;
};

// A click that did not become a drag. uid 0 = empty ground. Plain selects only that object (empty ground clears); Shift adds (a
// second Shift+click keeps it); Ctrl toggles. Both held: Ctrl wins.
inline void sel_click(std::set<uint32_t>& sel, uint32_t uid, SelMods m) {
    if (!uid) {
        if (!m.shift && !m.ctrl) sel.clear();
        return;
    }
    if (m.ctrl) {
        if (!sel.erase(uid)) sel.insert(uid);
    } else if (m.shift) {
        sel.insert(uid);
    } else {
        sel = {uid};
    }
}

// A box: plain replaces the selection with what is inside, Shift adds it, Ctrl takes it away.
inline void sel_box(std::set<uint32_t>& sel, const std::vector<uint32_t>& inside, SelMods m) {
    if (m.ctrl) {
        for (uint32_t u : inside) sel.erase(u);
    } else if (m.shift) {
        sel.insert(inside.begin(), inside.end());
    } else {
        sel = std::set<uint32_t>(inside.begin(), inside.end());
    }
}

// "The same type": the same template, or (an object with no template, a helper) the same class.
inline bool same_type(uint32_t cls_a, uint32_t tpl_a, uint32_t cls_b, uint32_t tpl_b) {
    return cls_a == cls_b && (!tpl_a || !tpl_b || tpl_a == tpl_b);
}

// A list: Shift+click selects the rows from the anchor to the clicked one (both ends included, whichever comes first). An anchor that is
// not in the list selects just the clicked row.
inline std::vector<uint32_t> list_range(const std::vector<uint32_t>& order, uint32_t anchor, uint32_t target) {
    auto a = std::find(order.begin(), order.end(), anchor), b = std::find(order.begin(), order.end(), target);
    if (b == order.end()) return {};
    if (a == order.end()) return {target};
    if (a > b) std::swap(a, b);
    return std::vector<uint32_t>(a, b + 1);
}

}  // namespace ed

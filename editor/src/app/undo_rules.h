// The undo rules that need no App: when a new action joins the step on top instead of pushing its own, and the history limits. Header-only so fmt_test checks them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>

namespace ed {

constexpr size_t kUndoSteps = 200;                    // history depth
constexpr uint64_t kUndoBudget = 1ull << 30;          // 1 GiB of snapshot memory (shared buffers counted once)
constexpr double kUndoMergeWindow = 1.5;              // seconds between two actions that still count as one step
constexpr double kUndoDropNoteEvery = 30.0;           // the "history is full" note at most this often

// Hash of a selection (order independent through std::set), so "the same selection" is one compare.
inline uint64_t undo_sel_hash(const std::set<uint32_t>& sel) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t u : sel) { h ^= u; h *= 1099511628211ull; }
    return h ^ (uint64_t(sel.size()) << 48);
}

// A new action tagged `tag` skips its own push (and so joins the step on top) when that step has the same tag, nothing was
// undone (redo empty), the selection is the same and the last merged action was under the window ago.
inline bool undo_should_merge(const std::string& prev_tag, uint64_t prev_sel_hash, double prev_t,
                              const std::string& tag, uint64_t sel_hash, double t, bool redo_empty) {
    return !tag.empty() && prev_tag == tag && redo_empty && prev_sel_hash == sel_hash && t >= prev_t && t - prev_t < kUndoMergeWindow;
}

}  // namespace ed

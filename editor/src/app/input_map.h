// The editor's one key table: every action, its ini name, the context it works in, its default keys, and the
// resolver that says which action a key press means. Header-only with no ImGui include, so tests\fmt_test.cpp can test it
// like tool_hints.h. Keys are ImGuiKey values written as plain ints (names below, checked against ImGui in app_input.cpp).
//
// Contexts, most specific first: Glue / Preview / Frame > Selection / Placing / Brush > Edit > Map > Global. A key resolves to the
// most specific active action that owns it; Settings > Keys only calls two actions a clash when they could be active at the
// same specificity at the same time, and shows a grey note when a more specific context overrides a key.
#pragma once
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace ed {

enum KeyMod { KM_CTRL = 1, KM_SHIFT = 2, KM_ALT = 4 };

struct KeyBind {
    int key = 0;   // ImGuiKey, 0 = unset
    int mods = 0;  // KeyMod bits
    bool operator==(const KeyBind& o) const { return key == o.key && mods == o.mods; }
};

// ---------------------------------------------------------------------------------------------------- key names
// ImGui's keyboard keys in its own order and with its own names (GetKeyName). Index + 512 = the ImGuiKey value.
namespace ik {
inline constexpr const char* const names[] = {
    "Tab", "LeftArrow", "RightArrow", "UpArrow", "DownArrow", "PageUp", "PageDown",
    "Home", "End", "Insert", "Delete", "Backspace", "Space", "Enter", "Escape",
    "LeftCtrl", "LeftShift", "LeftAlt", "LeftSuper", "RightCtrl", "RightShift", "RightAlt", "RightSuper", "Menu",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C", "D", "E", "F", "G", "H",
    "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
    "Apostrophe", "Comma", "Minus", "Period", "Slash", "Semicolon", "Equal", "LeftBracket",
    "Backslash", "RightBracket", "GraveAccent", "CapsLock", "ScrollLock", "NumLock", "PrintScreen",
    "Pause", "Keypad0", "Keypad1", "Keypad2", "Keypad3", "Keypad4", "Keypad5", "Keypad6",
    "Keypad7", "Keypad8", "Keypad9", "KeypadDecimal", "KeypadDivide", "KeypadMultiply",
    "KeypadSubtract", "KeypadAdd", "KeypadEnter", "KeypadEqual",
    "AppBack", "AppForward", "Oem102",
};
inline constexpr int kBegin = 512;
inline constexpr int kCount = int(sizeof names / sizeof names[0]);
inline constexpr int kEnd = kBegin + kCount;
// First and last key that are modifiers (LeftCtrl .. Menu): never bound on their own.
inline constexpr int kModFirst = kBegin + 15, kModLast = kBegin + 23;

constexpr int K(const char* n) {
    for (int i = 0; i < kCount; i++) {
        const char* a = names[i];
        const char* b = n;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) return kBegin + i;
    }
    return 0;
}
}  // namespace ik

inline const char* key_name(int key) { return key >= ik::kBegin && key < ik::kEnd ? ik::names[key - ik::kBegin] : ""; }
inline bool key_bindable(int key) { return key >= ik::kBegin && key < ik::kEnd && !(key >= ik::kModFirst && key <= ik::kModLast); }

inline bool ieq_(const std::string& a, const char* b) {
    size_t n = a.size();
    if (n != strlen(b)) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
inline bool ipre_(const std::string& s, const char* p) {
    size_t n = strlen(p);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[i]) != std::tolower((unsigned char)p[i])) return false;
    return true;
}

// The ini form ("Ctrl+Shift+S", "LeftBracket"): unchanged from earlier versions so saved profiles keep working.
inline std::string key_text(const KeyBind& k) {
    if (!k.key) return {};
    std::string t;
    if (k.mods & KM_CTRL) t += "Ctrl+";
    if (k.mods & KM_SHIFT) t += "Shift+";
    if (k.mods & KM_ALT) t += "Alt+";
    return t + key_name(k.key);
}

inline KeyBind parse_key(const std::string& text) {
    KeyBind k;
    size_t b = text.find_first_not_of(" \t\r\n"), e = text.find_last_not_of(" \t\r\n");
    std::string s = b == std::string::npos ? std::string() : text.substr(b, e - b + 1);
    for (;;) {
        if (ipre_(s, "Ctrl+")) { k.mods |= KM_CTRL; s = s.substr(5); }
        else if (ipre_(s, "Shift+")) { k.mods |= KM_SHIFT; s = s.substr(6); }
        else if (ipre_(s, "Alt+")) { k.mods |= KM_ALT; s = s.substr(4); }
        else break;
    }
    if (s.empty()) return {};
    for (int i = 0; i < ik::kCount; i++)
        if (ieq_(s, ik::names[i]) && key_bindable(ik::kBegin + i)) { k.key = ik::kBegin + i; return k; }
    return {};
}

// What the screen shows: "[", "Del", "Esc", "PgUp", "Up arrow", "Num +", "Ctrl+Shift+S", "?" for Shift+/.
inline std::string key_label_name(int key) {
    const char* n = key_name(key);
    static const struct { const char* from; const char* to; } map[] = {
        {"LeftArrow", "Left arrow"}, {"RightArrow", "Right arrow"}, {"UpArrow", "Up arrow"}, {"DownArrow", "Down arrow"},
        {"Escape", "Esc"}, {"Delete", "Del"}, {"Insert", "Ins"}, {"PageUp", "PgUp"}, {"PageDown", "PgDn"}, {"PrintScreen", "PrtSc"},
        {"LeftBracket", "["}, {"RightBracket", "]"}, {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"Minus", "-"}, {"Equal", "="},
        {"Semicolon", ";"}, {"Apostrophe", "'"}, {"Backslash", "\\"}, {"GraveAccent", "`"},
        {"KeypadAdd", "Num +"}, {"KeypadSubtract", "Num -"}, {"KeypadMultiply", "Num *"}, {"KeypadDivide", "Num /"},
        {"KeypadDecimal", "Num ."}, {"KeypadEnter", "Num Enter"}, {"KeypadEqual", "Num ="},
    };
    for (auto& m : map)
        if (strcmp(n, m.from) == 0) return m.to;
    if (strncmp(n, "Keypad", 6) == 0 && n[6] >= '0' && n[6] <= '9' && !n[7]) return std::string("Num ") + n[6];
    return n;
}

inline std::string key_label(const KeyBind& k) {
    if (!k.key) return {};
    if (k.key == ik::K("Slash") && k.mods == KM_SHIFT) return "?";
    std::string t;
    if (k.mods & KM_CTRL) t += "Ctrl+";
    if (k.mods & KM_SHIFT) t += "Shift+";
    if (k.mods & KM_ALT) t += "Alt+";
    return t + key_label_name(k.key);
}

// ---------------------------------------------------------------------------------------------------- the table
enum Act : int {
    A_NONE = -1,
    A_PAN_FWD = 0, A_PAN_BACK, A_PAN_LEFT, A_PAN_RIGHT, A_TOP_DOWN, A_RESET_VIEW, A_HELPERS, A_ZOOM_SEL,
    A_CANCEL, A_DELETE, A_TURN_LEFT, A_TURN_RIGHT, A_SNAP,
    A_UNDO, A_REDO, A_COPY, A_CUT, A_PASTE, A_DUPLICATE, A_SELECT_ALL, A_PLACE_AGAIN, A_PICK_TYPE,
    A_NEW, A_OPEN, A_SAVE, A_SAVE_AS, A_TEST, A_HELP, A_PREFS,
    A_BRUSH_SMALLER, A_BRUSH_BIGGER,
    // Additions
    A_ZOOM_IN, A_ZOOM_OUT, A_VIEW_LEFT, A_VIEW_RIGHT, A_VIEW_UP, A_VIEW_DOWN,
    A_NUDGE_FWD, A_NUDGE_BACK, A_NUDGE_LEFT, A_NUDGE_RIGHT, A_RAISE, A_LOWER,
    A_GIZMO_MOVE, A_GIZMO_ROTATE, A_GIZMO_TILT, A_GIZMO_CYCLE,
    A_INVERT_SEL, A_SELECT_SAME, A_BRUSH_WEAKER, A_BRUSH_STRONGER,
    A_BRING_FORWARD, A_SEND_BACK,
    A_FIND, A_TAB_NEXT, A_TAB_PREV, A_TAB_1, A_TAB_2, A_TAB_3, A_TAB_4, A_TAB_5, A_SHORTCUTS,
    A_SLOPE_MODE,  // 
    A_GRID_TOGGLE, A_GRID_SMALLER, A_GRID_BIGGER, A_SNAP_JOINTS, A_ANGLE_SNAP,  // 
    A_PAGE_SELECT, A_PAGE_PLACE, A_PAGE_SCATTER, A_PAGE_ROADS, A_PAGE_SCULPT, A_PAGE_PAINT, A_PAGE_WATER, A_PAGE_STARTS,  // The tool strip
    A_FRAME_USE,  // Use the framed view for the shot
    A_NEW_MISSION, A_CHECK,  // Commands without a key still have one name, here
    A_COUNT,
    // Derived, not in the table: the Turn keys with the quarter-turn hold key added.
    A_TURN_QUARTER_L = A_COUNT, A_TURN_QUARTER_R,
};

enum Ctx : int {
    C_GLOBAL = 1,     // file and window keys: work with or without a map
    C_MAP = 2,        // a map is open
    C_EDIT = 4,       // an editable map
    C_SELECTION = 8,  // Select tool, editable, something selected
    C_PLACING = 16,   // a placing tool (and the paste ghost)
    C_BRUSH = 32,     // a brush tool
    C_GLUE = 64,      // glue mode
    C_PREVIEW = 128,  // cutscene preview
    C_FRAME = 256,    // framing a shot's camera
};
// Never active together: the three tool states.
inline constexpr int kToolCtx = C_SELECTION | C_PLACING | C_BRUSH;

enum class Kind { Press, PressRepeat, Held };

struct ActRow {
    Act id;
    const char* label;
    const char* section;
    const char* ini;
    int ctx;      // Ctx bits the action works in
    Kind kind;
    KeyBind def[2];
    bool shift_ok;  // Shift only changes the amount (fast pan, x10 nudge, '+' for '='): it does not change which action a key is
    bool pending;   // in the table, but no handler yet (a later change clears it): the resolver skips it
};

namespace ik {
inline constexpr int Tab = K("Tab"), Left = K("LeftArrow"), Right = K("RightArrow"), Up = K("UpArrow"), Down = K("DownArrow"),
                     PgUp = K("PageUp"), PgDn = K("PageDown"), Home = K("Home"), Delete = K("Delete"), Space = K("Space"),
                     Escape = K("Escape"), Enter = K("Enter"), Comma = K("Comma"), Minus = K("Minus"), Slash = K("Slash"), Equal = K("Equal"),
                     LBracket = K("LeftBracket"), RBracket = K("RightBracket"), KpAdd = K("KeypadAdd"), KpSub = K("KeypadSubtract"),
                     F1 = K("F1"), F5 = K("F5"), N1 = K("1"), N2 = K("2"), N3 = K("3"), N4 = K("4"), N5 = K("5"), N6 = K("6"), N7 = K("7"), N8 = K("8"),
                     A = K("A"), C = K("C"), D = K("D"), E = K("E"), F = K("F"), G = K("G"), H = K("H"), I = K("I"), L = K("L"), N = K("N"),
                     O = K("O"), Q = K("Q"), R = K("R"), S = K("S"), T = K("T"), V = K("V"), W = K("W"), X = K("X"), Y = K("Y"),
                     Z = K("Z");
}  // namespace ik

inline const ActRow kActTable[A_COUNT] = {
    // id, label, section, ini, ctx, kind, defaults[2], shift_ok, pending
    {A_PAN_FWD, "Pan forward", "Camera", "PanForward", C_MAP, Kind::Held, {{ik::W, 0}, {ik::Up, 0}}, true, false},
    {A_PAN_BACK, "Pan back", "Camera", "PanBack", C_MAP, Kind::Held, {{ik::S, 0}, {ik::Down, 0}}, true, false},
    {A_PAN_LEFT, "Pan left", "Camera", "PanLeft", C_MAP, Kind::Held, {{ik::A, 0}, {ik::Left, 0}}, true, false},
    {A_PAN_RIGHT, "Pan right", "Camera", "PanRight", C_MAP, Kind::Held, {{ik::D, 0}, {ik::Right, 0}}, true, false},
    {A_TOP_DOWN, "Top-down view", "Camera", "TopDown", C_MAP, Kind::Press, {{ik::T, 0}, {}}, false, false},
    {A_RESET_VIEW, "Reset the view", "Camera", "ResetView", C_MAP, Kind::Press, {{ik::Home, 0}, {}}, false, false},
    {A_HELPERS, "Show helpers", "Camera", "Helpers", C_MAP, Kind::Press, {{ik::H, 0}, {}}, false, false},
    {A_ZOOM_SEL, "Zoom to the selection", "Camera", "ZoomSelection", C_MAP, Kind::Press, {{ik::F, 0}, {}}, false, false},
    {A_CANCEL, "Stop the tool / clear the selection", "Editing", "Cancel", C_MAP, Kind::Press, {{ik::Escape, 0}, {}}, false, false},
    {A_DELETE, "Delete", "Editing", "Delete", C_EDIT, Kind::Press, {{ik::Delete, 0}, {}}, false, false},
    {A_TURN_LEFT, "Turn left", "Editing", "TurnLeft", C_EDIT, Kind::PressRepeat, {{ik::Q, 0}, {}}, false, false},
    {A_TURN_RIGHT, "Turn right", "Editing", "TurnRight", C_EDIT, Kind::PressRepeat, {{ik::E, 0}, {}}, false, false},
    {A_SNAP, "Put on the ground", "Editing", "Snap", C_SELECTION, Kind::Press, {{ik::G, 0}, {}}, false, false},
    {A_UNDO, "Undo", "Editing", "Undo", C_EDIT, Kind::Press, {{ik::Z, KM_CTRL}, {}}, false, false},
    {A_REDO, "Redo", "Editing", "Redo", C_EDIT, Kind::Press, {{ik::Y, KM_CTRL}, {ik::Z, KM_CTRL | KM_SHIFT}}, false, false},
    {A_COPY, "Copy", "Editing", "Copy", C_MAP, Kind::Press, {{ik::C, KM_CTRL}, {}}, false, false},
    {A_CUT, "Cut", "Editing", "Cut", C_EDIT, Kind::Press, {{ik::X, KM_CTRL}, {}}, false, false},
    {A_PASTE, "Paste", "Editing", "Paste", C_EDIT, Kind::Press, {{ik::V, KM_CTRL}, {}}, false, false},
    {A_DUPLICATE, "Duplicate", "Editing", "Duplicate", C_SELECTION, Kind::Press, {{ik::D, KM_CTRL}, {}}, false, false},
    {A_SELECT_ALL, "Select all", "Editing", "SelectAll", C_MAP, Kind::Press, {{ik::A, KM_CTRL}, {}}, false, false},
    {A_PLACE_AGAIN, "Place again", "Editing", "PlaceAgain", C_EDIT, Kind::Press, {{ik::Space, 0}, {}}, false, false},
    {A_PICK_TYPE, "Place more of this", "Editing", "PickType", C_SELECTION, Kind::Press, {{ik::I, 0}, {}}, false, false},
    {A_NEW, "New map", "Files and windows", "New", C_GLOBAL, Kind::Press, {{ik::N, KM_CTRL}, {}}, false, false},
    {A_OPEN, "Open a saved one", "Files and windows", "Open", C_GLOBAL, Kind::Press, {{ik::O, KM_CTRL}, {}}, false, false},
    {A_SAVE, "Save", "Files and windows", "Save", C_GLOBAL, Kind::Press, {{ik::S, KM_CTRL}, {}}, false, false},
    {A_SAVE_AS, "Save as", "Files and windows", "SaveAs", C_GLOBAL, Kind::Press, {{ik::S, KM_CTRL | KM_SHIFT}, {}}, false, false},
    {A_TEST, "Test in the game", "Files and windows", "Test", C_GLOBAL, Kind::Press, {{ik::F5, 0}, {}}, false, false},
    {A_HELP, "How to use the editor", "Files and windows", "Help", C_GLOBAL, Kind::Press, {{ik::F1, 0}, {}}, false, false},
    {A_PREFS, "Settings", "Files and windows", "Preferences", C_GLOBAL, Kind::Press, {{ik::Comma, KM_CTRL}, {}}, false, false},
    {A_BRUSH_SMALLER, "Smaller brush", "Brush", "BrushSmaller", C_BRUSH, Kind::PressRepeat, {{ik::LBracket, 0}, {}}, false, false},
    {A_BRUSH_BIGGER, "Bigger brush", "Brush", "BrushBigger", C_BRUSH, Kind::PressRepeat, {{ik::RBracket, 0}, {}}, false, false},
    // ----
    {A_ZOOM_IN, "Zoom in", "Camera", "ZoomIn", C_MAP, Kind::PressRepeat, {{ik::Equal, 0}, {ik::KpAdd, 0}}, true, false},
    {A_ZOOM_OUT, "Zoom out", "Camera", "ZoomOut", C_MAP, Kind::PressRepeat, {{ik::Minus, 0}, {ik::KpSub, 0}}, true, false},
    {A_VIEW_LEFT, "Turn the view left", "Camera", "ViewLeft", C_MAP, Kind::Held, {{ik::Left, KM_ALT}, {}}, false, false},
    {A_VIEW_RIGHT, "Turn the view right", "Camera", "ViewRight", C_MAP, Kind::Held, {{ik::Right, KM_ALT}, {}}, false, false},
    {A_VIEW_UP, "Tilt the view up", "Camera", "ViewUp", C_MAP, Kind::Held, {{ik::Up, KM_ALT}, {}}, false, false},
    {A_VIEW_DOWN, "Tilt the view down", "Camera", "ViewDown", C_MAP, Kind::Held, {{ik::Down, KM_ALT}, {}}, false, false},
    {A_NUDGE_FWD, "Nudge forward", "Selection", "NudgeForward", C_SELECTION, Kind::PressRepeat, {{ik::Up, 0}, {}}, true, false},
    {A_NUDGE_BACK, "Nudge back", "Selection", "NudgeBack", C_SELECTION, Kind::PressRepeat, {{ik::Down, 0}, {}}, true, false},
    {A_NUDGE_LEFT, "Nudge left", "Selection", "NudgeLeft", C_SELECTION, Kind::PressRepeat, {{ik::Left, 0}, {}}, true, false},
    {A_NUDGE_RIGHT, "Nudge right", "Selection", "NudgeRight", C_SELECTION, Kind::PressRepeat, {{ik::Right, 0}, {}}, true, false},
    {A_RAISE, "Raise", "Selection", "Raise", C_SELECTION | C_PLACING, Kind::PressRepeat, {{ik::PgUp, 0}, {}}, true, false},
    {A_LOWER, "Lower", "Selection", "Lower", C_SELECTION | C_PLACING, Kind::PressRepeat, {{ik::PgDn, 0}, {}}, true, false},
    {A_GIZMO_MOVE, "Move handles", "Selection", "HandlesMove", C_SELECTION, Kind::Press, {{ik::N1, 0}, {}}, false, false},
    {A_GIZMO_ROTATE, "Rotate handles", "Selection", "HandlesRotate", C_SELECTION, Kind::Press, {{ik::N2, 0}, {}}, false, false},
    {A_GIZMO_TILT, "Tilt handles", "Selection", "HandlesTilt", C_SELECTION, Kind::Press, {{ik::N3, 0}, {}}, false, false},
    {A_GIZMO_CYCLE, "Next handle mode", "Selection", "HandlesNext", C_SELECTION, Kind::Press, {{ik::R, 0}, {}}, false, false},
    {A_INVERT_SEL, "Invert the selection", "Selection", "InvertSelection", C_MAP, Kind::Press, {{ik::I, KM_CTRL}, {}}, false, false},
    {A_SELECT_SAME, "Select all of this type", "Selection", "SelectSame", C_SELECTION, Kind::Press, {{}, {}}, false, false},
    {A_BRUSH_WEAKER, "Weaker brush", "Brush", "BrushWeaker", C_BRUSH, Kind::PressRepeat, {{ik::LBracket, KM_SHIFT}, {}}, false, false},
    {A_BRUSH_STRONGER, "Stronger brush", "Brush", "BrushStronger", C_BRUSH, Kind::PressRepeat, {{ik::RBracket, KM_SHIFT}, {}}, false, false},
    {A_BRING_FORWARD, "Bring forward", "Selection", "BringForward", C_SELECTION, Kind::Press, {{ik::RBracket, KM_CTRL}, {}}, false, false},
    {A_SEND_BACK, "Send back", "Selection", "SendBack", C_SELECTION, Kind::Press, {{ik::LBracket, KM_CTRL}, {}}, false, false},
    {A_FIND, "Search the object list", "Windows and tabs", "Find", C_MAP, Kind::Press, {{ik::F, KM_CTRL}, {}}, false, false},
    {A_TAB_NEXT, "Next tab", "Windows and tabs", "TabNext", C_GLOBAL, Kind::Press, {{ik::Tab, KM_CTRL}, {}}, false, false},
    {A_TAB_PREV, "Previous tab", "Windows and tabs", "TabPrevious", C_GLOBAL, Kind::Press, {{ik::Tab, KM_CTRL | KM_SHIFT}, {}}, false, false},
    {A_TAB_1, "Tab 1 (Map)", "Windows and tabs", "Tab1", C_GLOBAL, Kind::Press, {{ik::N1, KM_CTRL}, {}}, false, false},
    {A_TAB_2, "Tab 2 (Build)", "Windows and tabs", "Tab2", C_GLOBAL, Kind::Press, {{ik::N2, KM_CTRL}, {}}, false, false},
    {A_TAB_3, "Tab 3 (Objectives & Triggers)", "Windows and tabs", "Tab3", C_GLOBAL, Kind::Press, {{ik::N3, KM_CTRL}, {}}, false, false},
    {A_TAB_4, "Tab 4 (Cameras & Cutscenes)", "Windows and tabs", "Tab4", C_GLOBAL, Kind::Press, {{ik::N4, KM_CTRL}, {}}, false, false},
    {A_TAB_5, "Tab 5 (Environment)", "Windows and tabs", "Tab5", C_GLOBAL, Kind::Press, {{ik::N5, KM_CTRL}, {}}, false, false},
    {A_SHORTCUTS, "Keyboard shortcuts", "Files and windows", "Shortcuts", C_GLOBAL, Kind::Press, {{ik::Slash, KM_SHIFT}, {}}, false, false},
    {A_SLOPE_MODE, "On a slope: level, tilt or float", "Editing", "SlopeMode", C_PLACING, Kind::Press, {{ik::L, 0}, {}}, false, false},
    // ---- The snap grid (the brush tools keep [ and ]: the three tool contexts never overlap)
    {A_GRID_TOGGLE, "Grid on / off", "Editing", "GridToggle", C_MAP, Kind::Press, {{ik::G, KM_CTRL}, {}}, false, false},
    {A_GRID_SMALLER, "Smaller grid (while placing or with a selection)", "Editing", "GridSmaller", C_PLACING | C_SELECTION, Kind::PressRepeat, {{ik::LBracket, 0}, {}}, false, false},
    {A_GRID_BIGGER, "Bigger grid (while placing or with a selection)", "Editing", "GridBigger", C_PLACING | C_SELECTION, Kind::PressRepeat, {{ik::RBracket, 0}, {}}, false, false},
    {A_SNAP_JOINTS, "Snap to joints on / off", "Editing", "SnapJoints", C_MAP, Kind::Press, {{}, {}}, false, false},
    {A_ANGLE_SNAP, "Angle steps on / off", "Editing", "AngleSnap", C_MAP, Kind::Press, {{}, {}}, false, false},
    // ---- The tool strip (Unreal's mode keys; the number row alone is the handle modes with a selection)
    {A_PAGE_SELECT, "Select tool", "Tools", "PageSelect", C_MAP, Kind::Press, {{ik::N1, KM_SHIFT}, {}}, false, false},
    {A_PAGE_PLACE, "Place tool", "Tools", "PagePlace", C_MAP, Kind::Press, {{ik::N2, KM_SHIFT}, {}}, false, false},
    {A_PAGE_SCATTER, "Scatter tool", "Tools", "PageScatter", C_MAP, Kind::Press, {{ik::N3, KM_SHIFT}, {}}, false, false},
    {A_PAGE_ROADS, "Roads & walls tool", "Tools", "PageRoads", C_MAP, Kind::Press, {{ik::N4, KM_SHIFT}, {}}, false, false},
    {A_PAGE_SCULPT, "Sculpt tool", "Tools", "PageSculpt", C_MAP, Kind::Press, {{ik::N5, KM_SHIFT}, {}}, false, false},
    {A_PAGE_PAINT, "Paint tool", "Tools", "PagePaint", C_MAP, Kind::Press, {{ik::N6, KM_SHIFT}, {}}, false, false},
    {A_PAGE_WATER, "Water tool", "Tools", "PageWater", C_MAP, Kind::Press, {{ik::N7, KM_SHIFT}, {}}, false, false},
    {A_PAGE_STARTS, "Starts tool", "Tools", "PageStarts", C_MAP, Kind::Press, {{ik::N8, KM_SHIFT}, {}}, false, false},
    // ---- Framing a cutscene shot (Esc leaves, through A_CANCEL)
    {A_FRAME_USE, "Use this view for the shot (while framing)", "Camera", "FrameUse", C_FRAME, Kind::Press, {{ik::Enter, 0}, {}}, false, false},
    // ---- No default key (the menus and buttons use these names)
    {A_NEW_MISSION, "New mission", "Files and windows", "NewMission", C_GLOBAL, Kind::Press, {{}, {}}, false, false},
    {A_CHECK, "Check scenario", "Files and windows", "Check", C_MAP, Kind::Press, {{}, {}}, false, false},
};

// Section order on the Keys page and the shortcut sheet.
inline const char* const kSections[] = {"Camera", "Editing", "Selection", "Tools", "Brush", "Windows and tabs", "Files and windows"};
inline constexpr int kSectionCount = int(sizeof kSections / sizeof kSections[0]);

inline const ActRow& act_row(int a) { return kActTable[a]; }

// The hold keys (Settings > Keys): one KeyMod bit each.
struct Hold {
    int free = KM_SHIFT, through = KM_ALT, quarter = KM_CTRL, reverse = KM_CTRL, orbit = KM_ALT;
};

// ---------------------------------------------------------------------------------------------------- the resolver
inline int ctx_rank(int bit) {
    switch (bit) {
        case C_GLUE: case C_PREVIEW: case C_FRAME: return 4;
        case C_SELECTION: case C_PLACING: case C_BRUSH: return 3;
        case C_EDIT: return 2;
        case C_MAP: return 1;
        default: return 0;
    }
}
// The most specific context of a row that is active, or -1.
inline int row_rank(const ActRow& r, int active) {
    int best = -1;
    for (int b = 1; b <= C_FRAME; b <<= 1)
        if ((r.ctx & b) && (active & b) && ctx_rank(b) > best) best = ctx_rank(b);
    return best;
}

using KeyTable = KeyBind[A_COUNT][2];

// Which action does pressing `key` with `mods` mean in the active contexts? A_NONE when none.
//  1. The Turn keys with the quarter-turn hold key added are the quarter turns.
//  2. An exact (key, mods) match among the active actions; the most specific context wins.
//  3. No exact match: the hold keys that are down are taken off (place freely while placing or selecting, reverse while
//     brushing, through) and the match is tried again; actions where Shift only means "more" also ignore Shift.
inline Act resolve(const KeyTable keys, int key, int mods, int active, const Hold& hold) {
    if (!key) return A_NONE;
    if (active & C_EDIT) {
        for (int t : {int(A_TURN_LEFT), int(A_TURN_RIGHT)})
            for (int k = 0; k < 2; k++) {
                const KeyBind& b = keys[t][k];
                if (b.key == key && !(b.mods & hold.quarter) && (b.mods | hold.quarter) == mods) return t == A_TURN_LEFT ? A_TURN_QUARTER_L : A_TURN_QUARTER_R;
            }
    }
    int strip = hold.through;
    if (active & (C_SELECTION | C_PLACING)) strip |= hold.free;
    if (active & C_BRUSH) strip |= hold.reverse;
    const int loose = mods & ~strip;
    for (int pass = 0; pass < 2; pass++) {
        Act best = A_NONE;
        int best_rank = -1;
        for (int a = 0; a < A_COUNT; a++) {
            const ActRow& r = kActTable[a];
            if (r.pending) continue;
            const int rank = row_rank(r, active);
            if (rank < 0 || rank <= best_rank) continue;
            for (int k = 0; k < 2; k++) {
                const KeyBind& b = keys[a][k];
                if (b.key != key) continue;
                bool hit;
                if (pass == 0) hit = b.mods == mods;
                else hit = b.mods == loose || (r.shift_ok && (b.mods == (loose & ~KM_SHIFT) || b.mods == (mods & ~KM_SHIFT)));
                if (hit) { best = Act(a); best_rank = rank; break; }
            }
        }
        if (best != A_NONE) return best;
    }
    return A_NONE;
}

// Can two bound actions be active together? (The three tool contexts exclude each other.)
inline bool ctx_pair_possible(int ca, int cb) { return !((ca & kToolCtx) && (cb & kToolCtx) && ca != cb); }

// Actions that clash with `act` when it is bound to `kb`: the same key on another action that can be active at the same
// specificity at the same time (act's other slot counts too).
inline std::vector<int> clashes(const KeyTable keys, int act, const KeyBind& kb, int slot = -1) {
    std::vector<int> out;
    if (!kb.key) return out;
    for (int b = 0; b < A_COUNT; b++) {
        if (kActTable[b].pending) continue;
        bool same = false;
        for (int j = 0; j < 2; j++)
            if (!(b == act && j == slot) && keys[b][j] == kb) same = true;
        if (!same) continue;
        if (b == act) { out.push_back(b); continue; }
        bool clash = false;
        for (int ca = 1; ca <= C_FRAME && !clash; ca <<= 1)
            for (int cb = 1; cb <= C_FRAME && !clash; cb <<= 1)
                if ((kActTable[act].ctx & ca) && (kActTable[b].ctx & cb) && ctx_pair_possible(ca, cb) && ctx_rank(ca) == ctx_rank(cb)) clash = true;
        if (clash) out.push_back(b);
    }
    return out;
}

// An action with the same key that works in a more specific context (so this binding is overridden there), or A_NONE.
inline Act shadowed_by(const KeyTable keys, int act, const KeyBind& kb) {
    if (!kb.key) return A_NONE;
    for (int b = 0; b < A_COUNT; b++) {
        if (b == act || kActTable[b].pending) continue;
        bool same = false;
        for (int j = 0; j < 2; j++)
            if (keys[b][j] == kb) same = true;
        if (!same) continue;
        for (int ca = 1; ca <= C_FRAME; ca <<= 1)
            for (int cb = 1; cb <= C_FRAME; cb <<= 1)
                if ((kActTable[act].ctx & ca) && (kActTable[b].ctx & cb) && ctx_pair_possible(ca, cb) && ctx_rank(cb) > ctx_rank(ca)) return Act(b);
    }
    return A_NONE;
}

// "with a selection" ...: when a context's action overrides a key.
inline const char* ctx_phrase(int ctx) {
    if (ctx & C_GLUE) return "while gluing";
    if (ctx & C_PREVIEW) return "in a cutscene preview";
    if (ctx & C_FRAME) return "while framing a shot";
    if (ctx & C_SELECTION) return "with a selection";
    if (ctx & C_PLACING) return "while placing";
    if (ctx & C_BRUSH) return "with a brush tool";
    if (ctx & C_EDIT) return "on an editable map";
    return "with a map open";
}

// Brush size step: one fixed ladder of whole metres, 40 x 1.12^k (..., 32, 36, 40, 45, 50, 56, 63, ...), the same for [ ], Ctrl+[ ] and
// Ctrl+wheel: a size set by the slider joins the ladder with its next step, so every way of stepping gives the same sizes.
// dir = +1 / -1; beyond the ladder's ends it moves 1 m.
inline float brush_step(float r, int dir) {
    const float cur = std::round(r);
    float below = -1e9f;
    for (int k = -60; k <= 60; k++) {
        const float n = std::round(40.f * std::pow(1.12f, float(k)));
        if (dir > 0 && n > cur) return n;
        if (n < cur) below = std::max(below, n);
        else if (dir < 0) break;
    }
    return dir > 0 ? cur + 1.f : below > -1e8f ? below : cur - 1.f;
}

// ---------------------------------------------------------------------------------------------------- mouse sheet
struct MouseRow {
    std::string gesture, does;
};
// The mouse grammar (not rebindable), with the user's hold keys in the text. / / add their gestures here.
inline std::vector<MouseRow> mouse_rows(const char* through, const char* free_, const char* quarter_mod, const char* reverse,
                                        const std::string& turn_keys, const std::string& add_mod, const char* orbit = "Alt") {
    std::vector<MouseRow> r;
    r.push_back({"Click", "Select an object (or place, with a tool picked)"});
    r.push_back({"Drag on empty ground, or on an object that is not selected", "Select everything in the box (the boxed objects replace the selection)"});
    r.push_back({"Shift+click, Shift+box", "Add to the selection"});
    r.push_back({"Ctrl+click, Ctrl+box", "Toggle one object / take the boxed ones out of the selection"});
    r.push_back({"Double-click an object", "Select all of the same type on screen (Ctrl+double-click: the whole map)"});
    r.push_back({"Right-click an object", "Menu: delete, duplicate, copy, bring forward / send back, put on the ground, select all of this type, properties"});
    r.push_back({"Paste or Duplicate", "The copy hangs on the mouse: click the map to put it down (the turn keys turn it, the stop key drops it)"});
    r.push_back({std::string(through) + "+click", "Next object under the mouse (cycles through overlapping ones); while placing, on top of the object under the cursor (a roof, a deck, a rock)"});
    r.push_back({std::string("Hold ") + free_ + " while placing", "Place freely: no grid, no joining roads, walls or bridges, no roofs"});
    r.push_back({std::string(quarter_mod) + "+" + turn_keys, "Turn 90 degrees (while dragging to aim: 15-degree steps)"});
    r.push_back({std::string("Hold ") + reverse + " while shaping", "Terrain: the other half of the pair (Lower for Raise, Roughen for Smooth...); Flatten: a click picks the height"});
    r.push_back({"Drag the object or a handle", "Move / turn / tilt the selection"});
    r.push_back({"Drag an orange handle (a selected road, base, wall or bridge)", "Cut the piece to size: the side follows the mouse (double-click a handle: that side whole again)"});
    r.push_back({"Right-drag", "Pan the view (a right click stops the tool)"});
    r.push_back({"Middle-drag", "Turn the view"});
    r.push_back({std::string(orbit) + "+drag", "Turn the view in any tool (a click without dragging stays a click)"});
    r.push_back({"Mouse wheel", "Zoom"});
    r.push_back({"Ctrl+wheel with a brush", "Brush size (12 % a notch)"});
    r.push_back({"Shift+wheel with a brush", "Brush strength (5 points a notch)"});
    return r;
}

}  // namespace ed

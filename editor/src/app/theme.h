// The editor's one look: MechAssault 2 front-end colours, shapes and fonts.
// Every panel reads colours from here; nothing else sets its own colours, fonts or frame styles.
#pragma once
#include "imgui.h"

namespace theme {

// Colours that are not ImGui style slots (style-guide.md section 6 tokens).
struct Tokens {
    ImVec4 accent, accent_text, accent_hover, accent_active;  // menu yellow: the one UI accent (primary buttons, current value)
    ImVec4 text_bright, text_item, text_hint, text_value;      // headings / clickable items / key names / current value
    ImVec4 warn, error, ok;                                     // status only
    ImVec4 side_red, side_blue, side_ally, side_neutral;        // game state, never decoration
    ImVec4 bracket, info_card, glow_halo1, select_fill;
};
const Tokens& tok();

// Applies the palette, rounding and padding to `s` (call before ScaleAllSizes).
void apply(ImGuiStyle& s);

// Fonts: body (Segoe UI) and heading (Impact, Segoe UI Bold when missing). Call once after the context exists.
void load_fonts(ImGuiIO& io, const wchar_t* windir);
ImFont* heading_font();
ImFont* tab_font();  // the tab strip's labels (Segoe UI Bold; falls back to the heading face)
// ImGui::Begin / End with the title bar in the heading face (the window's contents stay in the body face).
// Always pair begin_window with end_window, also when it returns false.
bool begin_window(const char* name, bool* open = nullptr, int flags = 0);
void end_window();
// ImGui::BeginPopupModal with the heading-face title and the corner brackets; end_modal only when it returned true.
bool begin_modal(const char* name, bool* open = nullptr, int flags = 0);
void end_modal();

// A primary action button (yellow fill, dark text): Test, Create, Make an editable copy.
bool primary_button(const char* label, const ImVec2& size = ImVec2(0, 0));
// The "on" look for any on/off or pick-one button (chosen tool, current tab, active mode): select fill, bright label,
// thin halo frame. toggle_button draws a plain one; push/pop_toggle_on wrap image buttons or buttons with their own font.
bool toggle_button(const char* label, bool on, const ImVec2& size = ImVec2(0, 0));
// The same on a small (row) button: map-pick modes (Move on map, Draw flight path) stay lit while they wait for a click.
bool small_toggle_button(const char* label, bool on);
void push_toggle_on();
void pop_toggle_on();
// A Delete / Remove button: plain fill, error-red label, dark red on hover (style-guide section 5).
bool danger_button(const char* label, const ImVec2& size = ImVec2(0, 0));
// danger_button at SmallButton size (row actions).
bool danger_small_button(const char* label);
// The corner brackets on the current window's outer corners (modals and the start page only).
void bracket_window();
// A fold-out section header (ImGui::CollapsingHeader) in the plain button fill, so a closed section never wears
// the selected-row fill that chosen list rows and toggles use.
bool section(const char* label, int flags = 0);
// A slider whose value is written as text in a fixed column to the right of the bar (never inside the bar,
// where a narrow bar cut it off). The width is the pending SetNextItemWidth / the default item width, shared by the bar and the
// column. `fmt` is the display and typing format as for ImGui::SliderFloat; Ctrl+click and double-click still type. The last item
// stays the slider (hover, IsItemDeactivatedAfterEdit and tooltips read the bar).
bool slider_float(const char* id, float* v, float lo, float hi, const char* fmt = "%.2f", ImGuiSliderFlags flags = 0);
bool slider_int(const char* id, int* v, int lo, int hi, const char* fmt = "%d", ImGuiSliderFlags flags = 0);
// A heading in the condensed face with the game's dark drop shadow.
void heading(const char* text);
// The game's white L-shaped corner brackets around a rectangle (modals, start page cards only).
void bracket_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float scale);

}  // namespace theme

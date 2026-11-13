// MechAssault 2 front-end look for the editor (ux/style-guide.md: hex values sampled from the game's own menus,
// ux/style-shots). The game's language: dark navy-steel panels, cyan menu items with a white current item,
// menu yellow for the current value / primary action, condensed caps headings with a dark shadow.
#include "theme.h"

#include <windows.h>
#include <algorithm>
#include <string>

namespace theme {

static ImVec4 hex(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

static ImFont* g_heading = nullptr;

const Tokens& tok() {
    static const Tokens t = {
        hex(0xCECE48), hex(0x0F1A26), hex(0xDCDC6A), hex(0xB4B43A),
        hex(0xE4E4E4), hex(0x62AECE), hex(0x6A9CBC), hex(0xCECE48),  // text_hint lifted to 4.9:1
        hex(0xF4AE2F), hex(0xE0453A), hex(0x8FD49A),
        hex(0xB6453D), hex(0x086DB5), hex(0x5DAC86), hex(0x9CA2B1),
        hex(0xFCFBF8), hex(0x18293A), hex(0x8CC5DC), hex(0x326378),
    };
    return t;
}

void apply(ImGuiStyle& s) {
    s.WindowRounding = 0; s.ChildRounding = 0; s.FrameRounding = 2; s.PopupRounding = 2;
    s.GrabRounding = 2; s.TabRounding = 2; s.ScrollbarRounding = 2;
    s.WindowBorderSize = 1; s.FrameBorderSize = 0; s.PopupBorderSize = 1; s.TabBorderSize = 0;
    s.WindowPadding = {8, 8}; s.FramePadding = {6, 4}; s.ItemSpacing = {8, 6};
    s.ItemInnerSpacing = {6, 4}; s.IndentSpacing = 16; s.ScrollbarSize = 12; s.GrabMinSize = 10;
    s.TabBarOverlineSize = 2;
    s.DisabledAlpha = 0.5f;  // greyed controls read as greyed (ImGui's 0.6 looked enabled); 0.35 made a real level's values unreadable
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = hex(0xD7D8D9);
    c[ImGuiCol_TextDisabled]          = hex(0x8299A8);  // hint text: 4.9:1 on the panel (0x476470 was 2.3:1, too dim to read)
    c[ImGuiCol_WindowBg]              = hex(0x172B40);
    c[ImGuiCol_ChildBg]               = hex(0x172B40, 0.f);
    c[ImGuiCol_PopupBg]               = hex(0x102A47);
    c[ImGuiCol_Border]                = hex(0x2C4A64);
    c[ImGuiCol_BorderShadow]          = hex(0x000000, 0.f);
    c[ImGuiCol_FrameBg]               = hex(0x0C1F33);
    c[ImGuiCol_FrameBgHovered]        = hex(0x15304A);
    c[ImGuiCol_FrameBgActive]         = hex(0x1E3A52);
    c[ImGuiCol_TitleBg]               = hex(0x0F233A);
    c[ImGuiCol_TitleBgActive]         = hex(0x172B40);
    c[ImGuiCol_TitleBgCollapsed]      = hex(0x0F233A);
    c[ImGuiCol_MenuBarBg]             = hex(0x0F233A);
    c[ImGuiCol_ScrollbarBg]           = hex(0x0A0D10);
    c[ImGuiCol_ScrollbarGrab]         = hex(0x1E3A52);
    c[ImGuiCol_ScrollbarGrabHovered]  = hex(0x2A5373);
    c[ImGuiCol_ScrollbarGrabActive]   = hex(0x3C7891);
    c[ImGuiCol_CheckMark]             = hex(0xCECE48);
    c[ImGuiCol_SliderGrab]            = hex(0x62AECE);
    c[ImGuiCol_SliderGrabActive]      = hex(0x8CC5DC);
    c[ImGuiCol_Button]                = hex(0x1E3A52);
    c[ImGuiCol_ButtonHovered]         = hex(0x2A5373);
    c[ImGuiCol_ButtonActive]          = hex(0x3C7891);
    c[ImGuiCol_Header]                = hex(0x326378);
    c[ImGuiCol_HeaderHovered]         = hex(0x25475C);
    c[ImGuiCol_HeaderActive]          = hex(0x3C7891);
    c[ImGuiCol_Separator]             = hex(0x2C4A64);
    c[ImGuiCol_SeparatorHovered]      = hex(0x4997B3);
    c[ImGuiCol_SeparatorActive]       = hex(0x8CC5DC);
    c[ImGuiCol_ResizeGrip]            = hex(0x1E3A52, 0.6f);
    c[ImGuiCol_ResizeGripHovered]     = hex(0x2A5373);
    c[ImGuiCol_ResizeGripActive]      = hex(0x3C7891);
    c[ImGuiCol_InputTextCursor]       = hex(0xE4E4E4);
    c[ImGuiCol_Tab]                   = hex(0x0F233A);
    c[ImGuiCol_TabHovered]            = hex(0x2A5373);
    c[ImGuiCol_TabSelected]           = hex(0x172B40);
    c[ImGuiCol_TabSelectedOverline]   = hex(0x8CC5DC);
    c[ImGuiCol_TabDimmed]             = hex(0x0F233A);
    c[ImGuiCol_TabDimmedSelected]     = hex(0x172B40);
    c[ImGuiCol_TabDimmedSelectedOverline] = hex(0x4997B3);
    c[ImGuiCol_PlotLines]             = hex(0x62AECE);
    c[ImGuiCol_PlotLinesHovered]      = hex(0xCECE48);
    c[ImGuiCol_PlotHistogram]         = hex(0xD42A20);  // progress bars: the game's red loading bar
    c[ImGuiCol_PlotHistogramHovered]  = hex(0xE0453A);
    c[ImGuiCol_TableHeaderBg]         = hex(0x0F233A);
    c[ImGuiCol_TableBorderStrong]     = hex(0x2C4A64);
    c[ImGuiCol_TableBorderLight]      = hex(0x1E3A52);
    c[ImGuiCol_TableRowBg]            = hex(0x000000, 0.f);
    c[ImGuiCol_TableRowBgAlt]         = hex(0xFFFFFF, 0.025f);
    c[ImGuiCol_TextLink]              = hex(0x62AECE);
    c[ImGuiCol_TextSelectedBg]        = hex(0x3C7891, 0.6f);
    c[ImGuiCol_TreeLines]             = hex(0x2C4A64);
    c[ImGuiCol_DragDropTarget]        = hex(0xCECE48);
    c[ImGuiCol_NavCursor]             = hex(0xCECE48);
    c[ImGuiCol_NavWindowingHighlight] = hex(0xE4E4E4, 0.7f);
    c[ImGuiCol_NavWindowingDimBg]     = hex(0x0A0D10, 0.6f);
    c[ImGuiCol_ModalWindowDimBg]      = hex(0x0A0D10, 0.6f);
}

void load_fonts(ImGuiIO& io, const wchar_t* windir) {
    auto font = [&](const wchar_t* file) -> std::string {
        std::wstring p = std::wstring(windir) + L"\\Fonts\\" + file;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    };
    // Body: Segoe UI (covers the Latin text people type). Loaded first, so it is the default font.
    std::string body = font(L"segoeui.ttf");
    if (!body.empty()) io.Fonts->AddFontFromFileTTF(body.c_str());
    // Headings: the game's condensed heavy caps; Impact is on every Windows PC (Segoe UI Bold when it is not).
    std::string head = font(L"impact.ttf");
    if (head.empty()) head = font(L"segoeuib.ttf");
    if (!head.empty()) g_heading = io.Fonts->AddFontFromFileTTF(head.c_str());
}

ImFont* heading_font() { return g_heading; }

// The title bar is drawn inside Begin, so the heading face is pushed around Begin only; the body face is pushed back
// for the contents and popped before End, so the font stack depth matches at Begin and End.
static bool g_title_pushed[16];
static int g_title_depth = 0;

static void push_title(bool on) {
    if (g_title_depth < 16) g_title_pushed[g_title_depth] = on;
    g_title_depth++;
}

bool begin_window(const char* name, bool* open, int flags) {
    ImFont* body = ImGui::GetFont();
    const bool on = g_heading != nullptr;
    if (on) ImGui::PushFont(g_heading, 0.f);
    bool r = ImGui::Begin(name, open, flags);
    if (on) ImGui::PushFont(body, 0.f);
    push_title(on);
    return r;
}

void end_window() {
    const bool on = --g_title_depth < 16 ? g_title_pushed[g_title_depth] : false;
    if (on) ImGui::PopFont();
    ImGui::End();
    if (on) ImGui::PopFont();
}

bool begin_modal(const char* name, bool* open, int flags) {
    ImFont* body = ImGui::GetFont();
    const bool on = g_heading != nullptr;
    if (on) ImGui::PushFont(g_heading, 0.f);
    bool r = ImGui::BeginPopupModal(name, open, flags);
    if (!r) {
        if (on) ImGui::PopFont();
        return false;
    }
    if (on) ImGui::PushFont(body, 0.f);
    push_title(on);
    bracket_window();
    return true;
}

void end_modal() {
    const bool on = --g_title_depth < 16 ? g_title_pushed[g_title_depth] : false;
    if (on) ImGui::PopFont();
    ImGui::EndPopup();
    if (on) ImGui::PopFont();
}

bool primary_button(const char* label, const ImVec2& size) {
    const Tokens& t = tok();
    ImGui::PushStyleColor(ImGuiCol_Button, t.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, t.accent_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, t.accent_active);
    ImGui::PushStyleColor(ImGuiCol_Text, t.accent_text);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

void push_toggle_on() {
    const Tokens& t = tok();
    ImGui::PushStyleColor(ImGuiCol_Button, t.select_fill);
    ImGui::PushStyleColor(ImGuiCol_Text, t.text_bright);
    ImGui::PushStyleColor(ImGuiCol_Border, t.glow_halo1);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
}

void pop_toggle_on() {
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
}

bool toggle_button(const char* label, bool on, const ImVec2& size) {
    if (on) push_toggle_on();
    bool r = ImGui::Button(label, size);
    if (on) pop_toggle_on();
    return r;
}

static void push_danger() {
    ImGui::PushStyleColor(ImGuiCol_Text, tok().error);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(0x5A2420));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hex(0x7A2C26));
}

bool danger_button(const char* label, const ImVec2& size) {
    push_danger();
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return r;
}

bool danger_small_button(const char* label) {
    push_danger();
    bool r = ImGui::SmallButton(label);
    ImGui::PopStyleColor(3);
    return r;
}

void bracket_window() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize(), b(a.x + sz.x, a.y + sz.y);
    dl->PushClipRect(a, b, false);
    bracket_frame(dl, a, b, ImGui::GetFontSize() / 16.f);
    dl->PopClipRect();
}

bool small_toggle_button(const char* label, bool on) {
    if (on) push_toggle_on();
    bool r = ImGui::SmallButton(label);
    if (on) pop_toggle_on();
    return r;
}

bool section(const char* label, int flags) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_Button));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}

void heading(const char* text) {
    ImFont* f = g_heading;
    if (f) ImGui::PushFont(f, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 160), text);
    ImGui::TextColored(tok().text_bright, "%s", text);
    if (f) ImGui::PopFont();
}

void bracket_frame(ImDrawList* dl, ImVec2 a, ImVec2 b, float scale) {
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(tok().bracket);
    const float th = 3 * scale, ax = std::min(24 * scale, (b.x - a.x) / 3), ay = std::min(48 * scale, (b.y - a.y) / 3);
    auto L = [&](ImVec2 c, float sx, float sy) {  // corner c, arms going in +sx / +sy
        dl->AddRectFilled(ImVec2(std::min(c.x, c.x + sx * ax), std::min(c.y, c.y + sy * th)), ImVec2(std::max(c.x, c.x + sx * ax), std::max(c.y, c.y + sy * th)), col, 2 * scale);
        dl->AddRectFilled(ImVec2(std::min(c.x, c.x + sx * th), std::min(c.y, c.y + sy * ay)), ImVec2(std::max(c.x, c.x + sx * th), std::max(c.y, c.y + sy * ay)), col, 2 * scale);
    };
    L(a, 1, 1);
    L(ImVec2(b.x, a.y), -1, 1);
    L(ImVec2(a.x, b.y), 1, -1);
    L(b, -1, -1);
}

}  // namespace theme

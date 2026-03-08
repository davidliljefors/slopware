#include "theme.h"

#include "imgui.h"
#include "types.h"


// Gruvbox Dark palette

// bg0 #282828  bg1 #3c3836  bg2 #504945  bg3 #665c54  bg4 #7c6f64
// fg0 #fbf1c7  fg1 #ebdbb2  fg2 #d5c4a1  fg3 #bdae93  fg4 #a89984
// red #fb4934  green #b8bb26  yellow #fabd2f  blue #83a598
// purple #d3869b  aqua #8ec07c  orange #fe8019

static constexpr int hex_digit(char c)
{
	return (c >= '0' && c <= '9') ? (c - '0') :
	       (c >= 'a' && c <= 'f') ? (c - 'a' + 10) :
	       (c >= 'A' && c <= 'F') ? (c - 'A' + 10) : 0;
}

static constexpr ImVec4 hexToImv4(const char* s, f32 a = 1.0f)
{
	int i = (s[0] == '#') ? 1 : 0;
	f32 r = (hex_digit(s[i]) * 16 + hex_digit(s[i+1])) / 255.0f;
	f32 g = (hex_digit(s[i+2]) * 16 + hex_digit(s[i+3])) / 255.0f;
	f32 b = (hex_digit(s[i+4]) * 16 + hex_digit(s[i+5])) / 255.0f;
	return ImVec4(r, g, b, a);
}

static constexpr ImVec4 COLOR_BG                = hexToImv4("#282828");
static constexpr ImVec4 COLOR_FRAME_BG          = hexToImv4("#3c3836");
static constexpr ImVec4 COLOR_HEADER            = hexToImv4("#8EC066", 0.20f);
static constexpr ImVec4 COLOR_HEADER_HOVERED    = hexToImv4("#8EC066", 0.45f);
static constexpr ImVec4 COLOR_HEADER_ACTIVE     = hexToImv4("#8EC066", 0.65f);
static constexpr ImVec4 COLOR_SLIDER_ACTIVE     = hexToImv4("#8EC066");
static constexpr ImVec4 COLOR_CHECKMARK         = hexToImv4("#8EC066");
static constexpr ImVec4 COLOR_TEXT              = hexToImv4("#ebdbb2");
static constexpr ImVec4 COLOR_TEXT_DISABLED     = hexToImv4("#a89984");
static constexpr ImVec4 COLOR_TEXT_INDEXING     = hexToImv4("#fabd2f");
static constexpr ImVec4 COLOR_TEXT_ERROR        = hexToImv4("#fb4934");
static constexpr ImVec4 COLOR_HIGHLIGHT         = hexToImv4("#fabd2f");
static constexpr f32    COLOR_HIGHLIGHT_BG_ALPHA = 0.20f;
static constexpr ImVec4 COLOR_SEL_BG            = hexToImv4("#fabd2f", 0.2f);
static constexpr ImVec4 COLOR_SEL_TEXT          = hexToImv4("#fbf1c7");
static constexpr ImVec4 COLOR_DIR_SELECTED      = hexToImv4("#d5c4a1");
static constexpr ImVec4 COLOR_BUTTON            = hexToImv4("#504945");
static constexpr ImVec4 COLOR_BUTTON_HOVERED    = hexToImv4("#665c54");
static constexpr ImVec4 COLOR_BUTTON_ACTIVE     = hexToImv4("#7c6f64");
static constexpr ImVec4 COLOR_TITLE_BG          = hexToImv4("#2e2e2e");
static constexpr ImVec4 COLOR_TITLE_BG_ACTIVE   = hexToImv4("#504945");

static constexpr ImVec4 COLOR_SEPARATOR         = hexToImv4("#7c6f64");
static constexpr ImVec4 COLOR_SEPARATOR_ACTIVE  = hexToImv4("#ebdbb2");
static constexpr ImVec4 COLOR_SEPARATOR_HOVERED = hexToImv4("#8EC066", 0.65f);

static constexpr ImVec4 COLOR_SCROLL_GRAB       = hexToImv4("#7c6f64");
static constexpr ImVec4 COLOR_SCROLL_HOVER      = hexToImv4("#d5c4a1");
static constexpr ImVec4 COLOR_SCROLL_ACTIVE     = hexToImv4("#8EC066");



// Public API


void theme_apply(f32 dpi_scale)
{
	ImGuiStyle& style = ImGui::GetStyle();
	style = ImGuiStyle();
	ImGui::StyleColorsDark();
	style.FrameRounding = 4.0f;
	style.ChildRounding = 4.0f;
	style.WindowRounding = 6.0f;
	style.TextSelectedBgRounding = 4.0f;
	style.ScaleAllSizes(dpi_scale);
	style.Colors[ImGuiCol_WindowBg]          = COLOR_BG;
	style.Colors[ImGuiCol_Border]            = COLOR_BG;
	style.Colors[ImGuiCol_ChildBg]           = COLOR_BG;
	style.Colors[ImGuiCol_PopupBg]           = COLOR_BG;
	style.Colors[ImGuiCol_FrameBg]           = COLOR_FRAME_BG;
	style.Colors[ImGuiCol_FrameBgHovered]    = COLOR_BUTTON_HOVERED;
	style.Colors[ImGuiCol_FrameBgActive]     = COLOR_BUTTON_ACTIVE;
	style.Colors[ImGuiCol_CheckMark]         = COLOR_CHECKMARK;
	style.Colors[ImGuiCol_Header]            = COLOR_HEADER;
	style.Colors[ImGuiCol_HeaderHovered]     = COLOR_HEADER_HOVERED;
	style.Colors[ImGuiCol_HeaderActive]      = COLOR_HEADER_ACTIVE;
	style.Colors[ImGuiCol_TextSelectedBg]    = COLOR_SEL_BG;
	style.Colors[ImGuiCol_SliderGrab]        = COLOR_HEADER_ACTIVE;
	style.Colors[ImGuiCol_SliderGrabActive]  = COLOR_SLIDER_ACTIVE;
	style.Colors[ImGuiCol_Text]              = COLOR_TEXT;
	style.Colors[ImGuiCol_TextDisabled]      = COLOR_TEXT_DISABLED;
	style.Colors[ImGuiCol_Button]            = COLOR_BUTTON;
	style.Colors[ImGuiCol_ButtonHovered]     = COLOR_BUTTON_HOVERED;
	style.Colors[ImGuiCol_ButtonActive]      = COLOR_BUTTON_ACTIVE;
	style.Colors[ImGuiCol_TitleBg]           = COLOR_TITLE_BG;
	style.Colors[ImGuiCol_TitleBgActive]     = COLOR_TITLE_BG_ACTIVE;
	style.Colors[ImGuiCol_NavHighlight]      = ImVec4(0, 0, 0, 0);
	style.Colors[ImGuiCol_TableHeaderBg]     = COLOR_FRAME_BG;
	style.Colors[ImGuiCol_TableBorderStrong] = hexToImv4("#665c54");
	style.Colors[ImGuiCol_TableBorderLight]  = hexToImv4("#504945");
	
	style.Colors[ImGuiCol_Separator] = COLOR_SEPARATOR;
	style.Colors[ImGuiCol_SeparatorActive] = COLOR_SEPARATOR_ACTIVE;
	style.Colors[ImGuiCol_SeparatorHovered] = COLOR_SEPARATOR_HOVERED;


	style.Colors[ImGuiCol_ScrollbarGrab] = COLOR_SCROLL_GRAB;
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = COLOR_SCROLL_HOVER;
	style.Colors[ImGuiCol_ScrollbarGrabActive] = COLOR_SCROLL_ACTIVE;

}

void theme_clear_color(f32* rgba4)
{
	rgba4[0] = COLOR_BG.x;
	rgba4[1] = COLOR_BG.y;
	rgba4[2] = COLOR_BG.z;
	rgba4[3] = COLOR_BG.w;
}

ImVec4 theme_color_text()          { return COLOR_TEXT; }
ImVec4 theme_color_text_disabled() { return COLOR_TEXT_DISABLED; }
ImVec4 theme_color_text_indexing() { return COLOR_TEXT_INDEXING; }
ImVec4 theme_color_text_error()    { return COLOR_TEXT_ERROR; }
ImVec4 theme_color_highlight()     { return COLOR_HIGHLIGHT; }
f32    theme_highlight_bg_alpha()   { return COLOR_HIGHLIGHT_BG_ALPHA; }
ImVec4 theme_color_sel_bg()        { return COLOR_SEL_BG; }
ImVec4 theme_color_sel_text()      { return COLOR_SEL_TEXT; }
ImVec4 theme_color_dir_selected()  { return COLOR_DIR_SELECTED; }
ImVec4 theme_color_row_highlight() { return COLOR_HEADER_HOVERED; }

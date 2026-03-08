#pragma once

#include "types.h"

struct ImVec4;

// Gruvbox Dark theme

// Apply the Gruvbox Dark ImGui style, scaled to the given DPI factor.
void theme_apply(f32 dpi_scale);

// Background clear color
void theme_clear_color(f32* rgba4);

// Named colors for app-level rendering

ImVec4 theme_color_text();
ImVec4 theme_color_text_disabled();
ImVec4 theme_color_text_indexing();
ImVec4 theme_color_text_error();
ImVec4 theme_color_highlight();
f32    theme_highlight_bg_alpha();
ImVec4 theme_color_sel_bg();
ImVec4 theme_color_sel_text();
ImVec4 theme_color_dir_selected();
ImVec4 theme_color_row_highlight();

#pragma once

#include "types.h"

struct ImVec4;

static const char ICON_GEAR[] = "\xe2\x9a\x99";

// TokenQuery -- parse a string into lowercase space-separated tokens.


static constexpr i32 TOKEN_QUERY_MAX = 64;
static constexpr i32 TOKEN_QUERY_BUF = 512;

struct TokenQuery {
	char buf[TOKEN_QUERY_BUF];
	char* tokens[TOKEN_QUERY_MAX];
	i32 count;
};

// Parse text into lowercase space-separated tokens.
void token_query_parse(TokenQuery* q, const char* text);

// Clear a parsed query (sets count to 0).
void token_query_clear(TokenQuery* q);

// Returns true if every token in q appears in lower_text (via strstr).
bool token_query_match_all(const TokenQuery* q, const char* lower_text);


// Title bar buttons


// Draw minimize + close buttons pinned to top-right of the current window.
// title_bar_height is the unscaled height in pixels; dpi_scale is applied internally.
void draw_title_bar_buttons(i32 title_bar_height);

// Draw the app title vertically centered in the title bar area.
// subtitle is optional (pass nullptr to omit).
void draw_title_bar_title(i32 title_bar_height,
	const char* title, const char* subtitle);


// Highlighted text rendering


// Render text with matching token substrings highlighted.
// `text` is the display string, `lower_text` is its lowercased version
// (used for matching).  Matched ranges get a colored background and
// foreground overlay drawn on top of the base text.
void render_highlighted_text(const char* text, const char* lower_text,
	char* const* tokens, i32 token_count,
	ImVec4 normal_color, ImVec4 highlight_color, f32 highlight_bg_alpha);

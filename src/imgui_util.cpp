#include "imgui_util.h"

#include <math.h>
#include <string.h>

#include "host.h"
#include "imgui.h"
#include "os_window.h"
#include "theme.h"

// Title bar buttons

void draw_title_bar_buttons(i32 title_bar_height)
{
	f32 dpi_scale = window_get_dpi_scale(host_hwnd());
	f32 button_size = floorf((f32)title_bar_height * dpi_scale);
	ImVec2 window_size = ImGui::GetWindowSize();

	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

	auto draw_button = [&](const char* label, f32 x,
		ImVec4 normal, ImVec4 hovered, ImVec4 active) -> bool {
		ImGui::SetCursorPos(ImVec2(x, 0));
		ImGui::PushStyleColor(ImGuiCol_Button, normal);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
		bool clicked = ImGui::Button(label, ImVec2(button_size, button_size));
		ImGui::PopStyleColor(3);
		return clicked;
	};

	if (draw_button("-##min", window_size.x - button_size * 2.0f,
		ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
		ImVec4(0.40f, 0.36f, 0.33f, 1.0f),
		ImVec4(0.50f, 0.46f, 0.43f, 1.0f)))
		window_minimize(host_hwnd());

	if (draw_button("x##close", window_size.x - button_size,
		ImVec4(0.75f, 0.10f, 0.10f, 1.0f),
		ImVec4(0.90f, 0.15f, 0.15f, 1.0f),
		ImVec4(1.00f, 0.05f, 0.05f, 1.0f)))
		host_quit();

	ImGui::PopStyleVar(2);
}


// Title bar title


void draw_title_bar_title(i32 title_bar_height,
	const char* title, const char* subtitle)
{
	f32 dpi_scale = window_get_dpi_scale(host_hwnd());
	f32 title_height = floorf((f32)title_bar_height * dpi_scale);
	ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x,
		(title_height - ImGui::GetTextLineHeight()) * 0.5f));

	ImGui::TextColored(theme_color_highlight(), "%s", title);
	if (subtitle) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", subtitle);
	}
}


// TokenQuery

void token_query_parse(TokenQuery* q, const char* text)
{
	i32 len = 0;
	while (text[len]) len++;
	if (len >= TOKEN_QUERY_BUF)
		len = TOKEN_QUERY_BUF - 1;
	memcpy(q->buf, text, len);
	q->buf[len] = '\0';

	for (char* p = q->buf; *p; p++) {
		if (*p >= 'A' && *p <= 'Z')
			*p += 32;
	}

	q->count = 0;
	char* p = q->buf;
	while (*p) {
		while (*p == ' ') p++;
		if (!*p) break;
		q->tokens[q->count++] = p;
		while (*p && *p != ' ') p++;
		if (*p) *p++ = '\0';
		if (q->count >= TOKEN_QUERY_MAX) break;
	}
}

void token_query_clear(TokenQuery* q)
{
	q->count = 0;
}

bool token_query_match_all(const TokenQuery* q, const char* lower_text)
{
	for (i32 i = 0; i < q->count; i++) {
		if (!strstr(lower_text, q->tokens[i]))
			return false;
	}
	return true;
}

// Highlighted text rendering

void render_highlighted_text(const char* text, const char* lower_text,
	char* const* tokens, i32 token_count,
	ImVec4 normal_color, ImVec4 highlight_color, f32 highlight_bg_alpha)
{
	i32 text_len = (i32)strlen(text);
	if (text_len == 0)
		return;

	ImVec2 pos = ImGui::GetCursorScreenPos();
	ImDrawList* drawlist = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	f32 font_size = ImGui::GetFontSize();

	ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text, text + text_len);
	ImGui::Dummy(text_size);

	drawlist->AddText(font, font_size, pos, ImGui::GetColorU32(normal_color), text, text + text_len);

	if (token_count == 0)
		return;

	struct Range { i32 start, end; };
	Range ranges[128];
	i32 range_count = 0;

	for (i32 t = 0; t < token_count && range_count < 128; t++) {
		i32 tlen = (i32)strlen(tokens[t]);
		if (tlen == 0)
			continue;
		const char* p = lower_text;
		while ((p = strstr(p, tokens[t])) != nullptr && range_count < 128) {
			i32 offset = (i32)(p - lower_text);
			ranges[range_count++] = { offset, offset + tlen };
			p += tlen;
		}
	}

	if (range_count == 0)
		return;

	// sort by start position
	for (i32 i = 1; i < range_count; i++) {
		Range key = ranges[i];
		i32 j = i - 1;
		while (j >= 0 && ranges[j].start > key.start) {
			ranges[j + 1] = ranges[j];
			j--;
		}
		ranges[j + 1] = key;
	}

	// merge overlapping ranges
	i32 merged = 0;
	for (i32 i = 0; i < range_count; i++) {
		if (merged > 0 && ranges[i].start <= ranges[merged - 1].end) {
			if (ranges[i].end > ranges[merged - 1].end)
				ranges[merged - 1].end = ranges[i].end;
		} else {
			ranges[merged++] = ranges[i];
		}
	}

	ImU32 highlight = ImGui::GetColorU32(highlight_color);
	for (i32 i = 0; i < merged; i++) {
		f32 x_off = 0.0f;
		if (ranges[i].start > 0) {
			ImVec2 pre = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f,
				text, text + ranges[i].start);
			x_off = pre.x;
		}

		ImVec2 seg_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f,
			text + ranges[i].start,
			text + ranges[i].end);
		ImVec2 seg_pos = ImVec2(pos.x + x_off, pos.y);

		drawlist->AddRectFilled(
			ImVec2(seg_pos.x - 1.0f, seg_pos.y),
			ImVec2(seg_pos.x + seg_size.x + 1.0f, seg_pos.y + seg_size.y),
			ImGui::GetColorU32(ImVec4(highlight_color.x, highlight_color.y,
				highlight_color.z, highlight_bg_alpha)), 4.0f);

		drawlist->AddText(font, font_size, seg_pos, highlight,
			text + ranges[i].start, text + ranges[i].end);
	}
}

#include "plugin_goto_text_in_file.h"
#include "plugin_internal.h"
#include "plugin_host.h"
#include "string_util.h"
#include "text_search.h"

#include <string.h>

#include "allocators.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_util.h"
#include "os.h"
#include "os_window.h"
#include "theme.h"

// --------------------------------------------------------------------------
// Types
// --------------------------------------------------------------------------

struct InFileResult
{
	i32  line_number;
	i32  column;
	char line_text[256];
};

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

static constexpr i32 MAX_RESULTS = 10000;

static char g_search_buf[512] = {};
static i32  g_selected_index = 0;
static i32  g_prev_selected_index = -1;

static InFileResult g_results[MAX_RESULTS];
static i32 g_result_count = 0;

// File data
static char  g_file_path[1024] = {};
static char  g_file_name[256] = {};
static char* g_file_content = nullptr;
static i32   g_file_content_size = 0;

// Highlight
static char  g_query_lower[512] = {};
static char* g_hl_ptr = nullptr;

// Previous query (to detect changes)
static char g_prev_query[512] = {};

// --------------------------------------------------------------------------
// Search (synchronous -- single file, fast)
// --------------------------------------------------------------------------

static void do_search()
{
	i32 qlen = (i32)strlen(g_search_buf);
	if (qlen == 0 || !g_file_content || g_file_content_size == 0) {
		g_result_count = 0;
		return;
	}

	// Build lowercase query
	char query_lower[512];
	str_to_lower(query_lower, g_search_buf, (i32)sizeof(query_lower) - 1);

	BmhSearcher bmh;
	bmh.build(query_lower, qlen);

	const char* data = g_file_content;
	const char* data_end = data + g_file_content_size;
	const char* search_pos = data;
	i32 search_remaining = g_file_content_size;
	const char* nl_counted = data;
	i32 line_num = 1;
	i32 count = 0;

	while (search_remaining >= qlen && count < MAX_RESULTS) {
		const char* match = bmh.search(search_pos, search_remaining);
		if (!match) break;

		line_num += count_newlines(nl_counted, match);

		const char* ls = find_line_start(match, data);
		const char* le = find_line_end(match + qlen, data_end);
		i32 line_len = (i32)(le - ls);

		InFileResult& sr = g_results[count];
		sr.line_number = line_num;
		sr.column = (i32)(match - ls);

		i32 copy_len = line_len;
		if (copy_len >= (i32)sizeof(sr.line_text))
			copy_len = (i32)sizeof(sr.line_text) - 1;
		memcpy(sr.line_text, ls, copy_len);
		sr.line_text[copy_len] = '\0';

		// Trim leading whitespace
		char* trimmed = sr.line_text;
		while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
		if (trimmed != sr.line_text)
			memmove(sr.line_text, trimmed, strlen(trimmed) + 1);

		count++;

		if (le < data_end) {
			nl_counted = le + 1;
			line_num++;
			search_pos = (char*)(le + 1);
			search_remaining = (i32)(data_end - search_pos);
		} else {
			break;
		}
	}

	g_result_count = count;
}

// --------------------------------------------------------------------------
// Live preview: send cursor position to VS
// --------------------------------------------------------------------------

static void send_preview()
{
	if (g_result_count == 0 || g_selected_index >= g_result_count) return;
	if (!g_callbacks.preview) return;

	InFileResult& sr = g_results[g_selected_index];
	g_callbacks.preview(g_file_path, sr.line_number, sr.column);
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void plugin_goto_text_in_file_set_file(const char* path)
{
	// Free previous content
	if (g_file_content) {
		application_heap()->free(g_file_content);
		g_file_content = nullptr;
		g_file_content_size = 0;
	}

	str_copy(g_file_path, sizeof(g_file_path), path);

	// Extract filename
	const char* name = str_filename(path);
	str_copy(g_file_name, sizeof(g_file_name), name);

	// Read file content
	OsFile h = os_file_open_seq(path);
	if (!os_file_valid(h)) return;

	i64 size64 = os_file_size(h);
	if (size64 <= 0 || size64 > 64 * 1024 * 1024) {
		os_file_close(h);
		return;
	}

	i32 size = (i32)size64;
	g_file_content = (char*)application_heap()->alloc(size + 1, 1);

	i32 bytes_read = 0;
	if (os_file_read(h, g_file_content, size, &bytes_read) && bytes_read == size) {
		g_file_content[size] = '\0';
		g_file_content_size = size;
	} else {
		application_heap()->free(g_file_content);
		g_file_content = nullptr;
		g_file_content_size = 0;
	}
	os_file_close(h);
}

void plugin_goto_text_in_file_init()
{
	memset(g_search_buf, 0, sizeof(g_search_buf));
	memset(g_prev_query, 0, sizeof(g_prev_query));
	g_query_lower[0] = '\0';
	g_result_count = 0;
	g_selected_index = 0;
	g_prev_selected_index = -1;
}

void plugin_goto_text_in_file_tick()
{
	ImGuiIO& io = ImGui::GetIO();

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##gotextinfile", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus);

	// Focus search box
	if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
		ImGui::SetKeyboardFocusHere();

	ImGui::SetNextItemWidth(-1);

	char hint[320];
	snprintf(hint, sizeof(hint), "Search in %s...", g_file_name);
	bool enter = ImGui::InputTextWithHint("##search", hint,
		g_search_buf, sizeof(g_search_buf), ImGuiInputTextFlags_EnterReturnsTrue);

	// Confirm selection
	if (enter && g_result_count > 0 && g_selected_index < g_result_count) {
		InFileResult& sr = g_results[g_selected_index];
		if (g_callbacks.selection)
			g_callbacks.selection(g_file_path, sr.line_number, sr.column);
		host_quit();
	}

	// Trigger search on text change
	if (strcmp(g_search_buf, g_prev_query) != 0) {
		memcpy(g_prev_query, g_search_buf, sizeof(g_search_buf));

		// Build lowercase query for highlighting
		str_to_lower(g_query_lower, g_search_buf, (i32)sizeof(g_query_lower) - 1);
		g_hl_ptr = g_query_lower;

		do_search();
		g_selected_index = 0;
		g_prev_selected_index = -1; // force preview
	}

	// Navigation
	i32 old_sel = g_selected_index;
	if (g_result_count > 0) {
		if (key_pressed(ImGuiKey_UpArrow))   { g_selected_index--; if (g_selected_index < 0) g_selected_index = 0; }
		if (key_pressed(ImGuiKey_DownArrow)) { g_selected_index++; if (g_selected_index >= g_result_count) g_selected_index = g_result_count - 1; }
		if (key_pressed(ImGuiKey_PageUp))    { g_selected_index -= 15; if (g_selected_index < 0) g_selected_index = 0; }
		if (key_pressed(ImGuiKey_PageDown))  { g_selected_index += 15; if (g_selected_index >= g_result_count) g_selected_index = g_result_count - 1; }
		if (key_pressed(ImGuiKey_Home))      g_selected_index = 0;
		if (key_pressed(ImGuiKey_End))       g_selected_index = g_result_count - 1;

		f32 wheel = io.MouseWheel;
		if (wheel != 0.0f) {
			g_selected_index -= (i32)wheel;
			if (g_selected_index < 0) g_selected_index = 0;
			if (g_selected_index >= g_result_count) g_selected_index = g_result_count - 1;
			io.MouseWheel = 0.0f;
		}
	}
	if (g_selected_index >= g_result_count)
		g_selected_index = (g_result_count > 0) ? g_result_count - 1 : 0;

	// Send live preview when selection changes
	if (g_selected_index != g_prev_selected_index && g_result_count > 0) {
		g_prev_selected_index = g_selected_index;
		send_preview();
	}

	// Results list
	if (g_search_buf[0] == '\0') {
		ImGui::TextDisabled("Type to search in %s. Escape to close.", g_file_name);
	} else if (g_result_count > 0) {
		if (g_result_count >= MAX_RESULTS)
			ImGui::Text("Showing first %d results", MAX_RESULTS);
		else
			ImGui::Text("%d results", g_result_count);

		ImVec4 text_color   = theme_color_text();
		ImVec4 disabled_col = theme_color_text_disabled();
		ImVec4 hl_color     = theme_color_highlight();
		ImVec4 sel_bg       = theme_color_sel_bg();
		ImVec4 sel_text     = theme_color_sel_text();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		ImGuiListClipper clipper;
		clipper.Begin(g_result_count);
		while (clipper.Step()) {
			for (i32 row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				InFileResult& sr = g_results[row];

				ImGui::PushID(row);
				bool selected = (row == g_selected_index);
				ImVec2 row_min = ImGui::GetCursorScreenPos();
				f32 row_h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
				ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_h);

				if (selected)
					ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max,
						ImGui::GetColorU32(sel_bg), 3.0f);

				ImGui::SetCursorScreenPos(row_min);
				ImGui::Dummy(ImVec2(row_max.x - row_min.x, row_h));
				if (ImGui::IsItemClicked(0)) {
					g_selected_index = row;
					g_prev_selected_index = -1; // force preview
				}

				// Draw: line_number | content
				ImGui::SameLine();
				ImGui::SetCursorScreenPos(ImVec2(row_min.x + 4.0f, row_min.y + 2.0f));

				ImVec4 info_col = selected ? theme_color_dir_selected() : disabled_col;
				ImGui::PushStyleColor(ImGuiCol_Text, info_col);
				ImGui::Text("%d", sr.line_number);
				ImGui::PopStyleColor();

				// Content with highlighting
				ImGui::SameLine();
				f32 content_x = ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize("  ").x;
				ImGui::SetCursorScreenPos(ImVec2(content_x, row_min.y + 2.0f));

				i32 hl_count = g_query_lower[0] ? 1 : 0;
				char lt_lower[256];
				str_to_lower(lt_lower, sr.line_text, 255);

				ImVec4 content_col = selected ? sel_text : text_color;
				render_highlighted_text(sr.line_text, lt_lower,
					&g_hl_ptr, hl_count,
					content_col, hl_color, theme_highlight_bg_alpha());

				ImGui::PopID();
			}
		}

		// Scroll selected into view
		if (g_result_count > 0) {
			f32 item_h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
			f32 sel_top = g_selected_index * item_h;
			f32 sel_bot = sel_top + item_h;
			f32 scroll = ImGui::GetScrollY();
			f32 visible = ImGui::GetWindowHeight();
			if (sel_top < scroll)
				ImGui::SetScrollY(sel_top);
			else if (sel_bot > scroll + visible)
				ImGui::SetScrollY(sel_bot - visible);
		}

		ImGui::EndChild();
		ImGui::PopStyleVar();
	} else if (g_search_buf[0] != '\0') {
		ImGui::TextDisabled("No results.");
	}

	// Escape to close
	if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		host_quit();

	ImGui::End();
}

void plugin_goto_text_in_file_shutdown()
{
	g_result_count = 0;
	g_selected_index = 0;
	g_prev_selected_index = -1;
	memset(g_search_buf, 0, sizeof(g_search_buf));
	memset(g_prev_query, 0, sizeof(g_prev_query));

	if (g_file_content) {
		application_heap()->free(g_file_content);
		g_file_content = nullptr;
		g_file_content_size = 0;
	}
}

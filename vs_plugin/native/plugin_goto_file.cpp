#include "plugin_goto_file.h"
#include "plugin_internal.h"
#include "plugin_host.h"
#include "string_util.h"
#include "text_search.h"

#include <algorithm>
#include <atomic>
#include <ppl.h>
#include <string.h>

#include "allocators.h"
#include "imgui.h"
#include "imgui_util.h"
#include "os.h"
#include "os_window.h"
#include "theme.h"

// --------------------------------------------------------------------------
// Search state
// --------------------------------------------------------------------------

static constexpr i32 SEARCH_MAX_RESULTS = 10000;
static constexpr i32 STAGING_CAPACITY   = SEARCH_MAX_RESULTS * 4;

static char g_search_buf[512] = {};
static i32  g_selected_index = 0;

static i32 g_result_indices[SEARCH_MAX_RESULTS];
static i32 g_result_count = 0;

static i32 g_staging_indices[STAGING_CAPACITY];
static i32 g_staging_count = 0;
static std::atomic<bool> g_staging_ready { false };

static concurrency::task_group* g_search_tg = nullptr;
static std::atomic<bool> g_search_cancel { false };
static char g_bg_query[512] = {};

static TokenQuery g_hl_name;
static TokenQuery g_hl_proj;

// --------------------------------------------------------------------------
// Settings state
// --------------------------------------------------------------------------

static char g_include_ext_buf[1024] = {};

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static ImVec2 gear_size()
{
	f32 h = ImGui::GetFrameHeight();
	return ImVec2(h, h);
}

static void cancel_search()
{
	g_search_cancel.store(true, std::memory_order_release);
	if (g_search_tg) g_search_tg->wait();
	g_search_cancel.store(false, std::memory_order_release);
	g_staging_ready.store(false, std::memory_order_release);
}

static void clear_state()
{
	memset(g_search_buf, 0, sizeof(g_search_buf));
	memset(g_bg_query, 0, sizeof(g_bg_query));
	token_query_clear(&g_hl_name);
	token_query_clear(&g_hl_proj);
	g_result_count = 0;
	g_selected_index = 0;
}

static void parse_highlight_tokens()
{
	TokenQuery full;
	token_query_parse(&full, g_search_buf);

	i32 in_index = -1;
	for (i32 i = 0; i < full.count; i++) {
		if (strcmp(full.tokens[i], "in") == 0) { in_index = i; break; }
	}

	i32 name_end = (in_index >= 0) ? in_index : full.count;
	char name_str[TOKEN_QUERY_BUF];
	i32 npos = 0;
	for (i32 i = 0; i < name_end && npos < TOKEN_QUERY_BUF - 1; i++) {
		if (i > 0) name_str[npos++] = ' ';
		for (const char* s = full.tokens[i]; *s && npos < TOKEN_QUERY_BUF - 1; )
			name_str[npos++] = *s++;
	}
	name_str[npos] = '\0';
	token_query_parse(&g_hl_name, name_str);

	i32 proj_start = (in_index >= 0) ? in_index + 1 : full.count;
	char proj_str[TOKEN_QUERY_BUF];
	i32 ppos = 0;
	for (i32 i = proj_start; i < full.count && ppos < TOKEN_QUERY_BUF - 1; i++) {
		if (i > proj_start) proj_str[ppos++] = ' ';
		for (const char* s = full.tokens[i]; *s && ppos < TOKEN_QUERY_BUF - 1; )
			proj_str[ppos++] = *s++;
	}
	proj_str[ppos] = '\0';
	token_query_parse(&g_hl_proj, proj_str);
}

static void do_search_bg()
{
	char query[512];
	str_to_lower(query, g_bg_query, (i32)sizeof(query) - 1);
	i32 qlen = (i32)strlen(query);

	// Tokenise
	char* tokens[64];
	i32 token_count = 0;
	{
		char* p = query;
		while (*p) {
			while (*p == ' ') p++;
			if (!*p) break;
			tokens[token_count++] = p;
			while (*p && *p != ' ') p++;
			if (*p) *p++ = '\0';
			if (token_count >= 64) break;
		}
	}
	if (token_count == 0) {
		g_staging_count = 0;
		g_staging_ready.store(true, std::memory_order_release);
		return;
	}

	// Split at "in" keyword: name tokens before, project tokens after
	i32 in_index = -1;
	for (i32 i = 0; i < token_count; i++) {
		if (strcmp(tokens[i], "in") == 0) { in_index = i; break; }
	}
	i32 name_count = (in_index >= 0) ? in_index : token_count;
	i32 proj_start = (in_index >= 0) ? in_index + 1 : token_count;
	i32 proj_count = token_count - proj_start;

	if (name_count == 0 && proj_count == 0) {
		g_staging_count = 0;
		g_staging_ready.store(true, std::memory_order_release);
		return;
	}

	ReadGuard rg(&g_file_store.files_lock);
	i32 file_count = (i32)g_file_store.files.count;

	std::atomic<i32> match_count { 0 };
	constexpr i32 CHUNK = 4096;
	i32 num_chunks = (file_count + CHUNK - 1) / CHUNK;

	concurrency::parallel_for(0, num_chunks, [&](i32 ci) {
		if (g_search_cancel.load(std::memory_order_relaxed)) return;

		i32 start = ci * CHUNK;
		i32 end = start + CHUNK;
		if (end > file_count) end = file_count;

		for (i32 i = start; i < end; i++) {
			if (match_count.load(std::memory_order_relaxed) >= STAGING_CAPACITY) return;
			if ((i & 1023) == 0 && g_search_cancel.load(std::memory_order_relaxed)) return;

			const char* lower = g_file_store.files[i].filename_lower;
			const char* plower = g_file_store.files[i].project_lower;
			if (!lower) continue;

			// Match name tokens against filename
			bool match = true;
			for (i32 t = 0; t < name_count; t++) {
				if (!strstr(lower, tokens[t])) { match = false; break; }
			}
			if (!match) continue;

			// Match project tokens against project name
			if (proj_count > 0) {
				if (!plower) continue;
				for (i32 t = 0; t < proj_count; t++) {
					if (!strstr(plower, tokens[proj_start + t])) { match = false; break; }
				}
				if (!match) continue;
			}

			i32 idx = match_count.fetch_add(1, std::memory_order_relaxed);
			if (idx < STAGING_CAPACITY)
				g_staging_indices[idx] = i;
		}
	});

	if (!g_search_cancel.load(std::memory_order_relaxed)) {
		i32 cnt = match_count.load(std::memory_order_relaxed);
		if (cnt > STAGING_CAPACITY) cnt = STAGING_CAPACITY;
		if (cnt > SEARCH_MAX_RESULTS) cnt = SEARCH_MAX_RESULTS;
		g_staging_count = cnt;

		// Sort: exact stem match > prefix match > substring match
		if (cnt > 1 && name_count > 0) {
			i32 first_tok_len = (i32)strlen(tokens[0]);

			i32 bucket0[SEARCH_MAX_RESULTS]; i32 b0 = 0;
			i32 bucket1[SEARCH_MAX_RESULTS]; i32 b1 = 0;
			i32 bucket2[SEARCH_MAX_RESULTS]; i32 b2 = 0;

			for (i32 i = 0; i < cnt; i++) {
				i32 idx = g_staging_indices[i];
				if (idx >= (i32)g_file_store.files.count) { bucket2[b2++] = idx; continue; }
				const char* lo = g_file_store.files[idx].filename_lower;
				if (!lo) { bucket2[b2++] = idx; continue; }

				i32 lo_len = (i32)strlen(lo);
				const char* dot = nullptr;
				for (const char* p = lo; *p; p++)
					if (*p == '.') dot = p;
				i32 stem_len = dot ? (i32)(dot - lo) : lo_len;

				if (stem_len == first_tok_len && memcmp(lo, tokens[0], first_tok_len) == 0)
					bucket0[b0++] = idx;
				else if (first_tok_len <= lo_len && memcmp(lo, tokens[0], first_tok_len) == 0)
					bucket1[b1++] = idx;
				else
					bucket2[b2++] = idx;
			}

			// Within each bucket, sort by filename length (shorter first)
			auto by_len = [](i32 a, i32 b) {
				i32 la = (a < (i32)g_file_store.files.count && g_file_store.files[a].filename_lower)
					? (i32)strlen(g_file_store.files[a].filename_lower) : 0x7fffffff;
				i32 lb = (b < (i32)g_file_store.files.count && g_file_store.files[b].filename_lower)
					? (i32)strlen(g_file_store.files[b].filename_lower) : 0x7fffffff;
				return la < lb;
			};
			std::sort(bucket0, bucket0 + b0, by_len);
			std::sort(bucket1, bucket1 + b1, by_len);
			std::sort(bucket2, bucket2 + b2, by_len);

			i32 out = 0;
			memcpy(g_staging_indices + out, bucket0, b0 * sizeof(i32)); out += b0;
			memcpy(g_staging_indices + out, bucket1, b1 * sizeof(i32)); out += b1;
			memcpy(g_staging_indices + out, bucket2, b2 * sizeof(i32)); out += b2;
		}

		g_staging_ready.store(true, std::memory_order_release);
	}
}

// --------------------------------------------------------------------------
// Init / Tick / Shutdown
// --------------------------------------------------------------------------

void plugin_goto_file_init()
{
	clear_state();
	memcpy(g_include_ext_buf, g_file_store.include_extensions, sizeof(g_include_ext_buf));

	void* mem = application_heap()->alloc(sizeof(concurrency::task_group),
		alignof(concurrency::task_group));
	g_search_tg = new (mem) concurrency::task_group();
}

void plugin_goto_file_tick()
{
	ImGuiIO& io = ImGui::GetIO();

	// Collect staging results
	if (g_staging_ready.load(std::memory_order_acquire)) {
		memcpy(g_result_indices, g_staging_indices, g_staging_count * sizeof(i32));
		g_result_count = g_staging_count;
		g_staging_ready.store(false, std::memory_order_release);
	}

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##gotofile", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus);

	// Focus search box
	if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
		ImGui::SetKeyboardFocusHere();

	f32 btn_w = gear_size().x;
	ImGui::SetNextItemWidth(-(btn_w + ImGui::GetStyle().ItemSpacing.x));

	bool enter = ImGui::InputTextWithHint("##search", "Search files...",
		g_search_buf, sizeof(g_search_buf), ImGuiInputTextFlags_EnterReturnsTrue);

	if (enter && g_result_count > 0 && g_selected_index < g_result_count) {
		i32 fi = g_result_indices[g_selected_index];
		char path_copy[1024] = {};
		{
			ReadGuard rg(&g_file_store.files_lock);
			if (fi < (i32)g_file_store.files.count && g_file_store.files[fi].full_path) {
				i32 len = (i32)strlen(g_file_store.files[fi].full_path);
				if (len < (i32)sizeof(path_copy)) memcpy(path_copy, g_file_store.files[fi].full_path, len + 1);
			}
		}
		if (path_copy[0] && g_callbacks.selection) {
			g_callbacks.selection(path_copy, 0, 0);
		}
		host_quit();
	}

	ImGui::SameLine();
	if (ImGui::Button(ICON_GEAR, gear_size()))
		ImGui::OpenPopup("FileSettings");

	// Trigger search on change
	if (strcmp(g_search_buf, g_bg_query) != 0) {
		cancel_search();
		if (g_search_buf[0] == '\0') {
			g_result_count = 0;
			token_query_clear(&g_hl_name);
			token_query_clear(&g_hl_proj);
			memcpy(g_bg_query, g_search_buf, sizeof(g_search_buf));
		} else {
			parse_highlight_tokens();
			memcpy(g_bg_query, g_search_buf, sizeof(g_search_buf));
			g_selected_index = 0;
			g_search_tg->run([]() { do_search_bg(); });
		}
	}

	// Navigation
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

	// Help text or results
	if (g_search_buf[0] == '\0') {
		i32 file_count;
		{ ReadGuard rg(&g_file_store.files_lock); file_count = (i32)g_file_store.files.count; }
		ImGui::TextDisabled("Type to search %d solution files. Escape to close.", file_count);
	} else {
		if (g_result_count > 1)
			ImGui::Text("%d results", g_result_count);
		else if (g_result_count == 1)
			ImGui::Text("1 result");

		ImVec4 text_color   = theme_color_text();
		ImVec4 hl_color     = theme_color_highlight();
		ImVec4 sel_bg_color = theme_color_sel_bg();
		ImVec4 sel_text_col = theme_color_sel_text();
		ImVec4 disabled_col = theme_color_text_disabled();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		ImGuiListClipper clipper;
		clipper.Begin(g_result_count);
		ReadGuard rg(&g_file_store.files_lock);
		while (clipper.Step()) {
			for (i32 i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
				i32 fi = g_result_indices[i];
				const char* fname = nullptr;
				const char* fname_lower = nullptr;
				const char* proj = nullptr;
				const char* proj_lower = nullptr;
				if (fi < (i32)g_file_store.files.count) {
					fname = g_file_store.files[fi].filename;
					fname_lower = g_file_store.files[fi].filename_lower;
					proj = g_file_store.files[fi].project_name;
					proj_lower = g_file_store.files[fi].project_lower;
				}
				if (!fname) continue;

				ImGui::PushID(i);
				bool selected = (i == g_selected_index);
				ImVec2 row_min = ImGui::GetCursorScreenPos();
				f32 row_h = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
				ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_h);

				if (selected)
					ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max,
						ImGui::GetColorU32(sel_bg_color), 3.0f);

				ImGui::SetCursorScreenPos(row_min);
				ImGui::Dummy(ImVec2(row_max.x - row_min.x, row_h));
				if (ImGui::IsItemClicked(0))
					g_selected_index = i;

				ImGui::SameLine();
				ImGui::SetCursorScreenPos(ImVec2(row_min.x + 4.0f, row_min.y + 2.0f));

				ImVec4 col = selected ? sel_text_col : text_color;
				render_highlighted_text(fname, fname_lower,
					g_hl_name.tokens, g_hl_name.count,
					col, hl_color, theme_highlight_bg_alpha());

				if (proj) {
					ImVec4 proj_col = selected ? theme_color_dir_selected() : disabled_col;
					ImGui::SameLine();
					f32 proj_x = ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize("  ").x;
					ImGui::SetCursorScreenPos(ImVec2(proj_x, row_min.y + 2.0f));
					if (g_hl_proj.count > 0) {
						render_highlighted_text(proj, proj_lower,
							g_hl_proj.tokens, g_hl_proj.count,
							proj_col, hl_color, theme_highlight_bg_alpha());
					} else {
						ImGui::PushStyleColor(ImGuiCol_Text, proj_col);
						ImGui::TextUnformatted(proj);
						ImGui::PopStyleColor();
					}
				}

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
	}

	// Settings popup
	{
		f32 dpi = window_get_dpi_scale(host_hwnd());
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(500.0f * dpi, 0.0f));
		if (ImGui::BeginPopupModal("FileSettings", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
			ImGui::Text("Include extensions (also used by text search)");
			ImGui::Separator();
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##include", g_include_ext_buf, sizeof(g_include_ext_buf));
			ImGui::TextDisabled("Comma separated, e.g. .h, .cpp, .cs");
			ImGui::Separator();
			if (ImGui::Button("OK", ImVec2(-1, 0))) {
				if (memcmp(g_file_store.include_extensions, g_include_ext_buf, sizeof(g_include_ext_buf)) != 0) {
					memcpy(g_file_store.include_extensions, g_include_ext_buf, sizeof(g_include_ext_buf));
					plugin_save_extensions();
					ensure_refresh_done();
					file_store_reset_content(&g_file_store);
					begin_refresh();
				}
				ImGui::CloseCurrentPopup();
			}
			if (ImGui::Button("Cancel", ImVec2(-1, 0)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	// Escape to close
	if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		host_quit();

	ImGui::End();
}

void plugin_goto_file_cancel_search()
{
	cancel_search();
}

void plugin_goto_file_shutdown()
{
	cancel_search();
	if (g_search_tg) {
		g_search_tg->~task_group();
		application_heap()->free(g_search_tg);
		g_search_tg = nullptr;
	}
	clear_state();
}

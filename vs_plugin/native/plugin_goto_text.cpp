#include "plugin_goto_text.h"
#include "plugin_internal.h"
#include "plugin_host.h"
#include "string_util.h"
#include "text_search.h"

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
// Types
// --------------------------------------------------------------------------

struct TextSearchResult
{
	i32  file_index;
	i32  line_number;
	i32  column;       // 0-based column offset of match within the line
	char line_text[256];
};

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

static constexpr i32 MAX_RESULTS = 10000;
static constexpr i32 STAGING_CAP = MAX_RESULTS * 4;

static char g_search_buf[512] = {};
static i32  g_selected_index = 0;

static TextSearchResult g_results[MAX_RESULTS];
static i32 g_result_count = 0;

static TextSearchResult g_staging[STAGING_CAP];
static i32 g_staging_count = 0;
static std::atomic<bool> g_staging_ready { false };

static concurrency::task_group* g_search_tg = nullptr;
static std::atomic<bool> g_search_cancel { false };
static char g_bg_query[512] = {};

// Highlight
static char g_query_lower_copy[512] = {};
static char* g_hl_ptr = nullptr;

// Settings
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
	g_result_count = 0;
	g_selected_index = 0;
}

static void send_preview()
{
	if (g_result_count == 0 || g_selected_index >= g_result_count) return;
	if (!g_callbacks.preview) return;

	TextSearchResult& sr = g_results[g_selected_index];
	char path_copy[1024] = {};
	{
		ReadGuard rg(&g_file_store.files_lock);
		if (sr.file_index < (i32)g_file_store.files.count && g_file_store.files[sr.file_index].full_path) {
			i32 len = (i32)strlen(g_file_store.files[sr.file_index].full_path);
			if (len < (i32)sizeof(path_copy)) memcpy(path_copy, g_file_store.files[sr.file_index].full_path, len + 1);
		}
	}
	if (path_copy[0])
		g_callbacks.preview(path_copy, sr.line_number, sr.column);
}

// --------------------------------------------------------------------------
// Background search
// --------------------------------------------------------------------------

static void do_text_search_bg()
{
	ensure_refresh_done();

	// Poll directory watchers for changes since the last refresh and reload
	// any dirty files so that searches always see up-to-date content.
	refresh_stale_content_impl();

	char query[512];
	i32 qlen = (i32)strlen(g_bg_query);
	if (qlen >= (i32)sizeof(query)) qlen = (i32)sizeof(query) - 1;
	memcpy(query, g_bg_query, qlen);
	query[qlen] = '\0';

	char query_lower[512];
	str_to_lower(query_lower, query, (i32)sizeof(query_lower) - 1);

	if (qlen == 0) {
		g_staging_count = 0;
		g_staging_ready.store(true, std::memory_order_release);
		return;
	}

	BmhSearcher bmh;
	bmh.build(query_lower, qlen);

	ReadGuard rg(&g_file_store.files_lock);
	i32 file_count = (i32)g_file_store.files.count;

	std::atomic<i32> total_results { 0 };

	concurrency::parallel_for(0, file_count, [&](i32 i) {
		if (g_search_cancel.load(std::memory_order_relaxed)) return;
		if (total_results.load(std::memory_order_relaxed) >= STAGING_CAP) return;

		const char* content = g_file_store.files[i].content;
		i32 content_size = g_file_store.files[i].content_size;
		if (!content || content_size == 0) return;

		const char* data = content;
		const char* data_end = data + content_size;
		const char* search_pos = data;
		i32 search_remaining = content_size;
		const char* nl_counted = data;
		i32 line_num = 1;

		while (search_remaining >= qlen) {
			if (total_results.load(std::memory_order_relaxed) >= STAGING_CAP) break;
			if (g_search_cancel.load(std::memory_order_relaxed)) break;

			const char* match = bmh.search(search_pos, search_remaining);
			if (!match) break;

			line_num += count_newlines(nl_counted, match);

			const char* ls = find_line_start(match, data);
			const char* le = find_line_end(match + qlen, data_end);
			i32 line_len = (i32)(le - ls);

			TextSearchResult sr;
			sr.file_index = i;
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

			i32 idx = total_results.fetch_add(1, std::memory_order_relaxed);
			if (idx < STAGING_CAP)
				g_staging[idx] = sr;

			if (le < data_end) {
				nl_counted = le + 1;
				line_num++;
				search_pos = (char*)(le + 1);
				search_remaining = (i32)(data_end - search_pos);
			} else {
				break;
			}
		}
	});

	if (!g_search_cancel.load(std::memory_order_relaxed)) {
		i32 cnt = total_results.load(std::memory_order_relaxed);
		if (cnt > STAGING_CAP) cnt = STAGING_CAP;
		if (cnt > MAX_RESULTS) cnt = MAX_RESULTS;
		g_staging_count = cnt;
		g_staging_ready.store(true, std::memory_order_release);
	}
}

// --------------------------------------------------------------------------
// Init / Tick / Shutdown
// --------------------------------------------------------------------------

void plugin_goto_text_init()
{
	clear_state();
	memcpy(g_include_ext_buf, g_file_store.include_extensions, sizeof(g_include_ext_buf));

	void* mem = application_heap()->alloc(sizeof(concurrency::task_group),
		alignof(concurrency::task_group));
	g_search_tg = new (mem) concurrency::task_group();
}

void plugin_goto_text_tick()
{
	ImGuiIO& io = ImGui::GetIO();

	// Collect staging results
	if (g_staging_ready.load(std::memory_order_acquire)) {
		i32 count = g_staging_count;
		if (count > MAX_RESULTS) count = MAX_RESULTS;
		memcpy(g_results, g_staging, count * sizeof(TextSearchResult));
		g_result_count = count;
		g_staging_ready.store(false, std::memory_order_release);
	}

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("##gototext", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoBringToFrontOnFocus);

	// Loading indicator
	if (g_waiting_for_vs.load(std::memory_order_acquire)) {
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme_color_text_indexing());
		ImGui::ProgressBar(-1.0f * (f32)ImGui::GetTime(), ImVec2(-gear_size().x - ImGui::GetStyle().ItemSpacing.x, 0), "Waiting for VS...");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::Button(ICON_GEAR, gear_size()))
			ImGui::OpenPopup("TextSettings");
	} else if (g_preloading.load()) {
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme_color_text_indexing());
		ImGui::ProgressBar(-1.0f * (f32)ImGui::GetTime(), ImVec2(-gear_size().x - ImGui::GetStyle().ItemSpacing.x, 0), "Loading file contents...");
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::Button(ICON_GEAR, gear_size()))
			ImGui::OpenPopup("TextSettings");
	} else {
		// Focus search box
		if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
			ImGui::SetKeyboardFocusHere();

		f32 btn_w = gear_size().x;
		ImGui::SetNextItemWidth(-(btn_w + ImGui::GetStyle().ItemSpacing.x));

		bool enter = ImGui::InputTextWithHint("##search", "Search in files...",
			g_search_buf, sizeof(g_search_buf), ImGuiInputTextFlags_EnterReturnsTrue);

		if (enter && g_result_count > 0 && g_selected_index < g_result_count) {
			TextSearchResult& sr = g_results[g_selected_index];
			char path_copy[1024] = {};
			{
				ReadGuard rg(&g_file_store.files_lock);
				if (sr.file_index < (i32)g_file_store.files.count && g_file_store.files[sr.file_index].full_path) {
					i32 len = (i32)strlen(g_file_store.files[sr.file_index].full_path);
					if (len < (i32)sizeof(path_copy)) memcpy(path_copy, g_file_store.files[sr.file_index].full_path, len + 1);
				}
			}
			if (path_copy[0] && g_callbacks.selection) {
				g_callbacks.selection(path_copy, sr.line_number, sr.column);
			}
			host_quit();
		}

		ImGui::SameLine();
		if (ImGui::Button(ICON_GEAR, gear_size()))
			ImGui::OpenPopup("TextSettings");

		// Trigger search on change
		if (strcmp(g_search_buf, g_bg_query) != 0) {
			cancel_search();
			if (g_search_buf[0] == '\0') {
				g_result_count = 0;
				g_query_lower_copy[0] = '\0';
				memcpy(g_bg_query, g_search_buf, sizeof(g_search_buf));
			} else {
				// Build lowercase query for highlighting
				str_to_lower(g_query_lower_copy, g_search_buf, (i32)sizeof(g_query_lower_copy) - 1);
				g_hl_ptr = g_query_lower_copy;

				memcpy(g_bg_query, g_search_buf, sizeof(g_search_buf));
				g_selected_index = 0;
				g_search_tg->run([]() { do_text_search_bg(); });
			}
		}
	}

	// Navigation — only preview on explicit user navigation
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

	// Send preview only when user explicitly navigated
	if (g_selected_index != old_sel && g_result_count > 0)
		send_preview();

	// Help / results
	if (!g_preloading.load() && g_search_buf[0] == '\0') {
		i32 fc;
		{ ReadGuard rg(&g_file_store.files_lock); fc = (i32)g_file_store.files.count; }
		ImGui::TextDisabled("Type to search text in %d files. Escape to close.", fc);
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
		ReadGuard rg(&g_file_store.files_lock);
		while (clipper.Step()) {
			for (i32 row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				TextSearchResult& sr = g_results[row];

				const char* fname = "(unknown)";
				if (sr.file_index < (i32)g_file_store.files.count)
					fname = g_file_store.files[sr.file_index].filename;

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
					send_preview();
				}

				// Draw: filename:line_number  content
				ImGui::SameLine();
				ImGui::SetCursorScreenPos(ImVec2(row_min.x + 4.0f, row_min.y + 2.0f));

				ImVec4 name_col = selected ? sel_text : text_color;
				ImVec4 info_col = selected ? theme_color_dir_selected() : disabled_col;

				// Filename:line_number (draw as one string to avoid alignment issues)
				char label_buf[512];
				snprintf(label_buf, sizeof(label_buf), "%s:%d", fname, sr.line_number);
				ImGui::PushStyleColor(ImGuiCol_Text, name_col);
				ImGui::TextUnformatted(label_buf);
				ImGui::PopStyleColor();

				// Content with highlighting (dimmer, like project name in goto file)
				ImGui::SameLine();
				f32 content_x = ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize("  ").x;
				ImGui::SetCursorScreenPos(ImVec2(content_x, row_min.y + 2.0f));

				i32 hl_count = g_query_lower_copy[0] ? 1 : 0;
				char lt_lower[256];
				str_to_lower(lt_lower, sr.line_text, 255);

				ImVec4 content_col = selected ? theme_color_dir_selected() : disabled_col;
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
	}

	// Settings popup
	{
		f32 dpi = window_get_dpi_scale(host_hwnd());
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(500.0f * dpi, 0.0f));
		if (ImGui::BeginPopupModal("TextSettings", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
			ImGui::Text("Include extensions for content loading");
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

void plugin_goto_text_cancel_search()
{
	cancel_search();
}

void plugin_goto_text_shutdown()
{
	cancel_search();
	if (g_search_tg) {
		g_search_tg->~task_group();
		application_heap()->free(g_search_tg);
		g_search_tg = nullptr;
	}
	clear_state();
}

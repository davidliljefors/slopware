#include "gotofile.h"

#include <atomic>
#include <ppl.h>
#include <string.h>

#include "allocators.h"
#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "app_util.h"
#include "os.h"
#include "os_window.h"
#include "host.h"
#include "imgui_util.h"
#include "theme.h"
#include "types.h"
#include "usn_journal.h"
#include "utf.h"

// Index State

enum IndexState {
	IndexState_Idle,
	IndexState_Indexing,
	IndexState_Done,
	IndexState_Failed,
};

static constexpr i32 MAX_DRIVES = 26;

struct DriveState
{
	char letter; // 'A'-'Z'
	bool available; // NTFS drive present on system
	bool enabled; // user wants it indexed
	bool indexed; // indexing complete and successful
	UsnJournal* journal; // nullptr if not created
	char cache_path[512];
};

static DriveState g_drives[MAX_DRIVES] = { };
static bool g_settings_snapshot[MAX_DRIVES] = { };
static bool g_settings_loaded = false;
static bool g_first_run = false;
static char g_settings_dir[512] = { };

static Thread* g_index_thread = nullptr;
static std::atomic<i32> g_index_state { IndexState_Idle };
static std::atomic<u64> g_index_entry_count { 0 };
static f64 g_index_time_secs = 0.0;

// Progress tracking (written by index thread, read by UI thread)
static std::atomic<i32> g_index_drives_done { 0 };
static std::atomic<i32> g_index_drives_total { 0 };
static std::atomic<char> g_index_current_drive { ' ' };
static f32 g_index_drive_progress = 0.0f;

// Search State

static char g_search_buf[512] = { };
static i32 g_selected_index = 0;

static constexpr i32 SEARCH_MAX_RESULTS = 10000;
static constexpr i32 STAGING_CAPACITY = SEARCH_MAX_RESULTS * 16;

struct SearchEntry {
	u64 frn;
	u8 drive;
	u16 score;
};

static SearchEntry g_results[SEARCH_MAX_RESULTS];
static i32 g_result_count = 0;

static SearchEntry g_staging[STAGING_CAPACITY];
static i32 g_staging_count = 0;
static f64 g_staging_time_secs = 0.0;
static std::atomic<bool> g_staging_ready { false };

static concurrency::task_group* g_search_tg = nullptr;
static std::atomic<bool> g_search_cancel { false };
static char g_bg_query[512] = { };

static std::atomic<bool> g_pending_clear { false };

// Highlight tokens (UI thread only)
static TokenQuery g_hl_name;
static TokenQuery g_hl_dir;

// Forward declarations
static void gotofile_hide();

static ImVec2 gear_button_size()
{
	f32 h = ImGui::GetFrameHeight();
	return ImVec2(h, h);
}

static void cancel_pending_search()
{
	g_search_cancel.store(true, std::memory_order_release);
	if (g_search_tg)
		g_search_tg->wait();
	g_search_cancel.store(false, std::memory_order_release);
	g_staging_ready.store(false, std::memory_order_release);
}

static void clear_search_state()
{
	memset(g_search_buf, 0, sizeof(g_search_buf));
	memset(g_bg_query, 0, sizeof(g_bg_query));
	token_query_clear(&g_hl_name);
	token_query_clear(&g_hl_dir);
	g_result_count = 0;
	g_selected_index = 0;
}

// Search Helpers

static i32 build_dir_path_impl(UsnJournal* j, u64 parent_frn,
	char* buf, i32 buf_size, bool lowercase, bool prepend_drive)
{
	const char* segments[512];
	i32 segment_lens[512];
	i32 seg_count = 0;

	bool hit_root = false;
	u64 cur = parent_frn;
	while (cur != 0 && seg_count < 512) {
		UsnEntry* e = usn_journal_find(j, cur);
		if (!e)
			break;
		segments[seg_count] = lowercase ? usn_entry_lower_utf8(e) : usn_entry_utf8(e);
		segment_lens[seg_count] = usn_entry_utf8_len(e);
		seg_count++;
		if (usn_entry_parent_frn(e) == cur) {
			hit_root = true;
			break;
		}
		cur = usn_entry_parent_frn(e);
	}

	i32 pos = 0;
	if (prepend_drive) {
		buf[pos++] = usn_journal_drive_letter(j);
		buf[pos++] = ':';
		buf[pos++] = '\\';
	}

	i32 top = hit_root ? seg_count - 2 : seg_count - 1;
	for (i32 i = top; i >= 0; i--) {
		if (pos + segment_lens[i] + 1 >= buf_size) {
			buf[0] = '\0';
			return 0;
		}
		memcpy(buf + pos, segments[i], segment_lens[i]);
		pos += segment_lens[i];
		if (i > 0)
			buf[pos++] = '\\';
	}
	buf[pos] = '\0';
	return pos;
}

static i32 build_dir_path_lower_utf8(UsnJournal* j, u64 parent_frn,
	char* buf, i32 buf_size)
{
	return build_dir_path_impl(j, parent_frn, buf, buf_size, true, false);
}

static i32 build_dir_path_utf8(UsnJournal* j, u64 parent_frn,
	char* buf, i32 buf_size)
{
	return build_dir_path_impl(j, parent_frn, buf, buf_size, false, true);
}

static void parse_highlight_tokens()
{
	TokenQuery full;
	token_query_parse(&full, g_search_buf);

	i32 in_index = -1;
	for (i32 i = 0; i < full.count; i++) {
		if (strcmp(full.tokens[i], "in") == 0) {
			in_index = i;
			break;
		}
	}

	// Build name sub-query from tokens before "in"
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

	// Build dir sub-query from tokens after "in"
	i32 dir_start = (in_index >= 0) ? in_index + 1 : full.count;
	char dir_str[TOKEN_QUERY_BUF];
	i32 dpos = 0;
	for (i32 i = dir_start; i < full.count && dpos < TOKEN_QUERY_BUF - 1; i++) {
		if (i > dir_start) dir_str[dpos++] = ' ';
		for (const char* s = full.tokens[i]; *s && dpos < TOKEN_QUERY_BUF - 1; )
			dir_str[dpos++] = *s++;
	}
	dir_str[dpos] = '\0';
	token_query_parse(&g_hl_dir, dir_str);
}

static void do_search_bg()
{
	f64 t0 = timer_now();

	char query[512];
	i32 qlen = (i32)strlen(g_bg_query);
	if (qlen >= (i32)sizeof(query)) {
		qlen = (i32)sizeof(query) - 1;
	}

	memcpy(query, g_bg_query, qlen);
	query[qlen] = '\0';

	for (char* p = query; *p; p++) {
		if (*p >= 'A' && *p <= 'Z')
			*p += 32;
	}

	char* tokens[64];
	i32 token_count = 0;
	{
		char* p = query;
		while (*p) {
			while (*p == ' ')
				p++;
			if (!*p)
				break;
			tokens[token_count++] = p;
			while (*p && *p != ' ')
				p++;
			if (*p)
				*p++ = '\0';
			if (token_count >= 64)
				break;
		}
	}

	if (token_count == 0) {
		g_staging_count = 0;
		g_staging_time_secs = 0.0;
		g_staging_ready.store(true, std::memory_order_release);
		return;
	}

	i32 in_index = -1;
	for (i32 i = 0; i < token_count; i++) {
		if (strcmp(tokens[i], "in") == 0) {
			in_index = i;
			break;
		}
	}

	i32 name_count = (in_index >= 0) ? in_index : token_count;
	i32 dir_start = (in_index >= 0) ? in_index + 1 : token_count;
	i32 dir_count = token_count - dir_start;

	i32 token_len[64];
	bool token_has_dot[64];
	for (i32 i = 0; i < token_count; i++) {
		token_len[i] = (i32)strlen(tokens[i]);
		token_has_dot[i] = (strchr(tokens[i], '.') != nullptr);
	}

	// Sum of name token lengths for match scoring
	i32 name_token_total_len = 0;
	for (i32 i = 0; i < name_count; i++)
		name_token_total_len += token_len[i];

	if (name_count == 0 && dir_count == 0) {
		g_staging_count = 0;
		g_staging_time_secs = 0.0;
		g_staging_ready.store(true, std::memory_order_release);
		return;
	}

	std::atomic<i32> match_count { 0 };
	constexpr i32 CHUNK_SIZE = 8192;

	for (i32 di = 0; di < MAX_DRIVES; di++) {
		DriveState* d = &g_drives[di];
		if (!d->available || !d->enabled || !d->indexed || !d->journal)
			continue;
		if (g_search_cancel.load(std::memory_order_relaxed))
			break;

		u64 jcount = usn_journal_entry_count(d->journal);
		i32 num_chunks = (i32)((jcount + CHUNK_SIZE - 1) / CHUNK_SIZE);

		concurrency::parallel_for(0, num_chunks, [&, di, d, jcount](i32 chunk_idx) {
			if (g_search_cancel.load(std::memory_order_relaxed))
				return;

			u64 start = (u64)chunk_idx * CHUNK_SIZE;
			u64 end = start + CHUNK_SIZE;
			if (end > jcount)
				end = jcount;

			char stackbuffer[4096];

			for (u64 ei = start; ei < end; ei++) {
				if (match_count.load(std::memory_order_relaxed) >= STAGING_CAPACITY)
					return;
				if ((ei & 4095) == 0 && g_search_cancel.load(std::memory_order_relaxed))
					return;

				UsnEntry* e = usn_journal_entry_at(d->journal, ei);
				if (usn_entry_is_directory(e))
					continue;

				if (name_count > 0) {
					const char* lower = usn_entry_lower_utf8(e);
					bool match = true;
					for (i32 i = 0; i < name_count; i++) {
						const char* found = strstr(lower, tokens[i]);
						if (!found) {
							match = false;
							break;
						}
						if (token_has_dot[i]) {
							const char* after = found + token_len[i];
							// Pure extension token (starts with dot): must match to end of filename
							if (tokens[i][0] == '.') {
								if (*after != '\0') {
									match = false;
									break;
								}
							} else if (strchr(after, '.')) {
								match = false;
								break;
							}
						}
					}
					if (!match)
						continue;
				}

				if (dir_count > 0) {
					i32 dlen = build_dir_path_lower_utf8(d->journal, usn_entry_parent_frn(e),
						stackbuffer, 4096);
					if (dlen == 0)
						continue;
					bool match = true;
					for (i32 i = 0; i < dir_count; i++) {
						if (!strstr(stackbuffer, tokens[dir_start + i])) {
							match = false;
							break;
						}
					}
					if (!match)
						continue;
				}

				i32 idx = match_count.fetch_add(1, std::memory_order_relaxed);
				if (idx < STAGING_CAPACITY) {
					i32 fname_len = usn_entry_utf8_len(e);
					g_staging[idx].frn = ei;
					g_staging[idx].drive = (u8)di;
					g_staging[idx].score = (fname_len > 0)
						? (u16)(name_token_total_len * 1000 / fname_len)
						: 0;
				}
			}
		});
	}

	if (!g_search_cancel.load(std::memory_order_relaxed)) {
		i32 count = match_count.load(std::memory_order_relaxed);
		if (count > STAGING_CAPACITY)
			count = STAGING_CAPACITY;

		if (count > SEARCH_MAX_RESULTS)
			count = SEARCH_MAX_RESULTS;

		for (i32 i = 0; i < count; i++) {
			u64 entry_idx = g_staging[i].frn;
			u8 di = g_staging[i].drive;
			g_staging[i].frn = usn_journal_frn_at(g_drives[di].journal, entry_idx);
		}

		// Sort results by match score (descending), tie-break by frn for stability
		if (count > 1) {
			qsort(g_staging, count, sizeof(SearchEntry), [](const void* a, const void* b) -> int {
				const SearchEntry* ea = (const SearchEntry*)a;
				const SearchEntry* eb = (const SearchEntry*)b;
				if (ea->score != eb->score)
					return (eb->score > ea->score) - (eb->score < ea->score);
				return (ea->frn > eb->frn) - (ea->frn < eb->frn);
			});
		}

		g_staging_count = count;
		g_staging_time_secs = timer_now() - t0;
		g_staging_ready.store(true, std::memory_order_release);
	}
}


// ImGui Settings Handler


static void* settings_read_open(ImGuiContext*, ImGuiSettingsHandler*, const char* name)
{
	if (strcmp(name, "Settings") == 0)
		return (void*)1;
	return nullptr;
}

static void settings_read_line(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line)
{
	if (strncmp(line, "EnabledDrives=", 14) == 0) {
		for (i32 i = 0; i < MAX_DRIVES; i++)
			g_drives[i].enabled = false;
		const char* p = line + 14;
		while (*p) {
			if (*p >= 'A' && *p <= 'Z') {
				g_drives[*p - 'A'].enabled = true;
			} else if (*p >= 'a' && *p <= 'z') {
				g_drives[*p - 'a'].enabled = true;
			}
			p++;
		}
		g_settings_loaded = true;
	}
}

static void settings_write_all(ImGuiContext*, ImGuiSettingsHandler*, ImGuiTextBuffer* buf)
{
	buf->appendf("[GotoAll][Settings]\n");
	buf->appendf("EnabledDrives=");
	bool first = true;
	for (i32 i = 0; i < MAX_DRIVES; i++) {
		if (g_drives[i].available && g_drives[i].enabled) {
			if (!first)
				buf->appendf(",");
			buf->appendf("%c", 'A' + i);
			first = false;
		}
	}
	buf->appendf("\n\n");
}


// Index Thread


static const char* g_app_id = nullptr; // set during init

static void index_thread_func(void*)
{
	f64 t0 = timer_now();
	bool any_ok = false;

	i32 total_drives = 0;
	for (i32 i = 0; i < MAX_DRIVES; i++) {
		if (g_drives[i].available && g_drives[i].enabled)
			total_drives++;
	}
	g_index_drives_total.store(total_drives, std::memory_order_relaxed);
	g_index_drives_done.store(0, std::memory_order_relaxed);

	// Build cache paths using app_id directly (already UTF-8)
	for (i32 i = 0; i < MAX_DRIVES; i++) {
		DriveState* d = &g_drives[i];
		if (!d->available || !d->enabled)
			continue;

		g_index_current_drive.store(d->letter, std::memory_order_relaxed);
		g_index_drive_progress = 0.0f;

		d->journal = usn_journal_create();

		char filename[32];
		sprintf_s(filename, 32, "cache_%c.bin", d->letter);
		get_settings_path(d->cache_path, 512, g_app_id, filename);

		bool ok = false;
		if (d->cache_path[0] != '\0') {
			ok = usn_journal_load_cache(d->journal, d->cache_path);
		}
		if (!ok) {
			ok = usn_journal_init(d->journal, d->letter, &g_index_drive_progress);
		}

		d->indexed = ok;
		if (!ok) {
			usn_journal_destroy(d->journal);
			d->journal = nullptr;
		} else {
			any_ok = true;
		}

		g_index_drive_progress = 1.0f;
		g_index_drives_done.fetch_add(1, std::memory_order_relaxed);
	}

	u64 total = 0;
	for (i32 i = 0; i < MAX_DRIVES; i++) {
		if (g_drives[i].indexed && g_drives[i].journal)
			total += usn_journal_entry_count(g_drives[i].journal);
	}

	g_index_time_secs = timer_now() - t0;
	g_index_entry_count.store(total, std::memory_order_relaxed);
	bool ok = any_ok || (total_drives == 0);
	g_index_state.store(ok ? IndexState_Done : IndexState_Failed, std::memory_order_release);
}

static void start_reindex()
{
	cancel_pending_search();

	if (g_index_thread) {
		thread_join(g_index_thread);
		g_index_thread = nullptr;
	}

	for (i32 i = 0; i < MAX_DRIVES; i++) {
		if (g_drives[i].journal) {
			usn_journal_destroy(g_drives[i].journal);
			g_drives[i].journal = nullptr;
		}
		g_drives[i].indexed = false;
	}

	g_result_count = 0;

	g_index_drives_done.store(0, std::memory_order_relaxed);
	g_index_drives_total.store(0, std::memory_order_relaxed);
	g_index_current_drive.store(' ', std::memory_order_relaxed);
	g_index_drive_progress = 0.0f;

	g_index_state.store(IndexState_Indexing, std::memory_order_release);
	g_index_thread = thread_create(index_thread_func, nullptr);
}


// App Callbacks


static void gotofile_init()
{
	char letters[26];
	i32 count = enumerate_ntfs_drives(letters, 26);
	for (i32 i = 0; i < count; i++) {
		i32 idx = letters[i] - 'A';
		g_drives[idx].letter = letters[i];
		g_drives[idx].available = true;
		g_drives[idx].enabled = (letters[i] == 'C');
	}

	ImGuiSettingsHandler handler;
	handler.TypeName = "GotoAll";
	handler.TypeHash = ImHashStr("GotoAll");
	handler.ReadOpenFn = settings_read_open;
	handler.ReadLineFn = settings_read_line;
	handler.WriteAllFn = settings_write_all;
	ImGui::AddSettingsHandler(&handler);

	concurrency::SchedulerPolicy policy(1,
		concurrency::MaxConcurrency, 8);
	concurrency::Scheduler* sched = concurrency::Scheduler::Create(policy);
	sched->Attach();
	sched->Release();

	void* tg_mem = application_heap()->alloc(sizeof(concurrency::task_group), alignof(concurrency::task_group));
	g_search_tg = new (tg_mem) concurrency::task_group();
}

static void gotofile_tick(TempAllocator* frame_allocator)
{
	ImGuiIO& io = ImGui::GetIO();

	if (g_index_state.load(std::memory_order_acquire) == IndexState_Idle) {
		if (!g_settings_loaded) {
			g_first_run = true;
		} else {
			g_index_state.store(IndexState_Indexing, std::memory_order_release);
			g_index_thread = thread_create(index_thread_func, nullptr);
		}
	}

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("Goto All", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

#ifdef _DEBUG
	const u64 MB = 1024 * 1024;
	u64 total = 0;
	for(u32 i = 0; i < MAX_DRIVES; ++i)
	{
		if (g_drives[i].indexed)
		{
			u64 name_bytes = usn_journal_name_bytes_allocated(g_drives[i].journal);
			u64 hash_bytes = usn_journal_lookup_bytes_allocated(g_drives[i].journal);

			total += name_bytes;
			total += hash_bytes;

			ImGui::Text("Drive %c Journal Size: Hash %llu MB, Names %llu MB", g_drives[i].letter, hash_bytes / MB, name_bytes / MB);
		}
	}
	ImGui::Text("Total Journal Size %llu MB", total / MB);
#endif

	i32 state = g_index_state.load(std::memory_order_acquire);

	ImVec4 text_indexing_color = theme_color_text_indexing();
	ImVec4 text_error_color    = theme_color_text_error();

	switch (state) {
	case IndexState_Indexing: {
		i32 done = g_index_drives_done.load(std::memory_order_relaxed);
		i32 total = g_index_drives_total.load(std::memory_order_relaxed);
		char current = g_index_current_drive.load(std::memory_order_relaxed);
		f32 drive_p = g_index_drive_progress;

		f32 overall = 0.0f;
		if (total > 0) {
			overall = ((f32)done + drive_p) / (f32)total;
			if (overall > 1.0f)
				overall = 1.0f;
		}

		char overlay[64];
		if (total > 0 && current != ' ') {
			snprintf(overlay, sizeof(overlay), "Indexing %c: (%d/%d)", current, done + 1, total);
		} else {
			snprintf(overlay, sizeof(overlay), "Indexing...");
		}

		{
			f32 btn_w = gear_button_size().x;
			ImGui::SetNextItemWidth(-(btn_w + ImGui::GetStyle().ItemSpacing.x));
		}
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, text_indexing_color);
		ImGui::ProgressBar(overall, ImVec2(-gear_button_size().x - ImGui::GetStyle().ItemSpacing.x, 0), overlay);
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::Button(ICON_GEAR, gear_button_size())) {
			for (i32 i = 0; i < MAX_DRIVES; i++)
				g_settings_snapshot[i] = g_drives[i].enabled;
			ImGui::OpenPopup("Drive Settings");
		}
		break;
	}
	case IndexState_Done: {
		if (g_staging_ready.load(std::memory_order_acquire)) {
			memcpy(g_results, g_staging, g_staging_count * sizeof(SearchEntry));
			g_result_count = g_staging_count;
			g_staging_ready.store(false, std::memory_order_release);
		}

		if (g_pending_clear.exchange(false, std::memory_order_acquire)) {
			clear_search_state();
			g_staging_count = 0;
			g_staging_ready.store(false, std::memory_order_release);
			ImGui::ClearActiveID();
		}

		if (!ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
			ImGui::SetKeyboardFocusHere();
		{
			f32 btn_w = gear_button_size().x;
			ImGui::SetNextItemWidth(-(btn_w + ImGui::GetStyle().ItemSpacing.x));
		}
		if (ImGui::InputTextWithHint("##search", "Search everywhere...", g_search_buf, sizeof(g_search_buf),
				ImGuiInputTextFlags_EnterReturnsTrue)) {
			if (g_result_count > 0 && g_selected_index < g_result_count) {
				i32 di = g_results[g_selected_index].drive;
				u64 frn = g_results[g_selected_index].frn;
				UsnEntry* entry = usn_journal_find(g_drives[di].journal, frn);
				if (entry) {
					char path_utf8[4096];
					i32 dir_len = build_dir_path_utf8(g_drives[di].journal,
						usn_entry_parent_frn(entry),
						path_utf8, sizeof(path_utf8) - 512);
					if (dir_len > 0) {
						bool ctrl_held = ImGui::GetIO().KeyCtrl;
						if (!ctrl_held) {
							path_utf8[dir_len] = '\\';
							const char* name = usn_entry_utf8(entry);
							i32 name_len = usn_entry_utf8_len(entry);
							memcpy(path_utf8 + dir_len + 1, name, name_len);
							path_utf8[dir_len + 1 + name_len] = '\0';
						}

						clear_search_state();

						shell_open(path_utf8);
						host_hide();
					}
				}
			}
		}

		ImGui::SameLine();
		if (ImGui::Button(ICON_GEAR, gear_button_size())) {
			for (i32 i = 0; i < MAX_DRIVES; i++)
				g_settings_snapshot[i] = g_drives[i].enabled;
			ImGui::OpenPopup("Drive Settings");
		}

		if (strcmp(g_search_buf, g_bg_query) != 0) {
			cancel_pending_search();

			if (g_search_buf[0] == '\0') {
				g_result_count = 0;
				token_query_clear(&g_hl_name);
				token_query_clear(&g_hl_dir);
				memcpy(g_bg_query, g_search_buf, sizeof(g_search_buf));
			} else {
				parse_highlight_tokens();
				memcpy(g_bg_query, g_search_buf, sizeof(g_search_buf));
				g_selected_index = 0;
				g_search_tg->run([]() { do_search_bg(); });
			}
		}

		if (g_result_count > 0) {
			auto key_pressed = [](ImGuiKey key) -> bool {
				ImGuiIO& io = ImGui::GetIO();
				ImGuiKeyData& kd = io.KeysData[key - ImGuiKey_NamedKey_BEGIN];
				if (!kd.Down)
					return false;
				f32 t = kd.DownDuration;
				if (t == 0.0f)
					return true;
				f32 delay = io.KeyRepeatDelay;
				f32 rate = io.KeyRepeatRate;
				if (t < delay)
					return false;
				f32 prev = t - io.DeltaTime;
				if (prev < delay)
					return true;
				i32 count_prev = (i32)((prev - delay) / rate);
				i32 count_now = (i32)((t - delay) / rate);
				return count_now > count_prev;
			};
			if (key_pressed(ImGuiKey_UpArrow)) {
				g_selected_index--;
				if (g_selected_index < 0)
					g_selected_index = 0;
			}
			if (key_pressed(ImGuiKey_DownArrow)) {
				g_selected_index++;
				if (g_selected_index >= g_result_count)
					g_selected_index = g_result_count - 1;
			}
			if (key_pressed(ImGuiKey_PageUp)) {
				g_selected_index -= 15;
				if (g_selected_index < 0)
					g_selected_index = 0;
			}
			if (key_pressed(ImGuiKey_PageDown)) {
				g_selected_index += 15;
				if (g_selected_index >= g_result_count)
					g_selected_index = g_result_count - 1;
			}
			if (key_pressed(ImGuiKey_Home)) {
				g_selected_index = 0;
			}
			if (key_pressed(ImGuiKey_End)) {
				g_selected_index = g_result_count - 1;
			}
			f32 wheel = ImGui::GetIO().MouseWheel;
			if (wheel != 0.0f) {
				g_selected_index -= (i32)(wheel);
				if (g_selected_index < 0)
					g_selected_index = 0;
				if (g_selected_index >= g_result_count)
					g_selected_index = g_result_count - 1;
				ImGui::GetIO().MouseWheel = 0.0f;
			}
		}

		if (g_selected_index >= g_result_count) {
			g_selected_index = (g_result_count > 0) ? g_result_count - 1 : 0;
		}

		if (g_search_buf[0] == '\0') {
			ImGui::TextDisabled("Type to search filenames. Use in to filter directories.");
			ImGui::TextDisabled("Example: main cpp in src utils");
			ImGui::TextDisabled("Esc to hide");
			ImGui::TextDisabled("Ctrl + Alt + P to show");
			ImGui::TextDisabled("Alt + Q to exit");
		} else {
			if (g_result_count == SEARCH_MAX_RESULTS) {
				ImGui::Text("Showing first %d results", SEARCH_MAX_RESULTS);
			} else if (g_result_count > 1) {
				ImGui::Text("%d results", g_result_count);
			} else if (g_result_count == 1) {
				ImGui::Text("1 result");
			}

			ImVec4 text_color    = theme_color_text();
			ImVec4 disabled_color = theme_color_text_disabled();
			ImVec4 hl_color      = theme_color_highlight();
			ImVec4 sel_bg_color  = theme_color_sel_bg();
			ImVec4 sel_text_color = theme_color_sel_text();

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
			ImGui::BeginChild("##results", ImVec2(0, 0), ImGuiChildFlags_None,
				ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			ImGuiListClipper clipper;
			clipper.Begin(g_result_count);
			while (clipper.Step()) {
				for (i32 i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
					i32 di = g_results[i].drive;
					u64 frn = g_results[i].frn;
					UsnJournal* j = g_drives[di].journal;
					UsnEntry* entry = usn_journal_find(j, frn);
					if (!entry)
						continue;

					char dir_utf8[2048];
					build_dir_path_utf8(j, usn_entry_parent_frn(entry),
						dir_utf8, sizeof(dir_utf8));

					ImGui::PushID(i);

					bool is_selected = (i == g_selected_index);

					ImVec2 row_min = ImGui::GetCursorScreenPos();
					f32 row_height = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
					ImVec2 row_max = ImVec2(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_height);

					if (is_selected) {
						ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max,
							ImGui::GetColorU32(sel_bg_color), 3.0f);
					}

					ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y));
					ImGui::Dummy(ImVec2(row_max.x - row_min.x, row_height));
					if (ImGui::IsItemClicked(0)) {
						g_selected_index = i;
					}
					ImGui::SameLine();
					ImGui::SetCursorScreenPos(ImVec2(row_min.x + 4.0f, row_min.y + 2.0f));

					ImVec4 name_color = is_selected ? sel_text_color : text_color;
					ImVec4 dir_color = is_selected ? theme_color_dir_selected() : disabled_color;

					render_highlighted_text(usn_entry_utf8(entry), usn_entry_lower_utf8(entry),
						g_hl_name.tokens, g_hl_name.count,
						name_color, hl_color, theme_highlight_bg_alpha());

					ImGui::SameLine();
					f32 dir_x = ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize("  ").x;
					f32 row_y = row_min.y + 2.0f;
					if (g_hl_dir.count > 0) {
						char dir_lower[2048];
						build_dir_path_lower_utf8(j, usn_entry_parent_frn(entry),
							dir_lower, sizeof(dir_lower));
						ImGui::PushStyleColor(ImGuiCol_Text, dir_color);
						ImGui::SetCursorScreenPos(ImVec2(dir_x, row_y));
						ImGui::TextUnformatted(dir_utf8, dir_utf8 + 3);
						ImGui::PopStyleColor();
						ImGui::SameLine(0, 0);
						ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, row_y));
						render_highlighted_text(dir_utf8 + 3, dir_lower,
							g_hl_dir.tokens, g_hl_dir.count,
							dir_color, hl_color, theme_highlight_bg_alpha());
					} else {
						ImGui::SetCursorScreenPos(ImVec2(dir_x, row_y));
						ImGui::PushStyleColor(ImGuiCol_Text, dir_color);
						ImGui::Text("  %s", dir_utf8);
						ImGui::PopStyleColor();
					}

					ImGui::PopID();
				}
			}

			if (g_result_count > 0) {
				f32 item_height = ImGui::GetTextLineHeightWithSpacing() + 4.0f;
				f32 sel_top = g_selected_index * item_height;
				f32 sel_bot = sel_top + item_height;
				f32 scroll_y = ImGui::GetScrollY();
				f32 visible_h = ImGui::GetWindowHeight();
				if (sel_top < scroll_y) {
					ImGui::SetScrollY(sel_top);
				} else if (sel_bot > scroll_y + visible_h) {
					ImGui::SetScrollY(sel_bot - visible_h);
				}
			}

			ImGui::EndChild();
			ImGui::PopStyleVar();
		}
		break;
	}
	case IndexState_Failed:
		ImGui::TextColored(text_error_color,
			"Indexing failed (run as Administrator for MFT access)");
		ImGui::SameLine();
		if (ImGui::Button("Restart as Admin")) {
			restart_as_admin();
		}
		ImGui::SameLine();
		if (ImGui::Button("Exit")) {
			host_quit();
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_GEAR, gear_button_size())) {
			for (i32 i = 0; i < MAX_DRIVES; i++)
				g_settings_snapshot[i] = g_drives[i].enabled;
			ImGui::OpenPopup("Drive Settings");
		}
		break;
	default:
		break;
	}

	// First-run setup modal
	if (g_first_run) {
		ImGui::OpenPopup("Welcome");
	}
	{
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImVec2 avail = ImGui::GetContentRegionAvail();
		ImGui::SetNextWindowSize(ImVec2(avail.x * 0.4, 0.0f));
		if (ImGui::BeginPopupModal("Welcome", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
			ImGui::Text("Select drives to index:");
			ImGui::Separator();
			for (i32 i = 0; i < MAX_DRIVES; i++) {
				if (!g_drives[i].available)
					continue;
				char label[4] = { g_drives[i].letter, ':', '\0' };
				ImGui::Checkbox(label, &g_drives[i].enabled);
			}
			ImGui::Separator();
			if (ImGui::Button("OK", ImVec2(-1, 0))) {
				g_first_run = false;
				ImGui::MarkIniSettingsDirty();
				g_index_state.store(IndexState_Indexing, std::memory_order_release);
				g_index_thread = thread_create(index_thread_func, nullptr);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	// Drive settings modal
	{
		bool is_indexing = (state == IndexState_Indexing);

		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImVec2 avail = ImGui::GetContentRegionAvail();
		ImGui::SetNextWindowSize(ImVec2(avail.x * 0.4, 0.0f));
		if (ImGui::BeginPopupModal("Drive Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
			ImGui::Text("Indexed Drives");
			ImGui::Separator();
			if (is_indexing)
				ImGui::BeginDisabled();
			for (i32 i = 0; i < MAX_DRIVES; i++) {
				if (!g_drives[i].available)
					continue;
				char label[4] = { g_drives[i].letter, ':', '\0' };
				ImGui::Checkbox(label, &g_drives[i].enabled);
			}
			if (is_indexing)
				ImGui::EndDisabled();
			ImGui::Separator();
			if (g_settings_dir[0] != '\0') {
				if (ImGui::Button("Open Settings Folder", ImVec2(-1, 0))) {
					shell_open_folder(g_settings_dir);
				}
			}
			if (is_indexing)
				ImGui::BeginDisabled();
			if (ImGui::Button("OK", ImVec2(-1, 0))) {
				bool changed = false;
				for (i32 i = 0; i < MAX_DRIVES; i++) {
					if (g_drives[i].enabled != g_settings_snapshot[i]) {
						changed = true;
						break;
					}
				}
				if (changed) {
					ImGui::MarkIniSettingsDirty();
					start_reindex();
				}
				ImGui::CloseCurrentPopup();
			}
			if (is_indexing)
				ImGui::EndDisabled();
			if (ImGui::Button("Cancel", ImVec2(-1, 0))) {
				for (i32 i = 0; i < MAX_DRIVES; i++)
					g_drives[i].enabled = g_settings_snapshot[i];
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	// Escape: exit if indexing failed, otherwise hide
	if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
		if (io.KeyCtrl || g_index_state.load(std::memory_order_acquire) == IndexState_Failed) {
			host_quit();
		} else {
			gotofile_hide();
		}
	}

	ImGui::End();
}

static void gotofile_hide()
{
	clear_search_state();
	g_pending_clear.store(true, std::memory_order_release);
	host_hide();
}



static void gotofile_on_activated()
{
	cancel_pending_search();
	g_pending_clear.store(true, std::memory_order_release);

	IndexState istate = (IndexState)g_index_state.load(std::memory_order_acquire);
	if (istate != IndexState_Done)
		return;

	i32 changes = 0;
	for (i32 i = 0; i < MAX_DRIVES; i++) {
		if (!g_drives[i].indexed || !g_drives[i].journal)
			continue;
		i32 c = usn_journal_update(g_drives[i].journal);
		if (c > 0)
			changes += c;
	}
}

static void gotofile_on_resize()
{
	window_center_on_monitor(host_hwnd());
}

struct CacheSaveData
{
	i32 drive_count;
	struct Entry
	{
		UsnJournal* journal;
		char path[512];
	} entries[MAX_DRIVES];
};

static void cache_save_thread_func(void* user_data)
{
	CacheSaveData* data = (CacheSaveData*)user_data;
	for (i32 i = 0; i < data->drive_count; i++) {
		usn_journal_save_cache(data->entries[i].journal, data->entries[i].path);
		usn_journal_destroy(data->entries[i].journal);
	}
	application_heap()->free(data);
}

static Thread* g_shutdown_save_thread = nullptr;

static void gotofile_begin_shutdown()
{
	g_search_cancel.store(true, std::memory_order_release);
	if (g_search_tg) {
		g_search_tg->wait();
		g_search_tg->~task_group();
		application_heap()->free(g_search_tg);
		g_search_tg = nullptr;
	}

	thread_join(g_index_thread);
	g_index_thread = nullptr;

	g_shutdown_save_thread = nullptr;
	if (g_index_state.load(std::memory_order_relaxed) == IndexState_Done) {
		CacheSaveData* data = (CacheSaveData*)application_heap()->alloc(sizeof(CacheSaveData));
		if (data) {
			data->drive_count = 0;
			for (i32 i = 0; i < MAX_DRIVES; i++) {
				DriveState* d = &g_drives[i];
				if (!d->indexed || !d->journal || d->cache_path[0] == '\0')
					continue;
				CacheSaveData::Entry* e = &data->entries[data->drive_count++];
				e->journal = d->journal;
				memcpy(e->path, d->cache_path, sizeof(d->cache_path));
				d->journal = nullptr;
			}
			if (data->drive_count > 0) {
				g_shutdown_save_thread = thread_create(cache_save_thread_func, data);
			} else {
				application_heap()->free(data);
			}
		}
	}

	for (i32 i = 0; i < MAX_DRIVES; i++) {
		if (g_drives[i].journal) {
			usn_journal_destroy(g_drives[i].journal);
			g_drives[i].journal = nullptr;
		}
	}
}

static void gotofile_wait_for_shutdown()
{
	if (g_shutdown_save_thread) {
		thread_join(g_shutdown_save_thread);
		g_shutdown_save_thread = nullptr;
	}
}

static void gotofile_hotkey_show()
{
	host_show();
}

static void gotofile_hotkey_quit()
{
	host_quit();
}


// Hotkeys


static constexpr i32 HOTKEY_COUNT = 2;
static AppHotkey g_hotkeys[HOTKEY_COUNT] = {
	{ 1, HOTKEY_MOD_CONTROL | HOTKEY_MOD_ALT, 'P', gotofile_hotkey_show },
	{ 2, HOTKEY_MOD_ALT,               'Q', gotofile_hotkey_quit },
};


// Public: return the App descriptor


App* gotofile_get_app()
{
	static App app = {};
	app.name              = "Goto All";
	app.app_id            = "gotofile";
	app.init              = gotofile_init;
	app.tick              = gotofile_tick;
	app.on_activated      = gotofile_on_activated;
	app.on_resize         = gotofile_on_resize;
	app.begin_shutdown    = gotofile_begin_shutdown;
	app.wait_for_shutdown = gotofile_wait_for_shutdown;
	app.hotkeys           = g_hotkeys;
	app.hotkey_count      = HOTKEY_COUNT;
	app.initial_width     = 0;
	app.initial_height    = 0;
	app.title_bar_height        = 0;
	app.title_bar_buttons_width = 0;
	app.use_system_tray   = true;
	g_app_id              = app.app_id;
	return &app;
}

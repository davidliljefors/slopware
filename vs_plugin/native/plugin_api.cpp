#include "plugin_api.h"
#include "plugin_internal.h"
#include "plugin_host.h"
#include "plugin_goto_file.h"
#include "plugin_goto_text.h"
#include "plugin_goto_text_in_file.h"
#include "string_util.h"

#include <string.h>
#include <stdio.h>
#include <atomic>
#include <ppl.h>
#include <time.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "allocators.h"
#include "app_util.h"
#include "hashmap.h"
#include "murmurhash3.inl"
#include "os.h"
#include "utf.h"

static void debug_console_init()
{
	AllocConsole();
	SetConsoleTitleA("GotoSlop Debug");
	FILE* dummy;
	freopen_s(&dummy, "CONOUT$", "w", stdout);
}

// --------------------------------------------------------------------------
// Global state
// --------------------------------------------------------------------------

PluginFileStore g_file_store;
PluginCallbacks g_callbacks;

static i32               g_preload_version = -1;
static Thread*           g_preload_thread = nullptr;
std::atomic<bool>        g_preloading { false };
std::atomic<bool>        g_waiting_for_vs { false };

static Thread*           g_refresh_thread = nullptr;
static std::atomic<bool> g_refreshing { false };

static bool g_initialized = false;

// --------------------------------------------------------------------------
// Directory watchers
// --------------------------------------------------------------------------

struct DirWatcherEntry
{
	DirWatcher* watcher;  // os primitive
	char*       dir_path; // owned by path_arena
};

static Array<DirWatcherEntry> g_watchers;
static HashMap<i32> g_path_to_index; // full path hash -> file index
static Lock g_watchers_lock;          // protects g_watchers + g_path_to_index

static u64 hash_path_lower(const char* path, i32 len)
{
	char buf[1024];
	i32 n = len < (i32)sizeof(buf) - 1 ? len : (i32)sizeof(buf) - 1;
	for (i32 i = 0; i < n; i++) {
		char c = path[i];
		if (c >= 'A' && c <= 'Z') c += 32;
		if (c == '/') c = '\\';
		buf[i] = c;
	}
	buf[n] = '\0';
	return murmurhash3_64(buf, n);
}

struct DirWatchPollCtx
{
	char* dir_path;
};

static void on_file_changed(const char* name_utf8, void* user_data)
{
	DirWatchPollCtx* ctx = (DirWatchPollCtx*)user_data;

	char full[1024];
	i32 dlen = (i32)strlen(ctx->dir_path);
	i32 nlen = (i32)strlen(name_utf8);
	if (dlen + 1 + nlen >= (i32)sizeof(full)) return;
	memcpy(full, ctx->dir_path, dlen);
	full[dlen] = '\\';
	memcpy(full + dlen + 1, name_utf8, nlen + 1);

	u64 h = hash_path_lower(full, dlen + 1 + nlen);
	i32* idx = g_path_to_index.find(h);
	if (idx) {
		ReadGuard rg(&g_file_store.files_lock);
		if (*idx < (i32)g_file_store.files.count)
			g_file_store.files[*idx].dirty = true;
	}
}

// Internal: stop watchers, caller must hold g_watchers_lock for write.
static void dir_watchers_stop_locked()
{
	for (u64 i = 0; i < g_watchers.count; i++)
		dir_watcher_destroy(g_watchers[i].watcher);
	g_watchers.clear();
	g_path_to_index.reset();
}

void dir_watchers_start()
{
	WriteGuard wg(&g_watchers_lock);

	dir_watchers_stop_locked();

	PluginFileStore* fs = &g_file_store;
	ReadGuard rg(&fs->files_lock);
	i32 file_count = (i32)fs->files.count;
	if (file_count == 0) return;

	g_path_to_index.clear();
	g_path_to_index.reserve(file_count);
	g_watchers.init(application_heap());

	HashMap<i32> dir_seen;
	dir_seen.reserve(file_count / 4);

	for (i32 i = 0; i < file_count; i++) {
		PluginFile* pf = &fs->files[i];
		if (!pf->full_path) continue;

		i32 path_len = (i32)strlen(pf->full_path);
		g_path_to_index.insert_or_assign(hash_path_lower(pf->full_path, path_len), i);

		i32 dir_len = 0;
		for (i32 j = path_len - 1; j >= 0; j--) {
			if (pf->full_path[j] == '\\' || pf->full_path[j] == '/') {
				dir_len = j;
				break;
			}
		}
		if (dir_len == 0) continue;

		u64 dir_hash = hash_path_lower(pf->full_path, dir_len);
		if (dir_seen.contains(dir_hash)) continue;

		lock_write(&fs->path_arena_lock);
		char* dir_copy = (char*)fs->path_arena.alloc(dir_len + 1);
		unlock_write(&fs->path_arena_lock);
		memcpy(dir_copy, pf->full_path, dir_len);
		dir_copy[dir_len] = '\0';

		DirWatcher* w = dir_watcher_create(dir_copy);
		if (!w) continue;

		dir_seen.insert_or_assign(dir_hash, (i32)g_watchers.count);
		DirWatcherEntry entry = { w, dir_copy };
		g_watchers.push(entry);
	}

	PLUGIN_LOG("Watching %llu directories for changes", g_watchers.count);

	dir_seen.reset();
}

void dir_watchers_stop()
{
	WriteGuard wg(&g_watchers_lock);
	dir_watchers_stop_locked();
}

void dir_watchers_poll()
{
	ReadGuard rg(&g_watchers_lock);
	for (u64 i = 0; i < g_watchers.count; i++) {
		DirWatchPollCtx ctx = { g_watchers[i].dir_path };
		dir_watcher_poll(g_watchers[i].watcher, on_file_changed, &ctx);
	}
}

// --------------------------------------------------------------------------
// PluginFileStore operations
// --------------------------------------------------------------------------

static const char* s_default_include_extensions =
	".h, .hpp, .hxx, .hh,"
	" .c, .cpp, .cxx, .cc, .inl, .ixx,"
	" .cs,"
	" .hlsl, .fx, .fxh, .glsl, .vert, .frag, .comp, .geom,"
	" .natvis";

void file_store_init(PluginFileStore* fs)
{
	lock_init(&fs->files_lock);
	lock_init(&fs->path_arena_lock);
	lock_init(&fs->content_arena_lock);
	lock_init(&fs->hot_buffers_lock);
	fs->files.init(application_heap());
	fs->hot_buffers.init(application_heap());

	i32 len = (i32)strlen(s_default_include_extensions);
	memcpy(fs->include_extensions, s_default_include_extensions, len + 1);

	plugin_load_saved_extensions();
}

void file_store_clear(PluginFileStore* fs)
{
	fs->files.clear();

	lock_write(&fs->path_arena_lock);
	fs->path_arena.free_all();
	unlock_write(&fs->path_arena_lock);

	lock_write(&fs->content_arena_lock);
	fs->content_arena.free_all();
	unlock_write(&fs->content_arena_lock);

	lock_write(&fs->hot_buffers_lock);
	for (u64 i = 0; i < fs->hot_buffers.count; i++)
		application_heap()->free(fs->hot_buffers[i].data);
	fs->hot_buffers.clear();
	unlock_write(&fs->hot_buffers_lock);
}

void file_store_reset_content(PluginFileStore* fs)
{
	ReadGuard rg(&fs->files_lock);
	for (u64 i = 0; i < fs->files.count; i++) {
		fs->files[i].content = nullptr;
		fs->files[i].content_size = 0;
		fs->files[i].last_write_time = 0;
		fs->files[i].dirty = false;
	}

	lock_write(&fs->content_arena_lock);
	fs->content_arena.free_all();
	unlock_write(&fs->content_arena_lock);

	lock_write(&fs->hot_buffers_lock);
	for (u64 i = 0; i < fs->hot_buffers.count; i++)
		application_heap()->free(fs->hot_buffers[i].data);
	fs->hot_buffers.clear();
	unlock_write(&fs->hot_buffers_lock);
}

void file_store_destroy(PluginFileStore* fs)
{
	file_store_clear(fs);
	fs->files.destroy();

	lock_write(&fs->hot_buffers_lock);
	fs->hot_buffers.destroy();
	unlock_write(&fs->hot_buffers_lock);
}

static constexpr const char* SETTINGS_APP_NAME = "GotoSlop";
static constexpr const char* SETTINGS_EXT_FILE = "extensions.txt";

void plugin_save_extensions()
{
	char path[512];
	if (!get_settings_path(path, sizeof(path), SETTINGS_APP_NAME, SETTINGS_EXT_FILE))
		return;
	OsFile f = os_file_open_write(path);
	if (!os_file_valid(f)) return;
	i32 len = (i32)strlen(g_file_store.include_extensions);
	os_file_write(f, g_file_store.include_extensions, len);
	os_file_close(f);
}

void plugin_load_saved_extensions()
{
	char path[512];
	if (!get_settings_path(path, sizeof(path), SETTINGS_APP_NAME, SETTINGS_EXT_FILE))
		return;
	OsFile f = os_file_open_read(path);
	if (!os_file_valid(f)) return;
	i64 size = os_file_size(f);
	if (size <= 0 || size >= (i64)sizeof(g_file_store.include_extensions)) {
		os_file_close(f);
		return;
	}
	i32 bytes_read = 0;
	if (os_file_read(f, g_file_store.include_extensions, (i32)size, &bytes_read))
		g_file_store.include_extensions[bytes_read] = '\0';
	os_file_close(f);
}

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

static void log_dll_timestamp()
{
	HMODULE hm = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)log_dll_timestamp, &hm);
	if (!hm) return;
	auto* dos = (IMAGE_DOS_HEADER*)hm;
	auto* nt = (IMAGE_NT_HEADERS*)((char*)hm + dos->e_lfanew);
	time_t t = (time_t)nt->FileHeader.TimeDateStamp;
	struct tm tm;
	gmtime_s(&tm, &t);
	char buf[64];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
	PLUGIN_LOG("DLL built: %s", buf);
}

static void ensure_init()
{
	if (g_initialized) return;
	g_initialized = true;
#ifdef ENABLE_CONSOLE
	debug_console_init();
#endif
	log_dll_timestamp();
	lock_init(&g_watchers_lock);
	file_store_init(&g_file_store);
}

bool plugin_should_ignore_file(const char* filename)
{
	// Find last dot
	const char* dot = nullptr;
	for (const char* p = filename; *p; p++)
		if (*p == '.') dot = p;
	if (!dot) return true; // no extension -> not included

	char ext[64];
	str_to_lower(ext, dot, 63);

	// Parse the include list and match
	char buf[1024];
	i32 len = (i32)strlen(g_file_store.include_extensions);
	if (len >= (i32)sizeof(buf)) len = (i32)sizeof(buf) - 1;
	memcpy(buf, g_file_store.include_extensions, len);
	buf[len] = '\0';

	char* ctx = nullptr;
	char* tok = strtok_s(buf, ",", &ctx);
	while (tok) {
		while (*tok == ' ' || *tok == '\t') tok++;
		char* end = tok + strlen(tok) - 1;
		while (end > tok && (*end == ' ' || *end == '\t')) *end-- = '\0';

		if (*tok) {
			char tok_lower[64];
			str_to_lower(tok_lower, tok, 63);
			if (strcmp(ext, tok_lower) == 0) return false; // included
		}
		tok = strtok_s(nullptr, ",", &ctx);
	}
	return true; // not in include list -> ignore
}

void plugin_load_file_contents()
{
	PluginFileStore* fs = &g_file_store;
	ReadGuard rg(&fs->files_lock);
	i32 count = (i32)fs->files.count;

	concurrency::parallel_for(0, count, [fs](i32 i) {
		PluginFile* f = &fs->files[i];

		if (f->content) return;
		if (plugin_should_ignore_file(f->filename)) return;

		OsFile h = os_file_open_seq(f->full_path);
		if (!os_file_valid(h)) return;

		i64 size64 = os_file_size(h);
		if (size64 <= 0 || size64 > 64 * 1024 * 1024) {
			os_file_close(h);
			return;
		}

		i32 size = (i32)size64;
		lock_write(&fs->content_arena_lock);
		char* buf = (char*)fs->content_arena.alloc(size + 1);
		unlock_write(&fs->content_arena_lock);

		i32 bytes_read = 0;
		if (os_file_read(h, buf, size, &bytes_read) && bytes_read == size) {
			buf[size] = '\0';
			f->content = buf;
			f->content_size = size;
		}
		os_file_close(h);

		f->last_write_time = os_file_last_write_time(f->full_path);
	});
}

static void refresh_stale_content_impl()
{
	PluginFileStore* fs = &g_file_store;

	// Poll directory watchers to mark dirty files
	dir_watchers_poll();

	ReadGuard rg(&fs->files_lock);
	i32 count = (i32)fs->files.count;

	if (count == 0) return;

	f64 t0 = timer_now();
	std::atomic<i32> reloaded { 0 };

	bool preloading = g_preloading.load(std::memory_order_acquire);

	concurrency::parallel_for(0, count, [fs, &reloaded, preloading](i32 i) {
		PluginFile* f = &fs->files[i];

		if (!f->content) {
			// Not loaded yet -- load it now unless preload thread is handling it
			if (preloading) return;
			if (plugin_should_ignore_file(f->filename)) return;

			OsFile h = os_file_open_seq(f->full_path);
			if (!os_file_valid(h)) return;

			i64 size64 = os_file_size(h);
			if (size64 <= 0 || size64 > 64 * 1024 * 1024) {
				os_file_close(h);
				return;
			}

			i32 size = (i32)size64;
			char* buf = (char*)application_heap()->alloc(size + 1);

			i32 bytes_read = 0;
			if (os_file_read(h, buf, size, &bytes_read) && bytes_read == size) {
				buf[size] = '\0';
				f->content = buf;
				f->content_size = size;
				f->last_write_time = os_file_last_write_time(f->full_path);

				lock_write(&fs->hot_buffers_lock);
				HotBuffer hb = { buf, size + 1 };
				fs->hot_buffers.push(hb);
				unlock_write(&fs->hot_buffers_lock);

				reloaded.fetch_add(1, std::memory_order_relaxed);
			} else {
				application_heap()->free(buf);
			}
			os_file_close(h);
			f->dirty = false;
			return;
		}

		// Only check files flagged dirty by the directory watcher
		if (!f->dirty) return;
		f->dirty = false;

		i64 current_time = os_file_last_write_time(f->full_path);
		if (current_time == 0 || current_time == f->last_write_time) return;

		reloaded.fetch_add(1, std::memory_order_relaxed);

		// File changed -- reload into a new heap buffer
		OsFile h = os_file_open_seq(f->full_path);
		if (!os_file_valid(h)) return;

		i64 size64 = os_file_size(h);
		if (size64 <= 0 || size64 > 64 * 1024 * 1024) {
			os_file_close(h);
			return;
		}

		i32 size = (i32)size64;
		char* buf = (char*)application_heap()->alloc(size + 1);

		i32 bytes_read = 0;
		if (os_file_read(h, buf, size, &bytes_read) && bytes_read == size) {
			buf[size] = '\0';

			char* old_content = f->content;

			f->content = buf;
			f->content_size = size;
			f->last_write_time = current_time;

			lock_write(&fs->hot_buffers_lock);
			i32 old_index = -1;
			for (i32 j = 0; j < (i32)fs->hot_buffers.count; j++) {
				if (fs->hot_buffers[j].data == old_content) {
					old_index = j;
					break;
				}
			}
			if (old_index >= 0) {
				application_heap()->free(fs->hot_buffers[old_index].data);
				fs->hot_buffers[old_index].data = buf;
				fs->hot_buffers[old_index].capacity = size + 1;
			} else {
				HotBuffer hb = { buf, size + 1 };
				fs->hot_buffers.push(hb);
			}
			unlock_write(&fs->hot_buffers_lock);
		} else {
			application_heap()->free(buf);
		}
		os_file_close(h);
	});

	f64 elapsed_ms = (timer_now() - t0) * 1000.0;
	i32 changed = reloaded.load(std::memory_order_relaxed);
	if (changed > 0) {
		PLUGIN_LOG("refresh: %d reloaded, %.1fms", changed, elapsed_ms);
	}
}

static void refresh_thread_func(void*)
{
	refresh_stale_content_impl();
	g_refreshing.store(false, std::memory_order_release);
}

void begin_refresh()
{
	if (g_refresh_thread) {
		PLUGIN_LOG("begin_refresh: joining previous refresh thread");
		thread_join(g_refresh_thread);
		g_refresh_thread = nullptr;
	}
	g_refreshing.store(true, std::memory_order_release);
	g_refresh_thread = thread_create(refresh_thread_func, nullptr);
}

void ensure_refresh_done()
{
	if (g_refresh_thread) {
		thread_join(g_refresh_thread);
		g_refresh_thread = nullptr;
	}
}

// --------------------------------------------------------------------------
// Exported API
// --------------------------------------------------------------------------

extern "C" {

static PluginFile make_plugin_file(const char* src, const char* proj)
{
	PluginFileStore* fs = &g_file_store;
	i32 path_len = (i32)strlen(src);

	const char* name = str_filename(src);
	i32 name_offset = (i32)(name - src);
	i32 name_len = path_len - name_offset;

	i32 proj_len = proj ? (i32)strlen(proj) : 0;

	lock_write(&fs->path_arena_lock);
	char* path_copy = (char*)fs->path_arena.alloc(path_len + 1);
	char* lower_copy = (char*)fs->path_arena.alloc(name_len + 1);
	char* proj_copy = nullptr;
	char* proj_lower = nullptr;
	if (proj_len > 0) {
		proj_copy = (char*)fs->path_arena.alloc(proj_len + 1);
		proj_lower = (char*)fs->path_arena.alloc(proj_len + 1);
	}
	unlock_write(&fs->path_arena_lock);

	memcpy(path_copy, src, path_len + 1);
	str_to_lower(lower_copy, path_copy + name_offset, name_len);

	if (proj_len > 0) {
		memcpy(proj_copy, proj, proj_len + 1);
		str_to_lower(proj_lower, proj, proj_len);
	}

	PluginFile pf = {};
	pf.full_path = path_copy;
	pf.filename = path_copy + name_offset;
	pf.filename_lower = lower_copy;
	pf.project_name = proj_copy;
	pf.project_lower = proj_lower;
	pf.content = nullptr;
	pf.content_size = 0;
	pf.last_write_time = 0;
	pf.dirty = false;
	return pf;
}

PLUGIN_API void __stdcall plugin_begin_query_files(void)
{
	ensure_init();
	g_waiting_for_vs.store(true, std::memory_order_release);
	PLUGIN_LOG("begin_query_files: waiting for VS");
}

PLUGIN_API void __stdcall plugin_set_solution_files(const char** files, const char** projects, int count)
{
	ensure_init();
	g_waiting_for_vs.store(false, std::memory_order_release);

	PLUGIN_LOG("SetSolutionFiles: %d files", count);

	// Drain any in-flight refresh thread first -- it may be polling watchers
	if (g_refreshing.load(std::memory_order_acquire)) {
		PLUGIN_LOG("SetSolutionFiles: draining active refresh thread");
		f64 t0 = timer_now();
		ensure_refresh_done();
		PLUGIN_LOG("SetSolutionFiles: refresh drained in %.1fms", (timer_now() - t0) * 1000.0);
	} else {
		ensure_refresh_done();
	}

	// Cancel any running searches -- they hold ReadGuard on files_lock
	// and access path_arena pointers that file_store_clear will free.
	plugin_goto_file_cancel_search();
	plugin_goto_text_cancel_search();

	// Stop watchers before clearing files
	dir_watchers_stop();

	{
		PluginFileStore* fs = &g_file_store;
		WriteGuard wg(&fs->files_lock);
		file_store_clear(fs);

		fs->files.reserve(count);
		for (int i = 0; i < count; i++)
			fs->files.push(make_plugin_file(files[i], projects ? projects[i] : nullptr));

		fs->version++;
	}

	// Start watching the directories these files live in
	dir_watchers_start();

	PLUGIN_LOG("SetSolutionFiles: done, version=%d", g_file_store.version);
}

PLUGIN_API void __stdcall plugin_set_callback(PluginSelectionCallback callback)
{
	ensure_init();
	g_callbacks.selection = callback;
}

PLUGIN_API void __stdcall plugin_set_preview_callback(PluginSelectionCallback callback)
{
	ensure_init();
	g_callbacks.preview = callback;
}

PLUGIN_API void __stdcall plugin_show_goto_file(void)
{
	ensure_init();
	plugin_host_show(PluginMode_GoToFile);
}

PLUGIN_API void __stdcall plugin_show_goto_text(void)
{
	ensure_init();
	begin_refresh();
	plugin_host_show(PluginMode_GoToText);
}

PLUGIN_API void __stdcall plugin_show_goto_text_in_file(const char* file_path)
{
	ensure_init();
	begin_refresh();
	plugin_goto_text_in_file_set_file(file_path);
	plugin_host_show(PluginMode_GoToTextInFile);
}

PLUGIN_API int __stdcall plugin_is_window_open(void)
{
	return plugin_host_is_visible() ? 1 : 0;
}

PLUGIN_API void __stdcall plugin_refresh_stale_content(void)
{
	ensure_init();
	begin_refresh();
}

static void preload_thread_func(void*)
{
	f64 t0 = timer_now();
	plugin_load_file_contents();
	f64 elapsed_ms = (timer_now() - t0) * 1000.0;

	ReadGuard rg(&g_file_store.files_lock);
	i32 count = (i32)g_file_store.files.count;
	f64 arena_mb = g_file_store.content_arena.get_bytes_allocated() / (1024.0 * 1024.0);
	PLUGIN_LOG("Loaded %d files in %.0f ms  |  Arena: %.1f MB", count, elapsed_ms, arena_mb);

	g_preloading.store(false);
}

PLUGIN_API void __stdcall plugin_preload_content(void)
{
	ensure_init();

	if (g_preload_version == g_file_store.version && !g_preloading.load())
		return;

	if (g_preload_thread) {
		thread_join(g_preload_thread);
		g_preload_thread = nullptr;
	}

	g_preload_version = g_file_store.version;
	g_preloading.store(true);
	g_preload_thread = thread_create(preload_thread_func, nullptr);
}

PLUGIN_API void __stdcall plugin_shutdown(void)
{
	if (!g_initialized) return;

	PLUGIN_LOG("shutdown");

	// Cancel searches before draining threads
	plugin_goto_file_cancel_search();
	plugin_goto_text_cancel_search();

	ensure_refresh_done();
	dir_watchers_stop();

	if (g_preload_thread) {
		thread_join(g_preload_thread);
		g_preload_thread = nullptr;
	}

	plugin_host_shutdown();

	{
		WriteGuard wg(&g_file_store.files_lock);
		file_store_destroy(&g_file_store);
	}

	g_callbacks = {};
	g_initialized = false;
}

} // extern "C"

// --------------------------------------------------------------------------
// DllMain
// --------------------------------------------------------------------------

//extern "C" __declspec(dllimport) int __stdcall DisableThreadLibraryCalls(void*);

extern "C" int __stdcall DllMain(void* hModule, unsigned long reason, void*)
{
	if (reason == 1) // DLL_PROCESS_ATTACH
		DisableThreadLibraryCalls((HMODULE)hModule);
	return 1;
}

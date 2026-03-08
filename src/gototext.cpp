#include <atomic>
#include <ppl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocators.h"
#include "app.h"
#include "array.h"
#include "gototext.h"
#include "host.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_util.h"
#include "lz4.h"
#include "lz4hc.h"
#include "app_util.h"
#include "os.h"
#include "os_window.h"
#include "theme.h"
#include "types.h"


// Types and Constants


static constexpr i32 MAX_PATH_LEN = 1024;
static constexpr i32 MAX_SEARCH_LEN = 512;
static constexpr i32 DIR_ENUM_THREADS = 8;
static constexpr i32 MAX_RESULTS_DISPLAY = 10000;
static constexpr i32 MAX_SEARCH_RESULTS = 10000;
static constexpr i32 SMALL_FILE_THRESHOLD = 10 * 1024; // 10 KB
static constexpr i32 SCRATCH_BUFFER_SIZE = 10 * 1024 * 1024; // 10 MB

enum Codec {
	Codec_None = 0,
	Codec_LZ4 = 1,
	Codec_LZ4HC = 2,
};


// Data Structures


struct CompressedFile
{
	char* path;
	u8* compressed_data;
	i32 compressed_size;
	i32 original_size;
	i32 codec;
};

struct SearchResult
{
	i32 file_index;
	i32 line_number;
	i32 col;
	char line_text[256];
};


// Global State


// File types filter
static char g_file_types[MAX_SEARCH_LEN] = ".dbx, .json, .h, .cpp, .cs, .ddf, .xml, .build";

// File store
static Array<CompressedFile> g_files;
static Lock g_files_lock;

// Arena for file data (paths + compressed content)
static BumpAllocator g_file_arena(64 * 1024 * 1024);
static Lock g_file_arena_lock;

// Search state
static char g_search_query[MAX_SEARCH_LEN] = {};
static char g_search_query_lower[MAX_SEARCH_LEN] = {};
static char* g_search_token_ptrs[1] = {};
static Array<SearchResult> g_search_results;
static Lock g_search_results_lock;
static std::atomic<bool> g_searching { false };
static std::atomic<i32> g_search_file_count { 0 };
static std::atomic<i32> g_search_files_done { 0 };

// Grace period: keep showing stale results for a short window after a new
// search starts so the UI doesn't flash empty between consecutive searches.
static f64 g_search_started_at = 0.0;
static constexpr f64 SEARCH_GRACE_PERIOD = 0.05; // 50 ms
static i32 g_last_result_count = 0;
static i32 g_last_file_count = 0;
static f64 g_last_search_ms = 0.0;
static bool g_search_has_run = false;

// Directory input
static char g_directory[MAX_PATH_LEN] = {};
static std::atomic<bool> g_scanning { false };
static std::atomic<i32> g_scan_file_count { 0 };
static std::atomic<i32> g_scan_files_loaded { 0 };
static std::atomic<i32> g_enum_dirs_done { 0 };
static std::atomic<i32> g_enum_files_found { 0 };

// Scratch buffers (one per hardware thread)
static i32 g_num_threads = 0;
static u8** g_scratch_buffers = nullptr;

// Status
static char g_status[1024] = "Ready. Enter a directory and press Scan.";

// Size stats
static i64 g_total_original_bytes = 0;
static i64 g_total_compressed_bytes = 0;

// Options
static bool g_skip_compress_small = true;
static i32 g_compress_codec = Codec_LZ4;
static i32 g_lz4hc_level = LZ4HC_CLEVEL_DEFAULT;
static bool g_parallel_search = true;

// Settings
static char g_settings_dir[512] = {};

// Path exclusion patterns
static constexpr i32 MAX_EXCLUSION_PATTERNS = 64;
static constexpr i32 MAX_EXCLUSION_LEN = 256;
static char g_exclusion_patterns[MAX_EXCLUSION_PATTERNS][MAX_EXCLUSION_LEN] = {};
static i32 g_exclusion_count = 0;
static char g_new_exclusion[MAX_EXCLUSION_LEN] = {};

// Thread handles
static Thread* g_scan_thread = nullptr;
static Thread* g_search_thread = nullptr;


// Forward Declarations


static void scan_directory_thread(void* user_data);
static void perform_search_thread(void* user_data);
static void scan_directory();
static void perform_search();
static void init_scratch_buffers();
static void free_all_files();


// Utility


static bool matches_tracked_extension(const char* filename)
{
	const char* ext = os_path_find_extension(filename);
	if (!ext || !*ext)
		return false;

	char buf[MAX_SEARCH_LEN];
	strncpy_s(buf, g_file_types, sizeof(buf));
	buf[sizeof(buf) - 1] = '\0';

	char* ctx = nullptr;
	char* tok = strtok_s(buf, ",", &ctx);
	while (tok) {
		while (*tok == ' ' || *tok == '\t')
			tok++;
		char* end = tok + strlen(tok) - 1;
		while (end > tok && (*end == ' ' || *end == '\t'))
			*end-- = '\0';

		if (*tok) {
			if (_stricmp(ext, tok) == 0)
				return true;
		}
		tok = strtok_s(nullptr, ",", &ctx);
	}
	return false;
}


// Wildcard Pattern Matching


static char normalize_path_char(char c)
{
	if (c >= 'A' && c <= 'Z') return (char)(c + 32);
	if (c == '/') return '\\';
	return c;
}

// Simple glob match: '*' matches zero or more of any character.
// Case-insensitive; treats '/' and '\\' as equivalent separators.
static bool wildcard_match(const char* pattern, const char* text)
{
	const char* p = pattern;
	const char* t = text;
	const char* star_p = nullptr;
	const char* star_t = nullptr;

	while (*t) {
		if (*p == '*') {
			star_p = ++p;
			star_t = t;
		} else if (normalize_path_char(*p) == normalize_path_char(*t)) {
			p++;
			t++;
		} else if (star_p) {
			p = star_p;
			t = ++star_t;
		} else {
			return false;
		}
	}
	while (*p == '*') p++;
	return *p == '\0';
}

static bool path_excluded(const char* path)
{
	for (i32 i = 0; i < g_exclusion_count; i++) {
		if (wildcard_match(g_exclusion_patterns[i], path))
			return true;
	}
	return false;
}


// Parallel Directory Enumeration


struct ParallelEnumThreadResult
{
	Array<char*> files;
	char padding[64]; // avoid false sharing
};

struct ParallelEnumSharedCtx
{
	Lock* queue_lock;
	CondVar* cv_work;
	Array<char*>* work_queue;
	std::atomic<i32>* active_threads;
	bool* done;
	ParallelEnumThreadResult* thread_results;
	BumpAllocator* path_allocator;
	Lock* path_allocator_lock;
	std::atomic<i32>* dirs_done;
	std::atomic<i32>* files_found;
};

struct ParallelEnumWorkerCtx
{
	ParallelEnumSharedCtx* shared;
	i32 id;
};

static void parallel_enum_worker(ParallelEnumWorkerCtx* ctx)
{
	ParallelEnumSharedCtx* s = ctx->shared;
	i32 thread_id = ctx->id;

	constexpr u32 BUF_SIZE = 64 * 1024;
	u8* buffer = (u8*)virtual_alloc(BUF_SIZE);
	if (!buffer)
		return;

	Array<char*> local_dirs;
	local_dirs.init(application_heap());

	Array<char*> found_dirs;
	found_dirs.init(application_heap());

	for (;;) {
		local_dirs.clear();

		lock_write(s->queue_lock);
		while (s->work_queue->empty() && !*s->done) {
			if (s->active_threads->load() == 0) {
				*s->done = true;
				condvar_wake_all(s->cv_work);
				break;
			}
			condvar_wait(s->cv_work, s->queue_lock, 50);
		}
		if (*s->done && s->work_queue->empty()) {
			unlock_write(s->queue_lock);
			break;
		}
		i32 batch = (i32)s->work_queue->count;
		if (batch > 64)
			batch = 64;
		for (i32 i = (i32)s->work_queue->count - batch; i < (i32)s->work_queue->count; i++)
			local_dirs.push(s->work_queue->data[i]);
		s->work_queue->count -= batch;
		s->active_threads->fetch_add(1);
		unlock_write(s->queue_lock);

		found_dirs.clear();
		for (u64 d = 0; d < local_dirs.count; d++) {
			char* dir = local_dirs[d];
			i32 dir_len = (i32)strlen(dir);
			OsFile hDir = os_dir_open(dir);
			if (!os_file_valid(hDir))
				continue;

			DirEnumState des;
			dir_enum_init(&des, hDir, buffer, BUF_SIZE);
			DirEntry entry;
			while (dir_enum_next(&des, &entry)) {
				bool skip = (entry.name_len >= 1 && entry.name[0] == '.');
				if (skip)
					continue;

				if (entry.is_directory) {
					// dir + '\\' + name + '\\' + '\0' (extra backslash for exclusion check)
					i32 full_len = dir_len + 1 + entry.name_len;
					lock_write(s->path_allocator_lock);
					char* full_path = (char*)s->path_allocator->alloc(full_len + 2);
					unlock_write(s->path_allocator_lock);
					memcpy(full_path, dir, dir_len);
					full_path[dir_len] = '\\';
					memcpy(full_path + dir_len + 1, entry.name, entry.name_len);
					full_path[full_len] = '\0';

					// Check with trailing backslash so */pattern/* matches
					full_path[full_len] = '\\';
					full_path[full_len + 1] = '\0';
					bool excluded = path_excluded(full_path);
					full_path[full_len] = '\0';
					if (!excluded)
						found_dirs.push(full_path);
				} else {
					if (matches_tracked_extension(entry.name)) {
						i32 full_len = dir_len + 1 + entry.name_len;
						lock_write(s->path_allocator_lock);
						char* full_path = (char*)s->path_allocator->alloc(full_len + 1);
						unlock_write(s->path_allocator_lock);
						memcpy(full_path, dir, dir_len);
						full_path[dir_len] = '\\';
						memcpy(full_path + dir_len + 1, entry.name, entry.name_len);
						full_path[full_len] = '\0';
						if (!path_excluded(full_path)) {
							s->thread_results[thread_id].files.push(full_path);
							s->files_found->fetch_add(1);
						}
					}
				}
			}

			os_file_close(hDir);
			s->dirs_done->fetch_add(1);
		}

		lock_write(s->queue_lock);
		if (!found_dirs.empty()) {
			for (u64 i = 0; i < found_dirs.count; i++)
				s->work_queue->push(found_dirs[i]);
			condvar_wake_all(s->cv_work);
		}
		s->active_threads->fetch_sub(1);
		if (s->work_queue->empty() && s->active_threads->load() == 0) {
			*s->done = true;
			condvar_wake_all(s->cv_work);
		}
		unlock_write(s->queue_lock);
	}

	virtual_free(buffer);
	local_dirs.destroy();
	found_dirs.destroy();
}

static void parallel_enum_worker_proc(void* p)
{
	parallel_enum_worker((ParallelEnumWorkerCtx*)p);
}

static void enumerate_directory_parallel(
	const char* root_dir,
	Array<char*>* out_paths,
	BumpAllocator* path_arena,
	Lock* arena_lock)
{
	Lock queue_lock;
	lock_init(&queue_lock);

	Array<char*> work_queue;
	work_queue.init(application_heap());
	{
		i32 len = (i32)strlen(root_dir);
		lock_write(arena_lock);
		char* root_copy = (char*)path_arena->alloc(len + 1);
		unlock_write(arena_lock);
		memcpy(root_copy, root_dir, len + 1);
		work_queue.push(root_copy);
	}

	std::atomic<i32> active_threads { 0 };
	CondVar cv_work;
	condvar_init(&cv_work);
	bool done = false;

	ParallelEnumThreadResult* thread_results = (ParallelEnumThreadResult*)application_heap()->alloc(
		DIR_ENUM_THREADS * sizeof(ParallelEnumThreadResult), alignof(ParallelEnumThreadResult));
	for (i32 i = 0; i < DIR_ENUM_THREADS; i++)
		thread_results[i].files.init(application_heap());

	ParallelEnumSharedCtx shared = {};
	shared.queue_lock = &queue_lock;
	shared.cv_work = &cv_work;
	shared.work_queue = &work_queue;
	shared.active_threads = &active_threads;
	shared.done = &done;
	shared.thread_results = thread_results;
	shared.path_allocator = path_arena;
	shared.path_allocator_lock = arena_lock;
	shared.dirs_done = &g_enum_dirs_done;
	shared.files_found = &g_enum_files_found;

	Thread* threads[DIR_ENUM_THREADS - 1];
	ParallelEnumWorkerCtx ctxs[DIR_ENUM_THREADS];
	for (i32 i = 0; i < DIR_ENUM_THREADS; i++)
		ctxs[i] = { &shared, i };

	for (i32 i = 0; i < DIR_ENUM_THREADS - 1; i++)
		threads[i] = thread_create(parallel_enum_worker_proc, &ctxs[i + 1]);

	// Current thread is worker 0
	parallel_enum_worker(&ctxs[0]);

	for (i32 i = 0; i < DIR_ENUM_THREADS - 1; i++)
		thread_join(threads[i]);

	// Merge per-thread results
	for (i32 i = 0; i < DIR_ENUM_THREADS; i++) {
		for (u64 j = 0; j < thread_results[i].files.count; j++)
			out_paths->push(thread_results[i].files[j]);
		thread_results[i].files.destroy();
	}
	application_heap()->free(thread_results);
	work_queue.destroy();
}


// File Loading & Compression


static bool read_and_compress_file(const char* path, CompressedFile* out,
	BumpAllocator* arena, Lock* arena_lock,
	u8* scratch, i32 scratch_size,
	f64* out_read_s = nullptr, f64* out_compress_s = nullptr)
{
	f64 t0 = timer_now();

	OsFile h_file = os_file_open_seq(path);
	if (!os_file_valid(h_file))
		return false;

	i64 file_size = os_file_size(h_file);
	if (file_size <= 0 || file_size > 256 * 1024 * 1024) {
		os_file_close(h_file);
		return false;
	}

	i32 original_size = (i32)file_size;

	// Read into scratch buffer if it fits, otherwise temp VirtualAlloc
	u8* raw_data;
	bool allocated_raw = false;
	if (original_size <= scratch_size) {
		raw_data = scratch;
	} else {
		raw_data = (u8*)virtual_alloc(original_size);
		if (!raw_data) {
			os_file_close(h_file);
			return false;
		}
		allocated_raw = true;
	}

	i32 bytes_read = 0;
	bool ok = os_file_read(h_file, raw_data, original_size, &bytes_read);
	os_file_close(h_file);

	if (!ok || bytes_read != original_size) {
		if (allocated_raw) virtual_free(raw_data);
		return false;
	}

	f64 t1 = timer_now();

	i32 path_len = (i32)strlen(path);

	// No compression path
	if ((g_skip_compress_small && original_size <= SMALL_FILE_THRESHOLD) ||
		g_compress_codec == Codec_None) {
		if (out_read_s)
			*out_read_s = t1 - t0;
		if (out_compress_s)
			*out_compress_s = 0.0;

		lock_write(arena_lock);
		out->path = (char*)arena->alloc(path_len + 1);
		out->compressed_data = (u8*)arena->alloc(original_size + 1);
		unlock_write(arena_lock);
		memcpy(out->path, path, path_len + 1);
		memcpy(out->compressed_data, raw_data, original_size);
		out->compressed_data[original_size] = '\0';
		if (allocated_raw) virtual_free(raw_data);
		out->compressed_size = original_size;
		out->original_size = original_size;
		out->codec = Codec_None;
		return true;
	}

	// Compress from scratch/temp buffer directly into the arena
	i32 max_compressed = LZ4_compressBound(original_size);
	lock_write(arena_lock);
	out->path = (char*)arena->alloc(path_len + 1);
	out->compressed_data = (u8*)arena->alloc(max_compressed);
	unlock_write(arena_lock);
	memcpy(out->path, path, path_len + 1);

	i32 compressed_size;
	i32 used_codec;
	if (g_compress_codec == Codec_LZ4HC) {
		compressed_size = LZ4_compress_HC(
			(const char*)raw_data, (char*)out->compressed_data,
			original_size, max_compressed, g_lz4hc_level);
		used_codec = Codec_LZ4HC;
	} else {
		compressed_size = LZ4_compress_default(
			(const char*)raw_data, (char*)out->compressed_data,
			original_size, max_compressed);
		used_codec = Codec_LZ4;
	}
	if (allocated_raw) virtual_free(raw_data);

	f64 t2 = timer_now();

	if (compressed_size <= 0)
		return false;

	if (out_read_s)
		*out_read_s = t1 - t0;
	if (out_compress_s)
		*out_compress_s = t2 - t1;

	out->compressed_size = compressed_size;
	out->original_size = original_size;
	out->codec = used_codec;

	return true;
}


// Scan Directory


static void scan_directory()
{
	g_scanning.store(true);
	g_scan_file_count.store(0);
	g_scan_files_loaded.store(0);
	g_enum_dirs_done.store(0);
	g_enum_files_found.store(0);

	// Clear stale search results before freeing files they reference
	{
		WriteGuard wg(&g_search_results_lock);
		g_search_results.clear();
	}

	free_all_files();
	g_search_has_run = false;

	snprintf(g_status, sizeof(g_status), "Scanning directory...");

	Array<char*> file_paths;
	file_paths.init(application_heap());

	f64 enum_start = timer_now();

	BumpAllocator enum_arena(64 * 1024 * 1024);
	Lock enum_arena_lock;
	lock_init(&enum_arena_lock);

	snprintf(g_status, sizeof(g_status),
		"Running NtQueryDirectoryFile parallel (%d threads)...", DIR_ENUM_THREADS);
	enumerate_directory_parallel(g_directory, &file_paths, &enum_arena, &enum_arena_lock);

	f64 enum_ms = (timer_now() - enum_start) * 1000.0;

	i32 total_files = (i32)file_paths.count;
	g_scan_file_count.store(total_files);

	if (total_files == 0) {
		snprintf(g_status, sizeof(g_status), "No matching files found.");
		g_scanning.store(false);
		file_paths.destroy();
		return;
	}

	snprintf(g_status, sizeof(g_status), "Loading %d files...", total_files);

	// Pre-allocate file slots
	{
		WriteGuard wg(&g_files_lock);
		g_files.resize_zeroed(total_files);
	}

	init_scratch_buffers();

	// Per-slot locks so each thread claims its own scratch buffer
	struct SlotData
	{
		Lock lock;
		char padding[64];
	};
	SlotData* slots = (SlotData*)application_heap()->alloc(
		g_num_threads * sizeof(SlotData), alignof(SlotData));
	for (i32 s = 0; s < g_num_threads; s++) {
		lock_init(&slots[s].lock);
		memset(slots[s].padding, 0, sizeof(slots[s].padding));
	}

	// Load and compress files in parallel
	std::atomic<i32> success_count { 0 };
	std::atomic<i64> total_original { 0 };
	std::atomic<i64> total_compressed { 0 };
	std::atomic<i64> total_read_us { 0 };
	std::atomic<i64> total_compress_us { 0 };

	f64 wall_start = timer_now();

	auto scan_body = [&](i32 i) {
		// Acquire a scratch buffer slot
		i32 slot = -1;
		while (slot == -1) {
			for (i32 s = 0; s < g_num_threads; s++) {
				if (try_lock_write(&slots[s].lock)) {
					slot = s;
					break;
				}
			}
			if (slot == -1)
				thread_yield();
		}

		CompressedFile cf = {};
		f64 read_s = 0, compress_s = 0;
		if (read_and_compress_file(file_paths[i], &cf, &g_file_arena, &g_file_arena_lock,
				g_scratch_buffers[slot], SCRATCH_BUFFER_SIZE,
				&read_s, &compress_s)) {
			{
				WriteGuard wg(&g_files_lock);
				g_files[i] = cf;
			}

			success_count.fetch_add(1);
			total_original.fetch_add(cf.original_size);
			total_compressed.fetch_add(cf.compressed_size);
			total_read_us.fetch_add((i64)(read_s * 1000000.0));
			total_compress_us.fetch_add((i64)(compress_s * 1000000.0));
		}

		unlock_write(&slots[slot].lock);
		g_scan_files_loaded.fetch_add(1);
	};
	if (g_parallel_search)
		concurrency::parallel_for(0, total_files, scan_body);
	else
		for (i32 i = 0; i < total_files; i++) scan_body(i);

	application_heap()->free(slots);

	// Free all enumerated path strings in bulk
	enum_arena.free_all();
	file_paths.destroy();

	f64 wall_ms = (timer_now() - wall_start) * 1000.0;

	g_total_original_bytes = total_original.load();
	g_total_compressed_bytes = total_compressed.load();

	f64 orig_mb = g_total_original_bytes / (1024.0 * 1024.0);
	f64 comp_mb = g_total_compressed_bytes / (1024.0 * 1024.0);
	f64 ratio = g_total_original_bytes > 0
		? (g_total_compressed_bytes * 100.0 / g_total_original_bytes)
		: 0.0;
	f64 read_ms_total = total_read_us.load() / 1000.0;
	f64 compress_ms_total = total_compress_us.load() / 1000.0;
	snprintf(g_status, sizeof(g_status),
		"Loaded %d / %d files  |  Original: %.1f MB  Compressed: %.1f MB  (%.0f%%)  |  "
		"Enum: %.0f ms  Wall: %.0f ms  Read: %.0f ms  Compress: %.0f ms",
		success_count.load(), total_files, orig_mb, comp_mb, ratio,
		enum_ms, wall_ms, read_ms_total, compress_ms_total);
	g_scanning.store(false);
}


// Scratch Buffers


static void init_scratch_buffers()
{
	if (g_num_threads > 0)
		return;
	g_num_threads = get_processor_count();
	g_scratch_buffers = (u8**)application_heap()->alloc(g_num_threads * sizeof(u8*));
	for (i32 i = 0; i < g_num_threads; i++)
		g_scratch_buffers[i] = (u8*)virtual_alloc(SCRATCH_BUFFER_SIZE);
}


// Search


static u8 g_lower[256];

static void init_lower_table()
{
	for (i32 i = 0; i < 256; i++)
		g_lower[i] = (u8)i;
	for (i32 i = 'A'; i <= 'Z'; i++)
		g_lower[i] = (u8)(i + 32);
}

static void build_bmh_skip_table(const char* needle_lower, i32 needle_len, i32 skip[256])
{
	for (i32 i = 0; i < 256; i++)
		skip[i] = needle_len;
	for (i32 i = 0; i < needle_len - 1; i++)
		skip[(u8)needle_lower[i]] = needle_len - 1 - i;
}

static const char* bmh_search_i(const char* haystack, i32 haystack_len,
	const char* needle_lower, i32 needle_len,
	const i32 skip[256])
{
	if (needle_len == 0)
		return haystack;
	if (needle_len > haystack_len)
		return nullptr;

	i32 last = needle_len - 1;
	i32 limit = haystack_len - needle_len;
	i32 pos = 0;

	while (pos <= limit) {
		i32 j = last;
		while (j >= 0 && g_lower[(u8)haystack[pos + j]] == (u8)needle_lower[j])
			j--;
		if (j < 0)
			return haystack + pos;
		pos += skip[g_lower[(u8)haystack[pos + last]]];
	}
	return nullptr;
}


// Text Helpers


// Count '\n' bytes in [start, end).
static i32 count_newlines(const char* start, const char* end)
{
	i32 count = 0;
	for (const char* p = start; p < end; p++) {
		if (*p == '\n') count++;
	}
	return count;
}

// Find the next '\n' or '\0' at or after 'start', never going past buf_end.
// Returns pointer TO the delimiter.
static const char* find_line_end(const char* start, const char* buf_end)
{
	for (const char* p = start; p < buf_end; p++) {
		if (*p == '\n' || *p == '\0') return p;
	}
	return buf_end;
}

// Find start of the line containing 'pos' (scan backward for '\n').
static const char* find_line_start(const char* pos, const char* buf_start)
{
	const char* p = pos;
	while (p > buf_start && *(p - 1) != '\n')
		p--;
	return p;
}

static void perform_search()
{
	g_searching.store(true);
	{
		WriteGuard wg(&g_search_results_lock);
		g_search_results.clear();
	}

	i32 file_count;
	{
		ReadGuard rg(&g_files_lock);
		file_count = (i32)g_files.count;
	}

	g_search_file_count.store(file_count);
	g_search_files_done.store(0);

	if (file_count == 0 || g_search_query[0] == '\0') {
		g_searching.store(false);
		return;
	}

	init_scratch_buffers();

	f64 search_start = timer_now();

	i32 query_len = (i32)strlen(g_search_query);

	char query_lower[MAX_SEARCH_LEN];
	for (i32 i = 0; i < query_len; i++)
		query_lower[i] = (char)g_lower[(u8)g_search_query[i]];
	query_lower[query_len] = '\0';

	i32 bmh_skip[256];
	build_bmh_skip_table(query_lower, query_len, bmh_skip);

	std::atomic<i32> total_results { 0 };

	// Build an index striped by thread count so each thread's contiguous
	// range gets an even mix of large and small files.  Sort descending by
	// size, then deal round-robin into num_threads groups.  With
	// static_partitioner each thread gets one contiguous range.
	i32 num_threads = g_num_threads;
	i32* tmp_idx = (i32*)application_heap()->alloc(
		file_count * sizeof(i32), alignof(i32));
	for (i32 i = 0; i < file_count; i++)
		tmp_idx[i] = i;

	struct SortCtx { CompressedFile* files; };
	SortCtx sort_ctx = { g_files.data };
	qsort_s(tmp_idx, file_count, sizeof(i32),
		[](void* ctx, const void* a, const void* b) -> int {
			auto* files = ((SortCtx*)ctx)->files;
			i32 sa = files[*(const i32*)a].original_size;
			i32 sb = files[*(const i32*)b].original_size;
			return (sb > sa) - (sb < sa); // descending
		}, &sort_ctx);

	// Stripe: deal sorted files round-robin into num_threads groups laid out
	// contiguously.  Group g gets sorted positions g, g+T, g+2T, ...
	// With static_partitioner each thread gets a contiguous range containing
	// an even mix of large and small files.
	i32* sorted_idx = (i32*)application_heap()->alloc(
		file_count * sizeof(i32), alignof(i32));
	i32 pos = 0;
	for (i32 t = 0; t < num_threads; t++) {
		for (i32 j = t; j < file_count; j += num_threads)
			sorted_idx[pos++] = tmp_idx[j];
	}
	application_heap()->free(tmp_idx);

	// Each PPL thread claims a unique slot via atomic counter on first
	// entry.  static_partitioner guarantees each thread keeps its range,
	// so the slot is assigned once and reused for every file in that range.
	std::atomic<i32> next_slot { 0 };

	auto search_body = [&](i32 idx) {
		// One-time slot assignment for this thread
		thread_local i32 my_slot = -1;
		if (my_slot == -1 || my_slot >= num_threads)
			my_slot = next_slot.fetch_add(1, std::memory_order_relaxed);

		i32 i = sorted_idx[idx];
		if (total_results.load(std::memory_order_relaxed) >= MAX_SEARCH_RESULTS)
			return;
		if (!g_files[i].compressed_data)
			return;

		const CompressedFile& file = g_files[i];

		bool use_scratch = false;
		u8* scratch = nullptr;
		i32 decompressed = 0;

		if (file.codec == Codec_None) {
			scratch = file.compressed_data;
			decompressed = file.original_size;
		} else {
			i32 needed = file.original_size + 1;
			use_scratch = (needed <= SCRATCH_BUFFER_SIZE);
			if (use_scratch) {
				scratch = g_scratch_buffers[my_slot];
			} else {
				scratch = (u8*)virtual_alloc(needed);
				if (!scratch)
					return;
			}

			i32 decomp_capacity = use_scratch ? SCRATCH_BUFFER_SIZE - 1 : needed - 1;
			decompressed = LZ4_decompress_safe(
				(const char*)file.compressed_data,
				(char*)scratch,
				file.compressed_size,
				decomp_capacity);
		}

		if (decompressed > 0) {
			if (file.codec != Codec_None)
				scratch[decompressed] = '\0';

			// Search the whole buffer with BMH instead of scanning byte-by-byte
			// for newlines.  Files with zero matches (the vast majority) now
			// cost one fast BMH call instead of touching every byte looking for
			// '\n'.  Line boundaries are only resolved around actual matches.
			const char* data     = (const char*)scratch;
			const char* data_end = data + decompressed;

			const char* search_pos  = data;
			i32 search_remaining    = decompressed;
			const char* nl_counted  = data;   // newlines counted up to here
			i32 line_num            = 1;

			while (search_remaining >= query_len) {
				if (total_results.load(std::memory_order_relaxed) >= MAX_SEARCH_RESULTS)
					break;

				const char* match = bmh_search_i(search_pos, search_remaining,
					query_lower, query_len, bmh_skip);
				if (!match)
					break;

				// Resolve line number by counting newlines since last position
				line_num += count_newlines(nl_counted, match);

				// Resolve line boundaries around the match
				const char* ls = find_line_start(match, data);
				const char* le = find_line_end(match + query_len, data_end);
				i32 line_len = (i32)(le - ls);

				SearchResult sr;
				sr.file_index   = i;
				sr.line_number  = line_num;
				sr.col          = (i32)(match - ls);

				i32 copy_len = line_len;
				if (copy_len >= (i32)sizeof(sr.line_text))
					copy_len = (i32)sizeof(sr.line_text) - 1;
				memcpy(sr.line_text, ls, copy_len);
				sr.line_text[copy_len] = '\0';

				// Trim leading whitespace
				char* trimmed = sr.line_text;
				while (*trimmed == ' ' || *trimmed == '\t')
					trimmed++;
				if (trimmed != sr.line_text)
					memmove(sr.line_text, trimmed, strlen(trimmed) + 1);

				{
					WriteGuard wg(&g_search_results_lock);
					g_search_results.push(sr);
				}
				total_results.fetch_add(1, std::memory_order_relaxed);

				// Advance past end of this line to avoid duplicate matches
				if (le < data_end) {
					nl_counted = le + 1;  // past the '\n'
					line_num++;           // account for that '\n'
					search_pos = (char*)(le + 1);
					search_remaining = (i32)(data_end - search_pos);
				} else {
					break;
				}
			}
		}

		if (file.codec != Codec_None && !use_scratch)
			virtual_free(scratch);
		g_search_files_done.fetch_add(1, std::memory_order_relaxed);
	};
	if (g_parallel_search)
		concurrency::parallel_for(0, file_count, search_body,
			concurrency::static_partitioner());
	else
		for (i32 i = 0; i < file_count; i++) search_body(i);

	application_heap()->free(sorted_idx);

	// Trim to hard cap
	bool capped;
	{
		WriteGuard wg(&g_search_results_lock);
		capped = (i32)g_search_results.count >= MAX_SEARCH_RESULTS;
		if (capped)
			g_search_results.count = MAX_SEARCH_RESULTS;
	}

	f64 search_ms = (timer_now() - search_start) * 1000.0;
	g_last_search_ms = search_ms;
	g_search_has_run = true;
	g_searching.store(false);
}


// Clean up


static void free_all_files()
{
	WriteGuard wg(&g_files_lock);
	g_files.clear();
	lock_write(&g_file_arena_lock);
	g_file_arena.free_all();
	unlock_write(&g_file_arena_lock);
}


// Thread entry points (for thread_create)


static void scan_directory_thread(void*)
{
	scan_directory();
}

static void perform_search_thread(void*)
{
	perform_search();
}


static constexpr i32 TITLE_BAR_HEIGHT = 32;
static constexpr i32 TITLE_BAR_BUTTONS = 2;


// App Interface -- tick (main UI)


static void gototext_tick(TempAllocator*)
{
	ImGuiIO& io = ImGui::GetIO();
	f32 dpi_scale = window_get_dpi_scale(host_hwnd());

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("Find it faster", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

	draw_title_bar_buttons(TITLE_BAR_HEIGHT);
	draw_title_bar_title(TITLE_BAR_HEIGHT, "Find it faster", "- Fast file content search");

	f32 title_h = floorf((f32)TITLE_BAR_HEIGHT * dpi_scale);

	// Memory usage
	if (g_total_compressed_bytes > 0) {
		f64 comp_mb = g_total_compressed_bytes / (1024.0 * 1024.0);
		f64 orig_mb = g_total_original_bytes / (1024.0 * 1024.0);
		char mem_text[128];
		snprintf(mem_text, sizeof(mem_text), "Cache: %.1f MB (%.1f MB orig)", comp_mb, orig_mb);
		f32 text_width = ImGui::CalcTextSize(mem_text).x;
		f32 btn_sz = floorf((f32)TITLE_BAR_HEIGHT * dpi_scale);
		f32 right_margin = 2.0f * btn_sz + ImGui::GetStyle().WindowPadding.x + 4.0f * dpi_scale;
		ImGui::SameLine(ImGui::GetWindowWidth() - text_width - right_margin);
		ImGui::TextDisabled("%s", mem_text);
	}

	ImGui::SetCursorPosY(title_h);
	ImGui::Separator();

	// Directory input
	f32 frame_h = ImGui::GetFrameHeight();
	f32 spacing = ImGui::GetStyle().ItemSpacing.x;
	// Reserve: Browse(80) + Scan(100) + gear(frame_h) + 3 gaps
	f32 reserve = 80.0f + 100.0f + frame_h + spacing * 3.0f;

	ImGui::Text("Directory:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reserve);
	bool dir_enter = ImGui::InputTextWithHint("##dir", "Enter directory... C:\\...",
		g_directory, sizeof(g_directory), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();

	bool scanning = g_scanning.load();
	bool browse_picked = false;
	if (scanning)
		ImGui::BeginDisabled();
	if (ImGui::Button("Browse...", ImVec2(80, 0)))
		browse_picked = browse_for_folder(g_directory, sizeof(g_directory), host_hwnd());
	ImGui::SameLine();
	bool scan_clicked = ImGui::Button("Scan", ImVec2(100, 0));
	if (scanning)
		ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(ICON_GEAR, ImVec2(frame_h, frame_h)))
		ImGui::OpenPopup("Settings");

	// File types filter
	ImGui::Text("File types:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(600.0f);
	if (scanning)
		ImGui::BeginDisabled();
	ImGui::InputText("##filetypes", g_file_types, sizeof(g_file_types));
	if (scanning)
		ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::TextDisabled("(comma separated, e.g. .json, .xml, .h, .cpp)");

	if ((scan_clicked || dir_enter || browse_picked) && !scanning && g_directory[0] != '\0') {
		if (g_search_thread) {
			thread_join(g_search_thread);
			g_search_thread = nullptr;
		}
		if (g_scan_thread) {
			thread_join(g_scan_thread);
			g_scan_thread = nullptr;
		}
		g_scan_thread = thread_create(scan_directory_thread, nullptr);
	}

	// Progress during scan
	if (scanning) {
		i32 total = g_scan_file_count.load();
		i32 loaded = g_scan_files_loaded.load();
		if (total > 0) {
			f32 progress = (f32)loaded / (f32)total;
			char overlay[64];
			snprintf(overlay, sizeof(overlay), "%d / %d", loaded, total);
			ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
		} else {
			i32 dirs_done = g_enum_dirs_done.load();
			i32 files_found = g_enum_files_found.load();
			ImGui::Text("Scanning directory... %d dirs scanned, %d files found",
				dirs_done, files_found);
		}
	}

	ImGui::Spacing();

	// Search input
	ImGui::Text("Search:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	bool search_trigger = ImGui::InputText("##search", g_search_query, sizeof(g_search_query));

	bool searching = g_searching.load();
	if (search_trigger && !searching && !scanning && g_search_query[0] != '\0') {
		// Build lowered query for highlight matching
		i32 qlen = (i32)strlen(g_search_query);
		for (i32 i = 0; i < qlen; i++)
			g_search_query_lower[i] = (char)g_lower[(u8)g_search_query[i]];
		g_search_query_lower[qlen] = '\0';
		g_search_token_ptrs[0] = g_search_query_lower;

		if (g_search_thread) {
			thread_join(g_search_thread);
			g_search_thread = nullptr;
		}
		g_search_started_at = timer_now();
		g_search_thread = thread_create(perform_search_thread, nullptr);
	}

	// Determine visible result count -- during the grace period after a new
	// search starts, keep showing the previous snapshot so the table doesn't
	// flash empty between consecutive searches.
	bool in_grace = searching && (timer_now() - g_search_started_at) < SEARCH_GRACE_PERIOD;
	i32 vis_result_count;
	i32 vis_file_count;
	{
		ReadGuard rg(&g_search_results_lock);
		i32 live_results = (i32)g_search_results.count;
		i32 live_files = (i32)g_files.count;
		if (in_grace && live_results == 0 && g_last_result_count > 0) {
			// Still inside grace window with no new results yet -- use cached counts
			vis_result_count = g_last_result_count;
			vis_file_count = g_last_file_count;
		} else {
			vis_result_count = live_results;
			vis_file_count = live_files;
			g_last_result_count = live_results;
			g_last_file_count = live_files;
		}
	}

	// Status + results line (single row so the table position stays stable)
	{
		bool show_search_results = !scanning && vis_file_count > 0
			&& (vis_result_count > 0 || searching || g_search_has_run);
		if (show_search_results) {
			if (searching) {
				ImGui::TextColored(theme_color_text_indexing(), "Found %d results%s",
					vis_result_count,
					vis_result_count >= MAX_SEARCH_RESULTS ? " (capped)" : "");
			} else {
				ImGui::TextColored(theme_color_text_indexing(), "Found %d results%s in %.0f ms",
					vis_result_count,
					vis_result_count >= MAX_SEARCH_RESULTS ? " (capped)" : "",
					g_last_search_ms);
			}
		} else {
			ImGui::TextColored(theme_color_text_indexing(), "%s", g_status);
		}

		// Right side: search progress on the same line
		if (searching) {
			i32 s_total = g_search_file_count.load();
			i32 s_done = g_search_files_done.load();
			if (s_total > 0) {
				f32 bar_w = 200.0f;
				ImGui::SameLine(ImGui::GetWindowWidth() - bar_w - ImGui::GetStyle().WindowPadding.x);
				f32 s_progress = (f32)s_done / (f32)s_total;
				char s_overlay[64];
				snprintf(s_overlay, sizeof(s_overlay), "%d / %d", s_done, s_total);
				ImGui::ProgressBar(s_progress, ImVec2(bar_w, ImGui::GetTextLineHeight()), s_overlay);
			} else {
				ImGui::SameLine();
				ImGui::TextColored(theme_color_text_indexing(), "Searching...");
			}
		}
	}

	ImGui::Separator();

	// Results table
	{
		ReadGuard rg(&g_search_results_lock);
		i32 result_count = in_grace && (i32)g_search_results.count == 0
			? vis_result_count : (i32)g_search_results.count;
		i32 file_count = (i32)g_files.count;
		if (result_count > 0 && !scanning && file_count > 0) {
			i32 display_count = result_count < MAX_RESULTS_DISPLAY
				? result_count
				: MAX_RESULTS_DISPLAY;

			if (ImGui::BeginTable("Results", 3,
					ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
					ImGui::GetContentRegionAvail())) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 0.35f);
				ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 60.0f);
				ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch, 0.65f);
				ImGui::TableHeadersRow();

				ImGuiListClipper clipper;
				clipper.Begin(display_count);
				while (clipper.Step()) {
					for (i32 row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
						SearchResult& sr = g_search_results[row];
						ImGui::TableNextRow();

						// File column
						ImGui::TableSetColumnIndex(0);
						const char* display_path = "(unknown)";
						if (sr.file_index < file_count && g_files[sr.file_index].path)
							display_path = g_files[sr.file_index].path;

						const char* filename = display_path;
						const char* sep = strrchr(display_path, '\\');
						if (sep)
							filename = sep + 1;

						ImGui::PushID(row);
						if (ImGui::Selectable("##row", false,
								ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
							if (sr.file_index < file_count && g_files[sr.file_index].path)
								shell_open(display_path);
						}
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", display_path);

						i32 hl_count = g_search_query_lower[0] ? 1 : 0;
						ImVec4 text_color = theme_color_text();
						ImVec4 hl_color = theme_color_highlight();
						f32 hl_alpha = theme_highlight_bg_alpha();

						// Draw highlighted filename on top of the selectable
						ImGui::SameLine();
						{
							i32 fname_len = (i32)strlen(filename);
							char fname_lower[256];
							for (i32 c = 0; c < fname_len; c++)
								fname_lower[c] = (char)g_lower[(u8)filename[c]];
							fname_lower[fname_len] = '\0';
							render_highlighted_text(filename, fname_lower,
								g_search_token_ptrs, hl_count,
								text_color, hl_color, hl_alpha);
						}
						ImGui::PopID();

						// Line column
						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%d", sr.line_number);

						// Content column
						ImGui::TableSetColumnIndex(2);
						{
							i32 line_len = (i32)strlen(sr.line_text);
							char line_lower[256];
							for (i32 c = 0; c < line_len; c++)
								line_lower[c] = (char)g_lower[(u8)sr.line_text[c]];
							line_lower[line_len] = '\0';
							render_highlighted_text(sr.line_text, line_lower,
								g_search_token_ptrs, hl_count,
								text_color, hl_color, hl_alpha);
						}
					}
				}
				ImGui::EndTable();
			}
		}
	}

	// Settings modal
	{
		ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(460.0f * dpi_scale, 0.0f));
		if (ImGui::BeginPopupModal("Settings", nullptr,
				ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
			ImGui::Text("Compression");
			ImGui::Separator();

			{
				char small_label[128];
				snprintf(small_label, sizeof(small_label),
					"Skip compression for small files (<%d KB)",
					SMALL_FILE_THRESHOLD / 1024);
				ImGui::Checkbox(small_label, &g_skip_compress_small);
			}

			ImGui::SetNextItemWidth(140.0f);
			const char* codec_items[] = { "None", "LZ4", "LZ4 HC" };
			if (scanning)
				ImGui::BeginDisabled();
			ImGui::Combo("Codec", &g_compress_codec, codec_items,
				IM_ARRAYSIZE(codec_items));
			if (scanning)
				ImGui::EndDisabled();

			if (g_compress_codec == Codec_LZ4HC) {
				ImGui::SetNextItemWidth(140.0f);
				if (scanning)
					ImGui::BeginDisabled();
				ImGui::SliderInt("HC Level", &g_lz4hc_level,
					LZ4HC_CLEVEL_MIN, LZ4HC_CLEVEL_MAX);
				if (scanning)
					ImGui::EndDisabled();
			}

			ImGui::Separator();
			ImGui::Checkbox("Parallel search", &g_parallel_search);

			ImGui::Spacing();
			ImGui::Text("Path Exclusions");
			ImGui::Separator();
			ImGui::TextDisabled("Wildcard patterns (e.g. */debug/*)");

			// List existing exclusion patterns with remove buttons
			for (i32 i = 0; i < g_exclusion_count; i++) {
				ImGui::PushID(i);
				if (ImGui::SmallButton("X")) {
					for (i32 j = i; j < g_exclusion_count - 1; j++)
						memcpy(g_exclusion_patterns[j], g_exclusion_patterns[j + 1], MAX_EXCLUSION_LEN);
					g_exclusion_count--;
					ImGui::MarkIniSettingsDirty();
					ImGui::PopID();
					break;
				}
				ImGui::SameLine();
				ImGui::Text("%s", g_exclusion_patterns[i]);
				ImGui::PopID();
			}

			// Add new exclusion pattern
			{
				f32 add_btn_w = 50.0f;
				ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - add_btn_w - ImGui::GetStyle().ItemSpacing.x);
				bool add_enter = ImGui::InputTextWithHint("##new_exclusion", "*/debug/*, *.log, ...",
					g_new_exclusion, sizeof(g_new_exclusion), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine();
				bool add_clicked = ImGui::Button("Add", ImVec2(-1, 0));
				if ((add_enter || add_clicked) && g_new_exclusion[0] != '\0'
						&& g_exclusion_count < MAX_EXCLUSION_PATTERNS) {
					strncpy_s(g_exclusion_patterns[g_exclusion_count], g_new_exclusion, MAX_EXCLUSION_LEN - 1);
					g_exclusion_count++;
					g_new_exclusion[0] = '\0';
					ImGui::MarkIniSettingsDirty();
				}
			}

			ImGui::Separator();
			if (g_settings_dir[0] != '\0') {
				if (ImGui::Button("Open Settings Folder", ImVec2(-1, 0)))
					shell_open_folder(g_settings_dir);
			}
			if (ImGui::Button("Close", ImVec2(-1, 0))) {
				ImGui::MarkIniSettingsDirty();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	ImGui::End();
}


// App Interface -- lifecycle


static void gototext_init()
{
	init_lower_table();
	lock_init(&g_files_lock);
	lock_init(&g_search_results_lock);
	lock_init(&g_file_arena_lock);

	g_files.init(application_heap());
	g_search_results.init(application_heap());

	get_settings_path(g_settings_dir, (i32)sizeof(g_settings_dir), "gototext", nullptr);

	// Register ImGui settings handler
	ImGuiSettingsHandler handler;
	handler.TypeName = "gototext";
	handler.TypeHash = ImHashStr("gototext");
	handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) -> void* {
		return (strcmp(name, "State") == 0) ? (void*)1 : nullptr;
	};
	handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
		if (strncmp(line, "FileTypes=", 10) == 0)
			strncpy_s(g_file_types, line + 10, sizeof(g_file_types) - 1);
		else if (strncmp(line, "Directory=", 10) == 0)
			strncpy_s(g_directory, line + 10, sizeof(g_directory) - 1);
		else if (strncmp(line, "Codec=", 6) == 0)
			g_compress_codec = atoi(line + 6);
		else if (strncmp(line, "LZ4HCLevel=", 11) == 0)
			g_lz4hc_level = atoi(line + 11);
		else if (strncmp(line, "SkipSmall=", 10) == 0)
			g_skip_compress_small = (atoi(line + 10) != 0);
		else if (strncmp(line, "Parallel=", 9) == 0)
			g_parallel_search = (atoi(line + 9) != 0);
		else if (strncmp(line, "Exclusion=", 10) == 0) {
			if (g_exclusion_count < MAX_EXCLUSION_PATTERNS)
				strncpy_s(g_exclusion_patterns[g_exclusion_count++], line + 10, MAX_EXCLUSION_LEN - 1);
		}
	};
	handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
		buf->appendf("[%s][State]\n", h->TypeName);
		buf->appendf("FileTypes=%s\n", g_file_types);
		buf->appendf("Directory=%s\n", g_directory);
		buf->appendf("Codec=%d\n", g_compress_codec);
		buf->appendf("LZ4HCLevel=%d\n", g_lz4hc_level);
		buf->appendf("SkipSmall=%d\n", g_skip_compress_small ? 1 : 0);
		buf->appendf("Parallel=%d\n", g_parallel_search ? 1 : 0);
		for (i32 i = 0; i < g_exclusion_count; i++)
			buf->appendf("Exclusion=%s\n", g_exclusion_patterns[i]);
		buf->append("\n");
	};
	ImGui::AddSettingsHandler(&handler);
}

static void gototext_begin_shutdown()
{
	if (g_search_thread) {
		thread_join(g_search_thread);
		g_search_thread = nullptr;
	}
	if (g_scan_thread) {
		thread_join(g_scan_thread);
		g_scan_thread = nullptr;
	}

	free_all_files();

	if (g_scratch_buffers) {
		for (i32 i = 0; i < g_num_threads; i++)
			virtual_free(g_scratch_buffers[i]);
		application_heap()->free(g_scratch_buffers);
		g_scratch_buffers = nullptr;
		g_num_threads = 0;
	}

	g_search_results.destroy();
	g_files.destroy();
}

static void gototext_wait_for_shutdown()
{
}

// Public: return the App descriptor

App* gototext_get_app()
{
	static App app = {};
	app.name = "Find it faster";
	app.app_id = "gototext";
	app.init = gototext_init;
	app.tick = gototext_tick;
	app.on_activated = nullptr;
	app.on_resize = nullptr;
	app.begin_shutdown = gototext_begin_shutdown;
	app.wait_for_shutdown = gototext_wait_for_shutdown;
	app.hotkeys = nullptr;
	app.hotkey_count = 0;
	app.initial_width = 1280;
	app.initial_height = 800;
	app.title_bar_height = TITLE_BAR_HEIGHT;
	app.title_bar_buttons_width = TITLE_BAR_HEIGHT * TITLE_BAR_BUTTONS;
	app.use_system_tray = false;
	return &app;
}

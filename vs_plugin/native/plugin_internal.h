#pragma once

// --------------------------------------------------------------------------
// Internal shared state between plugin modules.
// NOT part of the public DLL API.
// --------------------------------------------------------------------------

#include "types.h"
#include "array.h"
#include "os.h"
#include "allocators.h"
#include "plugin_api.h"

#include <atomic>

// A file in the current solution.
struct PluginFile
{
	char* full_path;       // full UTF-8 path       (owned by path_arena)
	char* filename;        // points into full_path (the name portion)
	char* filename_lower;  // lowercased filename   (owned by path_arena)
	char* project_name;    // project name          (owned by path_arena, nullptr if unknown)
	char* project_lower;   // lowercased project    (owned by path_arena, nullptr if unknown)
	char* content;         // raw UTF-8 content     (owned by content_arena or hot buffer)
	i32   content_size;    // byte length of content
	i64   last_write_time; // FILETIME as i64 -- used to detect changes
	bool  dirty;           // set by directory watcher when file may have changed
};

// Heap-allocated content buffer for files that have been touched at runtime.
// Lives outside the content arena so it survives incremental updates.
struct HotBuffer
{
	char* data;
	i32   capacity;
};

// All file data for the current solution.
struct PluginFileStore
{
	Array<PluginFile> files;
	Lock              files_lock;

	BumpAllocator     path_arena{64 * 1024 * 1024};      // paths and filenames
	Lock              path_arena_lock;

	BumpAllocator     content_arena{64 * 1024 * 1024};   // bulk file contents
	Lock              content_arena_lock;

	Array<HotBuffer>  hot_buffers;                       // heap-allocated content for touched files
	Lock              hot_buffers_lock;

	i32               version = 0;
	char              include_extensions[1024];
};

void file_store_init(PluginFileStore* fs);
void file_store_clear(PluginFileStore* fs);          // reset contents, keep capacity (solution switch)
void file_store_reset_content(PluginFileStore* fs);   // reset file content only (extension change)
void file_store_destroy(PluginFileStore* fs);         // full teardown (shutdown)

// Save/load the include-extensions string to/from disk.
void plugin_save_extensions();
void plugin_load_saved_extensions();

// Returns true if file should be skipped for text content loading.
bool plugin_should_ignore_file(const char* filename);

// Load raw contents of all non-ignored files into PluginFile::content.
// Call with files already populated.  Thread-safe.
void plugin_load_file_contents();

// Poll directory watchers and reload files whose directories reported changes.
// Also loads content for files that haven't been loaded yet.
// Called before each search to pick up external modifications.
void refresh_stale_content_impl();

// Start watching directories for the current set of files.
void dir_watchers_start();

// Stop and close all directory watchers.
void dir_watchers_stop();

// Poll all directory watchers and mark changed files dirty (non-blocking).
void dir_watchers_poll();

// Kick off a background refresh of file contents.
void begin_refresh();

// Block until background refresh thread (if any) has finished.
void ensure_refresh_done();

// true while file contents are being loading (new sln opened)
bool is_file_content_loading();

// True while plugin_preload_content background thread is running.
extern std::atomic<bool> g_preloading;


// Callbacks from the extension.
struct PluginCallbacks
{
	PluginSelectionCallback selection = nullptr;
	PluginSelectionCallback preview   = nullptr;
};

extern PluginFileStore g_file_store;
extern PluginCallbacks g_callbacks;

#pragma once

// --------------------------------------------------------------------------
// Public C API
// --------------------------------------------------------------------------

#ifdef PLUGIN_CORE_EXPORTS
#define PLUGIN_API __declspec(dllexport)
#else
#define PLUGIN_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Signature of the callback invoked when the user selects a result.
//   path    - UTF-8 full file path
//   line    - 1-based line number (0 when coming from Go-To-File)
//   column  - 0-based column offset of the match (0 for Go-To-File)
typedef void (__stdcall *PluginSelectionCallback)(const char* path, int line, int column);

// Signal that the host is about to query solution files from VS.
// The native side will show a "Waiting for VS..." indicator until
// plugin_set_solution_files() is called.
PLUGIN_API void __stdcall plugin_begin_query_files(void);

// Provide the list of solution files (full replacement).  The DLL copies all data.
//   files    - array of pointers to null-terminated UTF-8 paths
//   projects - array of pointers to null-terminated UTF-8 project names (parallel to files)
//   count    - number of entries
PLUGIN_API void __stdcall plugin_set_solution_files(const char** files, const char** projects, int count);

// Register a callback for when the user selects a result.
PLUGIN_API void __stdcall plugin_set_callback(PluginSelectionCallback callback);

// Register a callback for live cursor preview (search-in-file).
PLUGIN_API void __stdcall plugin_set_preview_callback(PluginSelectionCallback callback);

// Show the Go-To-File window.  Non-blocking.
PLUGIN_API void __stdcall plugin_show_goto_file(void);

// Show the Go-To-Text window.  Non-blocking.
PLUGIN_API void __stdcall plugin_show_goto_text(void);

// Show the Go-To-Text-In-File window for the given file.  Non-blocking.
PLUGIN_API void __stdcall plugin_show_goto_text_in_file(const char* file_path);

// True while a plugin window is visible.
PLUGIN_API int __stdcall plugin_is_window_open(void);

// Check all loaded files for modifications and reload stale content.
// Runs timestamp checks in parallel. Non-blocking for callers.
PLUGIN_API void __stdcall plugin_refresh_stale_content(void);

// Start loading file contents in the background (for Go-To-Text search).
// Non-blocking.  Safe to call multiple times — no-ops if already loaded.
PLUGIN_API void __stdcall plugin_preload_content(void);

// Release all resources.
PLUGIN_API void __stdcall plugin_shutdown(void);

#ifdef __cplusplus
}
#endif

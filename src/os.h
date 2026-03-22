#pragma once

#include "types.h"

// Memory

// Reserve + commit pages from the OS.
void* virtual_alloc(u64 size);

// Release pages back to the OS. Must be allocated with virtual_alloc.
void virtual_free(void* ptr);

// Threads

struct Thread;

typedef void (*ThreadFunc)(void* user_data);

// Spawn a new OS thread.
Thread* thread_create(ThreadFunc func, void* user_data);

// Block until the thread finishes, then release its handle.
void thread_join(Thread* t);

// Get the number of logical processors.
i32 get_processor_count();

// Yield the current thread
void thread_yield();

// Returns the current wall-clock time in seconds.
f64 timer_now();

// File system

// Get the user application data directory.
// Returns false on failure.
bool os_get_appdata_dir(char* buf, i32 buf_size);

// Create a directory.
void os_create_directory(const char* path);

// File I/O

struct OsFile
{
	void* _handle;
};

static const OsFile OS_FILE_INVALID = { nullptr };

// True if the file handle is valid.
static inline bool os_file_valid(OsFile f) { return f._handle != nullptr; }

// Open a file for reading.
OsFile os_file_open_read(const char* path);

// Open a file for writing (creates or truncates).
OsFile os_file_open_write(const char* path);

// Open a file for reading with sequential-scan hint.
OsFile os_file_open_seq(const char* path);

// Open a directory for listing.
OsFile os_dir_open(const char* path);

// Get file size in bytes. Returns -1 on error.
i64 os_file_size(OsFile file);

// Get last write time of a file by path (UTF-8). Returns 0 on failure.
i64 os_file_last_write_time(const char* path);

// Read bytes from a file. Returns true on success.
// *bytes_read is set to the number of bytes actually read.
bool os_file_read(OsFile file, void* buf, i32 size, i32* bytes_read);

// Write bytes to a file. Returns true on success.
bool os_file_write(OsFile file, const void* buf, i32 size);

// Close a file or directory handle.
void os_file_close(OsFile file);

// Directory enumeration (NtQueryDirectoryFile)

struct DirEntry
{
	const char* name;  // UTF-8
	i32 name_len;
	bool is_directory;
};

struct DirEnumState
{
	u8 _internal[1088];
};

// Begin directory enumeration. buffer is scratch space for the OS to fill.
void dir_enum_init(DirEnumState* state, OsFile dir, void* buffer, u32 buffer_size);

// Get the next directory entry. Returns false when done.
bool dir_enum_next(DirEnumState* state, DirEntry* out);

// Drive enumeration

// Enumerate fixed NTFS/ReFS drives. Fills letters[] with drive letters (e.g. 'C').
// Returns the number of drives found (up to max_count).
i32 enumerate_ntfs_drives(char* letters, i32 max_count);

// Shell

// Open a file or URL with the system default handler.
void shell_open(const char* path_utf8);

// Open a folder in Windows Explorer.
void shell_open_folder(const char* path_utf8);

// Re-launch the current process elevated (UAC prompt). Exits the current process on success.
void restart_as_admin();

// Path utilities

// Return a pointer to the extension (including dot) within a UTF-8 filename.
// Returns pointer to the null terminator if no extension.
const char* os_path_find_extension(const char* filename);

// Lock

struct Lock
{
	// Opaque storage sized and aligned for SRWLOCK (pointer-sized on Windows).
	void* _opaque;
};

void lock_init(Lock* l);
void lock_read(Lock* l);
void unlock_read(Lock* l);
void lock_write(Lock* l);
void unlock_write(Lock* l);

// Try to acquire a write lock without blocking. Returns true if acquired.
bool try_lock_write(Lock* l);

struct ReadGuard
{
	Lock* l;
	ReadGuard(Lock* lk) : l(lk) { lock_read(l); }
	~ReadGuard() { unlock_read(l); }
};

struct WriteGuard
{
	Lock* l;
	WriteGuard(Lock* lk) : l(lk) { lock_write(l); }
	~WriteGuard() { unlock_write(l); }
};

// Condition variable

struct CondVar
{
	void* _opaque;
};

void condvar_init(CondVar* cv);
void condvar_wait(CondVar* cv, Lock* l, u32 timeout_ms);
void condvar_wake_all(CondVar* cv);

// Condition variable helpers -- thin wrappers so callers don't need windows.h.
// cv must point to a zero-initialized CONDITION_VARIABLE (pointer-sized).
void lock_sleep_condition(Lock* l, void* cv, u32 ms);
void lock_wake_all_condition(void* cv);

// Directory change watcher
struct DirWatcher;

// Callback invoked for each changed file.
//   name_utf8: filename that changed
//   user_data: forwarded from dir_watcher_poll
typedef void (*DirWatchCallback)(const char* name_utf8, void* user_data);

// Create a watcher for a single directory (UTF-8 path).
// Returns nullptr on failure.
DirWatcher* dir_watcher_create(const char* dir_utf8);

// Non-blocking poll. Calls cb for each file that changed since the
// last poll. Re-arms the watcher automatically.
void dir_watcher_poll(DirWatcher* w, DirWatchCallback cb, void* user_data);

// Destroy the watcher and release all resources.
void dir_watcher_destroy(DirWatcher* w);

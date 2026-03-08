#include "os.h"

#include <stdio.h>

#include "allocators.h"
#include "utf.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

// Thread

struct Thread
{
	HANDLE handle;
};

struct ThreadInitData
{
	ThreadFunc func;
	void* user_data;
};

static DWORD WINAPI thread_entry(LPVOID param)
{
	ThreadInitData* t = (ThreadInitData*)param;
	ThreadFunc func = t->func;
	void* ud = t->user_data;

	application_heap()->free(t);
	func(ud);
	return 0;
}

Thread* thread_create(ThreadFunc func, void* user_data)
{
	ThreadInitData* init_data = (ThreadInitData*)application_heap()->alloc(
		sizeof(ThreadInitData), alignof(ThreadInitData));
	if (!init_data)
		return nullptr;
	init_data->func = func;
	init_data->user_data = user_data;

	HANDLE hThread = CreateThread(nullptr, 0, thread_entry, init_data, 0, nullptr);
	if (!hThread) {
		application_heap()->free(init_data);
		return nullptr;
	}

	Thread* thread = (Thread*)application_heap()->alloc(sizeof(Thread), alignof(Thread));
	thread->handle = hThread;
	return thread;
}

void thread_join(Thread* t)
{
	if (!t)
		return;
	WaitForSingleObject(t->handle, INFINITE);
	CloseHandle(t->handle);
	application_heap()->free(t);
}


// Timer


static f64 g_timer_freq_inv;

static struct TimerInit
{
	TimerInit()
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		g_timer_freq_inv = 1.0 / (f64)freq.QuadPart;
	}
} g_timer_init;

f64 timer_now()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (f64)now.QuadPart * g_timer_freq_inv;
}

// Memory

void* virtual_alloc(u64 size)
{
	return VirtualAlloc(nullptr, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

void virtual_free(void* ptr)
{
	if (ptr)
		VirtualFree(ptr, 0, MEM_RELEASE);
}

// Drive enumeration

i32 enumerate_ntfs_drives(char* letters, i32 max_count)
{
	DWORD mask = GetLogicalDrives();
	i32 count = 0;
	for (i32 i = 0; i < 26 && count < max_count; i++) {
		if (!(mask & (1u << i)))
			continue;
		wchar_t root[] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
		UINT type = GetDriveTypeW(root);
		if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
			continue;
		wchar_t fs_name[64] = { };
		if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fs_name, 64))
			continue;
		if (wcscmp(fs_name, L"NTFS") != 0 && wcscmp(fs_name, L"ReFS") != 0)
			continue;
		letters[count++] = 'A' + (char)i;
	}
	return count;
}


// Shell


#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

void shell_open(const char* path_utf8)
{
	wchar_t buf[4096];
	if (wide_from_utf8(buf, 4096, path_utf8) > 0) {
		HINSTANCE result = ShellExecuteW(nullptr, L"open", buf, nullptr, nullptr, SW_SHOWNORMAL);
		if ((INT_PTR)result <= 32) {
			printf("Failed to open file. Error code: %ld\n", GetLastError());
		}
	}
}


// File system


bool os_get_appdata_dir(char* buf, i32 buf_size)
{
	buf[0] = '\0';
	wchar_t appdata[MAX_PATH];
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
		return false;
	return utf8_from_wide(buf, buf_size, appdata) > 0;
}

void os_create_directory(const char* path)
{
	wchar_t wide[4096];
	if (wide_from_utf8(wide, 4096, path) > 0)
		CreateDirectoryW(wide, nullptr);
}

void shell_open_folder(const char* path_utf8)
{
	wchar_t buf[4096];
	if (wide_from_utf8(buf, 4096, path_utf8) > 0)
		ShellExecuteW(nullptr, L"explore", buf, nullptr, nullptr, SW_SHOWNORMAL);
}

void restart_as_admin()
{
	wchar_t exe_path[MAX_PATH];
	GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

	SHELLEXECUTEINFOW sei = { };
	sei.cbSize = sizeof(sei);
	sei.lpVerb = L"runas";
	sei.lpFile = exe_path;
	sei.nShow = SW_SHOWNORMAL;
	if (ShellExecuteExW(&sei)) {
		ExitProcess(0);
	}
}


// Lock (SRWLOCK wrapper)


static_assert(sizeof(SRWLOCK) <= sizeof(void*), "SRWLOCK must fit in Lock._opaque");

void lock_init(Lock* l)
{
	InitializeSRWLock((SRWLOCK*)&l->_opaque);
}

void lock_read(Lock* l)
{
	AcquireSRWLockShared((SRWLOCK*)&l->_opaque);
}

void unlock_read(Lock* l)
{
	ReleaseSRWLockShared((SRWLOCK*)&l->_opaque);
}

void lock_write(Lock* l)
{
	AcquireSRWLockExclusive((SRWLOCK*)&l->_opaque);
}

void unlock_write(Lock* l)
{
	ReleaseSRWLockExclusive((SRWLOCK*)&l->_opaque);
}

bool try_lock_write(Lock* l)
{
	return TryAcquireSRWLockExclusive((SRWLOCK*)&l->_opaque) != 0;
}

void lock_sleep_condition(Lock* l, void* cv, u32 ms)
{
	SleepConditionVariableSRW((CONDITION_VARIABLE*)cv, (SRWLOCK*)&l->_opaque, ms, 0);
}

void lock_wake_all_condition(void* cv)
{
	WakeAllConditionVariable((CONDITION_VARIABLE*)cv);
}


// Condition variable


static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(void*), "CONDITION_VARIABLE must fit in CondVar._opaque");

void condvar_init(CondVar* cv)
{
	InitializeConditionVariable((CONDITION_VARIABLE*)&cv->_opaque);
}

void condvar_wait(CondVar* cv, Lock* l, u32 timeout_ms)
{
	SleepConditionVariableSRW((CONDITION_VARIABLE*)&cv->_opaque, (SRWLOCK*)&l->_opaque, timeout_ms, 0);
}

void condvar_wake_all(CondVar* cv)
{
	WakeAllConditionVariable((CONDITION_VARIABLE*)&cv->_opaque);
}


// Processor count / thread yield


i32 get_processor_count()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	i32 n = (i32)si.dwNumberOfProcessors;
	return n > 0 ? n : 1;
}

void thread_yield()
{
	SwitchToThread();
}

// File I/O

OsFile os_file_open_read(const char* path)
{
	wchar_t wide[4096];
	if (wide_from_utf8(wide, 4096, path) <= 0)
		return OS_FILE_INVALID;
	HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return OS_FILE_INVALID;
	return { (void*)h };
}

OsFile os_file_open_seq(const char* path)
{
	wchar_t wide[4096];
	if (wide_from_utf8(wide, 4096, path) <= 0)
		return OS_FILE_INVALID;
	HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return OS_FILE_INVALID;
	return { (void*)h };
}

OsFile os_dir_open(const char* path)
{
	wchar_t wide[4096];
	if (wide_from_utf8(wide, 4096, path) <= 0)
		return OS_FILE_INVALID;
	HANDLE h = CreateFileW(wide, FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return OS_FILE_INVALID;
	return { (void*)h };
}

i64 os_file_size(OsFile file)
{
	LARGE_INTEGER size;
	if (!GetFileSizeEx((HANDLE)file._handle, &size))
		return -1;
	return (i64)size.QuadPart;
}

bool os_file_read(OsFile file, void* buf, i32 size, i32* bytes_read)
{
	DWORD read = 0;
	BOOL ok = ReadFile((HANDLE)file._handle, buf, (DWORD)size, &read, nullptr);
	if (bytes_read)
		*bytes_read = (i32)read;
	return ok != 0;
}

void os_file_close(OsFile file)
{
	if (file._handle)
		CloseHandle((HANDLE)file._handle);
}


// Directory enumeration (NtQueryDirectoryFile)


typedef struct _FILE_DIRECTORY_INFORMATION
{
	ULONG NextEntryOffset;
	ULONG FileIndex;
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER EndOfFile;
	LARGE_INTEGER AllocationSize;
	ULONG FileAttributes;
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_DIRECTORY_INFORMATION;

extern "C" NTSTATUS NTAPI NtQueryDirectoryFile(
	HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
	PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
	FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry,
	PUNICODE_STRING FileName, BOOLEAN RestartScan);

struct DirEnumInternal
{
	HANDLE dir_handle;
	u8* buffer;
	u32 buffer_size;
	u8* current;         // current position in buffer
	bool first_call;
	bool done;
	char name_utf8[1024]; // scratch for UTF-8 conversion
};

static_assert(sizeof(DirEnumInternal) <= sizeof(DirEnumState),
	"DirEnumInternal must fit in DirEnumState");

void dir_enum_init(DirEnumState* state, OsFile dir, void* buffer, u32 buffer_size)
{
	DirEnumInternal* s = (DirEnumInternal*)state->_internal;
	s->dir_handle = (HANDLE)dir._handle;
	s->buffer = (u8*)buffer;
	s->buffer_size = buffer_size;
	s->current = nullptr;
	s->first_call = true;
	s->done = false;
}

static void fill_dir_entry(DirEnumInternal* s, FILE_DIRECTORY_INFORMATION* info, DirEntry* out)
{
	i32 wchar_count = (i32)(info->FileNameLength / sizeof(wchar_t));
	i32 utf8_len = utf8_from_wide(s->name_utf8, (i32)sizeof(s->name_utf8),
		info->FileName, wchar_count);
	if (utf8_len <= 0) {
		s->name_utf8[0] = '\0';
		utf8_len = 0;
	}
	out->name = s->name_utf8;
	out->name_len = utf8_len;
	out->is_directory = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool dir_enum_next(DirEnumState* state, DirEntry* out)
{
	DirEnumInternal* s = (DirEnumInternal*)state->_internal;
	if (s->done)
		return false;

	// Advance to next entry in current buffer
	if (s->current) {
		FILE_DIRECTORY_INFORMATION* info = (FILE_DIRECTORY_INFORMATION*)s->current;
		if (info->NextEntryOffset != 0) {
			s->current += info->NextEntryOffset;
			FILE_DIRECTORY_INFORMATION* next = (FILE_DIRECTORY_INFORMATION*)s->current;
			fill_dir_entry(s, next, out);
			return true;
		}
		// Buffer exhausted, fall through to refill
	}

	// Fill the buffer
	IO_STATUS_BLOCK iosb = {};
	NTSTATUS status = NtQueryDirectoryFile(
		s->dir_handle, nullptr, nullptr, nullptr, &iosb,
		s->buffer, s->buffer_size,
		(FILE_INFORMATION_CLASS)1, // FileDirectoryInformation
		FALSE, nullptr, s->first_call ? TRUE : FALSE);
	s->first_call = false;

	if (status != 0) {
		s->done = true;
		return false;
	}

	s->current = s->buffer;
	FILE_DIRECTORY_INFORMATION* info = (FILE_DIRECTORY_INFORMATION*)s->current;
	fill_dir_entry(s, info, out);
	return true;
}


// Path utilities


const char* os_path_find_extension(const char* filename)
{
	const char* last_dot = nullptr;
	const char* p = filename;
	while (*p) {
		if (*p == '.')
			last_dot = p;
		else if (*p == '\\' || *p == '/')
			last_dot = nullptr; // reset on path separator
		p++;
	}
	return last_dot ? last_dot : p; // p points to null terminator
}


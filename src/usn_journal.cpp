#include "usn_journal.h"

#include "allocators.h"
#include "hashmap.h"
#include "lz4.h"
#include "utf.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <winioctl.h>

struct UsnEntry
{
	u64 parent_frn;
	char* name; // bump-allocated block: [utf8 \0] [lower_utf8 \0]
	u32 attributes; // FILE_ATTRIBUTE_* flags
	i32 name_len; // UTF-8 byte count (excludes null)
};

struct UsnJournal
{
	wchar_t volume_path[8]; // e.g. \\.\C:
	u64 journal_id;
	i64 next_usn;
	HashMap<UsnEntry> entries;
	BumpAllocator name_allocator; // bulk storage for all UsnEntry::name blocks
	bool initialized;
};

// free entry names and reset journal state without freeing the struct.
static void clear_journal(UsnJournal* j)
{
	j->entries.reset();
	j->name_allocator.free_all();
	j->journal_id = 0;
	j->next_usn = 0;
	j->initialized = false;
}

// Cache file format

static constexpr u32 CACHE_MAGIC = 0x4E535543; // 'CUSN'
static constexpr u32 CACHE_VERSION = 2;

#pragma pack(push, 1)
struct CacheHeader
{
	u32 magic;
	u32 version;
	wchar_t volume_path[8];
	u64 journal_id;
	i64 next_usn;
	u32 entry_count;
	u32 uncompressed_size;
	u32 compressed_size;
};
#pragma pack(pop)

// Internal helpers

// Store a name as UTF-8 in the bump allocator: [utf8 \0] [lower_utf8 \0]
static void init_entry_name(BumpAllocator* a, UsnEntry* e,
	const char* utf8_src, i32 utf8_len)
{
	i32 total = (utf8_len + 1) + (utf8_len + 1);

	e->name = (char*)a->alloc(total, 1);
	memcpy(e->name, utf8_src, utf8_len);
	e->name[utf8_len] = '\0';
	e->name_len = utf8_len;

	char* lower = e->name + utf8_len + 1;
	memcpy(lower, e->name, utf8_len + 1);
	for (i32 i = 0; i < utf8_len; i++) {
		if (lower[i] >= 'A' && lower[i] <= 'Z')
			lower[i] += 32;
	}
}

static void insert_or_update_entry(UsnJournal* j, u64 frn, u64 parent_frn,
	u32 attributes, const char* utf8_name, i32 utf8_len)
{
	UsnEntry* existing = j->entries.find(frn);
	if (existing) {
		// Old name leaks in the allocator, churn should not leak enough to matter
		init_entry_name(&j->name_allocator, existing, utf8_name, utf8_len);
		existing->parent_frn = parent_frn;
		existing->attributes = attributes;
	} else {
		UsnEntry e;
		e.parent_frn = parent_frn;
		e.attributes = attributes;
		init_entry_name(&j->name_allocator, &e, utf8_name, utf8_len);
		j->entries.add(frn, e);
	}
}

static i32 apply_usn_records(UsnJournal* j, u8* buffer, DWORD bytes_returned)
{
	i32 changes = 0;
	USN_RECORD_V2* rec = (USN_RECORD_V2*)(buffer + sizeof(USN));
	u8* buf_end = buffer + bytes_returned;

	while ((u8*)rec < buf_end && rec->RecordLength > 0) {
		u64 frn = rec->FileReferenceNumber;
		u64 parent = rec->ParentFileReferenceNumber;
		DWORD reason = rec->Reason;

		if (reason & USN_REASON_FILE_DELETE) {
			// Name leaks in the allocator, freed in bulk
			UsnEntry* found = j->entries.find(frn);
			if (found) {
				j->entries.erase(frn);
				changes++;
			}
		} else if (reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)) {
			const wchar_t* name_ptr = (const wchar_t*)((u8*)rec + rec->FileNameOffset);
			i32 name_wchars = rec->FileNameLength / sizeof(wchar_t);

			char utf8_buf[1024];
			i32 utf8_len = utf8_from_wide(utf8_buf, sizeof(utf8_buf), name_ptr, name_wchars);

			UsnEntry* existing = j->entries.find(frn);
			if (existing && existing->parent_frn == parent && existing->name_len == utf8_len && memcmp(existing->name, utf8_buf, utf8_len) == 0) {
			} else {
				insert_or_update_entry(j, frn, parent, rec->FileAttributes, utf8_buf, utf8_len);
				changes++;
			}
		}
		// RENAME_OLD_NAME records are ignored -- the subsequent delete or
		// RENAME_NEW_NAME record handles the actual index update.

		rec = (USN_RECORD_V2*)((u8*)rec + rec->RecordLength);
	}
	return changes;
}

// usn_journal_init

bool usn_journal_init(UsnJournal* j, char drive_letter, f32* out_progress)
{
	clear_journal(j);
	if (out_progress)
		*out_progress = 0.0f;

	swprintf_s(j->volume_path, _countof(j->volume_path), L"\\\\.\\%c:", (wchar_t)drive_letter);

	HANDLE hVol = CreateFileW(j->volume_path,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, 0, nullptr);
	if (hVol == INVALID_HANDLE_VALUE)
		return false;

	USN_JOURNAL_DATA_V0 jdata = { };
	DWORD br = 0;
	if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL,
			nullptr, 0, &jdata, sizeof(jdata), &br, nullptr)) {
		CloseHandle(hVol);
		return false;
	}

	j->journal_id = jdata.UsnJournalID;

	// Query total MFT size for progress estimation
	u64 max_frn = 0;
	if (out_progress) {
		NTFS_VOLUME_DATA_BUFFER vol_data = { };
		DWORD vbr = 0;
		if (DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
				&vol_data, sizeof(vol_data), &vbr, nullptr)) {
			if (vol_data.BytesPerFileRecordSegment > 0) {
				max_frn = (u64)(vol_data.MftValidDataLength.QuadPart / vol_data.BytesPerFileRecordSegment);
			}
		}
	}

	// Pre-size the map for a typical volume
	j->entries.reserve(2 * 1024 * 1024);

	constexpr DWORD BUF_SIZE = 512 * 1024;
	HeapAllocator* heap = application_heap();
	u8* buffer = (u8*)heap->alloc(BUF_SIZE);
	if (!buffer) {
		CloseHandle(hVol);
		return false;
	}

	MFT_ENUM_DATA_V0 enum_data = { };
	enum_data.StartFileReferenceNumber = 0;
	enum_data.LowUsn = 0;
	enum_data.HighUsn = jdata.NextUsn;

	while (DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA,
		&enum_data, sizeof(enum_data),
		buffer, BUF_SIZE, &br, nullptr)) {
		if (br <= sizeof(USN))
			break;

		DWORDLONG next_frn = *(DWORDLONG*)buffer;
		USN_RECORD_V2* rec = (USN_RECORD_V2*)(buffer + sizeof(USN));
		u8* buf_end = buffer + br;

		while ((u8*)rec < buf_end && rec->RecordLength > 0) {
			const wchar_t* name_ptr = (const wchar_t*)((u8*)rec + rec->FileNameOffset);
			i32 name_wchars = rec->FileNameLength / sizeof(wchar_t);

			char utf8_buf[1024];
			i32 utf8_len = utf8_from_wide(utf8_buf, sizeof(utf8_buf), name_ptr, name_wchars);

			insert_or_update_entry(j, rec->FileReferenceNumber,
				rec->ParentFileReferenceNumber,
				rec->FileAttributes, utf8_buf, utf8_len);

			rec = (USN_RECORD_V2*)((u8*)rec + rec->RecordLength);
		}

		enum_data.StartFileReferenceNumber = next_frn;

		if (out_progress && max_frn > 0) {
			f32 p = (f32)((f64)next_frn / (f64)max_frn);
			if (p > 1.0f)
				p = 1.0f;
			*out_progress = p;
		}
	}

	if (out_progress)
		*out_progress = 1.0f;
	heap->free(buffer);
	CloseHandle(hVol);

	j->next_usn = jdata.NextUsn;
	j->initialized = true;
	return true;
}

// usn_journal_create

UsnJournal* usn_journal_create()
{
	// Placement new so BumpAllocator is properly constructed
	void* mem = application_heap()->alloc(sizeof(UsnJournal), alignof(UsnJournal));
	UsnJournal* j = new (mem) UsnJournal();
	return j;
}

// usn_journal_destroy

void usn_journal_destroy(UsnJournal* j)
{
	if (!j)
		return;
	clear_journal(j);
	j->~UsnJournal();
	application_heap()->free(j);
}

// usn_journal_update

i32 usn_journal_update(UsnJournal* j)
{
	if (!j->initialized)
		return -1;

	HANDLE hVol = CreateFileW(j->volume_path,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, 0, nullptr);
	if (hVol == INVALID_HANDLE_VALUE)
		return -1;

	USN_JOURNAL_DATA_V0 jdata = { };
	DWORD br = 0;
	if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL,
			nullptr, 0, &jdata, sizeof(jdata), &br, nullptr)) {
		CloseHandle(hVol);
		return -1;
	}

	if (jdata.UsnJournalID != j->journal_id) {
		CloseHandle(hVol);
		return -1; // journal was recreated
	}

	if (j->next_usn < (i64)jdata.LowestValidUsn) {
		CloseHandle(hVol);
		return -1; // our saved USN has been overwritten
	}

	if (j->next_usn >= jdata.NextUsn) {
		CloseHandle(hVol);
		return 0;
	}

	constexpr DWORD BUF_SIZE = 64 * 1024;
	u8 buffer[BUF_SIZE];

	READ_USN_JOURNAL_DATA_V0 read_data = { };
	read_data.StartUsn = j->next_usn;
	// We only care about name changes: creates, deletes, and renames.
	// Content modifications and attribute changes do not affect our index.
	read_data.ReasonMask = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME;
	read_data.ReturnOnlyOnClose = FALSE;
	read_data.Timeout = 0;
	read_data.BytesToWaitFor = 0;
	read_data.UsnJournalID = j->journal_id;

	i32 total_changes = 0;

	for (;;) {
		BOOL ok = DeviceIoControl(hVol, FSCTL_READ_USN_JOURNAL,
			&read_data, sizeof(read_data),
			buffer, BUF_SIZE, &br, nullptr);

		if (!ok) {
			// ERROR_HANDLE_EOF means we have consumed everything
			if (GetLastError() == ERROR_HANDLE_EOF)
				break;
			CloseHandle(hVol);
			return -1;
		}

		if (br <= sizeof(USN))
			break;

		USN next_usn = *(USN*)buffer;
		total_changes += apply_usn_records(j, buffer, br);

		if (next_usn == read_data.StartUsn)
			break; // no progress
		read_data.StartUsn = next_usn;
	}

	j->next_usn = read_data.StartUsn;
	CloseHandle(hVol);
	return total_changes;
}

// usn_journal_save_cache

bool usn_journal_save_cache(const UsnJournal* j, const char* cache_path)
{
	if (!j->initialized)
		return false;

	wchar_t cache_path_wide[512];
	wide_from_utf8(cache_path_wide, 512, cache_path);

	u64 payload_size = 0;
	for (auto& e : j->entries) {
		// frn(8) + parent_frn(8) + attributes(4) + name_len(2) + utf8_data
		payload_size += 8 + 8 + 4 + 2;
		payload_size += (u64)e.value.name_len;
	}

	if (payload_size > (u64)LZ4_MAX_INPUT_SIZE)
		return false;

	HeapAllocator* heap = application_heap();
	u8* payload = (u8*)heap->alloc((u64)payload_size);
	if (!payload)
		return false;

	u8* wp = payload;
	u32 count = 0;
	for (auto& e : j->entries) {
		u64 frn = e.key;
		u16 name_len = (u16)e.value.name_len;

		memcpy(wp, &frn, 8);
		wp += 8;
		memcpy(wp, &e.value.parent_frn, 8);
		wp += 8;
		memcpy(wp, &e.value.attributes, 4);
		wp += 4;
		memcpy(wp, &name_len, 2);
		wp += 2;
		memcpy(wp, e.value.name, name_len);
		wp += name_len;
		count++;
	}

	u32 actual_payload = (u32)(wp - payload);

	int compress_bound = LZ4_compressBound((int)actual_payload);
	u8* compressed = (u8*)heap->alloc((u64)compress_bound);
	if (!compressed) {
		heap->free(payload);
		return false;
	}

	int compressed_size = LZ4_compress_default(
		(const char*)payload, (char*)compressed,
		(int)actual_payload, compress_bound);
	heap->free(payload);

	if (compressed_size <= 0) {
		heap->free(compressed);
		return false;
	}

	CacheHeader hdr = { };
	hdr.magic = CACHE_MAGIC;
	hdr.version = CACHE_VERSION;
	memcpy(hdr.volume_path, j->volume_path, sizeof(hdr.volume_path));
	hdr.journal_id = j->journal_id;
	hdr.next_usn = j->next_usn;
	hdr.entry_count = count;
	hdr.uncompressed_size = actual_payload;
	hdr.compressed_size = (u32)compressed_size;

	HANDLE hFile = CreateFileW(cache_path_wide, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) {
		heap->free(compressed);
		return false;
	}

	DWORD written = 0;
	bool ok = true;
	ok = ok && WriteFile(hFile, &hdr, sizeof(hdr), &written, nullptr);
	ok = ok && WriteFile(hFile, compressed, (DWORD)compressed_size, &written, nullptr);

	CloseHandle(hFile);
	heap->free(compressed);
	return ok;
}

// usn_journal_load_cache

bool usn_journal_load_cache(UsnJournal* j, const char* cache_path)
{
	HeapAllocator* heap = application_heap();

	wchar_t cache_path_wide[512];
	wide_from_utf8(cache_path_wide, 512, cache_path);

	HANDLE hFile = CreateFileW(cache_path_wide, GENERIC_READ,
		FILE_SHARE_READ, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER file_size;
	if (!GetFileSizeEx(hFile, &file_size) || file_size.QuadPart < (LONGLONG)sizeof(CacheHeader)) {
		CloseHandle(hFile);
		return false;
	}

	u8* file_buf = (u8*)heap->alloc((u64)file_size.QuadPart);
	if (!file_buf) {
		CloseHandle(hFile);
		return false;
	}

	DWORD bytes_read = 0;
	u8* rp = file_buf;
	LONGLONG remaining = file_size.QuadPart;
	while (remaining > 0) {
		DWORD chunk = (remaining > 0x40000000) ? 0x40000000 : (DWORD)remaining;
		if (!ReadFile(hFile, rp, chunk, &bytes_read, nullptr) || bytes_read == 0) {
			heap->free(file_buf);
			CloseHandle(hFile);
			return false;
		}
		rp += bytes_read;
		remaining -= bytes_read;
	}
	CloseHandle(hFile);

	CacheHeader* hdr = (CacheHeader*)file_buf;
	if (hdr->magic != CACHE_MAGIC || hdr->version != CACHE_VERSION) {
		heap->free(file_buf);
		return false;
	}

	if ((LONGLONG)(sizeof(CacheHeader) + hdr->compressed_size) > file_size.QuadPart) {
		heap->free(file_buf);
		return false;
	}

	wchar_t vol_path[8];
	memcpy(vol_path, hdr->volume_path, sizeof(vol_path));

	HANDLE hVol = CreateFileW(vol_path,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, 0, nullptr);

	if (hVol == INVALID_HANDLE_VALUE) {
		heap->free(file_buf);
		return false;
	}

	USN_JOURNAL_DATA_V0 jdata = { };
	DWORD br = 0;
	if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL,
			nullptr, 0, &jdata, sizeof(jdata), &br, nullptr)) {
		CloseHandle(hVol);
		heap->free(file_buf);
		return false;
	}
	CloseHandle(hVol);

	if (jdata.UsnJournalID != hdr->journal_id) {
		heap->free(file_buf);
		return false;
	}
	if (hdr->next_usn < (i64)jdata.LowestValidUsn) {
		heap->free(file_buf);
		return false;
	}

	u8* payload = (u8*)heap->alloc((u64)hdr->uncompressed_size);
	if (!payload) {
		heap->free(file_buf);
		return false;
	}

	int decompressed = LZ4_decompress_safe(
		(const char*)(file_buf + sizeof(CacheHeader)),
		(char*)payload,
		(int)hdr->compressed_size,
		(int)hdr->uncompressed_size);

	if (decompressed != (int)hdr->uncompressed_size) {
		heap->free(payload);
		heap->free(file_buf);
		return false;
	}

	clear_journal(j);
	memcpy(j->volume_path, hdr->volume_path, sizeof(j->volume_path));
	j->journal_id = hdr->journal_id;
	j->next_usn = hdr->next_usn;
	j->entries.reserve(hdr->entry_count + hdr->entry_count / 16); // some headroom

	const u8* rp2 = payload;
	const u8* payload_end = payload + hdr->uncompressed_size;
	for (u32 i = 0; i < hdr->entry_count && rp2 < payload_end; i++) {
		if (rp2 + 22 > payload_end)
			break; // minimum entry: 8+8+4+2 = 22

		u64 frn, parent_frn;
		u32 attributes;
		u16 name_len;

		memcpy(&frn, rp2, 8);
		rp2 += 8;
		memcpy(&parent_frn, rp2, 8);
		rp2 += 8;
		memcpy(&attributes, rp2, 4);
		rp2 += 4;
		memcpy(&name_len, rp2, 2);
		rp2 += 2;

		if (rp2 + name_len > payload_end)
			break;

		insert_or_update_entry(j, frn, parent_frn, attributes, (const char*)rp2, name_len);
		rp2 += name_len;
	}

	heap->free(payload);
	heap->free(file_buf);

	j->initialized = true;

	// Replay USN records since the cached snapshot to bring us up to date
	i32 replayed = usn_journal_update(j);
	if (replayed < 0) {
		// Journal was invalidated during replay -- caller must do full init
		clear_journal(j);
		return false;
	}

	return true;
}

// usn_journal_find

UsnEntry* usn_journal_find(UsnJournal* j, u64 frn)
{
	return j->entries.find(frn);
}

// usn_journal_dir_frn

u64 usn_journal_dir_frn(const char* dir_path)
{
	wchar_t dir_path_wide[512];
	wide_from_utf8(dir_path_wide, 512, dir_path);

	HANDLE hDir = CreateFileW(dir_path_wide,
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (hDir == INVALID_HANDLE_VALUE)
		return 0;

	BY_HANDLE_FILE_INFORMATION info = { };
	GetFileInformationByHandle(hDir, &info);
	CloseHandle(hDir);

	return ((u64)info.nFileIndexHigh << 32) | info.nFileIndexLow;
}


// Journal query accessors


u64 usn_journal_entry_count(const UsnJournal* j)
{
	return j->entries.size();
}

char usn_journal_drive_letter(const UsnJournal* j)
{
	return (char)j->volume_path[4];
}

UsnEntry* usn_journal_entry_at(UsnJournal* j, u64 index)
{
	return &j->entries.begin()[index].value;
}

u64 usn_journal_frn_at(UsnJournal* j, u64 index)
{
	return j->entries.begin()[index].key;
}

u64 usn_journal_name_bytes_allocated(UsnJournal* j)
{
	return j->name_allocator.get_bytes_allocated();
}

u64 usn_journal_lookup_bytes_allocated(UsnJournal* j)
{
	u64 hash_cap = j->entries.capacity();
	u64 hash_bytes = hash_cap * sizeof(HashEntry<UsnEntry>) + hash_cap * sizeof(u64);

	return hash_bytes;
}

// Entry accessors

u64 usn_entry_parent_frn(const UsnEntry* e)
{
	return e->parent_frn;
}

u32 usn_entry_attributes(const UsnEntry* e)
{
	return e->attributes;
}

bool usn_entry_is_directory(const UsnEntry* e)
{
	return (e->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

const char* usn_entry_utf8(const UsnEntry* e)
{
	return e->name;
}

const char* usn_entry_lower_utf8(const UsnEntry* e)
{
	return e->name + e->name_len + 1;
}

i32 usn_entry_utf8_len(const UsnEntry* e)
{
	return e->name_len;
}

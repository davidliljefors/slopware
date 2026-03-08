#pragma once

#include "types.h"

struct UsnJournal;
struct UsnEntry;

// Journal lifecycle

// Allocate an empty journal. Must be freed with usn_journal_destroy.
UsnJournal* usn_journal_create();

// Free all names, entries, and the journal itself.
void usn_journal_destroy(UsnJournal* j);

// Read the entire MFT for the volume identified by drive_letter (e.g. 'C').
// Requires elevation. Returns true on success.
// If out_progress is non-null, it is written with 0.0..1.0 as the scan proceeds.
bool usn_journal_init(UsnJournal* j, char drive_letter, f32* out_progress = nullptr);

// Poll for changes since last init/update/load.
// Returns entries touched, or -1 if journal was invalidated.
i32 usn_journal_update(UsnJournal* j);

// Serialize to LZ4-compressed cache file (UTF-8 path).
bool usn_journal_save_cache(const UsnJournal* j, const char* cache_path);

// Restore from cache then replay changes. Returns false on failure (UTF-8 path).
bool usn_journal_load_cache(UsnJournal* j, const char* cache_path);

// Journal queries

u64 usn_journal_entry_count(const UsnJournal* j);
char usn_journal_drive_letter(const UsnJournal* j);

// Lookup by FRN. Returns nullptr if not found.
UsnEntry* usn_journal_find(UsnJournal* j, u64 frn);

// Get the FRN of a directory from its full path (UTF-8). Returns 0 on failure.
u64 usn_journal_dir_frn(const char* dir_path);

// Index-based iteration over the internal dense array.
UsnEntry* usn_journal_entry_at(UsnJournal* j, u64 index);
u64 usn_journal_frn_at(UsnJournal* j, u64 index);

u64 usn_journal_name_bytes_allocated(UsnJournal* j);
u64 usn_journal_lookup_bytes_allocated(UsnJournal* j);

// Entry accessors

u64 usn_entry_parent_frn(const UsnEntry* e);
u32 usn_entry_attributes(const UsnEntry* e);
bool usn_entry_is_directory(const UsnEntry* e);
const char* usn_entry_utf8(const UsnEntry* e);
const char* usn_entry_lower_utf8(const UsnEntry* e);
i32 usn_entry_utf8_len(const UsnEntry* e);

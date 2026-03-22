#pragma once

#include "types.h"

// Lowercase an ASCII string into dst. Copies at most max_len chars + null terminator.
void str_to_lower(char* dst, const char* src, i32 max_len);

// Return pointer to the filename portion of a path (after last '\\' or '/').
// Returns the original pointer if no separator found.
const char* str_filename(const char* path);

// Safe bounded string copy with null terminator.
// If src_len < 0, uses strlen(src). Copies at most dst_size-1 chars.
void str_copy(char* dst, i32 dst_size, const char* src, i32 src_len = -1);

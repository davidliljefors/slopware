#include "string_util.h"
#include <string.h>

void str_to_lower(char* dst, const char* src, i32 max_len)
{
	i32 i = 0;
	for (; i < max_len && src[i]; i++) {
		char c = src[i];
		dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}
	dst[i] = '\0';
}

const char* str_filename(const char* path)
{
	const char* name = path;
	for (const char* p = path; *p; p++) {
		if (*p == '\\' || *p == '/')
			name = p + 1;
	}
	return name;
}

void str_copy(char* dst, i32 dst_size, const char* src, i32 src_len)
{
	if (src_len < 0) src_len = (i32)strlen(src);
	if (src_len >= dst_size) src_len = dst_size - 1;
	memcpy(dst, src, src_len);
	dst[src_len] = '\0';
}

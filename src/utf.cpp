#include "utf.h"

#include "allocators.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

i32 wide_from_utf8(wchar_t* buf, i32 buf_count, const char* utf8)
{
	if (!buf || buf_count <= 0 || !utf8)
		return 0;
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, buf_count);
	if (len <= 0) {
		buf[0] = L'\0';
		return 0;
	}
	return len - 1;
}

i32 utf8_from_wide(char* buf, i32 buf_size, const wchar_t* wide)
{
	if (!buf || buf_size <= 0 || !wide)
		return 0;
	int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, buf_size, nullptr, nullptr);
	if (len <= 0) {
		buf[0] = '\0';
		return 0;
	}
	return len - 1;
}

i32 utf8_from_wide(char* buf, i32 buf_size, const wchar_t* wide, i32 wide_len)
{
	if (!buf || buf_size <= 0 || !wide || wide_len <= 0)
		return 0;
	int len = WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, buf, buf_size - 1, nullptr, nullptr);
	if (len <= 0) {
		buf[0] = '\0';
		return 0;
	}
	buf[len] = '\0';
	return len;
}

wchar_t* wide_from_utf8_a(Allocator* a, const char* utf8)
{
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	wchar_t* buf = (wchar_t*)a->alloc(len * sizeof(wchar_t), alignof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, len);
	return buf;
}

char* utf8_from_wide_a(Allocator* a, const wchar_t* wide)
{
	int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	char* buf = (char*)a->alloc(len);
	WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, len, nullptr, nullptr);
	return buf;
}
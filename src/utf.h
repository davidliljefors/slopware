#pragma once

#include "types.h"

struct Allocator;

i32 wide_from_utf8(wchar_t* buf, i32 buf_count, const char* utf8);
i32 utf8_from_wide(char* buf, i32 buf_size, const wchar_t* wide);
i32 utf8_from_wide(char* buf, i32 buf_size, const wchar_t* wide, i32 wide_len);

wchar_t* wide_from_utf8_a(Allocator* a, const char* utf8);
char* utf8_from_wide_a(Allocator* a, const wchar_t* wide);
#pragma once

#include "types.h"

// Boyer-Moore-Horspool case-insensitive search

struct BmhSearcher
{
	char needle[512];
	i32  needle_len;
	i32  skip[256];

	// Build from a *lowercase* query string.
	void build(const char* query_lower, i32 qlen);

	// Find next match in haystack. Returns nullptr on miss.
	const char* search(const char* hay, i32 hlen) const;
};

// Line helpers

i32         count_newlines(const char* start, const char* end);
const char* find_line_start(const char* pos, const char* buf_start);
const char* find_line_end(const char* s, const char* buf_end);

// ImGui key repeat helper

bool key_pressed(int key);

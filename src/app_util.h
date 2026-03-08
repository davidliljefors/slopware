#pragma once

#include "types.h"

// Get a path under the app's settings directory: <appdata>\<app_name>\<filename>.
// If filename is nullptr, returns the directory path itself.
// Creates the directory if it does not exist.
// Returns false on failure.
bool get_settings_path(char* buf, i32 buf_size,
	const char* app_name, const char* filename);

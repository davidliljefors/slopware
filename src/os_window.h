#pragma once

#include "types.h"


// Window management

// Browse for a folder using the system file dialog.
// Fills out_path with the selected folder's UTF-8 path.
// parent_hwnd is the owner window (pass host_hwnd()).
// Returns true if the user picked a folder.

bool browse_for_folder(char* out_path, i32 out_size, void* parent_hwnd);

// Minimize a window.
void window_minimize(void* hwnd);

// Get the DPI scale factor for a window
f32 window_get_dpi_scale(void* hwnd);

// Center a window on its monitor's work area.
void window_center_on_monitor(void* hwnd);

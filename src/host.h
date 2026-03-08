#pragma once

#include "types.h"

struct App;

// Request the host to run the given App. This will not return until the app quits.
i32 host_run(App* app);

// Request the host to show/activate the window
void host_show();

// Request the host to hide the window
void host_hide();

// Request the host to quit
void host_quit();

// Get the host window handle
void* host_hwnd();

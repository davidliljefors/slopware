#pragma once

#include "types.h"

struct TempAllocator;


// The host owns the Win32 window, D3D11 device, ImGui context, and the
// message loop. The app fills in this struct with its name, callbacks,
// and hotkey registrations. The host calls these at the appropriate times.

// Hotkey modifier flags (matches Win32 MOD_* values for RegisterHotKey)
static constexpr u32 HOTKEY_MOD_ALT     = 0x0001;
static constexpr u32 HOTKEY_MOD_CONTROL = 0x0002;
static constexpr u32 HOTKEY_MOD_SHIFT   = 0x0004;
static constexpr u32 HOTKEY_MOD_WIN     = 0x0008;

struct AppHotkey
{
	i32 id;            // unique hotkey ID
	u32 modifiers;     // MOD_CONTROL, MOD_ALT, etc.
	u32 vk;            // virtual-key code
	void (*callback)(); // called when this hotkey fires
};

struct App
{
	// Display name, windows title
	const char* name;

	// App identifier used for AppData directory, ini filename, etc.
	// e.g. "app_id" -> %APPDATA%\app_id\imgui.ini
	const char* app_id;

	// app initialization
	void (*init)();

	// app tick / update
	void (*tick)(TempAllocator* frame_alloc);

	// call when app is activating
	void (*on_activated)();

	// call when resized
	void (*on_resize)();

	// begin shutdown
	void (*begin_shutdown)();

	// wait for app to finish shutting down
	void (*wait_for_shutdown)();

	// Hotkeys registered via RegisterHotKey.  The host registers them on
	// startup and dispatches WM_HOTKEY to the matching callback.
	AppHotkey* hotkeys;
	i32 hotkey_count;

	// Initial window size in pixels.
	i32 initial_width;
	i32 initial_height;

	// Height in logical pixels of the title-bar drag region at the top of
	// the client area.
	i32 title_bar_height;

	// Width in logical pixels excluded from the right side of the drag region
	// (for minimize/close buttons).
	i32 title_bar_buttons_width;

	// If true, a system-tray icon is created and the window can hide to it.
	bool use_system_tray;
};

#pragma once

// --------------------------------------------------------------------------
// Plugin host  –  Win32 window + D3D11 + ImGui on a persistent worker thread.
// The window is created once and shown/hidden as needed.
// --------------------------------------------------------------------------

enum PluginMode
{
	PluginMode_GoToFile,
	PluginMode_GoToText,
	PluginMode_GoToTextInFile,
};

// Ensure the window thread is running (creates window + D3D on first call).
void plugin_host_ensure_running();

// Show the plugin window with the given mode.  If already visible, bring to front.
void plugin_host_show(PluginMode mode);

// Hide the plugin window (keeps thread + D3D alive).
void plugin_host_hide();

// Tear down everything (window, D3D, thread).
void plugin_host_shutdown();

// True while the window is visible.
bool plugin_host_is_visible();

// --------------------------------------------------------------------------
// Functions expected by imgui_util.cpp and others.
// --------------------------------------------------------------------------

void* host_hwnd();
void  host_show();
void  host_hide();
void  host_quit();

#include "plugin_host.h"
#include "plugin_internal.h"
#include "plugin_goto_file.h"
#include "plugin_goto_text.h"
#include "plugin_goto_text_in_file.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <atomic>
#include <d3d11.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <windows.h>
#include <windowsx.h>
#include <dbghelp.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dbghelp.lib")

#include "allocators.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "os.h"
#include "os_window.h"
#include "theme.h"
#include "utf.h"
#include "app_util.h"

// --------------------------------------------------------------------------
// Crash handler
// --------------------------------------------------------------------------

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;

	// Prevent re-entrancy if logging itself crashes
	static volatile LONG handling = 0;
	if (InterlockedCompareExchange(&handling, 1, 0) != 0)
		return EXCEPTION_CONTINUE_SEARCH;

	// Build crash log path next to the DLL
	char log_path[MAX_PATH] = {};
	{
		HMODULE hm = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)crash_handler, &hm);
		GetModuleFileNameA(hm, log_path, MAX_PATH);
		char* slash = strrchr(log_path, '\\');
		if (slash) *(slash + 1) = '\0';
		strcat_s(log_path, "gotoslop_crash.log");
	}

	// Fault address and instruction pointer
	void* fault_addr = (void*)ep->ExceptionRecord->ExceptionInformation[1];
#if defined(_M_X64) || defined(__x86_64__)
	void* rip = (void*)ep->ContextRecord->Rip;
#elif defined(_M_ARM64) || defined(__aarch64__)
	void* rip = (void*)ep->ContextRecord->Pc;
#elif defined(_M_IX86) || defined(__i386__)
	void* rip = (void*)ep->ContextRecord->Eip;
#else
	void* rip = (void*)ep->ExceptionRecord->ExceptionAddress;
#endif

	// Find which module the crash is in
	char mod_name[MAX_PATH] = "(unknown)";
	HMODULE crash_mod = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)rip, &crash_mod);
	if (crash_mod) {
		GetModuleFileNameA(crash_mod, mod_name, MAX_PATH);
		char* s = strrchr(mod_name, '\\');
		if (s) memmove(mod_name, s + 1, strlen(s + 1) + 1);
	}
	ULONG_PTR mod_offset = crash_mod ? ((ULONG_PTR)rip - (ULONG_PTR)crash_mod) : 0;

	// Stack trace (up to 32 frames)
	void* stack[32] = {};
	USHORT frames = CaptureStackBackTrace(0, 32, stack, nullptr);

	// Try to init symbols for readable names
	HANDLE proc = GetCurrentProcess();
	SymInitialize(proc, nullptr, TRUE);

	FILE* f = nullptr;
	fopen_s(&f, log_path, "w");

	char msg[2048];
	snprintf(msg, sizeof(msg),
		"ACCESS VIOLATION (0xc0000005)\n\n"
		"Thread ID:   %lu\n"
		"Instruction: %s + 0x%llX (addr %p)\n"
		"Fault addr:  %p (%s)\n\n"
		"Stack trace:\n",
		GetCurrentThreadId(),
		mod_name, (unsigned long long)mod_offset, rip,
		fault_addr,
		ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" : "WRITE");

	char stack_buf[4096] = {};
	for (USHORT i = 0; i < frames; i++) {
		char frame_mod[MAX_PATH] = "???";
		HMODULE fm = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)stack[i], &fm);
		ULONG_PTR fm_off = 0;
		if (fm) {
			GetModuleFileNameA(fm, frame_mod, MAX_PATH);
			char* s = strrchr(frame_mod, '\\');
			if (s) memmove(frame_mod, s + 1, strlen(s + 1) + 1);
			fm_off = (ULONG_PTR)stack[i] - (ULONG_PTR)fm;
		}

		// Try to resolve symbol name
		char sym_buf[sizeof(SYMBOL_INFO) + 256];
		SYMBOL_INFO* sym = (SYMBOL_INFO*)sym_buf;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		const char* sym_name = "";
		if (SymFromAddr(proc, (DWORD64)stack[i], &disp, sym))
			sym_name = sym->Name;

		// Try to resolve file + line
		IMAGEHLP_LINE64 line_info = {};
		line_info.SizeOfStruct = sizeof(line_info);
		DWORD line_disp = 0;
		char line_str[512] = {};
		if (SymGetLineFromAddr64(proc, (DWORD64)stack[i], &line_disp, &line_info))
			snprintf(line_str, sizeof(line_str), " [%s:%lu]", line_info.FileName, line_info.LineNumber);

		char tmp[512];
		snprintf(tmp, sizeof(tmp), "  [%2d] %s+0x%llX  %s%s\n",
			i, frame_mod, (unsigned long long)fm_off, sym_name, line_str);
		strcat_s(stack_buf, tmp);
	}

	SymCleanup(proc);

	strcat_s(msg, stack_buf);

	if (f) {
		fputs(msg, f);
		fclose(f);
	}

	// Also write to debug output
	OutputDebugStringA(msg);

	// Show message box with crash info
	char mb[2048];
	snprintf(mb, sizeof(mb),
		"ACCESS VIOLATION\n\n"
		"Instruction: %s + 0x%llX\n"
		"Fault addr: %p (%s)\n\n"
		"Full details written to:\n%s",
		mod_name, (unsigned long long)mod_offset,
		fault_addr,
		ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" : "WRITE",
		log_path);
	MessageBoxA(nullptr, mb, "GotoSlop Crash", MB_OK | MB_ICONERROR);

	return EXCEPTION_CONTINUE_SEARCH;
}

// --------------------------------------------------------------------------
// State
// --------------------------------------------------------------------------

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRTV = nullptr;
static HWND                     g_hwnd = nullptr;
static f32                      g_dpi_scale = 1.0f;
static std::atomic<bool>        g_visible { false };
static std::atomic<bool>        g_shutdown_requested { false };
static std::atomic<bool>        g_hide_requested { false };
static Thread*                  g_window_thread = nullptr;
static PluginMode               g_current_mode = PluginMode_GoToFile;
static bool                     g_mode_initialized = false;
static BumpAllocator            g_frame_alloc(512 * 1024);
static DWORD                    g_show_tick = 0;  // for activation grace period

// Custom window messages posted to the window thread
static constexpr UINT WM_PLUGIN_SHOW     = WM_APP + 100;
static constexpr UINT WM_PLUGIN_SHUTDOWN = WM_APP + 101;

// --------------------------------------------------------------------------
// Forward declarations
// --------------------------------------------------------------------------

static bool  create_d3d(HWND hwnd);
static void  cleanup_d3d();
static void  create_rtv();
static void  cleanup_rtv();
static void  render_frame();
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// --------------------------------------------------------------------------
// Glyph ranges for Segoe UI + gear icon
// --------------------------------------------------------------------------

static const ImWchar g_glyph_ranges[] = {
	0x0020, 0x00FF, // Basic Latin + Latin Supplement
	0x2699, 0x2699, // Gear icon ⚙
	0,
};

// --------------------------------------------------------------------------
// D3D11 helpers
// --------------------------------------------------------------------------

static bool create_d3d(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
		fl, 2, D3D11_SDK_VERSION, &sd,
		&g_pSwapChain, &g_pd3dDevice, &got, &g_pd3dDeviceContext);
	if (FAILED(hr)) return false;
	create_rtv();
	return true;
}

static void cleanup_d3d()
{
	cleanup_rtv();
	if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
	if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
	if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static void create_rtv()
{
	ID3D11Texture2D* bb = nullptr;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
	g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_mainRTV);
	bb->Release();
}

static void cleanup_rtv()
{
	if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

// --------------------------------------------------------------------------
// Font setup (simplified – no disk cache)
// --------------------------------------------------------------------------

static void setup_fonts(f32 dpi_scale)
{
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	f32 font_size = floorf(22.0f * dpi_scale);
	ImFontConfig cfg;
	cfg.PixelSnapH = true;

	if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", font_size, &cfg, g_glyph_ranges)) {
		if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", font_size, &cfg, g_glyph_ranges)) {
			ImFontConfig def_cfg;
			def_cfg.SizePixels = 13.0f * dpi_scale;
			io.Fonts->AddFontDefault(&def_cfg);
		}
	}

	io.Fonts->Build();
}

// --------------------------------------------------------------------------
// Showing / Hiding helpers (called on window thread)
// --------------------------------------------------------------------------

static void activate_mode(PluginMode mode)
{
	if (g_mode_initialized) {
		// Shutdown previous mode
		switch (g_current_mode) {
		case PluginMode_GoToFile: plugin_goto_file_shutdown(); break;
		case PluginMode_GoToText: plugin_goto_text_shutdown(); break;
		case PluginMode_GoToTextInFile: plugin_goto_text_in_file_shutdown(); break;
		}
	}
	g_current_mode = mode;
	switch (mode) {
	case PluginMode_GoToFile: plugin_goto_file_init(); break;
	case PluginMode_GoToText: plugin_goto_text_init(); break;
	case PluginMode_GoToTextInFile: plugin_goto_text_in_file_init(); break;
	}
	g_mode_initialized = true;
}

static void request_hide()
{
	g_hide_requested.store(true, std::memory_order_release);
}

static void do_show(PluginMode mode)
{
    window_center_on_monitor(g_hwnd);

	activate_mode(mode);

	const wchar_t* title = L"Go To File";
	if (mode == PluginMode_GoToText) title = L"Go To Text";
	else if (mode == PluginMode_GoToTextInFile) title = L"Search in File";
	SetWindowTextW(g_hwnd, title);

	// Clear any stale hide request before showing (focus-stealing can
	// trigger transient WM_ACTIVATE(WA_INACTIVE) during do_show)
	g_hide_requested.store(false, std::memory_order_release);

	g_show_tick = GetTickCount();
	ShowWindow(g_hwnd, SW_SHOW);
	UpdateWindow(g_hwnd);

	// Steal foreground focus from the VS process.
	DWORD fg_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
	DWORD our_thread = GetCurrentThreadId();
	if (fg_thread != our_thread) {
		AttachThreadInput(fg_thread, our_thread, TRUE);
		SetForegroundWindow(g_hwnd);
		AttachThreadInput(fg_thread, our_thread, FALSE);
	} else {
		SetForegroundWindow(g_hwnd);
	}

	g_visible.store(true);
}

static void do_hide()
{
	if (!g_visible.load()) return;

	if (g_mode_initialized) {
		switch (g_current_mode) {
		case PluginMode_GoToFile: plugin_goto_file_shutdown(); break;
		case PluginMode_GoToText: plugin_goto_text_shutdown(); break;
		case PluginMode_GoToTextInFile: plugin_goto_text_in_file_shutdown(); break;
		}
		g_mode_initialized = false;
	}

	ShowWindow(g_hwnd, SW_HIDE);
	ImGui::GetIO().ClearInputKeys();
	g_visible.store(false);
}

// --------------------------------------------------------------------------
// Rendering
// --------------------------------------------------------------------------

static void render_frame()
{
	g_frame_alloc.reset();

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	switch (g_current_mode) {
	case PluginMode_GoToFile: plugin_goto_file_tick(); break;
	case PluginMode_GoToText: plugin_goto_text_tick(); break;
	case PluginMode_GoToTextInFile: plugin_goto_text_in_file_tick(); break;
	}

	ImGui::Render();

	f32 cc[4];
	theme_clear_color(cc);
	g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
	g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, cc);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	g_pSwapChain->Present(1, 0);

	// Process deferred hide — runs after tick/render so shutdown
	if (g_hide_requested.exchange(false, std::memory_order_acquire)) {
		do_hide();
	}
}

// --------------------------------------------------------------------------
// WndProc
// --------------------------------------------------------------------------

static LRESULT WINAPI PluginWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg) {
	case WM_NCCALCSIZE:
		if (wParam) return 0;
		break;
	case WM_NCACTIVATE:
		return TRUE;
	case WM_NCHITTEST: {
		RECT rc;
		GetWindowRect(hWnd, &rc);
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		constexpr int EDGE = 6;
		if (x < rc.left + EDGE)   return HTLEFT;
		if (x > rc.right - EDGE)  return HTRIGHT;
		if (y < rc.top + EDGE)    return HTTOP;
		if (y > rc.bottom - EDGE) return HTBOTTOM;
		return HTCLIENT;
	}
	case WM_SIZE:
		if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
			cleanup_rtv();
			g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
				DXGI_FORMAT_UNKNOWN, 0);
			create_rtv();
			render_frame();
		}
		return 0;
	case WM_DPICHANGED: {
		f32 new_scale = HIWORD(wParam) / 96.0f;
		g_dpi_scale = new_scale;
		theme_apply(new_scale);
		setup_fonts(new_scale);
		ImGui_ImplDX11_InvalidateDeviceObjects();
		RECT* r = (RECT*)lParam;
		SetWindowPos(hWnd, nullptr, r->left, r->top,
			r->right - r->left, r->bottom - r->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_ACTIVATE:
		if (LOWORD(wParam) == WA_INACTIVE) {
			// Grace period: ignore deactivation right after showing
			if (GetTickCount() - g_show_tick > 500) {
				request_hide();
			}
		}
		return 0;
	case WM_CLOSE:
		request_hide();
		return 0;  // Don't destroy — just hide
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_PLUGIN_SHOW:
		do_show((PluginMode)wParam);
		return 0;
	case WM_PLUGIN_SHUTDOWN:
		do_hide();
		DestroyWindow(hWnd);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --------------------------------------------------------------------------
// Window thread  (long-lived — runs until plugin_shutdown)
// --------------------------------------------------------------------------

static void plugin_window_thread_func(void*)
{
	AddVectoredExceptionHandler(1, crash_handler);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// Enable dark title bar
	HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
	if (uxtheme) {
		typedef int (WINAPI *SetPreferredAppModeFunc)(int);
		auto fn = (SetPreferredAppModeFunc)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
		if (fn) fn(2); // ForceDark
	}

	HINSTANCE hInstance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = PluginWndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"GotoSlopPlugin";
	RegisterClassExW(&wc);

	HMONITOR hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { sizeof(mi) };
	GetMonitorInfoW(hmon, &mi);
	int sw = mi.rcWork.right - mi.rcWork.left;
	int sh = mi.rcWork.bottom - mi.rcWork.top;
	int ww = sw * 60 / 100;
	int wh = sh * 45 / 100;
	int wx = mi.rcWork.left + (sw - ww) / 2;
	int wy = mi.rcWork.top + (sh - wh) / 2;

	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST,
		wc.lpszClassName, L"GotoSlop",
		WS_POPUP | WS_THICKFRAME,
		wx, wy, ww, wh,
		nullptr, nullptr, hInstance, nullptr);

	// Dark mode for window
	BOOL dark = TRUE;
	DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
	// Minimal margin so DWM applies rounded corners without flooding
	// the client area with the accent color on show.
	MARGINS margins = { 0, 0, 1, 0 };
	DwmExtendFrameIntoClientArea(hwnd, &margins);

	g_hwnd = hwnd;

	if (!create_d3d(hwnd)) {
		cleanup_d3d();
		DestroyWindow(hwnd);
		UnregisterClassW(wc.lpszClassName, hInstance);
		g_hwnd = nullptr;
		return;
	}

	// Window starts hidden — shown via WM_PLUGIN_SHOW
	ShowWindow(hwnd, SW_HIDE);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;

	{
		DWORD delay_idx = 0, speed = 0;
		SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay_idx, 0);
		SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
		io.KeyRepeatDelay = 0.25f + delay_idx * 0.25f;
		io.KeyRepeatRate = 1.0f / (2.5f + speed * (30.0f - 2.5f) / 31.0f);
	}

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	g_dpi_scale = (f32)GetDpiForWindow(hwnd) / 96.0f;
	setup_fonts(g_dpi_scale);
	theme_apply(g_dpi_scale);

	// Message loop — runs until WM_QUIT
	bool done = false;
	while (!done) {
		MSG msg;

		if (g_visible.load()) {
			// Active: pump messages and render
			while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
				if (msg.message == WM_QUIT)
					done = true;
			}
			if (!done && g_visible.load())
				render_frame();
		} else {
			// Hidden: block until a message arrives (no CPU burn)
			if (GetMessage(&msg, nullptr, 0, 0) <= 0) {
				done = true;
			} else {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
	}

	// Final cleanup
	if (g_mode_initialized) {
		switch (g_current_mode) {
		case PluginMode_GoToFile: plugin_goto_file_shutdown(); break;
		case PluginMode_GoToText: plugin_goto_text_shutdown(); break;
		case PluginMode_GoToTextInFile: plugin_goto_text_in_file_shutdown(); break;
		}
		g_mode_initialized = false;
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanup_d3d();

	UnregisterClassW(wc.lpszClassName, hInstance);
	g_hwnd = nullptr;
	g_visible.store(false);
}

// --------------------------------------------------------------------------
// Public API
// --------------------------------------------------------------------------

void plugin_host_ensure_running()
{
	if (g_window_thread) return;
	g_shutdown_requested.store(false);
	g_window_thread = thread_create(plugin_window_thread_func, nullptr);

	// Wait for the window to be created so we can post messages to it
	while (!g_hwnd && !g_shutdown_requested.load())
		Sleep(1);
}

void plugin_host_show(PluginMode mode)
{
	plugin_host_ensure_running();
	if (!g_hwnd) return;

	// Post to window thread — it will do the actual ShowWindow + mode init
	PostMessageW(g_hwnd, WM_PLUGIN_SHOW, (WPARAM)mode, 0);
}

void plugin_host_hide()
{
	if (g_hwnd && g_visible.load()) {
		PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
	}
}

void plugin_host_shutdown()
{
	if (!g_window_thread) return;

	if (g_hwnd)
		PostMessageW(g_hwnd, WM_PLUGIN_SHUTDOWN, 0, 0);

	thread_join(g_window_thread);
	g_window_thread = nullptr;
}

bool plugin_host_is_visible()
{
	return g_visible.load();
}

// --------------------------------------------------------------------------
// host_* functions required by imgui_util.cpp
// --------------------------------------------------------------------------

void* host_hwnd()
{
	return (void*)g_hwnd;
}

void host_show()
{
	if (g_hwnd) {
		ShowWindow(g_hwnd, SW_SHOW);
		SetForegroundWindow(g_hwnd);
	}
}

void host_hide()
{
	do_hide();
}

void host_quit()
{
	request_hide();
}

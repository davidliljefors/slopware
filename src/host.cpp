#include "host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <atomic>
#include <d3d11.h>
#include <dwmapi.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

#include "allocators.h"
#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "murmurhash3.inl"
#include "app_util.h"
#include "os.h"
#include "theme.h"
#include "utf.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// Global State

static BumpAllocator g_frame_alloc(1 * 1024 * 1024);

// D3D11 state
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwnd = nullptr;
static f32 g_dpi_scale = 1.0f;

// The app we are hosting
static App* g_app = nullptr;
static bool g_app_initialized = false;
static bool g_quit_requested = false;

// System tray
static constexpr UINT WM_TRAYICON = WM_APP + 1;
static constexpr UINT TRAY_UID = 1;
static bool g_tray_added = false;

// Async font atlas state
static std::atomic<bool> g_font_atlas_ready { false };
static ImFontAtlas* g_pending_atlas = nullptr;
static Thread* g_font_thread = nullptr;
static f32 g_pending_dpi = 1.0f;

// Forward Declarations

static bool create_device_d3d(HWND hWnd);
static void cleanup_device_d3d();
static void create_render_target();
static void cleanup_render_target();
static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void rebuild_font_atlas(f32 dpi_scale);
static void render_frame();
static void tray_init(HWND hwnd, const char* tooltip);
static void tray_remove();
static void tray_show_context_menu(HWND hwnd);
static void enable_dark_mode();
static void enable_dark_mode_for_window(HWND hwnd);

// Allow dark mode tray menu on windows 10+

enum PreferredAppMode { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
typedef PreferredAppMode (WINAPI *SetPreferredAppModeFunc)(PreferredAppMode);
typedef BOOL (WINAPI *AllowDarkModeForWindowFunc)(HWND, BOOL);
typedef void (WINAPI *FlushMenuThemesFunc)();

static SetPreferredAppModeFunc    g_SetPreferredAppMode    = nullptr;
static AllowDarkModeForWindowFunc g_AllowDarkModeForWindow = nullptr;
static FlushMenuThemesFunc        g_FlushMenuThemes        = nullptr;

static void enable_dark_mode()
{
	HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
	if (!uxtheme)
		return;
	// Ordinal 135 = SetPreferredAppMode (Win10 1903+)
	g_SetPreferredAppMode = (SetPreferredAppModeFunc)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
	// Ordinal 133 = AllowDarkModeForWindow
	g_AllowDarkModeForWindow = (AllowDarkModeForWindowFunc)GetProcAddress(uxtheme, MAKEINTRESOURCEA(133));
	// Ordinal 136 = FlushMenuThemes
	g_FlushMenuThemes = (FlushMenuThemesFunc)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));

	if (g_SetPreferredAppMode)
		g_SetPreferredAppMode(ForceDark);
	if (g_FlushMenuThemes)
		g_FlushMenuThemes();
}

static void enable_dark_mode_for_window(HWND hwnd)
{
	if (g_AllowDarkModeForWindow)
		g_AllowDarkModeForWindow(hwnd, TRUE);

	// DWMWA_USE_IMMERSIVE_DARK_MODE = 20
	BOOL dark = TRUE;
	DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
}

// System Tray

static void tray_init(HWND hwnd, const char* tooltip)
{
	NOTIFYICONDATAW nid = {};
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = TRAY_UID;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
	nid.uCallbackMessage = WM_TRAYICON;
	nid.uVersion = NOTIFYICON_VERSION_4;

	nid.hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
	if (!nid.hIcon)
		nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

	wchar_t tip_wide[128];
	wide_from_utf8(tip_wide, 128, tooltip);
	wcscpy_s(nid.szTip, _countof(nid.szTip), tip_wide);

	Shell_NotifyIconW(NIM_ADD, &nid);
	Shell_NotifyIconW(NIM_SETVERSION, &nid);
	g_tray_added = true;
}

static void tray_remove()
{
	if (!g_tray_added)
		return;
	NOTIFYICONDATAW nid = {};
	nid.cbSize = sizeof(nid);
	nid.hWnd = g_hwnd;
	nid.uID = TRAY_UID;
	Shell_NotifyIconW(NIM_DELETE, &nid);
	g_tray_added = false;
}

enum TrayMenuId
{
	TrayMenuId_Show = 1,
	TrayMenuId_Quit = 2,
};

static void tray_show_context_menu(HWND hwnd)
{
	HMENU menu = CreatePopupMenu();

	bool visible = IsWindowVisible(hwnd) != 0;

	if (visible) {
		AppendMenuW(menu, MF_STRING, TrayMenuId_Show, L"Hide");
	} else {
		AppendMenuW(menu, MF_STRING | MF_DEFAULT, TrayMenuId_Show, L"Show");
	}

	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING, TrayMenuId_Quit, L"Quit");

	// Required so the menu dismisses when clicking elsewhere
	SetForegroundWindow(hwnd);

	POINT pt;
	GetCursorPos(&pt);
	UINT cmd = (UINT)TrackPopupMenu(menu,
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
		pt.x, pt.y, 0, hwnd, nullptr);

	DestroyMenu(menu);

	switch (cmd) {
	case TrayMenuId_Show:
		if (visible)
			host_hide();
		else
			host_show();
		break;
	case TrayMenuId_Quit:
		host_quit();
		break;
	}
}

// D3D11 Helpers

static bool create_device_d3d(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hWnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	UINT createDeviceFlags = 0;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[] = {
		D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0
	};
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
		featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
		&g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
	if (hr != S_OK)
		return false;

	create_render_target();
	return true;
}

static void cleanup_device_d3d()
{
	cleanup_render_target();
	if (g_pSwapChain) {
		g_pSwapChain->Release();
		g_pSwapChain = nullptr;
	}
	if (g_pd3dDeviceContext) {
		g_pd3dDeviceContext->Release();
		g_pd3dDeviceContext = nullptr;
	}
	if (g_pd3dDevice) {
		g_pd3dDevice->Release();
		g_pd3dDevice = nullptr;
	}
}

static void create_render_target()
{
	ID3D11Texture2D* pBackBuffer = nullptr;
	g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
	g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
	pBackBuffer->Release();
}

static void cleanup_render_target()
{
	if (g_mainRenderTargetView) {
		g_mainRenderTargetView->Release();
		g_mainRenderTargetView = nullptr;
	}
}

static bool reset_device_d3d(HWND hWnd)
{
	ImGui_ImplDX11_Shutdown();
	cleanup_device_d3d();

	if (!create_device_d3d(hWnd))
		return false;

	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
	rebuild_font_atlas(g_dpi_scale);
	return true;
}

// Font Atlas Cache

static constexpr u32 FONT_CACHE_MAGIC = 0x464E5443; // 'FNTC'
static constexpr u32 FONT_CACHE_VERSION = 3;

struct FontCacheHeader
{
	u32 magic;
	u32 version;
	u64 input_hash;
	i32 tex_width;
	i32 tex_height;
	i32 font_count;
	i32 custom_rect_count;
	ImVec2 tex_uv_scale;
	ImVec2 tex_uv_white_pixel;
	ImVec4 tex_uv_lines[IM_DRAWLIST_TEX_LINES_WIDTH_MAX + 1];
};

struct CachedFontHeader
{
	f32 font_size;
	f32 ascent;
	f32 descent;
	i32 metrics_total_surface;
	i32 num_glyphs;
};

struct CachedCustomRect
{
	u16 x, y, width, height;
	u32 glyph_id_and_colored;
	f32 glyph_advance_x;
	f32 glyph_offset_x, glyph_offset_y;
};

static void hash_append_font_file(u8* buf, i32& pos, const char* path)
{
	i32 path_len = 0;
	while (path[path_len])
		path_len++;
	memcpy(buf + pos, path, path_len);
	pos += path_len;

	wchar_t wide_path[MAX_PATH];
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, MAX_PATH)) {
		WIN32_FILE_ATTRIBUTE_DATA fad;
		if (GetFileAttributesExW(wide_path, GetFileExInfoStandard, &fad)) {
			memcpy(buf + pos, &fad.ftLastWriteTime, sizeof(fad.ftLastWriteTime));
			pos += sizeof(fad.ftLastWriteTime);
			memcpy(buf + pos, &fad.nFileSizeLow, sizeof(fad.nFileSizeLow));
			pos += sizeof(fad.nFileSizeLow);
			memcpy(buf + pos, &fad.nFileSizeHigh, sizeof(fad.nFileSizeHigh));
			pos += sizeof(fad.nFileSizeHigh);
		}
	}
}

static u64 compute_font_atlas_hash(const char** font_paths, i32 font_path_count,
	f32 font_size, const ImWchar* glyph_ranges, i32 tex_desired_width)
{
	// Pack all inputs into a contiguous buffer and hash once
	u8 buf[4096];
	i32 pos = 0;

	for (i32 i = 0; i < font_path_count; i++)
		hash_append_font_file(buf, pos, font_paths[i]);

	memcpy(buf + pos, &font_size, sizeof(font_size));
	pos += sizeof(font_size);

	const ImWchar* p = glyph_ranges;
	while (*p)
		p += 2;
	p++;
	i32 ranges_bytes = (i32)((const u8*)p - (const u8*)glyph_ranges);
	memcpy(buf + pos, glyph_ranges, ranges_bytes);
	pos += ranges_bytes;
	memcpy(buf + pos, &tex_desired_width, sizeof(tex_desired_width));
	pos += sizeof(tex_desired_width);

	return murmurhash3_64(buf, pos);
}

static bool get_font_cache_path(char* buf, i32 buf_size, f32 font_size)
{
	if (!g_app || !g_app->app_id)
		return false;

	char filename[64];
	i32 size_int = (i32)font_size;
	sprintf_s(filename, 64, "font_atlas_%d.cache", size_int);
	return get_settings_path(buf, buf_size, g_app->app_id, filename);
}

static bool load_font_atlas_cache(ImFontAtlas* atlas, u64 expected_hash, f32 font_size)
{
	char cache_path[512];
	if (!get_font_cache_path(cache_path, 512, font_size))
		return false;

	wchar_t cache_path_wide[512];
	wide_from_utf8(cache_path_wide, 512, cache_path);

	HANDLE hFile = CreateFileW(cache_path_wide, GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER file_size_li;
	GetFileSizeEx(hFile, &file_size_li);
	i64 file_size = file_size_li.QuadPart;
	if (file_size < (i64)sizeof(FontCacheHeader) || file_size > 64 * 1024 * 1024) {
		CloseHandle(hFile);
		return false;
	}

	u8* file_data = (u8*)VirtualAlloc(nullptr, (SIZE_T)file_size,
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!file_data) {
		CloseHandle(hFile);
		return false;
	}

	DWORD bytes_read;
	BOOL ok = ReadFile(hFile, file_data, (DWORD)file_size, &bytes_read, nullptr);
	CloseHandle(hFile);
	if (!ok || (i64)bytes_read != file_size) {
		VirtualFree(file_data, 0, MEM_RELEASE);
		return false;
	}

	u8* cursor = file_data;
	u8* file_end = file_data + bytes_read;
	bool success = false;

	// --- Validation pass (no atlas modifications until fully validated) ---

	FontCacheHeader* header;
	CachedCustomRect* cached_rects;
	struct FontRef
	{
		CachedFontHeader* hdr;
		ImFontGlyph* glyphs;
	};
	FontRef font_refs[16];

	if (cursor + sizeof(FontCacheHeader) > file_end)
		goto done;
	header = (FontCacheHeader*)cursor;
	cursor += sizeof(FontCacheHeader);

	if (header->magic != FONT_CACHE_MAGIC || header->version != FONT_CACHE_VERSION || header->input_hash != expected_hash || header->font_count != atlas->Fonts.Size || header->tex_width <= 0 || header->tex_height <= 0 || header->custom_rect_count < 0 || header->custom_rect_count > 1024)
		goto done;

	{
		i64 rects_bytes = (i64)header->custom_rect_count * sizeof(CachedCustomRect);
		if (cursor + rects_bytes > file_end)
			goto done;
	}
	cached_rects = (CachedCustomRect*)cursor;
	cursor += header->custom_rect_count * sizeof(CachedCustomRect);

	if (header->font_count > 16)
		goto done;

	for (i32 i = 0; i < header->font_count; i++) {
		if (cursor + sizeof(CachedFontHeader) > file_end)
			goto done;
		font_refs[i].hdr = (CachedFontHeader*)cursor;
		cursor += sizeof(CachedFontHeader);
		i32 ng = font_refs[i].hdr->num_glyphs;
		if (ng < 0 || ng > 200000)
			goto done;
		i64 glyph_bytes = (i64)ng * sizeof(ImFontGlyph);
		if (cursor + glyph_bytes > file_end)
			goto done;
		font_refs[i].glyphs = (ImFontGlyph*)cursor;
		cursor += glyph_bytes;
	}

	{
		i64 pixel_bytes = (i64)header->tex_width * header->tex_height;
		if (cursor + pixel_bytes > file_end)
			goto done;
	}

	// --- Apply pass (all data validated, no failure possible) ---

	atlas->TexUvScale = header->tex_uv_scale;
	atlas->TexUvWhitePixel = header->tex_uv_white_pixel;
	memcpy(atlas->TexUvLines, header->tex_uv_lines, sizeof(atlas->TexUvLines));

	atlas->CustomRects.resize(header->custom_rect_count);
	for (i32 i = 0; i < header->custom_rect_count; i++) {
		CachedCustomRect& cr = cached_rects[i];
		ImFontAtlasCustomRect& r = atlas->CustomRects[i];
		r.X = cr.x;
		r.Y = cr.y;
		r.Width = cr.width;
		r.Height = cr.height;
		r.GlyphID = cr.glyph_id_and_colored & 0x7FFFFFFF;
		r.GlyphColored = (cr.glyph_id_and_colored >> 31) & 1;
		r.GlyphAdvanceX = cr.glyph_advance_x;
		r.GlyphOffset = ImVec2(cr.glyph_offset_x, cr.glyph_offset_y);
		r.Font = nullptr;
	}

	for (i32 i = 0; i < header->font_count; i++) {
		ImFont* font = atlas->Fonts[i];
		CachedFontHeader* fh = font_refs[i].hdr;
		font->ContainerAtlas = atlas;
		font->FontSize = fh->font_size;
		font->Ascent = fh->ascent;
		font->Descent = fh->descent;
		font->MetricsTotalSurface = fh->metrics_total_surface;
		font->Glyphs.resize(fh->num_glyphs);
		memcpy(font->Glyphs.Data, font_refs[i].glyphs,
			fh->num_glyphs * sizeof(ImFontGlyph));
		font->BuildLookupTable();
	}

	{
		i32 pixel_count = header->tex_width * header->tex_height;
		atlas->TexPixelsAlpha8 = (unsigned char*)IM_ALLOC(pixel_count);
		memcpy(atlas->TexPixelsAlpha8, cursor, pixel_count);
	}

	atlas->TexWidth = header->tex_width;
	atlas->TexHeight = header->tex_height;
	atlas->TexReady = true;
	success = true;

done:
	VirtualFree(file_data, 0, MEM_RELEASE);
	return success;
}

static void save_font_atlas_cache(ImFontAtlas* atlas, u64 hash, f32 font_size)
{
	char cache_path[512];
	if (!get_font_cache_path(cache_path, 512, font_size))
		return;

	wchar_t cache_path_wide[512];
	wide_from_utf8(cache_path_wide, 512, cache_path);

	HANDLE hFile = CreateFileW(cache_path_wide, GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE)
		return;

	DWORD written;

	FontCacheHeader header = {};
	header.magic = FONT_CACHE_MAGIC;
	header.version = FONT_CACHE_VERSION;
	header.input_hash = hash;
	header.tex_width = atlas->TexWidth;
	header.tex_height = atlas->TexHeight;
	header.font_count = atlas->Fonts.Size;
	header.custom_rect_count = atlas->CustomRects.Size;
	header.tex_uv_scale = atlas->TexUvScale;
	header.tex_uv_white_pixel = atlas->TexUvWhitePixel;
	memcpy(header.tex_uv_lines, atlas->TexUvLines, sizeof(header.tex_uv_lines));
	WriteFile(hFile, &header, sizeof(header), &written, nullptr);

	for (i32 i = 0; i < atlas->CustomRects.Size; i++) {
		const ImFontAtlasCustomRect& r = atlas->CustomRects[i];
		CachedCustomRect cr = {};
		cr.x = r.X;
		cr.y = r.Y;
		cr.width = r.Width;
		cr.height = r.Height;
		cr.glyph_id_and_colored = (r.GlyphID & 0x7FFFFFFF) | ((u32)r.GlyphColored << 31);
		cr.glyph_advance_x = r.GlyphAdvanceX;
		cr.glyph_offset_x = r.GlyphOffset.x;
		cr.glyph_offset_y = r.GlyphOffset.y;
		WriteFile(hFile, &cr, sizeof(cr), &written, nullptr);
	}

	for (i32 i = 0; i < atlas->Fonts.Size; i++) {
		ImFont* font = atlas->Fonts[i];
		CachedFontHeader fh = {};
		fh.font_size = font->FontSize;
		fh.ascent = font->Ascent;
		fh.descent = font->Descent;
		fh.metrics_total_surface = font->MetricsTotalSurface;
		fh.num_glyphs = font->Glyphs.Size;
		WriteFile(hFile, &fh, sizeof(fh), &written, nullptr);
		WriteFile(hFile, font->Glyphs.Data,
			fh.num_glyphs * (DWORD)sizeof(ImFontGlyph), &written, nullptr);
	}

	if (!atlas->TexPixelsAlpha8) {
		unsigned char* pixels;
		int w, h;
		atlas->GetTexDataAsAlpha8(&pixels, &w, &h);
	}
	WriteFile(hFile, atlas->TexPixelsAlpha8,
		atlas->TexWidth * atlas->TexHeight, &written, nullptr);

	CloseHandle(hFile);
}

// Font Atlas

// Glyph range tables (file-scope, used by both build and hash)
static const ImWchar g_glyph_ranges[] = {
	0x0020,
	0x00FF, // Basic Latin + Latin Supplement
	0x0100,
	0x024F, // Latin Extended-A + B
	0x0370,
	0x03FF, // Greek and Coptic
	0x0400,
	0x052F, // Cyrillic + Cyrillic Supplement
	0x0590,
	0x05FF, // Hebrew
	0x0600,
	0x06FF, // Arabic
	0x0900,
	0x097F, // Devanagari
	0x0E00,
	0x0E7F, // Thai
	0x1100,
	0x11FF, // Hangul Jamo
	0x2000,
	0x206F, // General Punctuation
	0x2699,
	0x2699, // Gear icon
	0x3000,
	0x30FF, // CJK Symbols + Hiragana + Katakana
	0x3100,
	0x312F, // Bopomofo
	0x31F0,
	0x31FF, // Katakana Phonetic Extensions
	0x4E00,
	0x9FFF, // CJK Unified Ideographs
	0xAC00,
	0xD7AF, // Hangul Syllables
	0xFF00,
	0xFFEF, // Halfwidth and Fullwidth Forms
	0,
};

static const ImWchar g_cjk_ranges[] = {
	0x3000,
	0x30FF, // CJK Symbols + Hiragana + Katakana
	0x3100,
	0x312F, // Bopomofo
	0x31F0,
	0x31FF, // Katakana Phonetic Extensions
	0x4E00,
	0x9FFF, // CJK Unified Ideographs
	0xFF00,
	0xFFEF, // Halfwidth and Fullwidth Forms
	0,
};

static const ImWchar g_korean_ranges[] = {
	0x1100,
	0x11FF, // Hangul Jamo
	0xAC00,
	0xD7AF, // Hangul Syllables
	0,
};

// Configure fonts on an atlas, build or load from cache.
static void setup_and_build_atlas(ImFontAtlas* atlas, f32 dpi_scale)
{
	atlas->Clear();
	f32 font_size = floorf(22.0f * dpi_scale);
	ImFontConfig cfg;
	cfg.PixelSnapH = true;

	atlas->TexDesiredWidth = 4096;

	const char* font_path = nullptr;
	ImFont* font = atlas->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", font_size, &cfg, g_glyph_ranges);
	if (font) {
		font_path = "C:\\Windows\\Fonts\\seguisym.ttf";
	} else {
		font = atlas->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", font_size, &cfg, g_glyph_ranges);
		if (font)
			font_path = "C:\\Windows\\Fonts\\segoeui.ttf";
	}
	if (!font) {
		font_path = "imgui_default";
		ImFontConfig def_cfg;
		def_cfg.SizePixels = 13.0f * dpi_scale;
		atlas->AddFontDefault(&def_cfg);
	}

	const char* font_paths[4];
	i32 font_path_count = 0;
	font_paths[font_path_count++] = font_path;

	if (font) {
		ImFontConfig cjk_cfg;
		cjk_cfg.MergeMode = true;
		cjk_cfg.PixelSnapH = true;
		if (atlas->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", font_size, &cjk_cfg, g_cjk_ranges))
			font_paths[font_path_count++] = "C:\\Windows\\Fonts\\msyh.ttc";

		ImFontConfig kr_cfg;
		kr_cfg.MergeMode = true;
		kr_cfg.PixelSnapH = true;
		if (atlas->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", font_size, &kr_cfg, g_korean_ranges))
			font_paths[font_path_count++] = "C:\\Windows\\Fonts\\malgun.ttf";
	}

	u64 hash = compute_font_atlas_hash(font_paths, font_path_count, font_size,
		g_glyph_ranges, atlas->TexDesiredWidth);

	LARGE_INTEGER t0, t1, freq;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	bool from_cache = load_font_atlas_cache(atlas, hash, font_size);
	if (!from_cache) {
		atlas->Build();
		save_font_atlas_cache(atlas, hash, font_size);
	}

	QueryPerformanceCounter(&t1);
	f64 ms = (f64)(t1.QuadPart - t0.QuadPart) / (f64)freq.QuadPart * 1000.0;
	printf("Font atlas %s: %dx%d (%d glyphs) in %.1f ms\n",
		from_cache ? "loaded from cache" : "built",
		atlas->TexWidth, atlas->TexHeight,
		atlas->Fonts[0]->Glyphs.Size, ms);
}

struct FontBuildArgs
{
	f32 dpi_scale;
};

static void font_build_thread_func(void* user_data)
{
	FontBuildArgs* args = (FontBuildArgs*)user_data;
	
	ImFontAtlas* atlas = IM_NEW(ImFontAtlas);
	setup_and_build_atlas(atlas, args->dpi_scale);
	g_pending_atlas = atlas;
	g_pending_dpi = args->dpi_scale;
	g_font_atlas_ready.store(true, std::memory_order_release);
}

static FontBuildArgs g_font_build_args;

static void start_async_font_build(f32 dpi_scale)
{
	// If a previous build thread is still around, wait for it
	if (g_font_thread) {
		thread_join(g_font_thread);
		g_font_thread = nullptr;
	}
	if (g_pending_atlas) {
		IM_DELETE(g_pending_atlas);
		g_pending_atlas = nullptr;
	}
	g_font_atlas_ready.store(false, std::memory_order_relaxed);
	g_font_build_args.dpi_scale = dpi_scale;
	g_font_thread = thread_create(font_build_thread_func, &g_font_build_args);
}

// Rebuild io.Fonts through ImGui's normal path. If the background thread
// already cached the atlas this loads instantly; otherwise it builds.
static void rebuild_font_atlas(f32 dpi_scale)
{
	setup_and_build_atlas(ImGui::GetIO().Fonts, dpi_scale);
	ImGui_ImplDX11_InvalidateDeviceObjects();
	g_dpi_scale = dpi_scale;
}


// Rendering


static void render_frame()
{
	if (!g_app_initialized)
		return;

	g_frame_alloc.reset();

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	g_app->tick(&g_frame_alloc);

	ImGui::Render();

	f32 cc[4];
	theme_clear_color(cc);
	g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
	g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	HRESULT hr = g_pSwapChain->Present(1, DXGI_SWAP_EFFECT_FLIP_DISCARD);
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
		reset_device_d3d(g_hwnd);
	}
}


// Win32 Message Handler


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	switch (msg) {
	case WM_NCCALCSIZE:
		if (wParam) return 0;
		break;
	case WM_GETMINMAXINFO: {
		// Constrain maximized window to the monitor work area so it
		// doesn't cover the taskbar.
		HMONITOR mon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { sizeof(mi) };
		GetMonitorInfoW(mon, &mi);
		MINMAXINFO* mmi = (MINMAXINFO*)lParam;
		mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
		mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
		mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
		mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
		return 0;
	}
	case WM_NCACTIVATE:
		return TRUE;
	case WM_NCHITTEST: {
		RECT rc;
		GetWindowRect(hWnd, &rc);
		int x = GET_X_LPARAM(lParam);
		int y = GET_Y_LPARAM(lParam);
		constexpr int EDGE = 6;
		bool left = x < rc.left + EDGE;
		bool right = x > rc.right - EDGE;
		bool top = y < rc.top + EDGE;
		bool bottom = y > rc.bottom - EDGE;
		if (top && left)
			return HTTOPLEFT;
		if (top && right)
			return HTTOPRIGHT;
		if (bottom && left)
			return HTBOTTOMLEFT;
		if (bottom && right)
			return HTBOTTOMRIGHT;
		if (left)
			return HTLEFT;
		if (right)
			return HTRIGHT;
		if (top)
			return HTTOP;
		if (bottom)
			return HTBOTTOM;
		// Title-bar drag region
		if (g_app->title_bar_height > 0) {
			UINT dpi = GetDpiForWindow(hWnd);
			int title_h = MulDiv(g_app->title_bar_height, dpi, 96);
			int buttons_w = MulDiv(g_app->title_bar_buttons_width, dpi, 96);
			if (y - rc.top < title_h && x < rc.right - buttons_w)
				return HTCAPTION;
		}
		return HTCLIENT;
	}
	case WM_SIZE:
		if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
			cleanup_render_target();
			HRESULT hr = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
				DXGI_FORMAT_UNKNOWN, 0);
			if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
				reset_device_d3d(hWnd);
			} else {
				create_render_target();
			}
			render_frame();
		}
		return 0;
	case WM_DPICHANGED: {
		UINT new_dpi = HIWORD(wParam);
		f32 new_scale = new_dpi / 96.0f;
		theme_apply(new_scale);
		rebuild_font_atlas(new_scale);
		RECT* suggested = (RECT*)lParam;
		SetWindowPos(hWnd, nullptr,
			suggested->left, suggested->top,
			suggested->right - suggested->left,
			suggested->bottom - suggested->top,
			SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_EXITSIZEMOVE:
		if (g_app && g_app->on_resize)
			g_app->on_resize();
		return 0;
	case WM_HOTKEY:
		if (g_app) {
			for (i32 i = 0; i < g_app->hotkey_count; i++) {
				if ((i32)wParam == g_app->hotkeys[i].id) {
					g_app->hotkeys[i].callback();
					break;
				}
			}
		}
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		break;
	case WM_TRAYICON:
		switch (LOWORD(lParam)) {
		case WM_LBUTTONDBLCLK:
			if (IsWindowVisible(hWnd))
				host_hide();
			else
				host_show();
			return 0;
		case WM_RBUTTONUP:
			tray_show_context_menu(hWnd);
			return 0;
		}
		return 0;
	case WM_CLOSE:
		DestroyWindow(hWnd);
		return 0;
	case WM_DESTROY:
		tray_remove();
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Public host API (called by apps)

void host_show()
{
	if (g_hwnd) {
		ShowWindow(g_hwnd, SW_SHOW);
		SetForegroundWindow(g_hwnd);
		if (g_app && g_app->on_activated)
			g_app->on_activated();
	}
}

void host_hide()
{
	if (g_hwnd) {
		ShowWindow(g_hwnd, SW_HIDE);
		ImGui::GetIO().ClearInputKeys();
	}
}

void host_quit()
{
	g_quit_requested = true;
}

void* host_hwnd()
{
	return (void*)g_hwnd;
}

// host_run -- main entry point

i32 host_run(App* app)
{
	g_app = app;

#ifdef ENABLE_CONSOLE
	AllocConsole();
	FILE* dummy;
	freopen_s(&dummy, "CONOUT$", "w", stdout);
	freopen_s(&dummy, "CONOUT$", "w", stderr);
#endif

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	enable_dark_mode();

	HINSTANCE hInstance = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = L"HOST_WNDCLASSNAME";
	RegisterClassExW(&wc);

	HMONITOR hmon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO mi = { sizeof(mi) };
	GetMonitorInfoW(hmon, &mi);
	int screen_w = mi.rcWork.right - mi.rcWork.left;
	int screen_h = mi.rcWork.bottom - mi.rcWork.top;
	int win_w = (app->initial_width > 0) ? app->initial_width : screen_w * 40 / 100;
	int win_h = (app->initial_height > 0) ? app->initial_height : screen_h * 40 / 100;
	int win_x = mi.rcWork.left + (screen_w - win_w) / 2;
	int win_y = mi.rcWork.top + (screen_h - win_h) / 2;

	DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

	HWND hwnd = CreateWindowW(wc.lpszClassName, L"Loading...",
		style,
		win_x, win_y, win_w, win_h,
		nullptr, nullptr, hInstance, nullptr);
	g_hwnd = hwnd;

	enable_dark_mode_for_window(hwnd);

	{
		MARGINS margins = { -1, -1, -1, -1 };
		DwmExtendFrameIntoClientArea(hwnd, &margins);
	}

	{
		wchar_t title_wide[256];
		wide_from_utf8(title_wide, 256, app->name);
		SetWindowTextW(hwnd, title_wide);
	}

	if (!create_device_d3d(hwnd)) {
		cleanup_device_d3d();
		UnregisterClassW(wc.lpszClassName, hInstance);
		return 1;
	}

	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	if (app->use_system_tray)
		tray_init(hwnd, app->name);

	for (i32 i = 0; i < app->hotkey_count; i++) {
		RegisterHotKey(hwnd, app->hotkeys[i].id, app->hotkeys[i].modifiers, app->hotkeys[i].vk);
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

	static char ini_path[512] = {};
	if (get_settings_path(ini_path, (i32)sizeof(ini_path), app->app_id, "imgui.ini"))
		io.IniFilename = ini_path;

	// get windows repeat settings into imgui
	{
		DWORD delay_idx = 0;
		DWORD speed = 0;
		SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay_idx, 0);
		SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
		io.KeyRepeatDelay = 0.25f + delay_idx * 0.25f;
		io.KeyRepeatRate = 1.0f / (2.5f + speed * (30.0f - 2.5f) / 31.0f);
	}

	ImGui_ImplWin32_Init(hwnd);
	ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

	UINT dpi = GetDpiForWindow(hwnd);
	f32 dpi_scale = dpi / 96.0f;

	// load basic characters immediately
	{
		static const ImWchar startup_ranges[] = {
			0x0020,
			0x00FF, // Basic Latin + Latin Supplement
			0,
		};
		f32 font_size = floorf(22.0f * dpi_scale);
		ImFontConfig cfg;
		cfg.PixelSnapH = true;
		if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", font_size, &cfg, startup_ranges))
			io.Fonts->AddFontDefault(&cfg);
		io.Fonts->Build();
	}
	theme_apply(dpi_scale);
	// start building atlas in background
	start_async_font_build(dpi_scale);

	app->init();
	g_app_initialized = true;

	bool done = false;
	while (!done) {
		MSG msg;

		// When hidden, block on messages
		if (!IsWindowVisible(hwnd) && !g_quit_requested) {
			if (GetMessage(&msg, nullptr, 0, 0) <= 0) {
				done = true;
				break;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			continue;
		}

		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
				done = true;
		}
		if (done)
			break;

		render_frame();

		if (g_font_atlas_ready.exchange(false, std::memory_order_acquire)) {
			rebuild_font_atlas(g_pending_dpi);
			IM_DELETE(g_pending_atlas);
			g_pending_atlas = nullptr;
		}

		if (g_quit_requested) {
			PostQuitMessage(0);
			break;
		}
	}

	g_app_initialized = false;

	if (g_font_thread) {
		thread_join(g_font_thread);
		g_font_thread = nullptr;
	}
	if (g_pending_atlas) {
		IM_DELETE(g_pending_atlas);
		g_pending_atlas = nullptr;
	}

	if(app->begin_shutdown)
		app->begin_shutdown();

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();

	ImGui::DestroyContext();

	for (i32 i = 0; i < app->hotkey_count; i++) {
		UnregisterHotKey(hwnd, app->hotkeys[i].id);
	}

	if (app->use_system_tray)
		tray_remove();
	cleanup_device_d3d();
	DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, hInstance);

	if(app->wait_for_shutdown)
		app->wait_for_shutdown();

	return 0;
}

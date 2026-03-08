#include "os_window.h"

#include <stdio.h>

#include "utf.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#pragma comment(lib, "ole32.lib")

// Browse for folder (COM IFileOpenDialog)

bool browse_for_folder(char* out_path, i32 out_size, void* parent_hwnd)
{
	bool result = false;
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (FAILED(hr))
		return false;

	IFileOpenDialog* dialog = nullptr;
	hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
		IID_IFileOpenDialog, (void**)&dialog);
	if (SUCCEEDED(hr)) {
		DWORD options = 0;
		dialog->GetOptions(&options);
		dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		dialog->SetTitle(L"Select Directory to Scan");

		if (out_path[0] != '\0') {
			wchar_t wide[1024];
			wide_from_utf8(wide, 1024, out_path);
			IShellItem* folder = nullptr;
			if (SUCCEEDED(SHCreateItemFromParsingName(wide, nullptr, IID_IShellItem, (void**)&folder))) {
				dialog->SetFolder(folder);
				folder->Release();
			}
		}

		hr = dialog->Show((HWND)parent_hwnd);
		if (SUCCEEDED(hr)) {
			IShellItem* item = nullptr;
			hr = dialog->GetResult(&item);
			if (SUCCEEDED(hr)) {
				PWSTR path = nullptr;
				hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
				if (SUCCEEDED(hr) && path) {
					utf8_from_wide(out_path, out_size, path);
					CoTaskMemFree(path);
					result = true;
				}
				item->Release();
			}
		}
		dialog->Release();
	}
	CoUninitialize();
	return result;
}


// Window operations


void window_minimize(void* hwnd)
{
	ShowWindow((HWND)hwnd, SW_MINIMIZE);
}

f32 window_get_dpi_scale(void* hwnd)
{
	return (f32)GetDpiForWindow((HWND)hwnd) / 96.0f;
}

void window_center_on_monitor(void* hwnd)
{
	HWND h = (HWND)hwnd;
	HMONITOR hmon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = { sizeof(mi) };
	GetMonitorInfoW(hmon, &mi);
	RECT wr;
	GetWindowRect(h, &wr);
	int win_w = wr.right - wr.left;
	int win_h = wr.bottom - wr.top;
	int screen_w = mi.rcWork.right - mi.rcWork.left;
	int screen_h = mi.rcWork.bottom - mi.rcWork.top;
	int cx = mi.rcWork.left + (screen_w - win_w) / 2;
	int cy = mi.rcWork.top + (screen_h - win_h) / 2;
	SetWindowPos(h, nullptr, cx, cy, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

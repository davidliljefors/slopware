# slopware

A lightweight Win32/ImGui application shell. You fill in a callback struct (`App`), pass it to `host_run()`, and you get a DPI-aware window with D3D11, system tray, global hotkeys, and a per-frame arena allocator. The host handles all the boilerplate.

Three apps are included:

- **Goto File** -- Fuzzy file finder. Indexes NTFS drives via the USN journal so searches across millions of files are instant. Requires admin.
- **Goto Text** -- Full-text search over a directory. Compresses files with LZ4 and searches them in memory in parallel. 
- **Example App** -- Minimal starter (~60 lines) showing how to wire up an app.

There's also a Visual Studio plugin (`vs_plugin/`) that has similar functionality.

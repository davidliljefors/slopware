namespace GotoSlop;

using System.Runtime.InteropServices;

internal static class NativeBridge
{
    private const string DllName = "plugin_core";
    private static bool _resolverSet;

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate void SelectionCallback(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        int line,
        int column);

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern void plugin_set_solution_files(IntPtr[] paths, IntPtr[] projects, int count);

    public static void SetSolutionFiles(string[] paths, string[] projects)
    {
        var filePtrs = new IntPtr[paths.Length];
        var projPtrs = new IntPtr[paths.Length];
        try
        {
            for (int i = 0; i < paths.Length; i++)
            {
                filePtrs[i] = Marshal.StringToCoTaskMemUTF8(paths[i]);
                projPtrs[i] = projects[i] != null
                    ? Marshal.StringToCoTaskMemUTF8(projects[i])
                    : IntPtr.Zero;
            }
            plugin_set_solution_files(filePtrs, projPtrs, paths.Length);
        }
        finally
        {
            foreach (var p in filePtrs)
                if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p);
            foreach (var p in projPtrs)
                if (p != IntPtr.Zero) Marshal.FreeCoTaskMem(p);
        }
    }

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    public static extern void plugin_set_callback(SelectionCallback callback);

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    public static extern void plugin_show_goto_file();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    public static extern void plugin_show_goto_text();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    public static extern void plugin_set_preview_callback(SelectionCallback callback);

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    private static extern void plugin_show_goto_text_in_file(IntPtr filePath);

    public static void ShowGoToTextInFile(string path)
    {
        IntPtr ptr = Marshal.StringToCoTaskMemUTF8(path);
        try { plugin_show_goto_text_in_file(ptr); }
        finally { Marshal.FreeCoTaskMem(ptr); }
    }

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool plugin_is_window_open();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    public static extern void plugin_shutdown();

    [DllImport(DllName, CallingConvention = CallingConvention.StdCall)]
    public static extern void plugin_preload_content();

    public static void EnsureDllResolver()
    {
        if (_resolverSet) return;
        _resolverSet = true;

        NativeLibrary.SetDllImportResolver(
            typeof(NativeBridge).Assembly,
            (name, assembly, searchPath) =>
            {
                if (name == DllName)
                {
                    string dir = Path.GetDirectoryName(assembly.Location)!;
                    return NativeLibrary.Load(Path.Combine(dir, "plugin_core.dll"));
                }
                return IntPtr.Zero;
            });
    }
}

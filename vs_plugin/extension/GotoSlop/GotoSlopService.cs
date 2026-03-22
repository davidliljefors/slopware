namespace GotoSlop;

using System.Diagnostics;
using Microsoft.VisualStudio.Extensibility;
using Microsoft.VisualStudio.ProjectSystem.Query;
using Microsoft.VisualStudio.RpcContracts.OpenDocument;

/// <summary>
/// Manages native DLL state, caches the file list, and handles callbacks.
/// Files are queried once per solution and cached. A lightweight project-path
/// query detects when the user opens a different .sln.
/// </summary>
internal sealed class GotoSlopService
{
    private static GotoSlopService? _instance;
    private static readonly object _lock = new();

    private readonly VisualStudioExtensibility _extensibility;
    private readonly NativeBridge.SelectionCallback _callback;
    private readonly NativeBridge.SelectionCallback _previewCallback;
    private string _cachedSolutionFingerprint = string.Empty;
    private int _previewVersion;
    private CancellationTokenSource? _previewCts;
    private IDisposable? _solutionSubscription;

    private GotoSlopService(VisualStudioExtensibility extensibility)
    {
        _extensibility = extensibility;

        NativeBridge.EnsureDllResolver();

        _callback = OnSelectionCallback;
        NativeBridge.plugin_set_callback(_callback);

        _previewCallback = OnPreviewCallback;
        NativeBridge.plugin_set_preview_callback(_previewCallback);
    }

    public static GotoSlopService GetOrCreate(VisualStudioExtensibility extensibility)
    {
        if (_instance != null) return _instance;
        lock (_lock)
        {
            _instance ??= new GotoSlopService(extensibility);
        }
        return _instance;
    }

    /// <summary>
    /// Ensure the native side has the current solution's files.
    /// Does a cheap project-path query to detect solution changes;
    /// only re-queries all files when the solution actually changed.
    /// </summary>
    public async Task EnsureFilesAsync(CancellationToken ct)
    {
        var total = Stopwatch.StartNew();
        try
        {
            var workspace = _extensibility.Workspaces();

            var sw = Stopwatch.StartNew();
            var solutions = await workspace.QuerySolutionAsync(
                sln => sln.With(s => s.Guid).With(s => s.Path), ct);
            sw.Stop();
            Trace.WriteLine($"[GotoSlop] QuerySolutionAsync: {sw.ElapsedMilliseconds}ms");

            if (solutions.Any() == false) 
            {
                InvalidateFileCache();
            }

            ISolutionSnapshot solution = solutions.First();
            string solutionPath = solution.Path;

            if (solutionPath == _cachedSolutionFingerprint)
            {
                total.Stop();
                Trace.WriteLine($"[GotoSlop] QuerySolutionAsync: id match, skipped. Total: {total.ElapsedMilliseconds}ms");
                return;
            }

            _cachedSolutionFingerprint = solutionPath;

            // Solution changed - full file query with project names
            sw.Restart();
            var projectsWithFiles = await workspace.QueryProjectsAsync(
                project => project.With(p => p.Name)
                    .With(p => p.Files.With(f => f.Path)),
                ct);
            sw.Stop();
            Trace.WriteLine($"[GotoSlop] QueryProjectsAsync (files+projects): {sw.ElapsedMilliseconds}ms");

            sw.Restart();
            var paths = new List<string>();
            var projectNames = new List<string?>();
            foreach (var proj in projectsWithFiles)
            {
                string? projName = proj.Name;
                foreach (var file in proj.Files)
                {
                    if (!string.IsNullOrEmpty(file.Path))
                    {
                        paths.Add(file.Path);
                        projectNames.Add(projName);
                    }
                }
            }
            sw.Stop();
            Trace.WriteLine($"[GotoSlop] Enumerate file paths ({paths.Count} files): {sw.ElapsedMilliseconds}ms");

            sw.Restart();
            NativeBridge.SetSolutionFiles(paths.ToArray(), projectNames.ToArray()!);
            sw.Stop();
            Trace.WriteLine($"[GotoSlop] SetSolutionFiles (native): {sw.ElapsedMilliseconds}ms");

            // Kick off background content loading immediately
            NativeBridge.plugin_preload_content();

            // Subscribe to project system changes (files added/removed, projects loaded/unloaded)
            SubscribeToProjectChanges(workspace);

            total.Stop();
            Trace.WriteLine($"[GotoSlop] EnsureFilesAsync total: {total.ElapsedMilliseconds}ms ({paths.Count} files)");
        }
        catch (Exception ex)
        {
            total.Stop();
            Trace.WriteLine($"[GotoSlop] EnsureFilesAsync failed after {total.ElapsedMilliseconds}ms: {ex.Message}");
        }
    }

    private void OnSelectionCallback(string path, int line, int column)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                var sw = Stopwatch.StartNew();
                var documents = _extensibility.Documents();
                var uri = new Uri(path, UriKind.Absolute);

                if (line > 0)
                {
                    int l = line - 1;
                    var range = new Microsoft.VisualStudio.RpcContracts.Utilities.Range(l, column, l, column);
                    var options = new OpenDocumentOptions(selection: range);
                    await documents.OpenDocumentAsync(uri, options, CancellationToken.None);
                }
                else
                {
                    await documents.OpenDocumentAsync(uri, CancellationToken.None);
                }
                sw.Stop();
                Trace.WriteLine($"[GotoSlop] OnSelectionCallback OpenDocumentAsync: {sw.ElapsedMilliseconds}ms ({path})");
            }
            catch (Exception ex)
            {
                Trace.WriteLine($"[GotoSlop] OnSelectionCallback failed: {ex.Message}");
            }
        });
    }

    private void OnPreviewCallback(string path, int line, int column)
    {
        var version = Interlocked.Increment(ref _previewVersion);

        var newCts = new CancellationTokenSource();
        var oldCts = Interlocked.Exchange(ref _previewCts, newCts);
        oldCts?.Cancel();
        oldCts?.Dispose();
        var ct = newCts.Token;

        _ = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(50, ct);
                if (version != Volatile.Read(ref _previewVersion)) return;

                var sw = Stopwatch.StartNew();
                var documents = _extensibility.Documents();
                var uri = new Uri(path, UriKind.Absolute);

                int l = line > 0 ? line - 1 : 0;
                var range = new Microsoft.VisualStudio.RpcContracts.Utilities.Range(l, column, l, column);
                var options = new OpenDocumentOptions(selection: range, isPreview: true, activate: false);
                await documents.OpenDocumentAsync(uri, options, ct);
                sw.Stop();
                Trace.WriteLine($"[GotoSlop] OnPreviewCallback OpenDocumentAsync: {sw.ElapsedMilliseconds}ms ({path}:{line})");
            }
            catch (OperationCanceledException) { }
            catch (Exception ex)
            {
                Trace.WriteLine($"[GotoSlop] OnPreviewCallback failed: {ex.Message}");
            }
        });
    }

    private void SubscribeToProjectChanges(WorkspacesExtensibility workspace)
    {
        _solutionSubscription?.Dispose();
        _solutionSubscription = null;

        try
        {
            var observer = new SolutionChangeObserver(this);
            // SubscribeAsync returns a Task<IDisposable> — fire and forget the subscription setup
            _ = Task.Run(async () =>
            {
                try
                {
                    var solutions = await workspace.QuerySolutionAsync(
                        solution => solution.With(s => s.FileName),
                        CancellationToken.None);

                    var singleSolution = solutions.FirstOrDefault();
                    if (singleSolution == null) return;

                    var unsub = await singleSolution
                        .AsQueryable()
                        .With(s => s.Projects.With(p => p.Files.With(f => f.Path)))
                        .SubscribeAsync(observer, CancellationToken.None);

                    _solutionSubscription = unsub;
                    Trace.WriteLine("[GotoSlop] Subscribed to project system changes");
                }
                catch (Exception ex)
                {
                    Trace.WriteLine($"[GotoSlop] Failed to subscribe to project changes: {ex.Message}");
                }
            });
        }
        catch (Exception ex)
        {
            Trace.WriteLine($"[GotoSlop] SubscribeToProjectChanges failed: {ex.Message}");
        }
    }

    private void InvalidateFileCache()
    {
        _cachedSolutionFingerprint = string.Empty;
        Trace.WriteLine("[GotoSlop] File cache invalidated due to project system change");
    }

    private sealed class SolutionChangeObserver : IObserver<IQueryResults<ISolutionSnapshot>>
    {
        private readonly GotoSlopService _service;
        private bool _firstNotification = true;

        public SolutionChangeObserver(GotoSlopService service) => _service = service;

        public void OnNext(IQueryResults<ISolutionSnapshot> value)
        {
            // Skip the initial snapshot — we already have the data from the query
            Trace.WriteLine("[GotoSlop] Received project system notification");
            if (_firstNotification)
            {
                 Trace.WriteLine("[GotoSlop] Skipped first notification from project system subscription");
                _firstNotification = false;
                return;
            }
            _service.InvalidateFileCache();
        }

        public void OnError(Exception error)
        {
            Trace.WriteLine($"[GotoSlop] Solution subscription error: {error.Message}");
        }

        public void OnCompleted()
        {
            Trace.WriteLine("[GotoSlop] Solution subscription completed");
        }
    }

}

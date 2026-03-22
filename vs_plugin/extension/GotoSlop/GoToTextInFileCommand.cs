namespace GotoSlop;

using Microsoft.VisualStudio.Extensibility;
using Microsoft.VisualStudio.Extensibility.Commands;
using Microsoft.VisualStudio.Extensibility.Editor;

[VisualStudioContribution]
internal class GoToTextInFileCommand : Command
{
    private readonly GotoSlopService _service;

    public override CommandConfiguration CommandConfiguration => new("%GotoSlop.GoToTextInFileCommand.DisplayName%")
    {
        Placements = [CommandPlacement.KnownPlacements.ToolsMenu],
        Shortcuts = [new CommandShortcutConfiguration(ModifierKey.ControlShift, Key.D)],
    };

    public GoToTextInFileCommand(VisualStudioExtensibility extensibility)
        : base(extensibility)
    {
        _service = GotoSlopService.GetOrCreate(extensibility);
    }

    public override async Task ExecuteCommandAsync(IClientContext context, CancellationToken ct)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var textView = await Extensibility.Editor().GetActiveTextViewAsync(context, ct);
        sw.Stop();
        System.Diagnostics.Trace.WriteLine($"[GotoSlop] GetActiveTextViewAsync: {sw.ElapsedMilliseconds}ms");
        if (textView == null) return;

        var path = textView.Document.Uri.LocalPath;
        System.Diagnostics.Trace.WriteLine($"[GotoSlop] GoToTextInFileCommand: opening for {path}");
        NativeBridge.ShowGoToTextInFile(path);
    }
}

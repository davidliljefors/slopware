namespace GotoSlop;

using Microsoft.VisualStudio.Extensibility;
using Microsoft.VisualStudio.Extensibility.Commands;

[VisualStudioContribution]
internal class GoToFileCommand : Command
{
    private readonly GotoSlopService _service;

    public override CommandConfiguration CommandConfiguration => new("%GotoSlop.GoToFileCommand.DisplayName%")
    {
        Placements = [CommandPlacement.KnownPlacements.ToolsMenu],
        Shortcuts = [new CommandShortcutConfiguration(ModifierKey.ControlShift, Key.G)],
    };

    public GoToFileCommand(VisualStudioExtensibility extensibility)
        : base(extensibility)
    {
        _service = GotoSlopService.GetOrCreate(extensibility);
    }

    public override Task ExecuteCommandAsync(IClientContext context, CancellationToken cancellationToken)
    {
        System.Diagnostics.Trace.WriteLine("[GotoSlop] GoToFileCommand.ExecuteCommandAsync entered");
        NativeBridge.plugin_show_goto_file();
        _ = _service.EnsureFilesAsync(cancellationToken);
        return Task.CompletedTask;
    }
}

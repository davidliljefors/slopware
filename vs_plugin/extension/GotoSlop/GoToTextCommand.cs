namespace GotoSlop;

using Microsoft.VisualStudio.Extensibility;
using Microsoft.VisualStudio.Extensibility.Commands;

[VisualStudioContribution]
internal class GoToTextCommand : Command
{
    private readonly GotoSlopService _service;

    public override CommandConfiguration CommandConfiguration => new("%GotoSlop.GoToTextCommand.DisplayName%")
    {
        Placements = [CommandPlacement.KnownPlacements.ToolsMenu],
        Shortcuts = [new CommandShortcutConfiguration(ModifierKey.ControlShift, Key.T)],
    };

    public GoToTextCommand(VisualStudioExtensibility extensibility)
        : base(extensibility)
    {
        _service = GotoSlopService.GetOrCreate(extensibility);
    }

    public override Task ExecuteCommandAsync(IClientContext context, CancellationToken cancellationToken)
    {
        System.Diagnostics.Trace.WriteLine("[GotoSlop] GoToTextCommand.ExecuteCommandAsync entered");
        NativeBridge.plugin_show_goto_text();
        _ = _service.EnsureFilesAsync(cancellationToken);
        return Task.CompletedTask;
    }
}

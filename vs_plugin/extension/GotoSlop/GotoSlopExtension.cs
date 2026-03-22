namespace GotoSlop;

using System.Diagnostics;
using Microsoft.VisualStudio.Extensibility;

[VisualStudioContribution]
internal class GotoSlopExtension : Extension
{
    public override ExtensionConfiguration ExtensionConfiguration => new()
    {
        Metadata = new(
            id: "GotoSlop.B8A1C2D3-E4F5-6789-0ABC-DEF012345678",
            version: this.ExtensionAssemblyVersion,
            publisherName: "GotoSlop",
            displayName: "GotoSlop - Fast Go To File & Text",
            description: "Blazing fast Go-To-File and Go-To-Text"),
    };

    protected override Task OnInitializedAsync(VisualStudioExtensibility extensibility, CancellationToken ct)
    {
        _ = BackgroundWatchAsync(extensibility, ct);
        return Task.CompletedTask;
    }

    private static async Task BackgroundWatchAsync(VisualStudioExtensibility extensibility, CancellationToken ct)
    {
        var service = GotoSlopService.GetOrCreate(extensibility);
        while (!ct.IsCancellationRequested)
        {
            try
            {
                await Task.Delay(5000, ct);
                await service.EnsureFilesAsync(ct);
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                Trace.WriteLine($"[GotoSlop] BackgroundWatch: {ex.Message}");
            }
        }
    }
}

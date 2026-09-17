using UnrealBuildTool;
using System.IO;

public class UnrealWebUI : ModuleRules
{
    public UnrealWebUI(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        if (Target.Version.MajorVersion != 5 || Target.Version.MinorVersion < 7)
            throw new BuildException("UnrealWebUI requires Unreal Engine 5.7 or newer (UE5).");
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG", "Json" });
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "WebBrowser", "InputCore", "ApplicationCore", "RHI", "RenderCore", "HTTP", "Projects" });
        PrivateDependencyModuleNames.Add("D3D12RHI");
        AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
        PublicSystemLibraries.AddRange(new[] { "d3d12.lib", "dxgi.lib" });
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "regions.js"), StagedFileType.NonUFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "regions-delta.js"), StagedFileType.NonUFS);
        // The standalone executable reads bootstrap scripts before any Unreal resource requests.
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "selection.js"), StagedFileType.NonUFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Resources", "webui.js"), StagedFileType.NonUFS);
        RuntimeDependencies.Add(Path.Combine(PluginDirectory, "Binaries", "Win64", "WebUIHost.exe"));
    }
}

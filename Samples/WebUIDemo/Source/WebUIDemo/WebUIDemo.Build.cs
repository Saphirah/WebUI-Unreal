using UnrealBuildTool;
public class WebUIDemo : ModuleRules
{
    public WebUIDemo(ReadOnlyTargetRules Target) : base(Target)
    { PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs; PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UMG", "UnrealWebUI", "Slate", "SlateCore", "InputCore" }); }
}

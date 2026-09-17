using UnrealBuildTool;
public class WebUIDemoTarget : TargetRules
{
    public WebUIDemoTarget(TargetInfo Target) : base(Target)
    { Type = TargetType.Game; DefaultBuildSettings = BuildSettingsVersion.V6; IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7; WindowsPlatform.CompilerVersion = "Latest"; ExtraModuleNames.Add("WebUIDemo"); }
}

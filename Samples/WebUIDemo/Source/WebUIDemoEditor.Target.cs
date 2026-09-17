using UnrealBuildTool;
public class WebUIDemoEditorTarget : TargetRules
{
    public WebUIDemoEditorTarget(TargetInfo Target) : base(Target)
    { Type = TargetType.Editor; DefaultBuildSettings = BuildSettingsVersion.V6; IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7; WindowsPlatform.CompilerVersion = "Latest"; ExtraModuleNames.Add("WebUIDemo"); }
}

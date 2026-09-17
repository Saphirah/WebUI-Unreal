param(
    [string]$Engine = 'E:\Unreal\UE_5.7',
    [ValidateSet('Editor','Game','Shipping','PackagePlugin')][string]$Target = 'Editor',
    [string]$DeployProject = '',
    [switch]$SkipDeploy
)
$ErrorActionPreference = 'Stop'
$Workspace = Split-Path $PSScriptRoot -Parent
& powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot/BuildHost.ps1" -Engine $Engine
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$Project = Join-Path $Workspace 'Samples/WebUIDemo/WebUIDemo.uproject'
if ($Target -eq 'PackagePlugin') {
    & "$Engine/Engine/Build/BatchFiles/RunUAT.bat" BuildPlugin "-Plugin=$Workspace/Plugins/UnrealWebUI/UnrealWebUI.uplugin" "-Package=$Workspace/Artifacts/UnrealWebUI" -TargetPlatforms=Win64 -Rocket
} else {
    $BuildTarget = if ($Target -eq 'Editor') { 'WebUIDemoEditor' } else { 'WebUIDemo' }
    $Config = if ($Target -eq 'Shipping') { 'Shipping' } else { 'Development' }
    & "$Engine/Engine/Build/BatchFiles/Build.bat" $BuildTarget Win64 $Config "-Project=$Project" -WaitMutex -NoHotReloadFromIDE
}
$BuildExitCode = $LASTEXITCODE
if ($BuildExitCode -eq 0 -and $Target -eq 'Editor') {
    # External plugins build into the demo's Binaries folder. Publish the editor
    # module to the actual linked plugin and avoid loading two DLL copies.
    $DemoBin = Join-Path $Workspace 'Samples/WebUIDemo/Binaries/Win64'
    $PluginBin = Join-Path $Workspace 'Plugins/UnrealWebUI/Binaries/Win64'
    $DemoDll = Join-Path $DemoBin 'UnrealEditor-UnrealWebUI.dll'
    if (Test-Path -LiteralPath $DemoDll) {
        New-Item -ItemType Directory -Force -Path $PluginBin | Out-Null
        foreach ($Extension in @('dll', 'pdb')) {
            $ModuleFile = "UnrealEditor-UnrealWebUI.$Extension"
            Copy-Item -LiteralPath (Join-Path $DemoBin $ModuleFile) -Destination (Join-Path $PluginBin $ModuleFile) -Force
        }
        $ManifestPath = Join-Path $DemoBin 'UnrealEditor.modules'
        $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
        @{ BuildId = $Manifest.BuildId; Modules = @{ UnrealWebUI = 'UnrealEditor-UnrealWebUI.dll' } } |
            ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PluginBin 'UnrealEditor.modules')
        $Manifest.Modules.PSObject.Properties.Remove('UnrealWebUI')
        $Manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ManifestPath
        Remove-Item -LiteralPath $DemoDll
    }
}
if ($BuildExitCode -eq 0 -and $Target -eq 'Editor' -and !$SkipDeploy -and $DeployProject) {
    $DeployScript = Join-Path $DeployProject 'Scripts/DeployUnrealWebUI.ps1'
    if (!(Test-Path -LiteralPath $DeployScript)) { throw "Deployment script missing: $DeployScript. Use -SkipDeploy for build-only." }
    & $DeployScript -SourceRepository $Workspace -Engine $Engine
}
exit $BuildExitCode


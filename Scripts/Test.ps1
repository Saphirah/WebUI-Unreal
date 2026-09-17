param([string]$Engine = 'E:\Unreal\UE_5.7')
$ErrorActionPreference = 'Stop'
$Workspace = Split-Path $PSScriptRoot -Parent
Push-Location $Workspace
try {
    & node --test Tests/bridge.test.cjs Tests/regions-delta.test.cjs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$Engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "$Workspace/Samples/WebUIDemo/WebUIDemo.uproject" -unattended -nop4 -nosplash -NullRHI '-ExecCmds=Automation RunTests UnrealWebUI' '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$Workspace/Artifacts/TestReport" -log
    exit $LASTEXITCODE
} finally { Pop-Location }

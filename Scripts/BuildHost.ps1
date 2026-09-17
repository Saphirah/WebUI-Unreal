param([string]$Engine='E:/Unreal/UE_5.7')
$ErrorActionPreference='Stop'
$Workspace=Split-Path $PSScriptRoot -Parent
& powershell -NoProfile -ExecutionPolicy Bypass -File "$Workspace/Plugins/UnrealWebUI/Scripts/BuildHost.ps1" -Engine $Engine
exit $LASTEXITCODE

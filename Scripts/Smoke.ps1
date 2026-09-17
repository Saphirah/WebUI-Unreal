param([string]$Engine = 'E:\Unreal\UE_5.7', [switch]$React, [switch]$ClickThrough, [switch]$DOM, [switch]$Selection)
$ErrorActionPreference = 'Stop'
$Workspace = Split-Path $PSScriptRoot -Parent
$Project = Join-Path $Workspace 'Samples/WebUIDemo/WebUIDemo.uproject'
$Arguments = @(('"' + $Project + '"'), '/Engine/Maps/Entry', '-game', '-d3d12', '-RenderOffscreen', '-ResX=1280', '-ResY=720', '-unattended', '-nosplash', '-nosound', '-WebUISmoke')
if ($React) { $Arguments += '-WebUIReact' }
if ($Selection) { $Arguments += '-WebUISelectionSmoke' }
if ($DOM) { $Arguments += '-WebUIDOMSmoke'; $ClickThrough = $true }
if ($ClickThrough) { $Arguments += '-WebUIClickSmoke' }
$Started = Get-Date
$Run = Start-Process -FilePath "$Engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $Arguments -WindowStyle Hidden -PassThru
if (!$Run.WaitForExit(120000)) {
    Stop-Process -Id $Run.Id
    throw 'Web UI smoke test timed out after 120 seconds.'
}
if ($Run.ExitCode -ne 0) { throw "Smoke process failed with exit code $($Run.ExitCode)." }
if ($Selection) {
    $File=Get-Item "$Workspace/Samples/WebUIDemo/Saved/WebUISelectionSmoke.txt"
    if ($File.LastWriteTime -lt $Started) { throw 'Selection report was not refreshed.' }
    $Report=Get-Content $File.FullName -Raw
    if (!($Report | ConvertFrom-Json).pass) { throw "Selection test failed: $Report" }
    Write-Output $Report
    exit 0
}
$ReportFile = Get-Item "$Workspace/Samples/WebUIDemo/Saved/WebUISmoke.txt"
if ($ReportFile.LastWriteTime -lt $Started) { throw 'Smoke report was not refreshed.' }
$Report = Get-Content $ReportFile -Raw
if ($Report -notmatch 'shared=1' -or $Report -match 'transparent=0' -or $Report -match 'opaque=0') {
    throw "GPU validation failed: $Report"
}
Write-Output $Report
if ($ClickThrough) {
    $ReportName = if ($DOM) { 'WebUIDOMSmoke.txt' } else { 'WebUIClickSmoke.txt' }
    $ClickReportFile = Get-Item "$Workspace/Samples/WebUIDemo/Saved/$ReportName"
    if ($ClickReportFile.LastWriteTime -lt $Started) { throw 'Click report was not refreshed.' }
    $ClickReport = Get-Content $ClickReportFile -Raw
    if ($ClickReport -notmatch 'pass=1') { throw "Click-through validation failed: $ClickReport" }
    Write-Output $ClickReport
}

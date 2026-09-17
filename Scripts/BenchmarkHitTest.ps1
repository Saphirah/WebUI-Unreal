param([int]$GameFPS=90, [int]$PumpMode=1, [string]$URL='', [string]$Name='HitTest', [string]$Engine='E:/Unreal/UE_5.7', [switch]$Independent)
$ErrorActionPreference='Stop'
$Workspace=Split-Path $PSScriptRoot -Parent
$ReportDir=Join-Path $Workspace 'Artifacts'
New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
$Log=Join-Path $ReportDir "$Name.log"
$Started=Get-Date
$Arguments=@("$Workspace/Samples/WebUIDemo/WebUIDemo.uproject",'/Engine/Maps/Entry','-game','-d3d12','-RenderOffscreen',
    '-ResX=1280','-ResY=720','-unattended','-nosplash','-nosound','-WebUIHitBenchmark','-WebUIFPS=120',
    ('-ExecCmds="t.MaxFPS '+$GameFPS+',webui.PumpEveryFrame '+$PumpMode+'"'), ('-abslog="'+$Log+'"'))
if ($URL) { $Arguments+=('-WebUIHitURL="'+$URL+'"') }
if (!$Independent) { $Arguments+='-WebUILegacy' }
$Run=Start-Process "$Engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $Arguments -WindowStyle Hidden -PassThru
if (!$Run.WaitForExit(90000)) { Stop-Process -Id $Run.Id; throw 'Benchmark timed out.' }
if ($Run.ExitCode -ne 0) { throw "Benchmark failed: $($Run.ExitCode)" }
foreach ($Extension in @('txt','csv')) {
    $ReportBase=if ($Independent) {'WebUIIndependentBenchmark'} else {'WebUIHitBenchmark'}
    $Source=Get-Item "$Workspace/Samples/WebUIDemo/Saved/$ReportBase.$Extension"
    if ($Source.LastWriteTime -lt $Started) { throw 'No fresh benchmark report.' }
    Copy-Item -LiteralPath $Source.FullName -Destination (Join-Path $ReportDir "$Name.$Extension") -Force
}
Get-Content (Join-Path $ReportDir "$Name.txt")

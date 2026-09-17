param([string]$URL='',[string]$Name='GPUCopy',[string]$Engine='E:/Unreal/UE_5.7')
$ErrorActionPreference='Stop'
$Workspace=Split-Path $PSScriptRoot -Parent
$ProfilePath=Join-Path $Workspace "Artifacts/$Name.csv"
$Previous=$env:WEBUI_GPU_PROFILE
$Started=Get-Date
try {
    $env:WEBUI_GPU_PROFILE=$ProfilePath
    & "$PSScriptRoot/BenchmarkHitTest.ps1" -Independent -Name "$Name-Hit" -URL $URL -Engine $Engine
} finally { $env:WEBUI_GPU_PROFILE=$Previous }
$File=Get-Item -LiteralPath $ProfilePath
if($File.LastWriteTime -lt $Started){throw 'No fresh GPU profile.'}
$Rows=@(Import-Csv -LiteralPath $ProfilePath)
if($Rows.Count -ne 120){throw "Expected 120 samples, got $($Rows.Count)."}
$Summary=@("url=$URL samples=$($Rows.Count) surface=$($Rows[0].width)x$($Rows[0].height) dropped=$($Rows[-1].dropped)")
foreach($Column in @('open_ms','copy_submit_ms','query_setup_ms','flush_ms','wait_ms','acquire_ms','gpu_copy_ms')){
    $Values=@($Rows | ForEach-Object {[double]($_.$Column)} | Where-Object {$_ -ge 0} | Sort-Object)
    if(!$Values.Count){$Summary+="$Column unavailable";continue}
    $Stats=@(.5,.95,.99,1) | ForEach-Object {$Values[[Math]::Max(0,[Math]::Ceiling($Values.Count*$_)-1)]}
    $Summary+=('{0} count={1} p50={2:F6} p95={3:F6} p99={4:F6} max={5:F6}' -f $Column,$Values.Count,$Stats[0],$Stats[1],$Stats[2],$Stats[3])
}
$Summary | Set-Content -LiteralPath (Join-Path $Workspace "Artifacts/$Name.txt")
$Summary

param([string]$URL='',[string]$Name='AB',[int]$Burst=1,[int]$Rounds=2,[ValidateSet('base','dom')][string[]]$Modes=@('base','dom'),[string]$Engine='E:/Unreal/UE_5.7')
$ErrorActionPreference='Stop'
$Workspace=Split-Path $PSScriptRoot -Parent
$Old=@{};foreach($Key in @('WEBUI_AB','WEBUI_INCREMENTAL_DOM')){$Old[$Key]=[Environment]::GetEnvironmentVariable($Key,'Process')}
$Results=@()
$Source=Join-Path $Workspace 'Plugins/UnrealWebUI/Resources/regions-delta.js'
$SourceHash=(Get-FileHash -LiteralPath $Source).Hash
Copy-Item -LiteralPath $Source -Destination "$Workspace/Artifacts/$Name-regions.js"
try {
  $env:WEBUI_AB='1'
  for($Round=0;$Round -lt $Rounds;$Round++){
    $Order=if($Round%2 -eq 0){@('base','dom')}else{@('dom','base')}
    foreach($Mode in $Order){
      if($Mode -notin $Modes){continue}
      $env:WEBUI_INCREMENTAL_DOM=if($Mode -eq 'dom'){'1'}else{'0'}
      $RunName="$Name-$Round-$Mode";$Started=Get-Date
      $Arguments=@((Join-Path $Workspace 'Samples/WebUIDemo/WebUIDemo.uproject'),'/Engine/Maps/Entry','-game','-d3d12','-RenderOffscreen','-ResX=1280','-ResY=720','-unattended','-nosplash','-nosound','-WebUIAB','-WebUIFPS=120',"-WebUIABBurst=$Burst",'-ExecCmds="t.MaxFPS 90"',('-abslog="'+(Join-Path $Workspace "Artifacts/$RunName.log")+'"'))
      if($URL){$Arguments+=('-WebUIHitURL="'+$URL+'"')}
      $Process=Start-Process "$Engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" -ArgumentList $Arguments -WindowStyle Hidden -PassThru
      if(!$Process.WaitForExit(60000)){Stop-Process -Id $Process.Id;throw "$RunName timed out"}
      if($Process.ExitCode -ne 0){throw "$RunName failed: $($Process.ExitCode)"}
      $Report=Get-Item "$Workspace/Samples/WebUIDemo/Saved/WebUIAB.json"
      if($Report.LastWriteTime -lt $Started){throw 'No fresh report'}
      $Data=Get-Content $Report.FullName -Raw | ConvertFrom-Json
      if($Data.received -le 0){throw "$RunName produced no mouse replies; invalid latency trial"}
      if($Data.paintHz -le 0 -or $Data.page.elements -lt 5){throw "$RunName loaded an empty page; invalid trial"}
      if($Data.nativeP50 -le 0){throw "$RunName lacks a native receipt timestamp"}
      if((Get-FileHash -LiteralPath $Source).Hash -ne $SourceHash){throw 'Collector changed during the experiment; discard this run'}
      $Data | Add-Member -NotePropertyName sourceHash -NotePropertyValue $SourceHash
      $Data | Add-Member -NotePropertyName mode -NotePropertyValue $Mode
      $Data | Add-Member -NotePropertyName round -NotePropertyValue $Round
      $Data | ConvertTo-Json -Depth 8 | Set-Content "$Workspace/Artifacts/$RunName.json"
      Copy-Item "$Workspace/Samples/WebUIDemo/Saved/WebUIAB.csv" "$Workspace/Artifacts/$RunName.csv"
      $Results+=$Data
      Write-Output ("{0} nativeP50={1} p95={2} paintHz={3} domMs/s={4} received={5}/{6}" -f $RunName,$Data.nativeP50,$Data.nativeP95,$Data.paintHz,$Data.page.domMsPerSecond,$Data.received,$Data.sent)
    }
  }
} finally {
  foreach($Key in $Old.Keys){[Environment]::SetEnvironmentVariable($Key,$Old[$Key],'Process')}
  $Results | ConvertTo-Json -Depth 8 | Set-Content "$Workspace/Artifacts/$Name-summary.json"
}

param([Parameter(Mandatory=$true)][string]$Engine, [string]$TargetType = 'Editor')
$ErrorActionPreference = 'Stop'
if ($TargetType -eq 'Server') { exit 0 }
$PluginRoot = Split-Path $PSScriptRoot -Parent
$CEF = Join-Path $Engine 'Engine/Source/ThirdParty/CEF3/cef_binary_128.4.13+ge76af7e+chromium-128.0.6613.138_windows64'
if (!(Test-Path -LiteralPath $CEF)) {
    $CEF = Join-Path $Engine 'Engine/Source/ThirdParty/CEF3/cef_binary_128.4.13+ge76af7e+chromium-128.0.6613.138+v2_windows64'
}
if (!(Test-Path -LiteralPath "$CEF/Release/libcef.lib")) { throw 'Requires the matching UE 5.7 CEF 128 development libraries; check -Engine.' }
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $VsWhere)) { throw 'Install Visual Studio 2022 or Build Tools with Desktop development with C++ and a Windows SDK.' }
$VS = & $VsWhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$VS) { throw 'No Visual Studio 2022 C++ toolchain found.' }
$DevCmd = Join-Path $VS 'Common7/Tools/VsDevCmd.bat'
# Import the supported VS developer environment, including its installed SDK.
$EnvironmentLines = & cmd.exe /d /s /c "`"$DevCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio developer environment failed.' }
foreach ($Line in $EnvironmentLines) {
    if ($Line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$Out = Join-Path $PluginRoot 'Binaries/Win64'
$Intermediate = Join-Path $PluginRoot 'Intermediate/Host'
New-Item -ItemType Directory -Force -Path $Out,$Intermediate | Out-Null
$Arguments = @('/nologo','/std:c++20','/EHsc','/MD','/O2','/DUNICODE','/D_UNICODE',"/I$CEF",
    "/Fo$Intermediate/WebUIHost.obj","/Fe$Out/WebUIHost.exe",
    "$PluginRoot/Source/WebUIHost/WebUIHost.cpp",
    "$CEF/Release/libcef.lib","$CEF/VS2015/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib",
    'd3d11.lib','dxgi.lib','user32.lib','shell32.lib','shlwapi.lib','advapi32.lib','ole32.lib','delayimp.lib',
    '/link','/SUBSYSTEM:WINDOWS','/DELAYLOAD:libcef.dll')
& cl.exe @Arguments
exit $LASTEXITCODE

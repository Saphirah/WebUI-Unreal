$ErrorActionPreference = 'Stop'
$Workspace = Split-Path $PSScriptRoot -Parent
$ArtifactDir = Join-Path $Workspace 'Artifacts'
New-Item -ItemType Directory -Path $ArtifactDir -Force | Out-Null
$ArchivePath = Join-Path $ArtifactDir 'WebUI-Unreal-Source.zip'
Add-Type -AssemblyName System.IO.Compression
$Stream = [System.IO.File]::Open($ArchivePath, [System.IO.FileMode]::Create)
$Archive = [System.IO.Compression.ZipArchive]::new($Stream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $Files = @((Join-Path $Workspace 'README.md'))
    foreach ($Directory in @('Plugins/UnrealWebUI/Source', 'Plugins/UnrealWebUI/Resources', 'Plugins/UnrealWebUI/Scripts', 'Scripts', 'Tests', 'Samples/WebUIDemo/Source', 'Samples/WebUIDemo/Config', 'Samples/WebUIDemo/Content/WebUI', 'Samples/WebUIDemo/Content/WebUIReact', 'Samples/ReactUI/src')) {
        $Files += Get-ChildItem -LiteralPath (Join-Path $Workspace $Directory) -Recurse -File | Select-Object -ExpandProperty FullName
    }
    foreach ($File in @('Plugins/UnrealWebUI/UnrealWebUI.uplugin', 'Samples/WebUIDemo/WebUIDemo.uproject', 'Samples/ReactUI/package.json', 'Samples/ReactUI/package-lock.json', 'Samples/ReactUI/index.html', 'Samples/ReactUI/vite.config.js')) {
        $Files += Join-Path $Workspace $File
    }
    foreach ($File in $Files) {
        $Relative = [System.IO.Path]::GetRelativePath($Workspace, $File).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Archive, $File, "WebUI-Unreal/$Relative", [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $Archive.Dispose(); $Stream.Dispose() }
Write-Output $ArchivePath

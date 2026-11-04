# Installs the FF7 Rebirth Mesh-Shaderless patch: the same as dragging the release zip's End folder into the
# game folder, plus finding the game and keeping another mod's d3d12.dll. Runs from the unzipped package
# folder that build.ps1 writes (dist\<name>, next to its End folder).
#
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Game <...\End\Binaries\Win64>]
#
#   -Game     the game's End\Binaries\Win64 folder (found automatically for Steam and Epic installs)
param([string]$Game)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path $here 'find-game.ps1')

Write-Host 'FF7 Rebirth Mesh-Shaderless patch installer' -ForegroundColor Cyan
$src = Join-Path $here 'End\Binaries\Win64'
if (-not (Test-Path (Join-Path $src 'd3d12.dll'))) { throw "No End\Binaries\Win64\d3d12.dll next to $PSCommandPath." }

$Game = Resolve-Game $Game
Write-Host "Game: $Game"
Assert-GameClosed

# Keep a d3d12.dll that is not ours (another mod) instead of overwriting it.
$dll = Join-Path $Game 'd3d12.dll'
$meshemu = Join-Path $Game 'meshemu'
$mark = Join-Path $meshemu 'installed-dll.sha256'
if (Test-Path $dll) {
    $cur = (Get-FileHash $dll -Algorithm SHA256).Hash
    $ours = (Test-Path $mark) -and ((Get-Content $mark -Raw).Trim() -eq $cur)
    $backup = "$dll.meshemu-backup"
    if (-not $ours -and -not (Test-Path $backup)) {
        Move-Item $dll $backup
        Write-Warning "An existing d3d12.dll (another mod?) was moved to $backup"
    }
}

# d3d12.dll, meshemu\installed-dll.sha256, meshemu\tools (meshemu-conv.exe, dxcompiler.dll) and the docs.
Copy-Item (Join-Path $src '*') $Game -Recurse -Force
# Converter files of v0.2.0 and earlier (Python converter, downloaded DXC); no longer used.
foreach ($old in 'convert.py', 'dxasm.exe', 'dxc.exe', 'dxil.dll') {
    Remove-Item (Join-Path $meshemu "tools\$old") -ErrorAction SilentlyContinue
}
Write-Host "Patch -> $dll and $meshemu"

Write-Host ''
Write-Host 'Installed. On the first launch, and after game updates, the game''s mesh shaders are converted in the' -ForegroundColor Green
Write-Host 'background (seconds to a minute, depending on the CPU); geometry missing meanwhile appears without a restart.' -ForegroundColor Green
Write-Host "Logs: $meshemu\meshemu.log and convert.log. Uninstall: uninstall.ps1"

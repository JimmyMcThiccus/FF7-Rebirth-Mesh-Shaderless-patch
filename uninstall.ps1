# Removes the FF7 Rebirth Mesh-Shaderless patch.
#
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-Game <...\End\Binaries\Win64>] [-KeepCache]
#
#   -KeepCache   keep <game>\meshemu (converted shader cache, logs) for a later reinstall
param([string]$Game, [switch]$KeepCache)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

. (Join-Path $here 'find-game.ps1')
$Game = Resolve-Game $Game
Write-Host "Game: $Game"
Assert-GameClosed

$meshemu = Join-Path $Game 'meshemu'
$dll = Join-Path $Game 'd3d12.dll'
$mark = Join-Path $meshemu 'installed-dll.sha256'
if (Test-Path $dll) {
    $ours = (Test-Path $mark) -and ((Get-Content $mark -Raw).Trim() -eq (Get-FileHash $dll -Algorithm SHA256).Hash)
    if ($ours) {
        Remove-Item $dll
        Write-Host "Removed $dll"
    } else {
        Write-Warning "$dll is not the one this patch installed; left in place."
    }
}
$backup = "$dll.meshemu-backup"
if ((Test-Path $backup) -and -not (Test-Path $dll)) {
    Move-Item $backup $dll
    Write-Host "Restored the previous d3d12.dll"
}
if (-not $KeepCache -and (Test-Path $meshemu)) {
    Remove-Item -Recurse -Force $meshemu
    Write-Host "Removed $meshemu"
}
Write-Host 'Uninstalled.' -ForegroundColor Green

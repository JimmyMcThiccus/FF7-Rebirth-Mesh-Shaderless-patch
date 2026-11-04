# Installs the FF7 Rebirth Mesh-Shaderless patch next to ff7rebirth_.exe.
#
#   Right-click > Run with PowerShell, or:
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Game <...\End\Binaries\Win64>] [-DxcDir <folder>]
#
#   -Game     the game's End\Binaries\Win64 folder (found automatically for Steam and Epic installs)
#   -DxcDir   folder that already has dxc.exe, dxcompiler.dll and dxil.dll (skips the download)
param([string]$Game, [string]$DxcDir)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
. (Join-Path $here 'get-dxc.ps1')
. (Join-Path $here 'find-game.ps1')

Write-Host 'FF7 Rebirth Mesh-Shaderless patch installer' -ForegroundColor Cyan

$Game = Resolve-Game $Game
Write-Host "Game: $Game"
Assert-GameClosed

# The proxy runs tools\meshemu-conv.exe (self-contained; it loads DXC's dxcompiler.dll and dxil.dll next to it).
$meshemu = Join-Path $Game 'meshemu'
$tools = Join-Path $meshemu 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
Copy-Item (Join-Path $here 'tools\meshemu-conv.exe') $tools -Force
# The Python converter of v0.2.0 and earlier; the proxy prefers the exe, so these would only be confusing.
foreach ($old in 'convert.py', 'dxasm.exe') { Remove-Item (Join-Path $tools $old) -ErrorAction SilentlyContinue }
Install-Dxc $tools $DxcDir
Write-Host "Converter tools -> $tools"

# Keep a d3d12.dll that is not ours (another mod) instead of overwriting it.
$dll = Join-Path $Game 'd3d12.dll'
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
Copy-Item (Join-Path $here 'd3d12.dll') $dll -Force
(Get-FileHash $dll -Algorithm SHA256).Hash | Set-Content $mark
Write-Host "Patch DLL -> $dll"

Write-Host ''
Write-Host 'Installed. On the first launch, and after game updates, the game''s mesh shaders are converted in the' -ForegroundColor Green
Write-Host 'background (seconds to a minute, depending on the CPU); geometry missing meanwhile appears without a restart.' -ForegroundColor Green
Write-Host "Logs: $meshemu\meshemu.log and convert.log. Uninstall: uninstall.ps1"

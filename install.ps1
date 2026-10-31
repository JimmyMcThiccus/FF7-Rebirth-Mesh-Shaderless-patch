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

function Get-Python {
    foreach ($cmd in 'python', 'py') {
        if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { continue }
        try {
            $v = & $cmd -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
            if ("$v" -match '^3\.(\d+)$' -and [int]$Matches[1] -ge 8) { return $cmd }
        } catch {}
    }
    return $null
}

Write-Host 'FF7 Rebirth Mesh-Shaderless patch installer' -ForegroundColor Cyan

$Game = Resolve-Game $Game
Write-Host "Game: $Game"
Assert-GameClosed

# The proxy runs the shader converter with "python" in the background.
$py = Get-Python
if ($py -ne 'python') {
    throw ('Python 3.8 or newer must be installed and on PATH as "python" (https://www.python.org/downloads/, ' +
           'tick "Add python.exe to PATH"). The Microsoft Store "python" alias without Python installed does not work.')
}

$meshemu = Join-Path $Game 'meshemu'
$tools = Join-Path $meshemu 'tools'
New-Item -ItemType Directory -Force $tools | Out-Null
Copy-Item (Join-Path $here 'tools\convert.py'), (Join-Path $here 'tools\dxasm.exe') $tools -Force
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
Write-Host 'Installed. On the first launch (and in new areas) some geometry is missing for a few seconds while' -ForegroundColor Green
Write-Host 'the mesh shaders are converted in the background; it appears without a restart.' -ForegroundColor Green
Write-Host "Logs: $meshemu\meshemu.log and convert.log. Uninstall: uninstall.ps1"

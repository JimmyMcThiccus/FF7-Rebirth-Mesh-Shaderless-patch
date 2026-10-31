# Builds the patch from source and packages the release zip.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-DxcDir <folder>]
#
#   -DxcDir   folder that already has dxc.exe, dxcompiler.dll and dxil.dll (else the pinned DXC release is
#             downloaded into third_party\dxc once)
#
# Needs Visual Studio 2022+ with "Desktop development with C++" (x64 tools, Windows SDK) and Python 3.8+.
# Output: proxy\d3d12.dll, converter\dxasm.exe (+ DXC copied next to convert.py for tests\run.sh), and
# dist\FF7R-Mesh-Shaderless-patch-<version>.zip.
param([string]$DxcDir)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. (Join-Path $root 'get-dxc.ps1')
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()

$dxc = Join-Path $root 'third_party\dxc'
if ($DxcDir -or -not (Test-Path (Join-Path $dxc 'dxc.exe'))) { Install-Dxc $dxc $DxcDir }
foreach ($f in $DxcFiles) { Copy-Item (Join-Path $dxc $f) (Join-Path $root 'converter') -Force }

$env:DXC_DIR = $dxc
foreach ($script in 'proxy\build.cmd', 'converter\build.cmd') {
    & cmd /c "`"$(Join-Path $root $script)`""
    if ($LASTEXITCODE) { throw "$script failed" }
}

$name = "FF7R-Mesh-Shaderless-patch-$version"
$dist = Join-Path $root 'dist'
$out = Join-Path $dist $name
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $out 'tools') | Out-Null
Copy-Item (Join-Path $root 'proxy\d3d12.dll') $out
foreach ($f in 'install.ps1', 'uninstall.ps1', 'get-dxc.ps1', 'find-game.ps1', 'README.md', 'LICENSE',
               'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md') {
    Copy-Item (Join-Path $root $f) $out
}
Copy-Item (Join-Path $root 'converter\convert.py'), (Join-Path $root 'converter\dxasm.exe') (Join-Path $out 'tools')
$zip = Join-Path $dist "$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $out -DestinationPath $zip
Write-Host "release package: $zip" -ForegroundColor Green

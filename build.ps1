# Builds the patch from source and packages the release zip.
#
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-DxcDir <folder>]
#
#   -DxcDir   folder that already has dxc.exe, dxcompiler.dll and dxil.dll (else the pinned DXC release is
#             downloaded into third_party\dxc once)
#
# Needs Visual Studio 2022+ with "Desktop development with C++" (x64 tools, Windows SDK), Rust (cargo, the
# x86_64-pc-windows-msvc target) and Python 3.8+ (build and test scripts only; players do not need it).
# Output: proxy\d3d12.dll, conv\target\release\meshemu-conv.exe, converter\dxasm.exe (+ DXC copied next to
# convert.py for tests\run.sh), and dist\FF7R-Mesh-Shaderless-patch-<version>.zip: a drag-and-drop package
# (End\Binaries\Win64\...) with the official DXC release's dxcompiler.dll bundled. The unzipped package
# folder also has install.ps1 / uninstall.ps1 for a scripted install.
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
& cargo build --release --manifest-path (Join-Path $root 'conv\Cargo.toml')
if ($LASTEXITCODE) { throw 'cargo build (conv) failed' }

# The release bundles dxcompiler.dll and the license files of the official DXC release, whatever -DxcDir
# the build itself used.
$dxcRel = Join-Path $root 'third_party\dxc-release'
$dxcRelFiles = 'dxcompiler.dll', 'LICENSE-LLVM.txt', 'LICENSE-MIT.txt', 'LICENSE-MS.txt'
if (@($dxcRelFiles | Where-Object { -not (Test-Path (Join-Path $dxcRel $_)) }).Count) { Get-DxcRelease $dxcRel $dxcRelFiles }

# Package layout = the game folder: the zip's End folder is dragged into the folder that has End and Engine.
$name = "FF7R-Mesh-Shaderless-patch-$version"
$dist = Join-Path $root 'dist'
$out = Join-Path $dist $name
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
$win64 = Join-Path $out 'End\Binaries\Win64'
$meshemu = Join-Path $win64 'meshemu'
$tools = Join-Path $meshemu 'tools'
New-Item -ItemType Directory -Force (Join-Path $tools 'dxc-licenses') | Out-Null
Copy-Item (Join-Path $root 'proxy\d3d12.dll') $win64
# The installer's mark for "this d3d12.dll is ours" (install.ps1 / uninstall.ps1), so they work after a
# drag-and-drop install too.
(Get-FileHash (Join-Path $win64 'd3d12.dll') -Algorithm SHA256).Hash | Set-Content (Join-Path $meshemu 'installed-dll.sha256')
Copy-Item (Join-Path $root 'conv\target\release\meshemu-conv.exe'), (Join-Path $dxcRel 'dxcompiler.dll') $tools
Copy-Item (Join-Path $dxcRel 'LICENSE-*.txt') (Join-Path $tools 'dxc-licenses')
Copy-Item (Join-Path $root 'conv\THIRD_PARTY_LICENSES.txt') (Join-Path $tools 'meshemu-conv-licenses.txt')
foreach ($f in 'README.md', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'CHANGELOG.md') { Copy-Item (Join-Path $root $f) $meshemu }
# Optional scripted install from the package folder (not in the zip).
foreach ($f in 'install.ps1', 'uninstall.ps1', 'find-game.ps1') { Copy-Item (Join-Path $root $f) $out }

$zip = Join-Path $dist "$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# Entries written one by one with '/' separators: Compress-Archive and ZipFile in PowerShell 5.1 write '\',
# which some unzip tools mangle.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem (Join-Path $out 'End') -Recurse -File) {
        $entry = $f.FullName.Substring($out.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
                                                                      [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
Write-Host "release package: $zip" -ForegroundColor Green

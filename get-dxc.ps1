# Fetches the DirectX Shader Compiler (DXC) release this patch is built and tested with, from Microsoft's
# official GitHub release. Dot-sourced by build.ps1. The release package bundles only its dxcompiler.dll
# (open source, see THIRD_PARTY_NOTICES.md); dxc.exe and dxil.dll are for building and testing.

$DxcVersion = '1.8.2502'
$DxcUrl = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2502/dxc_2025_02_20.zip'
$DxcFiles = 'dxc.exe', 'dxcompiler.dll', 'dxil.dll'

# Downloads the release and copies $Files into $Dest: binaries from bin\x64, other files (the licenses) from
# wherever the zip has them.
function Get-DxcRelease([string]$Dest, [string[]]$Files) {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('ff7r_dxc_' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        $zip = Join-Path $tmp 'dxc.zip'
        Write-Host "Downloading DirectX Shader Compiler $DxcVersion (about 26 MB) from Microsoft's GitHub release..."
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $oldProgress = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'  # the progress bar makes Invoke-WebRequest very slow in PS 5.1
        try { Invoke-WebRequest -Uri $DxcUrl -OutFile $zip -UseBasicParsing }
        finally { $ProgressPreference = $oldProgress }
        Expand-Archive $zip (Join-Path $tmp 'x') -Force
        foreach ($f in $Files) {
            $hit = Get-ChildItem (Join-Path $tmp 'x') -Recurse -Filter $f |
                Sort-Object { $_.FullName -notmatch '\\x64\\' } | Select-Object -First 1
            if (-not $hit) { throw "$f not found in the DXC download" }
            Copy-Item $hit.FullName $Dest -Force
        }
    } finally {
        Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
    }
}

# Copies dxc.exe, dxcompiler.dll and dxil.dll into $Dest: from $From if given, else from the download.
function Install-Dxc([string]$Dest, [string]$From) {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    if ($From) {
        foreach ($f in $DxcFiles) {
            $src = Join-Path $From $f
            if (-not (Test-Path $src)) { throw "$f not found in $From" }
            Copy-Item $src $Dest -Force
        }
    } else {
        Get-DxcRelease $Dest $DxcFiles
    }
    $v = (& (Join-Path $Dest 'dxc.exe') --version 2>&1 | Out-String).Trim()
    if ($v -notmatch [regex]::Escape($DxcVersion)) {
        Write-Warning "dxc reports '$v'; this patch was tested with $DxcVersion. The converter may misread other versions."
    }
}

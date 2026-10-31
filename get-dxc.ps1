# Fetches the DirectX Shader Compiler (DXC) release this patch is built and tested with, from Microsoft's
# official GitHub release. Dot-sourced by install.ps1 and build.ps1; DXC is not redistributed with the patch.

$DxcVersion = '1.8.2502'
$DxcUrl = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2502/dxc_2025_02_20.zip'
$DxcFiles = 'dxc.exe', 'dxcompiler.dll', 'dxil.dll'

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
            foreach ($f in $DxcFiles) {
                $hit = Get-ChildItem (Join-Path $tmp 'x') -Recurse -Filter $f |
                    Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
                if (-not $hit) { throw "$f (x64) not found in the DXC download" }
                Copy-Item $hit.FullName $Dest -Force
            }
        } finally {
            Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
        }
    }
    $v = (& (Join-Path $Dest 'dxc.exe') --version 2>&1 | Out-String).Trim()
    if ($v -notmatch [regex]::Escape($DxcVersion)) {
        Write-Warning "dxc reports '$v'; this patch was tested with $DxcVersion. The converter may misread other versions."
    }
}

# Locates FF7 Rebirth's End\Binaries\Win64 folder (Steam libraries, default Epic path). Dot-sourced by
# install.ps1 and uninstall.ps1.

$ExeName = 'ff7rebirth_.exe'
$GameRel = 'FINAL FANTASY VII REBIRTH\End\Binaries\Win64'

function Find-Game {
    $libs = @()
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $steam = $steam -replace '/', '\'
        $libs += $steam
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' -AllMatches).Matches |
                ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        }
    }
    $libs += "${env:ProgramFiles(x86)}\Steam"
    $candidates = @($libs | Select-Object -Unique | ForEach-Object { Join-Path $_ "steamapps\common\$GameRel" })
    $candidates += Join-Path $env:ProgramFiles "Epic Games\$GameRel"
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c $ExeName)) { return $c }
    }
    return $null
}

# Returns the game folder to use: $Game if given, else the one found; throws if neither has the exe.
function Resolve-Game([string]$Game) {
    if (-not $Game) { $Game = Find-Game }
    if (-not $Game -or -not (Test-Path (Join-Path $Game $ExeName))) {
        throw "Could not find $ExeName. Run again with -Game '<game folder>\End\Binaries\Win64'."
    }
    return (Resolve-Path $Game).Path
}

function Assert-GameClosed {
    if (Get-Process ($ExeName -replace '\.exe$', '') -ErrorAction SilentlyContinue) {
        throw 'Close the game first (the patch DLL cannot be replaced while it runs).'
    }
}

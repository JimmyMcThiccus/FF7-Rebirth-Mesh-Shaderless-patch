#!/bin/bash
# Build proxy + harness, render the WARP reference (real mesh shaders, in software) and the emulated image
# on the first NVIDIA GPU through the proxy, compare pixels. Run build.ps1 once first (fetches DXC, builds
# dxasm.exe). Git Bash; MESHEMU_RUN overrides the scratch folder.
#   tests/run.sh [mode]   mode: harness argument (default: direct): direct|indirect|indirect3|multi|batch
# Runs in a short directory (MAX_PATH). Set DEBUG=1 to enable the D3D12 debug layer for the emulated run.
# MESHEMU_TEST_MAX_GROUPS=<n> (e.g. 20 or 4) makes the converter cap maxGroups, so direct draws of the 9x9-group
# scene are split into chunks (ABI v10) and compared like any other run.
set -e
M="$(cd "$(dirname "$0")/.." && pwd)"
RUN="${MESHEMU_RUN:-$(cygpath -u "$TEMP")/meshemu_run}"   # short path: the harness hits MAX_PATH in deep folders
MODE="${1:-direct}"
mkdir -p "$RUN"
powershell -NoProfile -Command "Set-Location '$(cygpath -w "$M/proxy")'; .\build.cmd; exit \$LASTEXITCODE" > /tmp/meshemu_build.log 2>&1 || { cat /tmp/meshemu_build.log; exit 1; }
powershell -NoProfile -Command "Set-Location '$(cygpath -w "$M/tests")'; .\build.cmd; exit \$LASTEXITCODE" > /tmp/meshemu_hbuild.log 2>&1 || { cat /tmp/meshemu_hbuild.log; exit 1; }
cp "$M/proxy/d3d12.dll" "$M/tests/harness.exe" "$RUN/"
cd "$RUN"
rm -rf meshemu
./harness.exe C:/Windows/System32/d3d12.dll warp "$MODE" ref.rgba
./harness.exe ./d3d12.dll gpu "$MODE" emu.rgba > /dev/null           # first run dumps shaders
python -I "$M/converter/convert.py" meshemu/dump meshemu/cache
if [ -n "$DEBUG" ]; then
    python -I "$M/tests/dbwin.py" dbg.txt 15 &
    LISTENER=$!
    sleep 1
    HARNESS_DEBUG=1 ./harness.exe ./d3d12.dll gpu "$MODE" emu.rgba || true
    sleep 1
    kill $LISTENER 2>/dev/null || true
    grep -E "D3D12 (ERROR|CORRUPTION|WARNING)" dbg.txt | grep -v FENCE_ZERO | head -20 || true
else
    ./harness.exe ./d3d12.dll gpu "$MODE" emu.rgba
fi
grep -vE "loaded|CreateDevice|OPTIONS7" meshemu/meshemu.log | tail -12
python -I "$M/tests/compare.py" ref.rgba emu.rgba

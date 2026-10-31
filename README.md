# FF7:Rebirth Mesh-Shaderless patch

Play **FINAL FANTASY VII REBIRTH** (PC) on a DirectX 12 GPU **without mesh shader support**.

The PC version requires DirectX 12 Ultimate because it draws its geometry with mesh shaders, and refuses to
start on older GPUs. This patch is a drop-in `d3d12.dll` that sits next to the game. It reports mesh shader
support to the game and translates every mesh shader draw into compute and vertex shader work that older GPUs
can run. The game's files are not modified.

> **Status: v0.1.0 beta.** Developed and tested on one GPU, an NVIDIA TITAN V. Other GPUs that meet the
> requirements below should work in principle but are untested. Reports are welcome in the issues.

## Requirements

- Windows 10 or 11, 64-bit, and the Steam or Epic PC version of FINAL FANTASY VII REBIRTH.
- A DirectX 12 GPU with **feature level 12_1**, **Shader Model 6.6** and **resource binding tier 3**, with a
  current driver. The patch only fills in mesh shaders: if the game needs another feature your GPU lacks, it
  will still not run.
- About **800 MB of VRAM** on top of what the game uses (the patch's working buffer).
- **Python 3.8 or newer** installed and on PATH as `python` ([python.org](https://www.python.org/downloads/),
  tick "Add python.exe to PATH"). The patch converts the game's shaders in the background with it.
- An internet connection during install: the installer downloads Microsoft's
  [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) 1.8.2502 from its official
  GitHub release.

## Install

1. Download `FF7R-Mesh-Shaderless-patch-<version>.zip` from
   [Releases](../../releases) and extract it anywhere.
2. Close the game. Right-click `install.ps1` and choose **Run with PowerShell**, or run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File install.ps1
   ```

   The installer finds Steam and default Epic installs. Otherwise pass the game's binaries folder:

   ```powershell
   powershell -ExecutionPolicy Bypass -File install.ps1 -Game "D:\Games\FINAL FANTASY VII REBIRTH\End\Binaries\Win64"
   ```

3. Start the game normally.

**First launch:** the first time the game shows new kinds of geometry (the first launch, a new area, changed
graphics settings), some of it is missing for a few seconds. The patch extracts those mesh shaders and
converts them in the background, and they appear without a restart. Converted shaders are cached in
`<game>\End\Binaries\Win64\meshemu\cache`.

If another mod already uses a `d3d12.dll`, the installer moves it to `d3d12.dll.meshemu-backup`, and
uninstalling puts it back.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File uninstall.ps1
```

This removes the patch DLL and the `meshemu` folder. Add `-KeepCache` to keep the converted shaders for a
later reinstall.

## Performance

Measured on an NVIDIA TITAN V (Volta, 12 GB HBM2) at one busy outdoor spot, 30-second averages, 100% render
scale, GPU at about 1650 MHz:

| Preset | 1440p, TAAU | 1440p, FSR | 1080p, FSR |
|---|---|---|---|
| High | 46.5 FPS (min 42.3) | 42.6 FPS (min 39.0) | 51.3 FPS (min 46.5) |
| Medium | 49.8 FPS (min 46.4) | 46.1 FPS (min 43.3) | 53.9 FPS (min 50.1) |
| Low | 57.4 FPS (min 53.8) | 53.0 FPS (min 49.1) | 63.7 FPS (min 57.8) |

1080p was measured with FSR only; with TAAU, expect roughly 8% more there too.

Emulating the mesh shaders costs roughly a quarter to a third of the GPU's time; the rest is the game's own
rendering. Tips:

- **Use TAAU rather than FSR at 100% render scale.** At native resolution both only anti-alias, and FSR's
  pass is the heavier one. On the TITAN V, TAAU was 8-9% faster at every preset at the same GPU clock.
- **Check that your GPU actually boosts.** On NVIDIA, any app holding a GPU compute context (in testing, a
  photo editor left open) can pin the whole GPU in power state P2 at a reduced clock. On the TITAN V that
  cost about 15%. Check with `nvidia-smi --query-gpu=pstate,clocks.sm --format=csv` while the game runs;
  it should say P0.
- Keep the GPU cool. Under this load the TITAN V reaches its temperature limit and throttles unless the fan
  curve is raised.
- Lower resolution helps more than lower presets: about 40% of a frame scales with resolution.

## Troubleshooting

- `<game>\End\Binaries\Win64\meshemu\meshemu.log`: what the patch did (device creation, converted
  pipelines, skipped draws).
- `<game>\End\Binaries\Win64\meshemu\convert.log`: shader conversion output. A pair that keeps failing
  there stays invisible.
- Geometry missing for good: check that `python` works in a new command prompt and that `convert.log`
  shows conversions. Shaders the patch cannot convert (amplification shaders, per-primitive outputs) are
  logged and skipped. The game builds tested so far do not use them.
- Creating an empty file `meshemu\profile.txt` before launching adds GPU timings of the emulated draws to
  `meshemu.log` every few seconds.

## How it works

- **Capability spoofing:** when the GPU refuses feature level 12_2, the device is created at 12_1, and the
  game is told 12_2 and mesh shader tier 1 are available.
- **Shader conversion** (`converter/convert.py`): each mesh shader the game creates is dumped, rewritten at
  the DXIL level and re-signed with Microsoft's validator. The game's mesh shaders compute vertex t and
  primitive t in thread t without sharing data between threads. So the shader itself can run as the
  **vertex shader**, and a small compute shader version writes only the triangle indices.
- **Draw translation** (`proxy/proxy.cpp`): every `DispatchMesh` / `ExecuteIndirect(DISPATCH_MESH)` becomes
  "index compute pass, barrier, indexed draw" using the game's own indirect arguments, with the game's
  root arguments replayed for the compute pass.

[docs/DESIGN.md](docs/DESIGN.md) has the full design notes and the history of what was tried, including
measurements of the approaches that did not pay off.

## Building from source

Needs Visual Studio 2022 or newer with "Desktop development with C++", and Python 3.8+.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

This downloads the pinned DXC release into `third_party\dxc`, builds `proxy\d3d12.dll` and
`converter\dxasm.exe`, and writes the release zip to `dist\`. `tests/run.sh` (Git Bash) renders a test mesh
shader scene with real mesh shaders on WARP and through the patch on your GPU and compares the pixels.
`HARNESS_MS_GAME=1` uses a test shader shaped like the game's.

## Disclaimer

Unofficial fan project, not affiliated with or endorsed by Square Enix, NVIDIA or Microsoft. FINAL FANTASY
is a registered trademark of Square Enix. You need your own copy of the game. This repository contains no
game code or game data: shaders are extracted and converted on your machine and stay there. Use at your
own risk.

## License

[GNU General Public License v3.0](LICENSE), with an additional permission (GPLv3 section 7) for combining
the patch with Microsoft's DirectX Shader Compiler, which it needs at run time. Details and third-party
components: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

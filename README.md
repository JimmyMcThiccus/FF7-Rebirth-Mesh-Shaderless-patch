# FF7:Rebirth Mesh-Shaderless patch
# Initially made by JamieTheAnything with assistance with Claude, any further commits/merges/Forks should be made and authored by people. 

Play **FINAL FANTASY VII REBIRTH** (PC) on a DirectX 12 GPU **without mesh shader support**.

The PC version requires DirectX 12 Ultimate because it draws its geometry with mesh shaders, and refuses to
start on older GPUs. This patch is a drop-in `d3d12.dll` that sits next to the game. It reports mesh shader
support to the game and translates every mesh shader draw into compute and vertex shader work that older GPUs
can run. The game's files are not modified.

> **Status: v0.3.1 beta.** Tested on an NVIDIA TITAN V (Volta) and an NVIDIA Quadro P1000 (Pascal). Since
> v0.2.0 the patch also works on GPUs without native 16-bit shader math, such as Pascal; older generations
> should benefit too but are untested. Other GPUs that meet the requirements below should work in principle.
> Reports are welcome in the issues.

## Requirements

- Windows 11, or Windows 10 version 22H2, 64-bit, and the Steam or Epic PC version of FINAL FANTASY VII
  REBIRTH. Older Windows 10 releases (1909 and earlier) have a D3D12 runtime that predates DX12 Ultimate and
  fail at startup with `CreateCommandSignature ... E_INVALIDARG`.
- A DirectX 12 GPU with **feature level 12_1**, **Shader Model 6.6** and **resource binding tier 3**, with a
  current driver. The patch only fills in mesh shaders: if the game needs another feature your GPU lacks, it
  will still not run.
- About **300 MB of VRAM** on top of what the game uses (the patch's working buffer). A 4 GB card runs the
  game at the lowest settings, with little memory to spare.

## Install

1. Download `FF7R-Mesh-Shaderless-patch-<version>.zip` from [Releases](../../releases) and close the game.
2. Open the game folder. In Steam: right-click FINAL FANTASY VII REBIRTH → **Manage** → **Browse local
   files**. It is the folder that has `End`, `Engine` and `ff7rebirth.exe`.
3. Open the zip and drag its **`End`** folder into the game folder. Let Windows merge the folders and
   replace the files when it asks.
4. Start the game normally.

That puts `d3d12.dll` and a `meshemu` folder (the shader converter, the DirectX Shader Compiler DLL it uses,
and these docs) in `End\Binaries\Win64`. Nothing else is needed: no Python, no download, no script. To
update, do the same with the new zip.

**Another mod's `d3d12.dll`:** the patch has to be `End\Binaries\Win64\d3d12.dll`, so step 3 replaces any
other `d3d12.dll` there (for example a feature-level spoof, which the patch already does itself). Move it
somewhere safe first if you want it back later.

**First launch:** on the first launch, and after a game update, the patch reads the game's mesh shaders from
its pak file and converts them in the background. This takes seconds to about a minute, depending on the CPU;
geometry missing meanwhile appears without a restart. Later launches skip this. Converted shaders are cached
in `<game>\End\Binaries\Win64\meshemu\precache` and `meshemu\cache`.

## Uninstall

Close the game and delete `End\Binaries\Win64\d3d12.dll` and the `End\Binaries\Win64\meshemu` folder.

## Performance

Measured on an NVIDIA TITAN V (Volta, 12 GB HBM2) at one busy outdoor spot, 30-second averages, 100% render
scale, GPU at about 1650 MHz:

| Preset | 1440p, TAAU | 1440p, FSR | 1080p, FSR |
|---|---|---|---|
| High | 46.5 FPS (min 42.3) | 42.6 FPS (min 39.0) | 51.3 FPS (min 46.5) |
| Medium | 49.8 FPS (min 46.4) | 46.1 FPS (min 43.3) | 53.9 FPS (min 50.1) |
| Low | 57.4 FPS (min 53.8) | 53.0 FPS (min 49.1) | 63.7 FPS (min 57.8) |

1080p was measured with FSR only; with TAAU, expect roughly 8% more there too.

NVIDIA Quadro P1000 (Pascal, 4 GB), same spot, Lowest preset, TAA, GPU at 1544 MHz:

| Resolution | Render scale | FPS |
|---|---|---|
| 1080p | 50% | 24.3 (min 22.3) |
| 720p | 66% | 28.2 (min 25.6) |

On the P1000 about 16 ms of each frame does not shrink with resolution, and VRAM use sits at about 3.8 of
4 GB.

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
- On 4 GB cards, keep texture and other memory-heavy settings at their lowest: the game alone nearly fills
  the card.

## Troubleshooting

- `<game>\End\Binaries\Win64\meshemu\meshemu.log`: what the patch did (device creation, converted
  pipelines, skipped draws).
- `<game>\End\Binaries\Win64\meshemu\convert.log`: shader conversion output. A pair that keeps failing
  there stays invisible.
- Geometry missing for good: check that `convert.log` shows a `precache:` summary and conversions. Shaders the patch cannot convert (amplification shaders, per-primitive outputs) are
  logged and skipped. The game builds tested so far do not use them.
- The game crashes while loading with a "Fatal error": look for `pipeline creation failed` lines in
  `meshemu.log`. They name each shader the GPU rejected and its required-feature flags, and the shaders are
  saved to `meshemu\dumpailed\`. Please attach both to an issue.
- Creating an empty file `meshemu\profile.txt` before launching adds GPU timings of the emulated draws to
  `meshemu.log` every few seconds.

## How it works

- **Capability spoofing:** when the GPU refuses feature level 12_2, the device is created at 12_1, and the
  game is told 12_2 and mesh shader tier 1 are available.
- **Shader conversion** (`conv/`, `meshemu-conv.exe`, a self-contained Rust port of `converter/convert.py`
  with identical output): mesh shaders are rewritten at the DXIL level and re-signed with Microsoft's
  validator. The game's mesh shaders compute vertex t and primitive t in thread t without sharing data
  between threads. So the shader itself can run as the **vertex shader**, and a small compute shader
  version writes only the triangle indices. Those two do not depend on the pixel shader, so every mesh
  shader in the game's pak is converted ahead of time (**precache**, about 10 seconds on 40 threads for
  the game's 3,344 mesh shaders). Mesh shaders that cannot run as a vertex shader are converted per mesh
  and pixel shader pair once the game creates the pipeline.
- **16-bit shader math** (GPUs without native 16-bit support, such as Pascal): some of the game's shaders,
  mesh shaders and one particle compute shader, read 16-bit floats from typed buffers. The converter
  rewrites those reads as 32-bit reads; the GPU converts the stored values on load, so results are
  identical. When the GPU rejects one of the game's own pipelines for this reason, the patch rewrites the
  shader with `meshemu-conv --lower16` (precached in `meshemu\precache\lower16` on such GPUs) and retries.
- **Draw translation** (`proxy/proxy.cpp`): every `DispatchMesh` / `ExecuteIndirect(DISPATCH_MESH)` becomes
  "index compute pass, barrier, indexed draw" using the game's own indirect arguments, with the game's
  root arguments replayed for the compute pass. A `DispatchMesh` too large for the working buffer is drawn
  in several such passes.

[docs/DESIGN.md](docs/DESIGN.md) has the full design notes and the history of what was tried, including
measurements of the approaches that did not pay off.

## Building from source

Needs Visual Studio 2022 or newer with "Desktop development with C++", Rust (`cargo`), and Python 3.8+ for
the build and test scripts.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

This downloads the pinned DXC release into `third_party\dxc`, builds `proxy\d3d12.dll`,
`conv\target\release\meshemu-conv.exe` and `converter\dxasm.exe`, and writes the drag-and-drop release zip
to `dist\`. The package folder `dist\<name>` also has `install.ps1` and `uninstall.ps1`, which find the
game, keep another mod's `d3d12.dll` as `d3d12.dll.meshemu-backup` and put it back on uninstall.
`converter/convert.py` stays as the reference implementation: the proxy uses it when `meshemu-conv.exe` is
missing, and both must produce byte-identical shaders. `tests/run.sh` (Git Bash) renders a test mesh
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

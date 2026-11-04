# Changelog

## v0.3.1-beta (2026-11-03): drag-and-drop install

- **Install by dragging one folder:** the release zip now mirrors the game folder. Drag its `End` folder
  into the game folder (Steam: Manage → Browse local files) and start the game; no script, Python or
  download. Uninstall by deleting `End\Binaries\Win64\d3d12.dll` and `End\Binaries\Win64\meshemu`.
- **DirectX Shader Compiler bundled:** the package includes the official DXC 1.8.2502 `dxcompiler.dll`
  (open source; its licenses are in `meshemu\tools\dxc-licenses`). `dxil.dll` is no longer needed:
  `meshemu-conv.exe` signs shaders with `dxcompiler.dll`'s own validator when `dxil.dll` is absent, with
  byte-identical results.
- The docs are in `End\Binaries\Win64\meshemu`. `install.ps1` / `uninstall.ps1` remain in the repository for
  a scripted install from a built package (`dist\<name>`).

## v0.3.0-beta (2026-11-03): no Python, mesh shaders precached from the pak

- **Python is no longer needed.** The shader converter is now `meshemu-conv.exe`, a self-contained Rust
  port of `convert.py` (`conv/`) that calls DXC's `dxcompiler.dll` and `dxil.dll` directly. It produces
  byte-identical shaders (checked on the 32 game pairs in VS mode, the fallback scheme, chunked draws and
  16-bit lowering). The installer no longer checks for Python and removes the old `convert.py` and
  `dxasm.exe` from `meshemu\tools`.
- **Precache:** at the first start and after game updates, every mesh shader in the game's pak is converted
  ahead of time, in the background (`meshemu\precache`; about 10 seconds for 3,344 mesh shaders on 40
  threads). New areas and changed settings no longer show missing geometry while their shaders convert.
  2,639 of the mesh shaders run as vertex shaders and are precached; the rest (groupshared memory,
  per-primitive outputs) still convert per pair when the game first uses them. On GPUs without native 16-bit
  ops the game's other 16-bit shaders are lowered ahead of time too.
- A `DispatchMesh` too large for the patch's working buffer is now drawn in several pieces instead of
  losing the groups past the limit. `meshemu.log` shows a `split into N chunks` line when this happens. The
  converted shaders get each piece's group offset, so the game's shaders see their original group ids.
  `ExecuteIndirect` mesh draws are still clamped, because their size is only known on the GPU. The largest
  draw seen in the game fits in the buffer, so this is a safeguard. It is also a step towards a smaller
  working buffer on 4 GB cards. The idea comes from lonewolf0622's DirectMesh (see
  THIRD_PARTY_NOTICES.md).
- The converted shader cache moved to a new format (`*.cs10/.vs10/.meta10`); shaders are reconverted once.

## v0.2.0-beta (2026-10-31): Pascal and earlier GPUs

- Runs on GPUs without native 16-bit shader math, such as Pascal; tested on an NVIDIA Quadro P1000. Before,
  the game crashed with "Fatal error" while loading on these GPUs, because a particle compute shader of its
  own could not be created, and 9 of the 26 mesh shaders stayed invisible.
  - The converter rewrites 16-bit-float typed buffer loads as 32-bit loads (identical results), so all mesh
    shaders convert for every GPU.
  - When the GPU rejects one of the game's own pipelines for needing native 16-bit math, the patch rewrites
    the shader (`convert.py --lower16`, cached in `meshemu\cache\lower16`) and retries.
- The working buffer shrank from 768 MB to 256 MB of VRAM, which matters on 4 GB cards. It now only holds
  triangle indices; the largest draw seen needs about 220 MB.
- Diagnostics: every pipeline the GPU rejects is logged in `meshemu.log` with its shaders' required-feature
  flags, and the shaders are saved to `meshemu\dump\failed\`. The GPU's shader model, 16-bit, ray tracing
  and VRS support are logged at startup.
- The converted shader cache moved to a new format (`*.cs9/.vs9/.meta9`); older cache files are ignored and
  shaders are reconverted once.
- Performance on the P1000 (Lowest, TAA): 24 FPS at 1080p with 50% render scale, 28 FPS at 720p with 66%.

## v0.1.0-beta (2026-10-30)

First public beta.

- `d3d12.dll` proxy: reports feature level 12_2 and mesh shader tier 1 on GPUs that lack them (no game exe
  patch), and emulates `DispatchMesh` and `ExecuteIndirect(DISPATCH_MESH)`.
- Shader converter: runs each mesh shader as a vertex shader and writes the triangle indices with a small
  compute version of it. Shaders that do not fit that pattern fall back to a compute pass that also stores
  the vertices. All 32 mesh/pixel shader pairs seen in testing use the fast path.
- Background conversion with a cache. Pipelines appear as soon as their shaders are converted, without a
  restart.
- Installer and uninstaller with Steam and Epic detection. DXC 1.8.2502 is downloaded from Microsoft's
  release.
- Tested on an NVIDIA TITAN V only: about 42 FPS at 1440p High and 51 FPS at 1080p High at a busy outdoor
  spot.

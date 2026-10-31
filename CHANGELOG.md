# Changelog

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

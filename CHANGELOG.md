# Changelog

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

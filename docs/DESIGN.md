# meshemu: mesh shader emulation for FF7 Rebirth on GPUs without mesh shaders

> Development notes, kept as written during development. They record every approach that was tried,
> with its measurements, including those later dropped. The current design is **ABI v10** (the v7 draw scheme). Paths like
> `work\*.csv` and `*.bak` refer to measurement logs and old sources from the development machine;
> they are not part of this repository. All measurements are from one NVIDIA TITAN V.

Target: NVIDIA TITAN V (Volta, FL 12_1, SM 6.8, MeshShaderTier 0) running the **unmodified**
`ff7rebirth_.exe` (UE 4.26 fork, PCD3D_SM6 shaders only).

## Pieces

1. **`proxy/` — d3d12.dll proxy** dropped next to `ff7rebirth_.exe`.
   Forwards every export to `System32\d3d12.dll` (MASM jmp thunks, `exports.asm`) and patches vtables of the
   real device / command list / pipeline library objects.
2. **`converter/` — shader converter** (`convert.py` + DXC tools). Rewrites the disassembled mesh shader IR,
   reassembled and signed with `dxasm.exe` (IDxcAssembler + dxil.dll validator), into an index-only compute
   shader plus the mesh shader itself as a vertex shader (ABI v7 VS mode), or for shaders that do not
   qualify, a compute shader that also stores the vertices plus a generated pass-through vertex shader.
3. **Shader cache** (`<game>\meshemu\`): the proxy dumps every mesh/pixel shader pair (`dump\`), runs
   `tools\convert.py` in the background (`convert.log`), and loads converted shaders from `cache\`
   (`<ms>_<ps>.cs7.dxil`, `.meta7`, `.vs7.dxil`). Pipelines whose shaders are not converted yet are
   rebuilt at draw time once they are (retried every 3 s), so new areas/settings need no restart.

## Capability spoofing (no exe patch)

The game demands DX12 Ultimate. The proxy:
* retries `D3D12CreateDevice(12_2)` at 12_1 when the hardware refuses 12_2,
* then answers `CheckFeatureSupport(FEATURE_LEVELS)` with 12_2 when 12_2 is requested,
* answers `CheckFeatureSupport(OPTIONS7)` with `MeshShaderTier 1`.

With that the game's own `CreateRHI` picks the SM6 (mesh) platform. (Earlier versions patched the exe at
`0x1446d4a42`, `0x1446d95e7`, `0x1446d37f4`; see `../ff7r_nomesh_patch.py`, no longer needed.)

## Emulated draw (ABI v3; v2 notes below kept for history)

v3 drops the prep dispatch and the command ring: the converted CS reads the dispatch size `{x, y, z}` from the
game's own argument record (root SRV `t1 space999`), and group 0 thread 0 writes the `DrawIndexed` arguments to
`scratch[768 MB]`. Per game `ExecuteIndirect` command:

```
[scratch -> UAV]                     (UAV barrier if already UAV)
cs: ExecuteIndirect({DISPATCH}, game args)          compute root signature, graphics root args replayed
[scratch -> INDEX_BUFFER | NON_PIXEL_SR | INDIRECT_ARGUMENT | COPY_SOURCE]
gfx: ExecuteIndirect({DRAW_INDEXED}, scratch[768 MB], count buffer = game args)  -> x == 0 draws nothing
```

Known gap: a command with x > 0 but y or z == 0 runs no CS group and would reuse stale draw arguments.
Count buffers on the game's ExecuteIndirect are not supported (the game does not use them).
Draw-size stats (`draw sizes over last ...` in the log) come from copying each draw's arguments to a readback ring.

Measured on High, 1440p (same spot): v2 27.7 FPS avg -> v3 30.0 FPS avg; ~98 emulated draws per frame,
~3,300 meshlet groups per draw on average.

ABI v4 (current): converted CS / meta / VS are generated per MS+PS pair (`<ms>_<ps>.cs4.dxil`, `.meta4`,
`.vs4.dxil`); mesh outputs the PS never reads are not stored and the kept ones are packed back to back
(vertexBytes instead of padded 16-byte rows). ~19% fewer bytes per vertex over the game's pairs (depth-only
pairs up to 79%).

Profile (GPU timestamps, `meshemu\profile.txt` enables it), High at the same spot:

| | FPS avg | emulation per frame | cs per draw | draw per draw |
|---|---|---|---|---|
| v3 | 29.7 | 14.9 ms (44%) | 90 us | 60 us |
| v4 | 30.4 | 13.8 ms (42%) | 84 us | 57 us |

The cs time is mostly the original mesh shader work itself (~3,300 groups x 128 threads per draw), which
native mesh shader hardware would also pay; the draw time includes the game's own pixel shading.

ABI v5: same scratch layout and proxy sequence as v4, cheaper compute pass (`<ms>_<ps>.cs5.dxil`,
`.meta5`, `.vs5.dxil`):

* **No index pre-fill.** v2-v4 had every group write all `maxPrims` slots as degenerate triangles, group
  barrier, then let `EmitIndices` overwrite them. The game's meshlets are 128 verts x 128 (or 64) prims with
  the primitive limit binding first, so nearly every slot was written twice: for the depth-only pairs
  (16-32 B vertices) that pre-fill was ~1/3 of all bytes the CS wrote. Now the converter checks that the
  shader's single `SetMeshOutputCounts(nv, np)` call is on every path to the return (a CFG walk; true for
  all 26 game mesh shaders) and has each thread fill only the slots `[np, maxPrims)` right at that call.
  No barrier is needed: `EmitIndices` only writes slots below `np`. Shaders failing the check keep the old
  pre-fill + barrier (`pre-filled indices` in `convert.log`).
* **Aligned vector vertex stores/loads.** v4 stored every output component with its own 4-byte raw store
  at a 4-byte-aligned address (28 scattered stores for a 112 B vertex), and the VS did 4-byte-aligned
  `Load4`s. Now the layout puts 16 B elements first, pairs each 12 B element with a 4 B one (or pads it) to
  complete a 16 B unit, then 8 B and 4 B elements, so each element starts at a multiple of its size; the
  stride is a multiple of 8 (or 16 when that costs <= 4 B). Consecutive component stores of one output are
  merged into one `rawBufferStore` carrying the true alignment (16/8/4) in its alignment operand, and the VS
  reads the vertex through a `StructuredBuffer<uint4/uint2/uint>` in whole aligned units. Over the game's
  27 layouts the stride grows by 0 B (10 of them, all the 16 B and 32 B ones), 4 B (13) or 8 B (4, e.g.
  88 -> 96); a 112 B vertex goes from 28 stores to 7.

Both changes are in the converter only; the proxy just loads the v5 cache files (old v4 files are ignored and
can be deleted). Measured 2026-10-30 at the benchmark spot (High, identical clocks): v4 30.5 FPS (compute
87 us/draw) vs v5 28.7 FPS (103 us/draw), draw pass unchanged. The tail fill as first written was the cause:
with `select` redirecting the stores of used slots to one trash address, every thread of every group still
issued its store and ~450k 12-byte stores per draw hit one L2 line. v6 branches around the store instead
(see below).

With that branch fix backported (v5 proxy, `work\convert_v5.py`), same spot and clocks: **29.6 FPS, compute
88 us/draw, draw 60 us/draw** vs v4 30.5 FPS, 87 / 60 us. Per-second FPS samples have a standard deviation of
~2.4 (standard error ~0.45 per 30 s run), so the FPS gap is within noise and the per-draw GPU times are the
same: **neither v5 change helps.** The compute pass is not limited by the bytes or the number of store
instructions it writes; it is the game's own mesh shader math and loads (and the barrier drain in front of
it). Further converter-side write tweaks are not worth pursuing.

## ABI v10 (current, unreleased): oversized direct draws are split instead of clamped

Same draw scheme as v9. Until v9 a draw with more groups than fit in scratch (`maxGroups`, from the 256 MB
cap) was clamped: groups past the cap wrote to the trash range and their geometry was missing. The idea of
continuing an overflowing draw in order instead of dropping it comes from lonewolf0622's DirectMesh for
RADV (BC-250 / RDNA1, MIT), whose index route turns pool overflow into an ordered suffix of draws. No code
was taken from it.

* **Group offset.** The proxy appends one more root parameter to every rewritten root signature, graphics
  and compute variant: 3 root constants `{ox, oy, oz}` at `b0 space999` (the budget check is now
  `cost + 3*2 + 3 <= 64` DWORDs). `add_group_offset` in `convert.py` declares the matching cbuffer in both
  converted shaders. In the cs, scratch addressing, the degenerate fill and the DrawIndexed arguments stay
  chunk-local (raw `SV_GroupID` and the chunk dims). Every `groupId` the game's own code reads gets `o`
  added, and every `threadId` gets `o * numthreads`. In the VS, the group coordinates split from
  `SV_VertexID` by the chunk dims get `o` added. All 32 game pairs read `SV_GroupID`, so without the offset a
  split draw would fetch the wrong meshlets.
* **Chunking** (`Hk_ClDispatchMesh`). When `x*y*z > maxGroups` the dispatch is cut into whole xy slices
  (`cz = maxGroups / (x*y)`). If one slice is too big it is cut into x rows, and if one row is too big,
  into x runs of `maxGroups`. Each chunk gets its own upload slot with the chunk dims, its offset constants,
  a cs dispatch, the scratch transition and a DrawIndexedInstanced of `chunkGroups * maxPrims * 3`. The log
  line `DispatchMesh ... exceeds scratch (...): split into N chunks of AxBxC` replaces the old
  `exceeds scratch, clamped`. Draws that fit run exactly as before (one chunk, offset 0).
* **ExecuteIndirect is not split.** Its group count lives in a GPU buffer, so splitting it would need GPU-side
  argument rewriting. Indirect draws set the offset to 0 and still clamp at `maxGroups`. The largest draw seen
  in the game (143k groups, about 220 MB) fits, so this has not been hit in practice.
* **Testing.** `MESHEMU_TEST_MAX_GROUPS=<n>` caps `maxGroups` in the converter, which forces the harness's
  9x9-group scene into chunks: n=20 gives 5 chunks of 9x2x1 (y rows), n=4 gives 27 of 4x1x1 (x runs). Both
  shader shapes (`HARNESS_MS_GAME=1` VS mode, and the fallback) match WARP in `direct` and `multi`. As a
  negative control, `indirect` with n=4 loses 48,581 pixels, which shows the comparison catches dropped
  groups. The z-slice split has no harness case, because the test scene is a single z layer. All 32 game
  pairs convert and validate under v10.

Cache files are `.cs10/.vs10/.meta10`; v9 caches are ignored and reconverted once.

## ABI v9 (superseded by v10, v0.2.0): v7 plus GPUs without native 16-bit math

Same draw scheme as v7 below. Two changes, from bringing up an NVIDIA Quadro P1000 (Pascal GP107, 4 GB:
SM 6.8, binding tier 3, but **no native 16-bit shader ops**, no DXR, no VRS):

* **Native 16-bit lowering.** 9 of the 26 game mesh shaders, and one of the game's own particle compute
  shaders, use 16-bit types, always the same way: `bufferLoad.f16` from a `Buffer<half2/half4>` followed by
  `fpext half -> float`. On Pascal `CreateComputePipelineState` rejects such shaders with E_INVALIDARG. For
  the game's own compute shader that left UE with a null PSO and a crash in `RHISetGraphicsPipelineState`
  (access violation reading 0x10; `ff7rebirth_.exe` +0xc1db06). `lower_native16` in `convert.py` turns
  those loads into `bufferLoad.f32`, since a typed load converts the stored format, so results are
  identical. It also turns the `fpext` into a move, fixes the resource element type (metadata and
  `annotateHandle` properties) and clears the native-low-precision shader flag. Any other 16-bit use raises
  ConvertError. Mesh shaders are always lowered. For the game's own pipelines the proxy logs every failed
  creation with each shader's SFI0 feature flags and saves the shaders to `dump\failed\`. If a shader needs
  native 16-bit ops (flag 0x40000), the proxy runs `convert.py --lower16` on it synchronously (cached in
  `cache\lower16\<id>.dxil`) and retries the creation. The paired PS of a mesh pipeline is lowered up front
  when the device lacks 16-bit support.
* **256 MB scratch** (was 768). In VS mode scratch holds only indices; the largest draw seen is 143k groups,
  about 220 MB at 128 primitives per group. Draws beyond the cap are clamped, as before (v10 splits direct
  ones instead).

Cache files are `.cs9/.vs9/.meta9`. P1000 results at the benchmark spot (Lowest, TAA, 1544 MHz, no
throttling): 24.3 FPS at 1080p / 50% render scale, 28.2 at 720p / 66%. With profiling: 37% of GPU time is
emulation (per draw cs 51 us + draw 103 us, about 88 draws a frame). That is 4.5 ms of index compute plus
9.1 ms of emulated draws in a 37 ms frame. VRAM use is about 3.8 of 4 GB.

## ABI v7 (superseded by v9): the mesh shader runs as the vertex shader

Code: `proxy/` and `converter/` (forked from v5; the v6 sources are kept as `proxy_v6.cpp.bak` and
`convert_v6.py.bak`).

All 26 game mesh shaders have the same shape: 128 threads, thread t writes only vertex t and primitive t,
and threads never communicate (no groupshared memory, barriers, wave/quad ops or UAV writes). For such a
shader the vertex outputs can be computed by the shader itself in a vertex shader, so v7 splits the work:

* **cs** (`.cs7.dxil`): the v5 compute conversion with every `StoreVertexOutput` dropped: it writes only the
  index slots (EmitIndices + tail fill) and the DrawIndexed arguments. The driver removes the now-dead vertex
  math, so this pass only does the meshlet header and index loads.
* **vs** (`.vs7.dxil`, `convert_ms_to_vs`): the mesh shader DXIL rewritten as a vertex shader. `SV_VertexID`
  is the index value the cs wrote, `G * maxVerts + t`; `t` replaces the thread id, and the group id is `G`
  split by the dispatch size read from the game's argument record (root SRV `t1 space999`, which the proxy
  now also binds for the graphics stage). `StoreVertexOutput` becomes `StoreOutput` against the mesh
  shader's own vertex output signature; `SetMeshOutputCounts` / `EmitIndices` are dropped.
* `vs_mode_blocker()` decides per pair; pairs that do not qualify, or whose VS output registers do not
  match the PS inputs (`check_linkage`), get the v5 scheme under the same file names. `MESHEMU_NO_VS_MODE=1`
  forces the fallback. All 32 game pairs use VS mode.

Vertices never go through scratch, and the pass that sits between the two barriers shrinks to index work.
The VS may shade a vertex more than once (post-transform cache misses); the measurement says that costs far
less than the round trip did.

Measured 2026-10-30, benchmark spot, High, 1440p, SM clock 1335 MHz in every run (30 s each,
`work\fps_high_*_ab.csv`):

| | FPS avg | FPS min | frametime avg | cs / draw per draw | emulation share of GPU time |
|---|---|---|---|---|---|
| v4 | 30.5 | 26.2 | 47 ms | 87 / 60 us | 44% |
| v5 (fixed) | 29.6 | 24.6 | 50 ms | 88 / 60 us | 43% |
| v6 | 29.0 | 21.8 | 82 ms | ~80 / ~46 us | ~45% |
| **v7** | **36.2** | **32.6** | **40 ms** | **24 / 67 us** | **31%** |

+19% average and +24% minimum FPS over v4. The compute pass got 3.6x cheaper; the draw pass got ~7 us
slower, which is the vertex math now running there (plus any repeated shading).

All runs above were clock-capped: Canva Affinity was open, and its GPU compute context held the Titan V in
P2 at a fixed 1335 MHz. With Affinity closed (P0), v7 at the same spot: **38.8 FPS avg, 34.6 min**
(`work\fps_high_v7_p0.csv`), SM clock averaging 1511 MHz because the card then sits at its 83 C thermal
target and throttles (event reason SW thermal slowdown), ~147 W. +13% clock gave +7% FPS, so part of the
frame is bound by memory bandwidth, which the HBM2 clock (850 MHz, fixed) does not change. With the fan at
~81% and the card allowed to run at 87-88 C: **42.6 FPS avg, 39.0 min** at 1638 MHz, ~212 W
(`work\fps_high_v7_p0_run2.csv`); still briefly thermal-limited.

Presets, v7 uncapped, same spot, resolution and fan (`tools\bench.sh`, `work\fps_v7_p0_{medium,low}.csv`;
"early build" = `work\fps_{high,medium,low}.csv`, capped, pre-v3 emulation; the frame split uses the
emulation share from the `profile:` lines):

| preset | early build | v7 uncapped | frame | emulation | rest of the frame |
|---|---|---|---|---|---|
| High | 27.7 FPS | 42.6 FPS (min 39.0) | 23.5 ms | 7.5 ms (32%) | 16.0 ms |
| Medium | 31.1 FPS | 46.1 FPS (min 43.3) | 21.7 ms | 6.5 ms (30%) | 15.2 ms |
| Low | 37.3 FPS | 53.0 FPS (min 49.1) | 18.9 ms | 4.9 ms (26%) | 14.0 ms |

Lower presets shrink the meshlet work (avg 3,200 -> 2,800 -> 2,300 groups per draw), but the rest of the
frame only drops from 16 to 14 ms: it is dominated by work the presets leave alone.

Resolution, same spot and settings (`work\fps_v7_p0_*_1080p.csv`; the first 3 s of the 1080p Low run, ~118
FPS while the settings menu closed, are left out):

| preset | 1440p | 1080p | frame time saved | emulation share at 1080p |
|---|---|---|---|---|
| High | 42.6 FPS (min 39.0) | 51.3 FPS (min 46.5) | 4.0 ms (23.5 -> 19.5) | 35% |
| Medium | 46.1 FPS (min 43.3) | 53.9 FPS (min 50.1) | 3.1 ms (21.7 -> 18.6) | 32% |
| Low | 53.0 FPS (min 49.1) | 63.7 FPS (min 57.8) | 3.2 ms (18.9 -> 15.7) | 20% |

1080p has 44% fewer pixels and saves 3-4 ms, so roughly 7-9 ms of a 1440p frame (~40%) scales with
resolution; the other ~60% does not (the emulated geometry work, shadow maps and other fixed-size passes).
Emulated cost per draw barely moves with resolution (cs ~21 us, draw 59 -> 53 us on High), so at lower
resolutions emulation becomes a larger share of the frame. Check
`nvidia-smi --query-gpu=pstate,clocks.sm --format=csv` shows P0 before comparing new runs with this one.

Verified: the game-shaped test shader `tests/ms_game.hlsl` (`HARNESS_MS_GAME=1`) matches WARP in VS mode
in all harness modes, the debug layer is clean, and the fallback path (`ms_simple`, `HARNESS_WIDE=1`,
`MESHEMU_NO_VS_MODE=1`) still matches.

Follow-up worth trying on top of v7: repack the VS output signature to the PS input layout so pairs with
mismatched registers (none in the game so far) can use VS mode too.

### ABI v8 experiment (tried 2026-10-30, reverted): no index compute pass

Sources kept as `proxy/proxy_v8.cpp.bak`, `converter/convert_v8.py.bak`, `proxy/prep_v8.hlsl.bak`. Idea:
drop the index-only cs by drawing non-indexed, one instance per mesh group and 3 vertices per primitive
slot. The corner VS ran the mesh shader body twice, first as thread p up to its EmitIndices to pick corner
c's vertex index (kept in a local variable), then as that vertex's thread for the outputs. Corners of
unemitted primitives got a NaN position. A 1-thread prep cs still had to turn the game's `{x, y, z}` into
DrawInstanced arguments, so one ring transition per draw remained. Harness and debug layer were clean.

Live A/B in one game session, switched at run time with `meshemu\nocorner.txt`, 1440p High, interleaved
2 x 30 s each (`work\fps_v8_*_r{1,2}.csv`):

| path | FPS avg | FPS min | prep or cs per draw | draw per draw |
|---|---|---|---|---|
| v7 (index cs + mesh-as-VS) | 40.6 | 36.3 | 21 us | 62 us |
| v8 corner VS | 39.3 | 35.4 | 5 us | 87 us |

-3.1%, about 3.4 standard errors. The compute pass shrank as planned, but shading every primitive corner
(about 3x the vertex invocations, with the primitive-side loads on top) cost more in the draw than it
saved. v7 stays.

## ABI v6 (in the source tree, not installed: slower and flickers in-game, see the end of this section)

v3-v5 recorded `barrier, cs, barrier, draw` per emulated draw (~200 pipeline drains a frame; compute never
overlapped the previous draw's pixel work). v6 records all compute passes of a run of consecutive mesh draws
first and the draws afterwards, behind one barrier:

```
[scratch -> UAV]                              at the first draw of a batch
cs_1 ... cs_n                                  each into its own region of the 768 MB scratch ring
[scratch -> INDEX_BUFFER | NON_PIXEL_SR | INDIRECT_ARGUMENT | COPY_SOURCE]
draw_1 ... draw_n                              ExecuteIndirect(DRAW_INDEXED) from each region, count = game args
copy draw args -> readback ring, copy batch marker
restore the app's PSO, index buffer, topology, graphics and compute root arguments
```

* **Deferral.** `ExecuteIndirect(DISPATCH_MESH)` / `DispatchMesh` record the compute pass at once and queue the
  draw (`PendingDraw`: emulated PSO, region, capacity, a snapshot of the graphics root signature + arguments,
  the count buffer). The batch is flushed when the ring is full, at `Close`, and by *every* other command list
  method: all 77 recording slots of `ID3D12GraphicsCommandList6..10` are hooked (`kClHooks`, how many the
  object implements is probed with `QueryInterface`), as forwarding wrappers that flush first. Exceptions
  that only track state and do not flush: `SetPipelineState`, both `Set*RootSignature`, the 12 root argument
  setters, `IASetIndexBuffer`, `IASetPrimitiveTopology`, and the PIX marker calls. Flushing replays each
  pending draw's root arguments (deduplicated per batch), then restores the app's state, so a run of mesh
  draws with PSO and root argument changes between them stays one batch; a barrier, render target, viewport,
  descriptor heap, query or non-mesh draw between them ends it. A compute root setter after an emulated
  compute pass first restores the app's compute root signature (`cmpDirty`), since the setter's parameter
  index refers to it.
* **Per-draw regions.** Nothing about the draw size is baked into the shaders any more (`.cs6.dxil`, `.meta6`
  = `maxVerts maxPrims vertexBytes`, `.vs6.dxil`). The CS addresses relative to root UAV u0 = region base and
  reads a control record `{cap, trashOff, token}` from root SRV t0 (an upload-ring slot); the region is
  `[0, 256)` DrawIndexed arguments + token (written by group 0 thread 0), `[256, 256 + align256(cap * maxPrims
  * 12))` indices, then `cap * maxVerts` vertices; groups `>= cap` write to `trashOff` (the spare MB past the
  ring, as a region-relative offset). The VS gets root SRV t0 = the region's vertex area, so it is
  `vb = SV_VertexID * stride` with no constants. Index buffer view and draw arguments point into the region.
* **Size prediction.** The draw size exists only on the GPU, but a draw (same emulated PSO, same argument
  record address) recurs every frame. The flush copies each region's DrawIndexed arguments + token to a
  32 K-slot readback ring and the batch's last token to a marker; a slot the marker has passed holds the
  real size (token mismatch = the dispatch ran no group). Per key the proxy keeps the last 8 sizes;
  the reservation is `max + 25% + 64` groups, a draw that filled its region is assumed 4x bigger next time,
  a first-seen key gets the whole ring (so it flushes before and after). A low prediction loses geometry for
  one frame; the readback corrects it. `draw sizes:` lines in the log report predicted / first-seen /
  clipped counts and the average batch length; if argument addresses did not recur (table > 64 K keys) it
  says so and clears the table.
* **Tail fill without the trash line.** `degenerate_fill` puts each slot store behind a branch
  (`emu.tfs<j>` / `emu.tfn<j>` blocks, phi predecessors of the split block renamed) instead of redirecting
  unwanted stores to one address, which fixes the v5 regression above.

Profiling is per batch now (`profile:` lines: batches, draws per batch, cs and draw time per draw).
Verified 2026-10-30: `tests/run.sh` in all modes (`direct|indirect|indirect3|multi|batch`), plus
`HARNESS_WIDE=1`, matches WARP; `DEBUG=1` shows no debug-layer errors; all 32 game pairs convert. The new
`batch` mode issues 3 `ExecuteIndirect` calls with root constant changes between them over 3 frames: frame 1
is 3 whole-buffer batches of 1 draw, frames 2 and 3 one batch of 3 draws from the readback.

In-game, measured 2026-10-30 (High, 1440p, same spot and clocks as the v4 run; `work\fps_high_v6_ab.csv`):

| | FPS avg | FPS min | frametime avg | cs / draw per draw |
|---|---|---|---|---|
| v4 | 30.5 | 26.2 | 47 ms | 87 / 60 us |
| v6 | 29.0 | 21.8 | 82 ms | ~80 / ~46 us |

Per-draw GPU time dropped, but the frame got slower and spikier and **visibly flickers**. The log explains
both: only **2.2 draws per batch** (the game's own barriers and state changes split the ~98 draws a frame
into short runs, so there is little to overlap), and of 180 K predicted draws **1,789 were clipped** (a
too-small prediction drops geometry for a frame: the flicker) plus 5,117 first-seen whole-buffer batches.
Before v6 is worth another try: never clip (flush the batch and give the draw the whole ring whenever its
prediction is unknown or was ever exceeded, rather than risking a short region), and measure the CPU cost of
hooking all 77 command list methods, since the GPU-side gain did not reach the frame rate.

## Emulated draw (ABI v2)

For a mesh PSO the app gets a *null* pipeline (placeholder VS, no PS). The proxy builds:

* `cs`: converted mesh shader, on a compute variant of the root signature (see below).
* `gfx`: generated VS + original PS + the app's raster/blend/depth/RT state, triangle list.

Scratch buffer layout for one emulated draw (constants baked per shader by the converter, cap 768 MB):

```
[0, idxRegion)           uint3 indices[maxGroups][maxPrims]   global = G * maxVerts + local; unused = degenerate
[idxRegion, ...)         float4 rows[maxGroups * maxVerts][rowBytes / 16]
[768 MB, +1 MB)          trash for groups beyond maxGroups
```

The CS pre-fills every primitive slot of its group with a degenerate triangle (unrolled per thread), group
barrier, then runs the original code with `EmitIndices`/`StoreVertexOutput` turned into raw buffer stores
(`SetMeshOutputCounts` becomes a no-op). The VS is just `vb = idxRegion + SV_VertexID * rowBytes` + loads.

`ExecuteIndirect(DISPATCH_MESH)` (the only form the game uses: maxCount 1, no count buffer) becomes:

```
[scratch -> UAV, ring -> UAV]                     one barrier batch (state tracked per command list)
prep CS (1 thread): app args -> ring: per-command {consts baseGroup,x,y; dispatch} + one DrawIndexed record
[ring -> INDIRECT_ARGUMENT]
cs via ExecuteIndirect({root consts, DISPATCH}) on the compute root signature, graphics root args replayed
[scratch -> INDEX_BUFFER | NON_PIXEL_SHADER_RESOURCE]
gfx: IASetIndexBuffer(scratch), root SRV t0 space999 = scratch, ExecuteIndirect(DRAW_INDEXED)
restore app PSO, index buffer, topology, compute root signature + args
```

`DispatchMesh` does the same with CPU-known group counts (no prep, no ring).

## Root signatures

`CreateRootSignature` is intercepted for root signatures with MESH/AMPLIFICATION visibility:
* graphics: MESH -> VERTEX, AMPLIFICATION -> HULL (mesh pipelines have no VS/HS, so no register overlap),
  clear `DENY_VERTEX/HULL`, append root UAV `u0`, 4 root constants `b0`, root SRV `t0` (space 999, ALL).
* compute variant: compute PSOs only see ALL-visibility params, so MESH/ALL params become ALL and other
  stage-only params become same-typed placeholders in space 10000+i; same parameter count/order, so the
  game's graphics root arguments replay into it by index.

## Testing

`tests/run.sh direct|indirect|indirect3|multi|batch` (DEBUG=1 for the D3D12 debug layer) renders the same mesh shader scene
on WARP (real mesh shaders, system d3d12.dll) and on the Titan V through the proxy, and compares pixels.
`HARNESS_FL122=1` exercises the 12_2 spoof. `HARNESS_MS_GAME=1` uses `ms_game.hlsl` (shaped like the game's
mesh shaders, so the converter takes VS mode); without it `ms_simple.hlsl` exercises the v5-style fallback.
`HARNESS_WIDE=1` pairs the mesh shader with `ps_wide.hlsl` (16-byte vertex units; with `HARNESS_MS_GAME=1`
it also exercises the VS/PS linkage fallback). `MESHEMU_TEST_MAX_GROUPS=<n>` caps maxGroups in the converter
so direct draws are split into chunks (ABI v10).

## Not handled yet

* Amplification shaders, `StorePrimitiveOutput`, mesh payloads (converter rejects them -> null pipeline).
* `SV_PrimitiveID` read by a PS (would be the system value of the VS path).
* `ExecuteIndirect(DISPATCH_MESH)` with more groups than `maxGroups` is clamped (direct draws are split
  since v10).
* Batching (ABI v6) still pays 2 barriers per *batch*; a batch ends at any non-mesh command, so passes that
  interleave mesh draws with barriers or state the proxy does not snapshot get no overlap. Size prediction
  loses geometry for one frame when a draw grows past its reservation, and a key whose argument address
  changes every frame is "first seen" every frame (whole ring, no batching). Readback slots retire via a
  batch marker, so with command lists recorded in parallel a slot can be read a frame early (it then reads
  as "ran no group"; the max over 8 samples hides it).
* The draw pass runs at ~6 triangles/clock (~450k triangles in ~52 us), the Volta primitive setup rate, so
  it is bound by primitive count like native mesh shaders would be; compacting the few unused primitive
  slots out of the index stream would not change that.

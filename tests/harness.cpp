// Mesh shader test harness.
//   harness <d3d12.dll path> <gpu|warp> <direct|indirect|indirect3|multi|batch> <out.rgba>
// Renders ms_simple/ps_simple (9x9 mesh groups) into a 256x256 RGBA8 target and writes raw pixels.
// batch: three ExecuteIndirect calls (strips of 3, 4 and 2 group rows) with different root constants between
// them, rendered 3 frames in a row: frames 2 and 3 let the proxy batch the draws from frame 1's readback.
#include <windows.h>
#include <atlbase.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <string>
#include <vector>

#include "ms_simple.h"
#include "ms_game.h"
#include "ps_simple.h"
#include "ps_wide.h"

#define CHECK(x)                                                              \
    do {                                                                      \
        HRESULT _hr = (x);                                                    \
        if (FAILED(_hr)) {                                                    \
            fprintf(stderr, "%s failed: %08lx (line %d)\n", #x, _hr, __LINE__); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

template <typename T>
struct alignas(void*) SubObj {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T inner;
};

struct MeshStream {
    SubObj<ID3D12RootSignature*> rs;
    SubObj<D3D12_SHADER_BYTECODE> ms;
    SubObj<D3D12_SHADER_BYTECODE> ps;
    SubObj<D3D12_RASTERIZER_DESC> rast;
    SubObj<D3D12_BLEND_DESC> blend;
    SubObj<D3D12_DEPTH_STENCIL_DESC> ds;
    SubObj<D3D12_RT_FORMAT_ARRAY> rtv;
    SubObj<DXGI_SAMPLE_DESC> sample;
    SubObj<UINT> mask;
};

constexpr UINT W = 256, H = 256, GX = 9, GY = 9;

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: harness <d3d12.dll> <gpu|warp> <direct|indirect|indirect3|multi|batch> <out.rgba>\n");
        return 2;
    }
    std::string mode = argv[3];
    bool multi = mode == "multi";          // direct + indirect + direct in one command list
    bool three = mode == "indirect3";      // ExecuteIndirect with 3 commands (one with zero groups)
    bool batch = mode == "batch";          // 3 ExecuteIndirect calls with root constant changes between, 3 frames
    bool indirect = multi || three || batch || mode == "indirect";
    int frames = batch ? 3 : 1;
    bool gameMs = getenv("HARNESS_MS_GAME") != nullptr;  // ms_game: 128 threads, thread t writes vertex t
    bool wide = getenv("HARNESS_WIDE") != nullptr;  // ps_wide: reads TEXCOORD1 (float3) instead of TEXCOORD0
    argv[3] = argv[4];
    HMODULE d3d = LoadLibraryA(argv[1]);
    if (!d3d) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    auto createDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d, "D3D12CreateDevice");
    auto serialize = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)GetProcAddress(d3d, "D3D12SerializeVersionedRootSignature");
    bool warp = std::string(argv[2]) == "warp";

    CComPtr<IDXGIFactory6> fac;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
    CComPtr<IDXGIAdapter1> adapter;
    if (warp) {
        CHECK(fac->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    } else {
        for (UINT i = 0;; i++) {
            CComPtr<IDXGIAdapter1> a;
            if (fac->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d;
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = a; break; }  // first hardware GPU
        }
        if (!adapter) { fprintf(stderr, "no hardware adapter\n"); return 1; }
    }

    if (getenv("HARNESS_DEBUG")) {
        auto getDebug = (PFN_D3D12_GET_DEBUG_INTERFACE)GetProcAddress(d3d, "D3D12GetDebugInterface");
        CComPtr<ID3D12Debug> dbg;
        if (getDebug && SUCCEEDED(getDebug(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    CComPtr<ID3D12Device2> dev;
    // HARNESS_FL122: ask for DX12 Ultimate like FF7 Rebirth does (needs the proxy on 12_1 hardware).
    bool fl122 = getenv("HARNESS_FL122") != nullptr;
    CHECK(createDevice(adapter, fl122 ? D3D_FEATURE_LEVEL_12_2 : D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    if (fl122) {
        D3D_FEATURE_LEVEL req[1] = {D3D_FEATURE_LEVEL_12_2};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {1, req, (D3D_FEATURE_LEVEL)0};
        HRESULT hr = dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof(fls));
        printf("FEATURE_LEVELS({12_2}) -> %08lx max %x\n", hr, fls.MaxSupportedFeatureLevel);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
    printf("adapter=%s MeshShaderTier=%d\n", argv[2], o7.MeshShaderTier);

    // Root signature: b0 root constants (mesh), DENY_VERTEX like UE's mesh root signatures.
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants = {0, 0, 5};
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd = {};
    rsd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rsd.Desc_1_1 = {1, &p, 0, nullptr,
                    D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
                        D3D12_ROOT_SIGNATURE_FLAG_DENY_AMPLIFICATION_SHADER_ROOT_ACCESS};
    CComPtr<ID3DBlob> rsBlob, err;
    CHECK(serialize(&rsd, &rsBlob, &err));
    CComPtr<ID3D12RootSignature> rs;
    CHECK(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rs)));

    MeshStream s = {};
    s.rs = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, rs};
    s.ms = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, {kMs, sizeof(kMs)}};
    if (gameMs) s.ms.inner = {kMsGame, sizeof(kMsGame)};
    s.ps = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, {kPs, sizeof(kPs)}};
    if (wide) s.ps.inner = {kPsWide, sizeof(kPsWide)};
    s.rast.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
    s.rast.inner = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE, FALSE, 0, 0.f, 0.f, TRUE, FALSE, FALSE, 0,
                    D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
    s.blend.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
    s.blend.inner.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    s.ds.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
    s.rtv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
    s.rtv.inner.NumRenderTargets = 1;
    s.rtv.inner.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    s.sample = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, {1, 0}};
    s.mask = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, 0xffffffff};
    D3D12_PIPELINE_STATE_STREAM_DESC sd = {sizeof(s), &s};
    CComPtr<ID3D12PipelineState> pso;
    CHECK(dev->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)));

    // Render target + readback.
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rb = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 1}};
    CComPtr<ID3D12Resource> rt, readback;
    CHECK(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)));
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, W * H * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    CHECK(dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CComPtr<ID3D12DescriptorHeap> rtvHeap;
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    CComPtr<ID3D12CommandQueue> q;
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    CComPtr<ID3D12CommandAllocator> alloc;
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    CComPtr<ID3D12GraphicsCommandList6> cl;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
    CComPtr<ID3D12Fence> fence;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    CComPtr<ID3D12Resource> argBuf, cntBuf;
    CComPtr<ID3D12CommandSignature> sig;
    if (indirect) {
        // DISPATCH_MESH-only signature, stride 12 (like the game). Commands: {GX,5,1}, {GX,GY,1}, {0,1,1};
        // batch mode: {GX,3,1}, {GX,4,1}, {GX,2,1} (strips of 3 + 4 + 2 = GY rows).
        UINT argsData[9] = {GX, 5, 1, GX, GY, 1, 0, 1, 1};
        UINT batchArgs[9] = {GX, 3, 1, GX, 4, 1, GX, 2, 1};
        UINT cntData[1] = {2};
        D3D12_HEAP_PROPERTIES up = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC ad = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&argBuf)));
        CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ad, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cntBuf)));
        void* p;
        argBuf->Map(0, nullptr, &p); memcpy(p, batch ? batchArgs : argsData, sizeof(argsData)); argBuf->Unmap(0, nullptr);
        cntBuf->Map(0, nullptr, &p); memcpy(p, cntData, sizeof(cntData)); cntBuf->Unmap(0, nullptr);
        D3D12_INDIRECT_ARGUMENT_DESC arg = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH};
        D3D12_COMMAND_SIGNATURE_DESC sd2 = {12, 1, &arg, 0};
        CHECK(dev->CreateCommandSignature(&sd2, nullptr, IID_PPV_ARGS(&sig)));
    }

    for (int frame = 0; frame < frames; frame++) {
    if (frame) {
        CHECK(alloc->Reset());
        CHECK(cl->Reset(alloc, nullptr));
        D3D12_RESOURCE_BARRIER back = {};
        back.Transition = {rt, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        cl->ResourceBarrier(1, &back);
    }
    float clear[4] = {0, 0, 0, 1};
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    D3D12_RECT sc = {0, 0, (LONG)W, (LONG)H};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(rs);
    cl->SetPipelineState(pso);
    struct { float xform[4]; UINT groupsX; } cb = {{1.8f / 63, -1.8f / 63, -0.9f, 0.9f}, GX};
    cl->SetGraphicsRoot32BitConstants(0, 5, &cb, 0);
    if (batch) {
        // Strip k covers group rows [r0, r0 + rows): offset the y transform so the strips tile the 9x9 grid.
        UINT rows[3] = {3, 4, 2}, r0 = 0;
        for (UINT k = 0; k < 3; k++) {
            cb.xform[3] = 0.9f - r0 * 7 * (1.8f / 63);
            cl->SetGraphicsRoot32BitConstants(0, 5, &cb, 0);
            cl->ExecuteIndirect(sig, 1, argBuf, 12 * k, nullptr, 0);
            r0 += rows[k];
        }
    } else if (indirect) {
        if (multi) cl->DispatchMesh(GX, 4, 1);
        if (three)
            cl->ExecuteIndirect(sig, 3, argBuf, 0, nullptr, 0);
        else
            cl->ExecuteIndirect(sig, 1, argBuf, 12, nullptr, 0);  // the game's form: one command, no count buffer
        if (multi) cl->DispatchMesh(GX, GY, 1);
    } else {
        cl->DispatchMesh(GX, GY, 1);
    }

    D3D12_RESOURCE_BARRIER b = {};
    b.Transition = {rt, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, W * 4}};
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(cl->Close());
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    CHECK(q->Signal(fence, frame + 1));
    CHECK(fence->SetEventOnCompletion(frame + 1, ev));
    WaitForSingleObject(ev, 10000);
    CHECK(dev->GetDeviceRemovedReason());
    }  // frame

    void* px = nullptr;
    CHECK(readback->Map(0, nullptr, &px));
    FILE* f = fopen(argv[3], "wb");
    fwrite(px, 1, W * H * 4, f);
    fclose(f);
    readback->Unmap(0, nullptr);
    UINT covered = 0;
    for (UINT i = 0; i < W * H; i++) covered += ((const UINT*)px)[i] != 0xff000000u;
    printf("covered pixels: %u / %u\n", covered, W * H);
    // HARNESS_LINGER_MS=<n>: stay alive so the proxy's background converter (started at device creation) can
    // run, as it does in the game; the next run then finds the conversion without an explicit converter call.
    if (const char* ms = getenv("HARNESS_LINGER_MS")) Sleep((DWORD)atoi(ms));
    return 0;
}

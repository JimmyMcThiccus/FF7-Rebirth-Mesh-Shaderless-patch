// meshemu d3d12.dll proxy: emulates mesh shader pipelines on GPUs without mesh shader support.
// ABI v7: the mesh shader runs as the vertex shader, the compute pass writes only indices (see docs/DESIGN.md).
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "nullvs.h"

// ---------------------------------------------------------------------------------------------
// Export forwarding
// ---------------------------------------------------------------------------------------------

extern "C" void* p_tbl[18];
extern "C" unsigned char X_DRED[256] = {};  // D3D12DeviceRemovedExtendedData is a data export

static const char* const kExports[18] = {
    MAKEINTRESOURCEA(99),
    "SetAppCompatStringPointer",
    "D3D12GetDebugInterface",
    "D3D12CoreCreateLayeredDevice",
    "D3D12CoreGetLayeredDeviceSize",
    "D3D12CoreRegisterLayers",
    "D3D12CreateRootSignatureDeserializer",
    "D3D12CreateVersionedRootSignatureDeserializer",
    "D3D12DeviceRemovedExtendedData",
    "D3D12EnableExperimentalFeatures",
    "D3D12GetInterface",
    "D3D12PIXEventsReplaceBlock",
    "D3D12PIXGetThreadInfo",
    "D3D12PIXNotifyWakeFromFenceSignal",
    "D3D12PIXReportCounter",
    "D3D12SerializeRootSignature",
    "D3D12SerializeVersionedRootSignature",
    "GetBehaviorValue",
};

static HMODULE g_real;
static PFN_D3D12_CREATE_DEVICE g_realCreateDevice;
static PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER g_realCreateRsDeserializer;
static PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE g_realSerializeRs;

// ---------------------------------------------------------------------------------------------
// Logging / paths
// ---------------------------------------------------------------------------------------------

static std::wstring g_dir;  // <dll dir>\meshemu\  .
static FILE* g_log;
static std::mutex g_logMu;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_logMu);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

// Log the first `limit` occurrences of a message keyed by a static counter.
#define LOG_N(limit, ...)                                  \
    do {                                                   \
        static std::atomic<int> _n{0};                     \
        if (_n.fetch_add(1) < (limit)) Log(__VA_ARGS__);   \
    } while (0)

static bool ReadFileBytes(const std::wstring& path, std::vector<char>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    size_t got = fread(out.data(), 1, n, f);
    fclose(f);
    return got == (size_t)n;
}

// Writes the file unless it already exists; returns true if it was created.
static bool WriteFileBytes(const std::wstring& path, const void* data, size_t n) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(data, 1, n, f);
    fclose(f);
    return true;
}

static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// DXIL containers carry a 16-byte digest at offset 4; use it as the shader's identity.
static std::string ShaderId(const D3D12_SHADER_BYTECODE& bc) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    const unsigned char* p = (const unsigned char*)bc.pShaderBytecode;
    if (!p || bc.BytecodeLength < 20) return "none";
    for (int i = 4; i < 20; i++) {
        s += hex[p[i] >> 4];
        s += hex[p[i] & 15];
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Vtable hooking. A slot can be hooked on several distinct vtables (e.g. debug-layer wrappers),
// so originals are looked up by the object's vtable.
// ---------------------------------------------------------------------------------------------

struct SlotTab {
    void** vt[8] = {};
    void* fn[8] = {};
    std::atomic<int> n{0};
};

static std::mutex g_hookMu;

static void* OrigFor(SlotTab& t, void* self) {
    void** vt = *(void***)self;
    int n = t.n.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (t.vt[i] == vt) return t.fn[i];
    return t.fn[0];
}

static void HookSlot(SlotTab& t, void* obj, int slot, void* hook) {
    void** vt = *(void***)obj;
    std::lock_guard<std::mutex> lk(g_hookMu);
    int n = t.n.load();
    for (int i = 0; i < n; i++)
        if (t.vt[i] == vt) return;
    if (n == 8 || vt[slot] == hook) return;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    t.vt[n] = vt;
    t.fn[n] = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    t.n.store(n + 1, std::memory_order_release);
}

#define ORIG(tab, self, Type) ((Type)OrigFor(tab, self))

// Slot indices (from the SDK headers, see vtidx.c).
enum DevSlot {
    kDevCreateCommandAllocator = 9,
    kDevCreateCommandList = 12,
    kDevCheckFeatureSupport = 13,
    kDevCreateRootSignature = 16,
    kDevCreateCommandSignature = 41,
    kDevCreatePipelineLibrary = 44,
    kDevCreatePipelineState = 47,
    kDevCreateCommandList1 = 51,
};
enum ClSlot {
    kClClose = 9,
    kClReset = 10,
    kClClearState = 11,
    kClDrawInstanced = 12,
    kClDispatch = 14,
    kClCopyBufferRegion = 15,
    kClEndQuery = 53,
    kClResolveQueryData = 54,
    kClDrawIndexedInstanced = 13,
    kClIASetPrimitiveTopology = 20,
    kClIASetIndexBuffer = 43,
    kClResourceBarrier = 26,
    kClSetPipelineState = 25,
    kClSetComputeRootSignature = 29,
    kClSetGraphicsRootSignature = 30,
    kClSetComputeRootDescriptorTable = 31,
    kClSetGraphicsRootDescriptorTable = 32,
    kClSetComputeRoot32BitConstant = 33,
    kClSetGraphicsRoot32BitConstant = 34,
    kClSetComputeRoot32BitConstants = 35,
    kClSetGraphicsRoot32BitConstants = 36,
    kClSetComputeRootCbv = 37,
    kClSetGraphicsRootCbv = 38,
    kClSetComputeRootSrv = 39,
    kClSetGraphicsRootSrv = 40,
    kClSetComputeRootUav = 41,
    kClSetGraphicsRootUav = 42,
    kClExecuteIndirect = 59,
    kClDispatchMesh = 79,
};
enum { kAllocReset = 8 };
enum { kLibStorePipeline = 8, kLibLoadPipeline = 13 };

static SlotTab t_devCreateCommandAllocator, t_devCreateCommandList, t_devCheckFeatureSupport,
    t_devCreateRootSignature, t_devCreateCommandSignature, t_devCreatePipelineLibrary, t_devCreatePipelineState,
    t_devCreateCommandList1;
static SlotTab t_clClose, t_clReset, t_clClearState, t_clSetPipelineState, t_clSetComputeRootSignature,
    t_clSetGraphicsRootSignature, t_clSetRoot[12], t_clExecuteIndirect, t_clDispatchMesh, t_clIASetPrimitiveTopology,
    t_clIASetIndexBuffer;
static SlotTab t_allocReset, t_libStorePipeline, t_libLoadPipeline;

// ---------------------------------------------------------------------------------------------
// Root signatures
// ---------------------------------------------------------------------------------------------

constexpr UINT kEmuSpace = 999;

struct RsInfo {
    UINT numParams = 0;   // as seen by the app
    int emuUav = -1;      // index of appended root UAV (u0, space 999)
    int emuArgs = -1;     // index of appended root SRV (t1, space 999): dispatch size {x, y, z} for the CS
    int emuSrv = -1;      // index of appended root SRV (t0, space 999): scratch as read by the VS
    // Same parameter layout with mesh/ALL params visible to compute and other stage-only params turned into
    // placeholders, so graphics root arguments can be replayed by index for the mesh-as-compute dispatch.
    ID3D12RootSignature* compute = nullptr;
};

static std::shared_mutex g_rsMu;
static std::unordered_map<ID3D12RootSignature*, RsInfo> g_rs;

static RsInfo* FindRs(ID3D12RootSignature* rs) {
    std::shared_lock<std::shared_mutex> lk(g_rsMu);
    auto it = g_rs.find(rs);
    return it == g_rs.end() ? nullptr : &it->second;
}

static UINT RootCost(const D3D12_ROOT_PARAMETER1& p) {
    switch (p.ParameterType) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: return 1;
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: return p.Constants.Num32BitValues;
    default: return 2;
    }
}

static D3D12_SHADER_VISIBILITY RemapVis(D3D12_SHADER_VISIBILITY v, bool& changed) {
    if (v == D3D12_SHADER_VISIBILITY_MESH) { changed = true; return D3D12_SHADER_VISIBILITY_VERTEX; }
    if (v == D3D12_SHADER_VISIBILITY_AMPLIFICATION) { changed = true; return D3D12_SHADER_VISIBILITY_HULL; }
    return v;
}

constexpr UINT kPlaceholderSpace = 10000;

static ID3DBlob* Serialize(const D3D12_ROOT_SIGNATURE_DESC1& d) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC nd = {};
    nd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    nd.Desc_1_1 = d;
    ID3DBlob* out = nullptr;
    ID3DBlob* err = nullptr;
    HRESULT hr = g_realSerializeRs(&nd, &out, &err);
    if (FAILED(hr)) {
        Log("root signature serialize failed %08x: %s", hr, err ? (const char*)err->GetBufferPointer() : "");
        if (err) err->Release();
        return nullptr;
    }
    return out;
}

// Returns rewritten serialized graphics and compute root signatures, or nullptr to use the original.
static ID3DBlob* RewriteRootSignature(const void* blob, SIZE_T len, RsInfo& info, ID3DBlob** computeBlob) {
    *computeBlob = nullptr;
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    if (FAILED(g_realCreateRsDeserializer(blob, len, IID_PPV_ARGS(&des)))) return nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    if (FAILED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd))) {
        des->Release();
        return nullptr;
    }
    const D3D12_ROOT_SIGNATURE_DESC1& d = vd->Desc_1_1;
    info.numParams = d.NumParameters;
    if (d.Flags & D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE) {
        des->Release();
        return nullptr;
    }

    bool mesh = false;
    std::vector<D3D12_ROOT_PARAMETER1> params(d.pParameters, d.pParameters + d.NumParameters);
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(d.pStaticSamplers, d.pStaticSamplers + d.NumStaticSamplers);

    // Compute variant (built from the original visibilities).
    std::vector<D3D12_ROOT_PARAMETER1> cparams(params);
    std::vector<D3D12_DESCRIPTOR_RANGE1> placeholderRanges(params.size());
    std::vector<D3D12_STATIC_SAMPLER_DESC> csamplers;
    for (UINT i = 0; i < cparams.size(); i++) {
        auto& p = cparams[i];
        if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL || p.ShaderVisibility == D3D12_SHADER_VISIBILITY_MESH) {
            p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            continue;
        }
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            auto& r = placeholderRanges[i];
            r = p.DescriptorTable.NumDescriptorRanges ? p.DescriptorTable.pDescriptorRanges[0] : D3D12_DESCRIPTOR_RANGE1{};
            r.NumDescriptors = 1;
            r.BaseShaderRegister = 0;
            r.RegisterSpace = kPlaceholderSpace + i;
            r.OffsetInDescriptorsFromTableStart = 0;
            p.DescriptorTable = {1, &r};
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            p.Constants.ShaderRegister = 0;
            p.Constants.RegisterSpace = kPlaceholderSpace + i;
            break;
        default:
            p.Descriptor.ShaderRegister = 0;
            p.Descriptor.RegisterSpace = kPlaceholderSpace + i;
        }
    }
    for (auto& s : samplers)
        if (s.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL || s.ShaderVisibility == D3D12_SHADER_VISIBILITY_MESH) {
            csamplers.push_back(s);
            csamplers.back().ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

    UINT cost = 0;
    for (auto& p : params) {
        p.ShaderVisibility = RemapVis(p.ShaderVisibility, mesh);
        cost += RootCost(p);
    }
    for (auto& s : samplers) s.ShaderVisibility = RemapVis(s.ShaderVisibility, mesh);
    if (!mesh) {
        des->Release();
        return nullptr;
    }

    D3D12_ROOT_SIGNATURE_FLAGS flags = d.Flags & ~(D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                                                   D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS);
    if (cost + 3 * 2 <= 64) {
        D3D12_ROOT_PARAMETER1 uav = {};
        uav.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        uav.Descriptor = {0, kEmuSpace, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        uav.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        info.emuUav = (int)params.size();
        params.push_back(uav);
        cparams.push_back(uav);
        D3D12_ROOT_PARAMETER1 srv = uav;
        srv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        info.emuSrv = (int)params.size();
        params.push_back(srv);
        cparams.push_back(srv);
        D3D12_ROOT_PARAMETER1 argsSrv = srv;
        argsSrv.Descriptor.ShaderRegister = 1;
        info.emuArgs = (int)params.size();
        params.push_back(argsSrv);
        cparams.push_back(argsSrv);
        D3D12_ROOT_SIGNATURE_FLAGS cflags = d.Flags & (D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                                        D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
        *computeBlob = Serialize({(UINT)cparams.size(), cparams.data(), (UINT)csamplers.size(), csamplers.data(), cflags});
    } else {
        Log("root signature too large for emulation params (%u dwords)", cost);
    }

    ID3DBlob* out = Serialize({(UINT)params.size(), params.data(), (UINT)samplers.size(), samplers.data(), flags});
    des->Release();
    return out;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateRootSignature(ID3D12Device* self, UINT nodeMask, const void* blob,
                                                        SIZE_T len, REFIID riid, void** out) {
    auto orig = ORIG(t_devCreateRootSignature, self, decltype(&Hk_CreateRootSignature));
    RsInfo info;
    ID3DBlob* cb = nullptr;
    ID3DBlob* nb = RewriteRootSignature(blob, len, info, &cb);
    HRESULT hr = nb ? orig(self, nodeMask, nb->GetBufferPointer(), nb->GetBufferSize(), riid, out)
                    : orig(self, nodeMask, blob, len, riid, out);
    if (cb) {
        HRESULT chr = orig(self, nodeMask, cb->GetBufferPointer(), cb->GetBufferSize(), IID_PPV_ARGS(&info.compute));
        if (FAILED(chr)) {
            Log("compute variant root signature failed %08x", chr);
            info.compute = nullptr;
        }
        cb->Release();
    }
    if (nb) {
        LOG_N(50, "rewrote mesh root signature (%u params, emu uav %d srv %d args %d, compute %p) -> %08x",
              info.numParams, info.emuUav, info.emuSrv, info.emuArgs, info.compute, hr);
        nb->Release();
    }
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12RootSignature* rs = nullptr;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&rs)))) {
            std::unique_lock<std::shared_mutex> lk(g_rsMu);
            g_rs[rs] = info;
            rs->Release();
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Pipeline state streams
// ---------------------------------------------------------------------------------------------

template <typename T>
struct alignas(void*) SubObj {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T inner;
};

static size_t SubObjSize(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) {
    switch (t) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return sizeof(SubObj<ID3D12RootSignature*>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return sizeof(SubObj<D3D12_SHADER_BYTECODE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return sizeof(SubObj<D3D12_STREAM_OUTPUT_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return sizeof(SubObj<D3D12_BLEND_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return sizeof(SubObj<UINT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return sizeof(SubObj<D3D12_RASTERIZER_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return sizeof(SubObj<D3D12_DEPTH_STENCIL_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return sizeof(SubObj<D3D12_INPUT_LAYOUT_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return sizeof(SubObj<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return sizeof(SubObj<D3D12_PRIMITIVE_TOPOLOGY_TYPE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return sizeof(SubObj<D3D12_RT_FORMAT_ARRAY>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return sizeof(SubObj<DXGI_FORMAT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return sizeof(SubObj<DXGI_SAMPLE_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return sizeof(SubObj<UINT>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return sizeof(SubObj<D3D12_CACHED_PIPELINE_STATE>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return sizeof(SubObj<D3D12_PIPELINE_STATE_FLAGS>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return sizeof(SubObj<D3D12_DEPTH_STENCIL_DESC1>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return sizeof(SubObj<D3D12_VIEW_INSTANCING_DESC>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return sizeof(SubObj<D3D12_DEPTH_STENCIL_DESC2>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return sizeof(SubObj<D3D12_RASTERIZER_DESC1>);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return sizeof(SubObj<D3D12_RASTERIZER_DESC2>);
    default: return 0;
    }
}

struct StreamView {
    struct Item {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
        const char* ptr;
        size_t size;
    };
    std::vector<Item> items;
    bool ok = true;

    explicit StreamView(const D3D12_PIPELINE_STATE_STREAM_DESC* d) {
        const char* p = (const char*)d->pPipelineStateSubobjectStream;
        size_t off = 0;
        while (off < d->SizeInBytes) {
            auto t = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(p + off);
            size_t sz = SubObjSize(t);
            if (!sz) {
                ok = false;
                return;
            }
            items.push_back({t, p + off, sz});
            off += sz;
        }
    }
    const Item* find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) const {
        for (auto& i : items)
            if (i.type == t) return &i;
        return nullptr;
    }
    const D3D12_SHADER_BYTECODE* shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t) const {
        auto i = find(t);
        if (!i) return nullptr;
        auto bc = &((const SubObj<D3D12_SHADER_BYTECODE>*)i->ptr)->inner;
        return bc->pShaderBytecode && bc->BytecodeLength ? bc : nullptr;
    }
};

struct StreamBuilder {
    std::vector<char> buf;
    void raw(const char* p, size_t n) { buf.insert(buf.end(), p, p + n); }
    template <typename T>
    void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t, const T& v) {
        SubObj<T> s = {};
        s.type = t;
        s.inner = v;
        raw((const char*)&s, sizeof(s));
    }
    D3D12_PIPELINE_STATE_STREAM_DESC desc() { return {buf.size(), buf.data()}; }
};

// ---------------------------------------------------------------------------------------------
// Emulated mesh pipelines
// ---------------------------------------------------------------------------------------------

static ID3D12Device* g_emu_dev();

// The app always gets a null pipeline (placeholder VS, no PS) for a mesh PSO; the emulation uses its own
// cs/gfx pipelines, built as soon as converted shaders exist in the cache (possibly long after creation).
struct EmuPso {
    std::atomic<bool> ready{false};     // cs + gfx built
    std::atomic<bool> building{false};
    std::atomic<ULONGLONG> nextTry{0};
    bool amplification = false;
    ID3D12PipelineState* cs = nullptr;  // converted mesh shader
    ID3D12PipelineState* gfx = nullptr; // generated VS + original PS
    ID3D12RootSignature* rs = nullptr;
    ID3D12RootSignature* computeRs = nullptr;  // compute variant of rs (see RsInfo)
    UINT maxPrims = 0;
    UINT maxGroups = 0;                 // groups that fit in scratch (see converter, ABI v2)
    UINT idxRegion = 0;                 // bytes of the index region at the start of scratch
    int emuUav = -1, emuSrv = -1, emuArgs = -1;  // root parameter indices in rs
    std::string ms, ps;
    std::vector<char> baseStream;       // app subobjects minus MS/AS/PS/VS/CACHED_PSO/topology
    std::vector<char> psBytes;
};

static std::shared_mutex g_psoMu;
static std::unordered_map<ID3D12PipelineState*, EmuPso*> g_emuPso;

static EmuPso* FindEmu(ID3D12PipelineState* pso) {
    if (!pso) return nullptr;
    std::shared_lock<std::shared_mutex> lk(g_psoMu);
    auto it = g_emuPso.find(pso);
    return it == g_emuPso.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------------------------
// Background shader conversion: meshemu\tools\convert.py dump -> cache whenever a new pipeline is dumped.
// ---------------------------------------------------------------------------------------------

static std::mutex g_convMu;
static bool g_convRunning = false, g_convPending = false;

static void RunConverter() {
    std::wstring tools = g_dir + L"tools\\";
    std::wstring script = tools + L"convert.py";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LOG_N(1, "no tools\\convert.py next to the proxy; automatic conversion disabled");
        return;
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE logf = CreateFileW((g_dir + L"convert.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring cmd = L"python -I \"" + script + L"\" \"" + g_dir + L"dump\" \"" + g_dir + L"cache\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = logf;
    PROCESS_INFORMATION pi = {};
    ULONGLONG t0 = GetTickCount64();
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                       nullptr, tools.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("converter finished (exit %lu, %llu ms)", code, (unsigned long long)(GetTickCount64() - t0));
    } else {
        Log("could not start python for conversion (%lu)", GetLastError());
    }
    if (logf != INVALID_HANDLE_VALUE) CloseHandle(logf);
}

static void RequestConvert() {
    std::lock_guard<std::mutex> lk(g_convMu);
    if (g_convRunning) {
        g_convPending = true;
        return;
    }
    g_convRunning = true;
    std::thread([] {
        for (;;) {
            RunConverter();
            std::lock_guard<std::mutex> lk(g_convMu);
            if (!g_convPending) {
                g_convRunning = false;
                return;
            }
            g_convPending = false;
        }
    }).detach();
}

static void DumpMeshPipeline(const StreamView& sv, const std::string& ms, const std::string& ps, const std::string& as) {
    std::wstring dump = g_dir + L"dump\\";
    auto save = [&](D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t, const std::string& id, const wchar_t* ext) {
        if (auto bc = sv.shader(t)) WriteFileBytes(dump + Widen(id) + ext, bc->pShaderBytecode, bc->BytecodeLength);
    };
    save(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, ms, L".ms.dxil");
    save(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps, L".ps.dxil");
    save(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, as, L".as.dxil");
    std::string pair = ms + "_" + ps + (as != "none" ? "_" + as : "") + ".pair";
    if (WriteFileBytes(dump + Widen(pair), "", 0)) RequestConvert();
}

// Build the emulation pipelines for e from the cache. Returns false (with reason) if not possible yet.
static bool BuildEmu(EmuPso* e, ID3D12Device2* dev, const char** why) {
    std::vector<char> csBytes, vsBytes, metaBytes;
    std::wstring cache = g_dir + L"cache\\";
    if (e->amplification) { *why = "amplification shader"; return false; }
    if (!ReadFileBytes(cache + Widen(e->ms + "_" + e->ps) + L".cs7.dxil", csBytes) ||
        !ReadFileBytes(cache + Widen(e->ms + "_" + e->ps) + L".vs7.dxil", vsBytes) ||
        !ReadFileBytes(cache + Widen(e->ms + "_" + e->ps) + L".meta7", metaBytes)) {
        *why = "not converted";
        return false;
    }
    RsInfo* ri = e->rs ? FindRs(e->rs) : nullptr;
    if (!ri || ri->emuUav < 0 || !ri->compute) { *why = "root signature has no emulation params"; return false; }
    unsigned mv = 0, mp = 0, rowBytes = 0, maxGroups = 0, idxRegion = 0;
    metaBytes.push_back(0);
    if (sscanf(metaBytes.data(), "%u %u %u %u %u", &mv, &mp, &rowBytes, &maxGroups, &idxRegion) != 5 || !maxGroups) {
        *why = "bad meta";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = ri->compute;
    cd.CS = {csBytes.data(), csBytes.size()};
    ID3D12PipelineState *cs = nullptr, *gfx = nullptr;
    HRESULT hr = dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&cs));
    if (FAILED(hr)) {
        Log("CreateComputePipelineState for ms=%s failed %08x", e->ms.c_str(), hr);
        *why = "compute PSO failed";
        return false;
    }
    StreamBuilder b;
    b.raw(e->baseStream.data(), e->baseStream.size());
    if (!e->psBytes.empty())
        b.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE{e->psBytes.data(), e->psBytes.size()});
    b.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{vsBytes.data(), vsBytes.size()});
    b.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    auto nd = b.desc();
    hr = ORIG(t_devCreatePipelineState, dev, HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*,
                                                                       REFIID, void**))(dev, &nd, IID_PPV_ARGS(&gfx));
    if (FAILED(hr)) {
        Log("emulated graphics PSO for ms=%s ps=%s failed %08x", e->ms.c_str(), e->ps.c_str(), hr);
        cs->Release();
        *why = "graphics PSO failed";
        return false;
    }
    e->maxPrims = mp;
    e->maxGroups = maxGroups;
    e->idxRegion = idxRegion;
    e->emuSrv = ri->emuSrv;
    e->emuUav = ri->emuUav;
    e->emuArgs = ri->emuArgs;
    e->computeRs = ri->compute;
    e->cs = cs;
    e->gfx = gfx;
    e->ready.store(true, std::memory_order_release);
    return true;
}

// Called at draw time for pipelines that are not ready: retry every few seconds (the background converter
// may have produced the shaders since).
static bool TryLateBuild(EmuPso* e) {
    ULONGLONG now = GetTickCount64();
    if (now < e->nextTry.load()) return false;
    bool expected = false;
    if (!e->building.compare_exchange_strong(expected, true)) return false;
    e->nextTry = now + 3000;
    bool ok = false;
    ID3D12Device2* d2 = nullptr;
    if (g_emu_dev() && SUCCEEDED(g_emu_dev()->QueryInterface(IID_PPV_ARGS(&d2)))) {
        const char* why = "";
        ok = BuildEmu(e, d2, &why);
        if (ok) Log("late-built emulation for ms=%s ps=%s", e->ms.c_str(), e->ps.c_str());
        d2->Release();
    }
    e->building = false;
    return ok;
}

static HRESULT STDMETHODCALLTYPE Hk_CreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                                                        REFIID riid, void** out) {
    auto orig = ORIG(t_devCreatePipelineState, self, decltype(&Hk_CreatePipelineState));
    StreamView sv(desc);
    if (!sv.ok) {
        LOG_N(20, "CreatePipelineState: unknown subobject type, passing through");
        return orig(self, desc, riid, out);
    }
    auto msBc = sv.shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS);
    if (!msBc) return orig(self, desc, riid, out);

    auto psBc = sv.shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS);
    auto asBc = sv.shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS);
    auto e = new EmuPso;
    e->ms = ShaderId(*msBc);
    e->ps = psBc ? ShaderId(*psBc) : "none";
    e->amplification = asBc != nullptr;
    std::string as = asBc ? ShaderId(*asBc) : "none";
    DumpMeshPipeline(sv, e->ms, e->ps, as);
    if (auto rsItem = sv.find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE)) {
        e->rs = ((const SubObj<ID3D12RootSignature*>*)rsItem->ptr)->inner;
        if (e->rs) e->rs->AddRef();  // kept for late builds
    }
    if (psBc) e->psBytes.assign((const char*)psBc->pShaderBytecode, (const char*)psBc->pShaderBytecode + psBc->BytecodeLength);

    // Null pipeline for the app; base stream kept for building the emulated one.
    StreamBuilder base;
    for (auto& it : sv.items) {
        switch (it.type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            break;
        default:
            base.raw(it.ptr, it.size);
        }
    }
    e->baseStream = base.buf;
    StreamBuilder b;
    b.raw(base.buf.data(), base.buf.size());
    b.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{kNullVs, sizeof(kNullVs)});
    b.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    auto nd = b.desc();
    HRESULT hr = orig(self, &nd, riid, out);

    const char* why = "";
    bool have = SUCCEEDED(hr) && BuildEmu(e, self, &why);
    LOG_N(400, "mesh PSO ms=%s ps=%s as=%s -> %s%s %08x", e->ms.c_str(), e->ps.c_str(), as.c_str(),
          have ? "emulated" : "null: ", why, hr);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&pso)))) {
            std::unique_lock<std::shared_mutex> lk(g_psoMu);
            g_emuPso[pso] = e;
            pso->Release();
        }
    } else {
        delete e;
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Pipeline libraries: never serve or store emulated mesh pipelines.
// ---------------------------------------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Hk_LibLoadPipeline(ID3D12PipelineLibrary1* self, LPCWSTR name,
                                                    const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    StreamView sv(desc);
    if (sv.ok && sv.shader(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS)) return E_INVALIDARG;  // forced cache miss
    return ORIG(t_libLoadPipeline, self, decltype(&Hk_LibLoadPipeline))(self, name, desc, riid, out);
}

static HRESULT STDMETHODCALLTYPE Hk_LibStorePipeline(ID3D12PipelineLibrary* self, LPCWSTR name, ID3D12PipelineState* pso) {
    if (FindEmu(pso)) return S_OK;
    return ORIG(t_libStorePipeline, self, decltype(&Hk_LibStorePipeline))(self, name, pso);
}

static HRESULT STDMETHODCALLTYPE Hk_CreatePipelineLibrary(ID3D12Device1* self, const void* blob, SIZE_T len, REFIID riid,
                                                          void** out) {
    HRESULT hr = ORIG(t_devCreatePipelineLibrary, self, decltype(&Hk_CreatePipelineLibrary))(self, blob, len, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12PipelineLibrary1* lib = nullptr;
        if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&lib)))) {
            HookSlot(t_libLoadPipeline, lib, kLibLoadPipeline, (void*)&Hk_LibLoadPipeline);
            HookSlot(t_libStorePipeline, lib, kLibStorePipeline, (void*)&Hk_LibStorePipeline);
            lib->Release();
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Command signatures
// ---------------------------------------------------------------------------------------------

static std::shared_mutex g_sigMu;
// Mesh command signatures -> argument stride; 0 = layout we cannot emulate (skip).
static std::unordered_map<ID3D12CommandSignature*, UINT> g_meshSigs;

static HRESULT STDMETHODCALLTYPE Hk_CreateCommandSignature(ID3D12Device* self, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                           ID3D12RootSignature* rs, REFIID riid, void** out) {
    HRESULT hr = ORIG(t_devCreateCommandSignature, self, decltype(&Hk_CreateCommandSignature))(self, desc, rs, riid, out);
    bool mesh = false;
    for (UINT i = 0; i < desc->NumArgumentDescs; i++)
        mesh |= desc->pArgumentDescs[i].Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    if (mesh) {
        LOG_N(50, "mesh command signature: %u args, stride %u -> %08x", desc->NumArgumentDescs, desc->ByteStride, hr);
        if (SUCCEEDED(hr) && out && *out) {
            ID3D12CommandSignature* sig = nullptr;
            if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&sig)))) {
                bool simple = desc->NumArgumentDescs == 1;
                std::unique_lock<std::shared_mutex> lk(g_sigMu);
                g_meshSigs[sig] = simple ? desc->ByteStride : 0;
                sig->Release();
            }
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Command lists
// ---------------------------------------------------------------------------------------------

struct RootArg {
    enum Kind : uint8_t { None, Table, Consts, Cbv, Srv, Uav } kind = None;
    UINT64 value = 0;            // GPU handle / GPU VA
    std::vector<UINT> consts;    // root constants
};

struct BindState {
    ID3D12RootSignature* rs = nullptr;
    std::vector<RootArg> args;
    void reset(ID3D12RootSignature* r) {
        rs = r;
        args.clear();
    }
    RootArg& at(UINT i) {
        if (args.size() <= i) args.resize(i + 1);
        return args[i];
    }
};

struct ClState {
    ID3D12PipelineState* pso = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool hasIb = false;
    D3D12_INDEX_BUFFER_VIEW ib = {};
    BindState gfx, cmp;
    // States of the emulation buffers in this command list (buffers decay to COMMON between submissions).
    D3D12_RESOURCE_STATES scratchState = D3D12_RESOURCE_STATE_COMMON;
    void reset(ID3D12PipelineState* p) {
        pso = p;
        topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        hasIb = false;
        gfx.reset(nullptr);
        cmp.reset(nullptr);
        scratchState = D3D12_RESOURCE_STATE_COMMON;
    }
};

static std::shared_mutex g_clMu;
static std::unordered_map<void*, ClState*> g_cl;

static ClState* Cl(void* list) {
    thread_local void* lastList = nullptr;
    thread_local ClState* lastState = nullptr;
    if (list == lastList) return lastState;
    ClState* s;
    {
        std::shared_lock<std::shared_mutex> lk(g_clMu);
        auto it = g_cl.find(list);
        s = it == g_cl.end() ? nullptr : it->second;
    }
    if (!s) {
        std::unique_lock<std::shared_mutex> lk(g_clMu);
        auto& slot = g_cl[list];
        if (!slot) slot = new ClState;
        s = slot;
    }
    lastList = list;
    lastState = s;
    return s;
}

using List = ID3D12GraphicsCommandList6;

static HRESULT STDMETHODCALLTYPE Hk_ClReset(List* self, ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso) {
    Cl(self)->reset(pso);
    return ORIG(t_clReset, self, decltype(&Hk_ClReset))(self, alloc, pso);
}

static void STDMETHODCALLTYPE Hk_ClClearState(List* self, ID3D12PipelineState* pso) {
    Cl(self)->reset(pso);
    ORIG(t_clClearState, self, decltype(&Hk_ClClearState))(self, pso);
}

static void STDMETHODCALLTYPE Hk_ClSetPipelineState(List* self, ID3D12PipelineState* pso) {
    Cl(self)->pso = pso;
    ORIG(t_clSetPipelineState, self, decltype(&Hk_ClSetPipelineState))(self, pso);
}

static void STDMETHODCALLTYPE Hk_ClSetComputeRootSignature(List* self, ID3D12RootSignature* rs) {
    Cl(self)->cmp.reset(rs);
    ORIG(t_clSetComputeRootSignature, self, decltype(&Hk_ClSetComputeRootSignature))(self, rs);
}

static void STDMETHODCALLTYPE Hk_ClSetGraphicsRootSignature(List* self, ID3D12RootSignature* rs) {
    Cl(self)->gfx.reset(rs);
    ORIG(t_clSetGraphicsRootSignature, self, decltype(&Hk_ClSetGraphicsRootSignature))(self, rs);
}

// Root argument setters: slot 31..42 alternate compute/graphics.
#define ROOT_SETTER(Name, slot, isGfx, Params, Record, Call)                                       \
    static void STDMETHODCALLTYPE Name Params {                                                    \
        BindState& bs = isGfx ? Cl(self)->gfx : Cl(self)->cmp;                                     \
        Record;                                                                                    \
        ((decltype(&Name))OrigFor(t_clSetRoot[slot - 31], self))Call;                              \
    }

#define TABLE_REC { auto& a = bs.at(i); a.kind = RootArg::Table; a.value = h.ptr; }
#define C1_REC { auto& a = bs.at(i); a.kind = RootArg::Consts; if (a.consts.size() <= off) a.consts.resize(off + 1); a.consts[off] = v; }
#define CN_REC { auto& a = bs.at(i); a.kind = RootArg::Consts; if (a.consts.size() < off + n) a.consts.resize(off + n); memcpy(a.consts.data() + off, p, n * 4); }
#define VA_REC(K) { auto& a = bs.at(i); a.kind = RootArg::K; a.value = va; }

ROOT_SETTER(Hk_CSetTable, 31, false, (List* self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h), TABLE_REC, (self, i, h))
ROOT_SETTER(Hk_GSetTable, 32, true, (List* self, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h), TABLE_REC, (self, i, h))
ROOT_SETTER(Hk_CSetC1, 33, false, (List* self, UINT i, UINT v, UINT off), C1_REC, (self, i, v, off))
ROOT_SETTER(Hk_GSetC1, 34, true, (List* self, UINT i, UINT v, UINT off), C1_REC, (self, i, v, off))
ROOT_SETTER(Hk_CSetCN, 35, false, (List* self, UINT i, UINT n, const void* p, UINT off), CN_REC, (self, i, n, p, off))
ROOT_SETTER(Hk_GSetCN, 36, true, (List* self, UINT i, UINT n, const void* p, UINT off), CN_REC, (self, i, n, p, off))
ROOT_SETTER(Hk_CSetCbv, 37, false, (List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va), VA_REC(Cbv), (self, i, va))
ROOT_SETTER(Hk_GSetCbv, 38, true, (List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va), VA_REC(Cbv), (self, i, va))
ROOT_SETTER(Hk_CSetSrv, 39, false, (List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va), VA_REC(Srv), (self, i, va))
ROOT_SETTER(Hk_GSetSrv, 40, true, (List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va), VA_REC(Srv), (self, i, va))
ROOT_SETTER(Hk_CSetUav, 41, false, (List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va), VA_REC(Uav), (self, i, va))
ROOT_SETTER(Hk_GSetUav, 42, true, (List* self, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va), VA_REC(Uav), (self, i, va))

static void STDMETHODCALLTYPE Hk_ClIASetPrimitiveTopology(List* self, D3D12_PRIMITIVE_TOPOLOGY t) {
    Cl(self)->topo = t;
    ORIG(t_clIASetPrimitiveTopology, self, decltype(&Hk_ClIASetPrimitiveTopology))(self, t);
}

static void STDMETHODCALLTYPE Hk_ClIASetIndexBuffer(List* self, const D3D12_INDEX_BUFFER_VIEW* v) {
    ClState* s = Cl(self);
    s->hasIb = v != nullptr;
    if (v) s->ib = *v;
    ORIG(t_clIASetIndexBuffer, self, decltype(&Hk_ClIASetIndexBuffer))(self, v);
}

static void STDMETHODCALLTYPE Hk_ClExecuteIndirect(List* self, ID3D12CommandSignature* sig, UINT maxCount,
                                                   ID3D12Resource* args, UINT64 argsOff, ID3D12Resource* count,
                                                   UINT64 countOff);

// ---------------------------------------------------------------------------------------------
// Emulation resources (one set per process; the game has a single device)
// ---------------------------------------------------------------------------------------------

constexpr UINT64 kScratchCap = 768ull << 20;  // usable scratch; groups beyond it are dropped
constexpr UINT64 kScratchTrash = 1ull << 20;  // writes of dropped groups land here
constexpr UINT64 kDrawArgs = kScratchCap;    // DrawIndexed arguments written by the converted CS (ABI v3)
constexpr UINT kUploadSlots = 4096;            // dispatch sizes for direct DispatchMesh calls (16 bytes each)
constexpr UINT kStatSlots = 1 << 15;           // draw-size samples (DrawIndexed args copied back)
constexpr UINT kProfQueries = 1 << 16;         // timestamp ring: 4 slots per emulated draw (3 used)
constexpr UINT kProfWindow = 8192;             // draws per profile report

struct EmuDev {
    std::mutex mu;
    bool tried = false, ok = false;
    ID3D12Device* dev = nullptr;
    ID3D12Resource* scratch = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS scratchVA = 0;
    ID3D12CommandSignature* dispatchSig = nullptr;     // {DISPATCH}, stride 12: the game's own arguments
    ID3D12CommandSignature* drawIndexedSig = nullptr;  // {DRAW_INDEXED}
    ID3D12Resource* upload = nullptr;                  // upload heap, persistently mapped
    UINT* uploadPtr = nullptr;
    std::atomic<UINT> uploadHead{0};
    ID3D12Resource* stats = nullptr;                   // readback heap
    const D3D12_DRAW_INDEXED_ARGUMENTS* statsPtr = nullptr;
    std::atomic<UINT> statsHead{0};
    UINT statsMaxPrims[kStatSlots] = {};
    UINT statsMaxGroups[kStatSlots] = {};
    // Profiling (meshemu\profile.txt): 3 timestamps per emulated draw: start, after cs, after draw.
    bool profile = false;
    ID3D12QueryHeap* qheap = nullptr;
    ID3D12Resource* qrb = nullptr;      // readback, 8 bytes per query
    const UINT64* qptr = nullptr;
    double ticksPerUs = 1000.0;
    std::atomic<UINT64> profDraws{0};
};
static EmuDev g_emu;
static ID3D12Device* g_emu_dev() { return g_emu.dev; }

static ID3D12Resource* MakeHeapBuffer(ID3D12Device* d, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {heap};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource* r = nullptr;
    HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) Log("CreateCommittedResource(heap %d, %llu bytes) failed %08x", heap, (unsigned long long)size, hr);
    return r;
}

static ID3D12Resource* MakeBuffer(ID3D12Device* d, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    ID3D12Resource* r = nullptr;
    HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&r));
    if (FAILED(hr)) Log("CreateCommittedResource(%llu MB) failed %08x", (unsigned long long)(size >> 20), hr);
    return r;
}

static bool EmuInit(List* list) {
    std::lock_guard<std::mutex> lk(g_emu.mu);
    if (g_emu.tried) return g_emu.ok;
    g_emu.tried = true;
    ID3D12Device* d = nullptr;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&d)))) return false;
    g_emu.dev = d;
    g_emu.scratch = MakeBuffer(d, kScratchCap + kScratchTrash);
    g_emu.upload = MakeHeapBuffer(d, D3D12_HEAP_TYPE_UPLOAD, kUploadSlots * 16, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_emu.stats = MakeHeapBuffer(d, D3D12_HEAP_TYPE_READBACK, kStatSlots * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
                                 D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_emu.scratch || !g_emu.upload || !g_emu.stats) return false;
    g_emu.scratch->SetName(L"meshemu scratch");
    g_emu.scratchVA = g_emu.scratch->GetGPUVirtualAddress();
    D3D12_RANGE none = {0, 0};
    g_emu.upload->Map(0, &none, (void**)&g_emu.uploadPtr);
    g_emu.stats->Map(0, nullptr, (void**)&g_emu.statsPtr);

    g_emu.profile = GetFileAttributesW((g_dir + L"profile.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_emu.profile) {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kProfQueries, 0};
        ID3D12CommandQueue* q = nullptr;
        D3D12_COMMAND_QUEUE_DESC cqd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
        UINT64 freq = 0;
        if (SUCCEEDED(d->CreateCommandQueue(&cqd, IID_PPV_ARGS(&q)))) {
            q->GetTimestampFrequency(&freq);
            q->Release();
        }
        g_emu.qrb = MakeHeapBuffer(d, D3D12_HEAP_TYPE_READBACK, kProfQueries * 8ull, D3D12_RESOURCE_STATE_COPY_DEST);
        if (freq && g_emu.qrb && SUCCEEDED(d->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_emu.qheap)))) {
            g_emu.qrb->Map(0, nullptr, (void**)&g_emu.qptr);
            g_emu.ticksPerUs = freq / 1e6;
            Log("profiling enabled (timestamp frequency %llu Hz)", (unsigned long long)freq);
        } else {
            g_emu.profile = false;
            Log("profiling requested but timestamp queries unavailable");
        }
    }

    HRESULT hr;
    D3D12_INDIRECT_ARGUMENT_DESC xa = {D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
    D3D12_COMMAND_SIGNATURE_DESC xsd = {12, 1, &xa, 0};
    if (FAILED(hr = d->CreateCommandSignature(&xsd, nullptr, IID_PPV_ARGS(&g_emu.dispatchSig)))) {
        Log("dispatch command signature failed %08x", hr);
        return false;
    }
    D3D12_INDIRECT_ARGUMENT_DESC da = {D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
    D3D12_COMMAND_SIGNATURE_DESC dsd = {sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &da, 0};
    if (FAILED(hr = d->CreateCommandSignature(&dsd, nullptr, IID_PPV_ARGS(&g_emu.drawIndexedSig)))) {
        Log("draw-indexed command signature failed %08x", hr);
        return false;
    }
    g_emu.ok = true;
    Log("emulation resources ready: scratch %llu MB", (unsigned long long)(kScratchCap >> 20));
    return true;
}

// Original (unhooked) command list calls.
#define CL_ORIG(tab, fn) ((decltype(&fn))OrigFor(tab, self))
static inline void* ClVt(List* self, int slot) { return (*(void***)self)[slot]; }

static void Barriers(List* self, UINT n, const D3D12_RESOURCE_BARRIER* b) {
    // ResourceBarrier is not hooked, so the vtable entry is the runtime's.
    using Fn = void(STDMETHODCALLTYPE*)(List*, UINT, const D3D12_RESOURCE_BARRIER*);
    ((Fn)ClVt(self, kClResourceBarrier))(self, n, b);
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return x;
}

static D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    x.UAV.pResource = r;
    return x;
}

static void SetPso(List* self, ID3D12PipelineState* p) { CL_ORIG(t_clSetPipelineState, Hk_ClSetPipelineState)(self, p); }

constexpr D3D12_RESOURCE_STATES kScratchRead = D3D12_RESOURCE_STATE_INDEX_BUFFER |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT |
    D3D12_RESOURCE_STATE_COPY_SOURCE;

// Move scratch to the given state (tracked per command list); a UAV barrier when it stays UAV.
static void ToStates(List* self, ClState* s, D3D12_RESOURCE_STATES scratch) {
    D3D12_RESOURCE_BARRIER b;
    if (s->scratchState != scratch) {
        b = Transition(g_emu.scratch, s->scratchState, scratch);
        s->scratchState = scratch;
    } else if (scratch == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        b = UavBarrier(g_emu.scratch);
    } else {
        return;
    }
    Barriers(self, 1, &b);
}

// Bind graphics root arguments as compute root arguments (same root signature).
static void BindAsCompute(List* self, ID3D12RootSignature* rs, const BindState& b) {
    CL_ORIG(t_clSetComputeRootSignature, Hk_ClSetComputeRootSignature)(self, rs);
    for (UINT i = 0; i < b.args.size(); i++) {
        const RootArg& a = b.args[i];
        switch (a.kind) {
        case RootArg::Table:
            ((decltype(&Hk_CSetTable))OrigFor(t_clSetRoot[0], self))(self, i, D3D12_GPU_DESCRIPTOR_HANDLE{a.value});
            break;
        case RootArg::Consts:
            ((decltype(&Hk_CSetCN))OrigFor(t_clSetRoot[4], self))(self, i, (UINT)a.consts.size(), a.consts.data(), 0);
            break;
        case RootArg::Cbv: ((decltype(&Hk_CSetCbv))OrigFor(t_clSetRoot[6], self))(self, i, a.value); break;
        case RootArg::Srv: ((decltype(&Hk_CSetSrv))OrigFor(t_clSetRoot[8], self))(self, i, a.value); break;
        case RootArg::Uav: ((decltype(&Hk_CSetUav))OrigFor(t_clSetRoot[10], self))(self, i, a.value); break;
        default: break;
        }
    }
}

static void RestoreAfterEmu(List* self, ClState* s) {
    SetPso(self, s->pso);
    if (s->hasIb) CL_ORIG(t_clIASetIndexBuffer, Hk_ClIASetIndexBuffer)(self, &s->ib);
    if (s->cmp.rs) BindAsCompute(self, s->cmp.rs, s->cmp);
    if (s->topo != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
        CL_ORIG(t_clIASetPrimitiveTopology, Hk_ClIASetPrimitiveTopology)(self, s->topo);
}

static std::atomic<uint64_t> g_skippedDispatchMesh{0}, g_skippedIndirect{0}, g_emulatedIndirect{0}, g_emulatedDirect{0};

static bool EmuReady(List* self, ClState* s, EmuPso* e) {
    if (!e || !EmuInit(self)) return false;
    if (!e->ready.load(std::memory_order_acquire) && !TryLateBuild(e)) return false;
    if (s->gfx.rs != e->rs) {
        LOG_N(20, "mesh draw with root signature %p but PSO expects %p; skipping", s->gfx.rs, e->rs);
        return false;
    }
    return true;
}

// --- GPU timestamp profiling ------------------------------------------------------------------------

static void ProfReport(UINT64 firstDraw) {
    // Draws [firstDraw, firstDraw + kProfWindow) were recorded at least kProfWindow draws ago: long done on the GPU.
    double cs = 0, draw = 0;
    UINT64 lo = ~0ull, hi = 0;
    UINT n = 0;
    for (UINT64 i = firstDraw; i < firstDraw + kProfWindow; i++) {
        const UINT64* q = g_emu.qptr + (i * 4) % kProfQueries;
        if (!q[0] || q[1] < q[0] || q[2] < q[1]) continue;
        cs += double(q[1] - q[0]);
        draw += double(q[2] - q[1]);
        if (q[0] < lo) lo = q[0];
        if (q[2] > hi) hi = q[2];
        n++;
    }
    if (!n || hi <= lo) return;
    double span = double(hi - lo) / g_emu.ticksPerUs / 1000.0, csMs = cs / g_emu.ticksPerUs / 1000.0,
           drawMs = draw / g_emu.ticksPerUs / 1000.0;
    Log("profile: %u draws over %.1f ms of GPU time: emulation %.1f ms (%.0f%%) = cs %.1f ms + draw %.1f ms; "
        "per draw cs %.1f us, draw %.1f us",
        n, span, csMs + drawMs, 100.0 * (csMs + drawMs) / span, csMs, drawMs, 1000.0 * csMs / n, 1000.0 * drawMs / n);
}

static UINT64 ProfBegin(List* self) {
    if (!g_emu.profile) return ~0ull;
    UINT64 d = g_emu.profDraws.fetch_add(1);
    UINT slot = (UINT)((d * 4) % kProfQueries);
    UINT64* q = (UINT64*)g_emu.qptr + slot;
    q[0] = q[1] = q[2] = 0;  // invalid until the GPU resolves them
    using EndQueryFn = void(STDMETHODCALLTYPE*)(List*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
    ((EndQueryFn)ClVt(self, kClEndQuery))(self, g_emu.qheap, D3D12_QUERY_TYPE_TIMESTAMP, slot);
    if (d % kProfWindow == 0 && d >= 2 * kProfWindow) ProfReport(d - 2 * kProfWindow);
    return slot;
}

static void ProfMark(List* self, UINT64 slot, UINT i) {
    if (slot == ~0ull) return;
    using EndQueryFn = void(STDMETHODCALLTYPE*)(List*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
    ((EndQueryFn)ClVt(self, kClEndQuery))(self, g_emu.qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)slot + i);
    if (i == 2) {
        using ResolveFn = void(STDMETHODCALLTYPE*)(List*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT, ID3D12Resource*, UINT64);
        ((ResolveFn)ClVt(self, kClResolveQueryData))(self, g_emu.qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)slot, 3,
                                                     g_emu.qrb, slot * 8);
    }
}

// Copy this draw's DrawIndexed arguments (scratch must be readable) to the stats ring; logged periodically.
static void SampleDrawSize(List* self, EmuPso* e) {
    if (!g_emu.profile) return;
    UINT slot = g_emu.statsHead.fetch_add(1) % kStatSlots;
    g_emu.statsMaxPrims[slot] = e->maxPrims;
    g_emu.statsMaxGroups[slot] = e->maxGroups;
    using CopyFn = void(STDMETHODCALLTYPE*)(List*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
    ((CopyFn)ClVt(self, kClCopyBufferRegion))(self, g_emu.stats, slot * sizeof(D3D12_DRAW_INDEXED_ARGUMENTS),
                                              g_emu.scratch, kDrawArgs, sizeof(D3D12_DRAW_INDEXED_ARGUMENTS));
    if (slot != kStatSlots - 1) return;
    // Whole ring written once: summarize (entries are a frame or two stale at most).
    UINT n = 0, maxG = 0, full = 0;
    double sumG = 0, sumFrac = 0;
    for (UINT i = 0; i < kStatSlots; i++) {
        UINT mp = g_emu.statsMaxPrims[i];
        if (!mp) continue;
        UINT groups = g_emu.statsPtr[i].IndexCountPerInstance / (mp * 3);
        n++;
        sumG += groups;
        sumFrac += (double)groups / g_emu.statsMaxGroups[i];
        if (groups > maxG) maxG = groups;
        if (groups >= g_emu.statsMaxGroups[i]) full++;
    }
    if (n)
        Log("draw sizes over last %u emulated draws: avg %.0f groups, max %u, avg %.2f%% of capacity, %u hit the cap",
            n, sumG / n, maxG, 100.0 * sumFrac / n, full);
}

static void STDMETHODCALLTYPE Hk_ClDispatchMesh(List* self, UINT x, UINT y, UINT z) {
    ClState* s = Cl(self);
    EmuPso* e = FindEmu(s->pso);
    if (!EmuReady(self, s, e)) {
        if ((g_skippedDispatchMesh.fetch_add(1) & 0xffff) == 0)
            Log("DispatchMesh skipped (%s pipeline), total %llu", e ? "null" : "unknown",
                (unsigned long long)g_skippedDispatchMesh.load());
        return;
    }
    UINT64 groups = (UINT64)x * y * z;
    if (!groups) return;
    if (groups > e->maxGroups) LOG_N(20, "DispatchMesh %ux%ux%u exceeds scratch, clamped", x, y, z);
    g_emulatedDirect.fetch_add(1);
    UINT slot = g_emu.uploadHead.fetch_add(1) % kUploadSlots;
    UINT* dims = g_emu.uploadPtr + slot * 4;
    dims[0] = x;
    dims[1] = y;
    dims[2] = z;

    UINT64 prof = ProfBegin(self);
    ToStates(self, s, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SetPso(self, e->cs);
    BindAsCompute(self, e->computeRs, s->gfx);
    ((decltype(&Hk_CSetUav))OrigFor(t_clSetRoot[10], self))(self, e->emuUav, g_emu.scratchVA);
    ((decltype(&Hk_CSetSrv))OrigFor(t_clSetRoot[8], self))(self, e->emuArgs, g_emu.upload->GetGPUVirtualAddress() + slot * 16);
    using DispatchFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT);
    ((DispatchFn)ClVt(self, kClDispatch))(self, x, y, z);
    ProfMark(self, prof, 1);

    ToStates(self, s, kScratchRead);
    SetPso(self, e->gfx);
    CL_ORIG(t_clIASetPrimitiveTopology, Hk_ClIASetPrimitiveTopology)(self, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW ibv = {g_emu.scratchVA, e->idxRegion, DXGI_FORMAT_R32_UINT};
    CL_ORIG(t_clIASetIndexBuffer, Hk_ClIASetIndexBuffer)(self, &ibv);
    ((decltype(&Hk_GSetSrv))OrigFor(t_clSetRoot[9], self))(self, e->emuSrv, g_emu.scratchVA);
    // ABI v7: a mesh shader run as the VS reads the dispatch size to split SV_VertexID into group coordinates.
    ((decltype(&Hk_GSetSrv))OrigFor(t_clSetRoot[9], self))(self, e->emuArgs, g_emu.upload->GetGPUVirtualAddress() + slot * 16);
    UINT draw = (UINT)((groups < e->maxGroups ? groups : e->maxGroups) * e->maxPrims * 3);
    using DrawIdxFn = void(STDMETHODCALLTYPE*)(List*, UINT, UINT, UINT, INT, UINT);
    ((DrawIdxFn)ClVt(self, kClDrawIndexedInstanced))(self, draw, 1, 0, 0, 0);
    ProfMark(self, prof, 2);
    RestoreAfterEmu(self, s);
}

// ExecuteIndirect(DISPATCH_MESH) without a count buffer: each command is
//   [scratch -> UAV]  cs via ExecuteIndirect({DISPATCH}) on the game's own argument record
//   [scratch -> read] ExecuteIndirect(DRAW_INDEXED) from the arguments the cs wrote; the game's record is the
//                     count buffer, so a command with x == 0 (no groups, nothing written) draws nothing.
static void EmulateIndirect(List* self, ClState* s, EmuPso* e, UINT argStride, UINT maxCount, ID3D12Resource* args,
                            UINT64 argsOff) {
    auto ei = ORIG(t_clExecuteIndirect, self, decltype(&Hk_ClExecuteIndirect));
    D3D12_GPU_VIRTUAL_ADDRESS argsVA = args->GetGPUVirtualAddress() + argsOff;
    for (UINT k = 0; k < maxCount; k++) {
        g_emulatedIndirect.fetch_add(1);
        UINT64 off = argsOff + (UINT64)k * argStride;
        UINT64 prof = ProfBegin(self);
        ToStates(self, s, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SetPso(self, e->cs);
        BindAsCompute(self, e->computeRs, s->gfx);
        ((decltype(&Hk_CSetUav))OrigFor(t_clSetRoot[10], self))(self, e->emuUav, g_emu.scratchVA);
        ((decltype(&Hk_CSetSrv))OrigFor(t_clSetRoot[8], self))(self, e->emuArgs, argsVA + (UINT64)k * argStride);
        ei(self, g_emu.dispatchSig, 1, args, off, nullptr, 0);
        ProfMark(self, prof, 1);

        ToStates(self, s, kScratchRead);
        SampleDrawSize(self, e);
        SetPso(self, e->gfx);
        CL_ORIG(t_clIASetPrimitiveTopology, Hk_ClIASetPrimitiveTopology)(self, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW ibv = {g_emu.scratchVA, e->idxRegion, DXGI_FORMAT_R32_UINT};
        CL_ORIG(t_clIASetIndexBuffer, Hk_ClIASetIndexBuffer)(self, &ibv);
        ((decltype(&Hk_GSetSrv))OrigFor(t_clSetRoot[9], self))(self, e->emuSrv, g_emu.scratchVA);
        ((decltype(&Hk_GSetSrv))OrigFor(t_clSetRoot[9], self))(self, e->emuArgs, argsVA + (UINT64)k * argStride);
        ei(self, g_emu.drawIndexedSig, 1, g_emu.scratch, kDrawArgs, args, off);
        ProfMark(self, prof, 2);
    }
    RestoreAfterEmu(self, s);
}

static void STDMETHODCALLTYPE Hk_ClExecuteIndirect(List* self, ID3D12CommandSignature* sig, UINT maxCount,
                                                   ID3D12Resource* args, UINT64 argsOff, ID3D12Resource* count,
                                                   UINT64 countOff) {
    bool mesh = false;
    UINT stride = 0;
    {
        std::shared_lock<std::shared_mutex> lk(g_sigMu);
        auto it = g_meshSigs.find(sig);
        if (it != g_meshSigs.end()) {
            mesh = true;
            stride = it->second;
        }
    }
    if (!mesh) {
        ORIG(t_clExecuteIndirect, self, decltype(&Hk_ClExecuteIndirect))(self, sig, maxCount, args, argsOff, count, countOff);
        return;
    }
    ClState* s = Cl(self);
    EmuPso* e = FindEmu(s->pso);
    if (count) LOG_N(10, "ExecuteIndirect(DispatchMesh) with a count buffer is not supported; skipped");
    if (stride && maxCount && maxCount <= 64 && !count && EmuReady(self, s, e)) {
        LOG_N(30, "ExecuteIndirect(DispatchMesh) emulated: ms=%s maxCount=%u", e->ms.c_str(), maxCount);
        EmulateIndirect(self, s, e, stride, maxCount, args, argsOff);
        if ((g_emulatedIndirect.load() & 0x3fff) == 0)
            Log("emulated ExecuteIndirect: %llu, skipped: %llu", (unsigned long long)g_emulatedIndirect.load(),
                (unsigned long long)g_skippedIndirect.load());
        return;
    }
    if ((g_skippedIndirect.fetch_add(1) & 0xfff) == 0)
        Log("ExecuteIndirect(DispatchMesh) skipped (%s), total %llu", !e ? "unknown pipeline" : !e->ready ? "null pipeline" : "other",
            (unsigned long long)g_skippedIndirect.load());
}

static void HookCommandList(void* obj) {
    ID3D12GraphicsCommandList6* l = nullptr;
    if (FAILED(((IUnknown*)obj)->QueryInterface(IID_PPV_ARGS(&l)))) return;
    HookSlot(t_clReset, l, kClReset, (void*)&Hk_ClReset);
    HookSlot(t_clClearState, l, kClClearState, (void*)&Hk_ClClearState);
    HookSlot(t_clSetPipelineState, l, kClSetPipelineState, (void*)&Hk_ClSetPipelineState);
    HookSlot(t_clSetComputeRootSignature, l, kClSetComputeRootSignature, (void*)&Hk_ClSetComputeRootSignature);
    HookSlot(t_clSetGraphicsRootSignature, l, kClSetGraphicsRootSignature, (void*)&Hk_ClSetGraphicsRootSignature);
    void* setters[12] = {(void*)&Hk_CSetTable, (void*)&Hk_GSetTable, (void*)&Hk_CSetC1, (void*)&Hk_GSetC1,
                         (void*)&Hk_CSetCN,    (void*)&Hk_GSetCN,    (void*)&Hk_CSetCbv, (void*)&Hk_GSetCbv,
                         (void*)&Hk_CSetSrv,   (void*)&Hk_GSetSrv,   (void*)&Hk_CSetUav, (void*)&Hk_GSetUav};
    for (int i = 0; i < 12; i++) HookSlot(t_clSetRoot[i], l, 31 + i, setters[i]);
    HookSlot(t_clIASetPrimitiveTopology, l, kClIASetPrimitiveTopology, (void*)&Hk_ClIASetPrimitiveTopology);
    HookSlot(t_clIASetIndexBuffer, l, kClIASetIndexBuffer, (void*)&Hk_ClIASetIndexBuffer);
    HookSlot(t_clExecuteIndirect, l, kClExecuteIndirect, (void*)&Hk_ClExecuteIndirect);
    HookSlot(t_clDispatchMesh, l, kClDispatchMesh, (void*)&Hk_ClDispatchMesh);
    l->Release();
}

static HRESULT STDMETHODCALLTYPE Hk_CreateCommandList(ID3D12Device* self, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                                      ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso,
                                                      REFIID riid, void** out) {
    HRESULT hr = ORIG(t_devCreateCommandList, self, decltype(&Hk_CreateCommandList))(self, node, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_COPY) {
        HookCommandList(*out);
        Cl(*out)->reset(pso);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hk_CreateCommandList1(ID3D12Device4* self, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                                       D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out) {
    HRESULT hr = ORIG(t_devCreateCommandList1, self, decltype(&Hk_CreateCommandList1))(self, node, type, flags, riid, out);
    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_COPY) HookCommandList(*out);
    return hr;
}

// Set once a feature level 12_2 device request had to be served at 12_1 (no DX12 Ultimate hardware).
static std::atomic<bool> g_spoof122{false};

// Report what the game needs to pick its mesh shader path: mesh shader tier 1 (emulated) and, on 12_1
// hardware, feature level 12_2. This replaces patching ff7rebirth_.exe.
static HRESULT STDMETHODCALLTYPE Hk_CheckFeatureSupport(ID3D12Device* self, D3D12_FEATURE f, void* data, UINT size) {
    HRESULT hr = ORIG(t_devCheckFeatureSupport, self, decltype(&Hk_CheckFeatureSupport))(self, f, data, size);
    if (f == D3D12_FEATURE_D3D12_OPTIONS7 && SUCCEEDED(hr) && size >= sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7)) {
        auto o7 = (D3D12_FEATURE_DATA_D3D12_OPTIONS7*)data;
        if (o7->MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            o7->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
            LOG_N(5, "CheckFeatureSupport(OPTIONS7): reporting MeshShaderTier 1 (emulated)");
        }
    }
    if (f == D3D12_FEATURE_FEATURE_LEVELS && g_spoof122 && size >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) {
        auto fl = (D3D12_FEATURE_DATA_FEATURE_LEVELS*)data;
        D3D_FEATURE_LEVEL best = SUCCEEDED(hr) ? fl->MaxSupportedFeatureLevel : (D3D_FEATURE_LEVEL)0;
        for (UINT i = 0; i < fl->NumFeatureLevels; i++)
            if (fl->pFeatureLevelsRequested[i] == D3D_FEATURE_LEVEL_12_2 && best < D3D_FEATURE_LEVEL_12_2)
                best = D3D_FEATURE_LEVEL_12_2;
        if (best == D3D_FEATURE_LEVEL_12_2) {
            fl->MaxSupportedFeatureLevel = best;
            LOG_N(5, "CheckFeatureSupport(FEATURE_LEVELS): reporting 12_2 (was %08x)", hr);
            hr = S_OK;
        }
    }
    return hr;
}

static void HookDevice(void* obj) {
    ID3D12Device4* d = nullptr;
    if (FAILED(((IUnknown*)obj)->QueryInterface(IID_PPV_ARGS(&d)))) {
        Log("device does not implement ID3D12Device4; not hooking");
        return;
    }
    HookSlot(t_devCreateCommandList, d, kDevCreateCommandList, (void*)&Hk_CreateCommandList);
    HookSlot(t_devCreateCommandList1, d, kDevCreateCommandList1, (void*)&Hk_CreateCommandList1);
    HookSlot(t_devCheckFeatureSupport, d, kDevCheckFeatureSupport, (void*)&Hk_CheckFeatureSupport);
    HookSlot(t_devCreateRootSignature, d, kDevCreateRootSignature, (void*)&Hk_CreateRootSignature);
    HookSlot(t_devCreateCommandSignature, d, kDevCreateCommandSignature, (void*)&Hk_CreateCommandSignature);
    HookSlot(t_devCreatePipelineLibrary, d, kDevCreatePipelineLibrary, (void*)&Hk_CreatePipelineLibrary);
    HookSlot(t_devCreatePipelineState, d, kDevCreatePipelineState, (void*)&Hk_CreatePipelineState);
    d->Release();
}

extern "C" HRESULT WINAPI Proxy_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** out) {
    HRESULT hr = g_realCreateDevice(adapter, fl, riid, out);
    if (FAILED(hr) && fl == D3D_FEATURE_LEVEL_12_2) {
        // DX12 Ultimate requested on 12_1 hardware: the only 12_2 feature the game cannot do without is
        // mesh shaders, which we emulate.
        HRESULT hr121 = g_realCreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, riid, out);
        LOG_N(20, "D3D12CreateDevice(12_2) failed %08x, retried at 12_1 -> %08x", hr, hr121);
        if (SUCCEEDED(hr121)) {
            g_spoof122 = true;
            hr = hr121;
        }
    }
    LOG_N(20, "D3D12CreateDevice(fl=%x, out=%p) -> %08x", fl, out, hr);
    if (SUCCEEDED(hr) && out && *out) {
        HookDevice(*out);
        static std::once_flag once;
        std::call_once(once, RequestConvert);  // convert anything dumped by earlier sessions
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------------------------

static void Init(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"meshemu\\";
    g_dir = dir;
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"dump").c_str(), nullptr);
    CreateDirectoryW((dir + L"cache").c_str(), nullptr);
    g_log = _wfsopen((dir + L"meshemu.log").c_str(), L"w", _SH_DENYWR);

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    g_real = LoadLibraryW((std::wstring(sys) + L"\\d3d12.dll").c_str());
    if (!g_real) {
        Log("failed to load system d3d12.dll (%lu)", GetLastError());
        return;
    }
    for (int i = 0; i < 18; i++) p_tbl[i] = (void*)GetProcAddress(g_real, kExports[i]);
    g_realCreateDevice = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(g_real, "D3D12CreateDevice");
    g_realCreateRsDeserializer = (PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER)p_tbl[7];
    g_realSerializeRs = (PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE)p_tbl[16];
    Log("meshemu loaded, real d3d12 at %p", g_real);
}

BOOL WINAPI DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        Init(mod);
    }
    return TRUE;
}

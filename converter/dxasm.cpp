// dxasm: assemble DXIL LLVM text (.ll) into a validated, signed DXIL container.
//   dxasm <in.ll> <out.dxil>
// Uses dxcompiler.dll (IDxcAssembler) and dxil.dll (IDxcValidator) found next to the exe.
#include <windows.h>
#include <atlbase.h>
#include <dxcapi.h>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::wstring exeDir() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

static void printErrors(IDxcOperationResult* r) {
    CComPtr<IDxcBlobEncoding> err;
    if (SUCCEEDED(r->GetErrorBuffer(&err)) && err && err->GetBufferSize() > 1)
        fprintf(stderr, "%.*s\n", (int)err->GetBufferSize(), (const char*)err->GetBufferPointer());
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: dxasm <in.ll> <out.dxil>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    std::string text((std::istreambuf_iterator<char>(in)), {});

    HMODULE comp = LoadLibraryW((exeDir() + L"dxcompiler.dll").c_str());
    HMODULE dxil = LoadLibraryW((exeDir() + L"dxil.dll").c_str());
    if (!comp || !dxil) { fprintf(stderr, "dxcompiler.dll / dxil.dll not found next to dxasm.exe\n"); return 1; }
    auto compCreate = (DxcCreateInstanceProc)GetProcAddress(comp, "DxcCreateInstance");
    auto dxilCreate = (DxcCreateInstanceProc)GetProcAddress(dxil, "DxcCreateInstance");

    CComPtr<IDxcLibrary> lib;
    CComPtr<IDxcAssembler> as;
    CComPtr<IDxcValidator> val;
    if (FAILED(compCreate(CLSID_DxcLibrary, IID_PPV_ARGS(&lib))) ||
        FAILED(compCreate(CLSID_DxcAssembler, IID_PPV_ARGS(&as))) ||
        FAILED(dxilCreate(CLSID_DxcValidator, IID_PPV_ARGS(&val)))) {
        fprintf(stderr, "DxcCreateInstance failed\n");
        return 1;
    }

    CComPtr<IDxcBlobEncoding> src;
    lib->CreateBlobWithEncodingFromPinned(text.data(), (UINT32)text.size(), CP_UTF8, &src);
    CComPtr<IDxcOperationResult> ar;
    HRESULT st = E_FAIL;
    if (FAILED(as->AssembleToContainer(src, &ar)) || FAILED(ar->GetStatus(&st)) || FAILED(st)) {
        fprintf(stderr, "assembly failed\n");
        if (ar) printErrors(ar);
        return 1;
    }
    CComPtr<IDxcBlob> container;
    ar->GetResult(&container);

    // Validate and sign in place.
    CComPtr<IDxcOperationResult> vr;
    if (FAILED(val->Validate(container, DxcValidatorFlags_InPlaceEdit, &vr)) || FAILED(vr->GetStatus(&st)) || FAILED(st)) {
        fprintf(stderr, "validation failed\n");
        if (vr) printErrors(vr);
        return 1;
    }

    std::ofstream out(argv[2], std::ios::binary);
    out.write((const char*)container->GetBufferPointer(), container->GetBufferSize());
    printf("ok %zu bytes\n", (size_t)container->GetBufferSize());
    return 0;
}

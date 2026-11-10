/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "skeuomorph/CurlD3D.h"
#include "skeuomorph/CurlLayer.h"

// dcomp.dll is loaded by hand: Windows 7 has none, and then Curls don't play
using DCompCreateFn = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);

constexpr UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
constexpr UINT kBufferCount = 2;

struct CurlLayer {
    ComPtr<ID3D11Device> dev;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain2> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    HANDLE frameWait = nullptr;
    Size size;
    bool shown = false;
};

static DCompCreateFn LoadDComp() {
    static DCompCreateFn fn = [] {
        HMODULE dll = LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dll ? (DCompCreateFn)GetProcAddress(dll, "DCompositionCreateDevice") : nullptr;
    }();
    return fn;
}

// a swap chain can't be 0 x 0 (minimized window)
static Size AtLeastOne(Size s) {
    return {std::max(s.dx, 1), std::max(s.dy, 1)};
}

static bool MakeViews(CurlLayer* l) {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(l->swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        return false;
    }
    if (FAILED(l->dev->CreateRenderTargetView(back.Get(), nullptr, &l->rtv))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = (UINT)l->size.dx;
    dd.Height = (UINT)l->size.dy;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    if (FAILED(l->dev->CreateTexture2D(&dd, nullptr, &depth))) {
        return false;
    }
    return SUCCEEDED(l->dev->CreateDepthStencilView(depth.Get(), nullptr, &l->dsv));
}

static bool MakeSwapChain(CurlLayer* l, IDXGIDevice* dxgiDev, const char** why) {
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        *why = "no DXGI 1.2 factory";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)l->size.dx;
    sd.Height = (UINT)l->size.dy;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBufferCount;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = kSwapFlags;
    ComPtr<IDXGISwapChain1> swap1;
    if (FAILED(factory->CreateSwapChainForComposition(l->dev.Get(), &sd, nullptr, &swap1))) {
        *why = "CreateSwapChainForComposition failed";
        return false;
    }
    if (FAILED(swap1.As(&l->swap))) {
        *why = "no IDXGISwapChain2 (needs Windows 8.1)";
        return false;
    }
    l->swap->SetMaximumFrameLatency(1);
    l->frameWait = l->swap->GetFrameLatencyWaitableObject();
    if (!l->frameWait) {
        *why = "no frame latency handle";
        return false;
    }
    if (!MakeViews(l)) {
        *why = "swap chain views failed";
        return false;
    }
    return true;
}

// nullptr when DirectComposition is missing or fails; why says what failed
CurlLayer* NewCurlLayer(ID3D11Device* dev, HWND hwnd, Size size, const char** why) {
    *why = nullptr;
    DCompCreateFn create = LoadDComp();
    if (!create) {
        *why = "dcomp.dll not loaded";
        return nullptr;
    }

    auto l = new CurlLayer();
    l->dev = dev;
    l->size = AtLeastOne(size);
    ComPtr<IDXGIDevice> dxgiDev;
    bool ok = SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)));
    if (!ok) {
        *why = "no IDXGIDevice";
    }
    if (ok && FAILED(create(dxgiDev.Get(), IID_PPV_ARGS(&l->dcomp)))) {
        ok = false;
        *why = "DCompositionCreateDevice failed";
    }
    if (ok && !MakeSwapChain(l, dxgiDev.Get(), why)) {
        ok = false;
    }

    // topmost = FALSE: form-field and annotation edit boxes stay on top
    if (ok && FAILED(l->dcomp->CreateTargetForHwnd(hwnd, FALSE, &l->target))) {
        ok = false;
        *why = "CreateTargetForHwnd failed";
    }
    if (ok && (FAILED(l->dcomp->CreateVisual(&l->visual)) || FAILED(l->target->SetRoot(l->visual.Get())) ||
               FAILED(l->dcomp->Commit()))) {
        ok = false;
        *why = "composition visual failed";
    }
    if (!ok) {
        DeleteCurlLayer(l);
        return nullptr;
    }
    return l;
}

void DeleteCurlLayer(CurlLayer* l) {
    if (!l) {
        return;
    }
    if (l->shown) {
        CurlLayerShow(l, false);
    }
    if (l->frameWait) {
        CloseHandle(l->frameWait);
    }
    delete l;
}

Size CurlLayerSize(CurlLayer* l) {
    return l->size;
}

bool CurlLayerResize(CurlLayer* l, Size size) {
    size = AtLeastOne(size);
    if (size == l->size) {
        return true;
    }
    ID3D11DeviceContext* ctx = nullptr;
    l->dev->GetImmediateContext(&ctx);
    if (ctx) {
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx->Flush();
        ctx->Release();
    }
    l->rtv.Reset();
    l->dsv.Reset();
    l->size = size;
    HRESULT hr = l->swap->ResizeBuffers(kBufferCount, (UINT)size.dx, (UINT)size.dy, DXGI_FORMAT_UNKNOWN, kSwapFlags);
    if (FAILED(hr)) {
        return false;
    }
    return MakeViews(l);
}

// attaches or detaches the swap chain; takes effect at the next composition
void CurlLayerShow(CurlLayer* l, bool show) {
    l->visual->SetContent(show ? l->swap.Get() : nullptr);
    l->dcomp->Commit();
    l->shown = show;
}

bool CurlLayerShown(CurlLayer* l) {
    return l->shown;
}

// signalled when the swap chain takes a new frame (max frame latency 1)
HANDLE CurlLayerFrameWait(CurlLayer* l) {
    return l->frameWait;
}

ID3D11RenderTargetView* CurlLayerTarget(CurlLayer* l) {
    return l->rtv.Get();
}

ID3D11DepthStencilView* CurlLayerDepth(CurlLayer* l) {
    return l->dsv.Get();
}

bool CurlLayerPresent(CurlLayer* l) {
    return SUCCEEDED(l->swap->Present(1, 0));
}

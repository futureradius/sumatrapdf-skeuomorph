// Throwaway spike for "Can the Curl layer cover a jump with no flash?" (#19). Never merged.
//
// Off unless the environment variable SPIKE_NOFLASH is set, e.g.
//   SPIKE_NOFLASH=layer=ondemand,wait=1,flush=1,copy=lock,size=1600x1000,out=C:\x,label=A
//
//   layer=ondemand|attached  show the layer only while a Curl plays, or keep it attached (empty) all the time
//   wait=0|1                 hold SumatraPDF's repaint until the covering frame is on screen
//   flush=N                  screen refreshes to wait after SumatraPDF shows the new Spread before the layer
//                            turns see-through there (and before it goes away)
//   copy=ui|lock             screen copy: UI thread copies to memory, or drawing thread copies GDI straight
//                            into a GPU image while the UI thread waits
//   size=WxH|max             canvas size
//
// The run is automatic: it turns pages, records every frame the screen shows (Desktop Duplication), checks
// each turn for flashes and writes <out>\<label>.txt, <label>-frames.csv and filmstrips <label>-turnN.png.
//
//        UI thread                         drawing thread                 capture thread
//   GoToPage -> SpikeBeforeSpreadChange --> copy old screen, show layer --> records frames
//   posted task (before the repaint)  <-- "covered" (next frame signal)
//   paint -> SpikeAfterPaint ---------------> copy new Spread, see-through
//                                             Curl lands, layer goes away

#include "base/Base.h"
#include "base/Win.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "SpikeNoFlash.h"

#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN8
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#pragma pop_macro("NTDDI_VERSION")

#include <vector>
#include <algorithm>

#include "SpikeCurlShaders.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

static const double kCurlMs = 700.0;
static const float kPi = 3.14159265f;
static const int kDown = 4;         // recorded frames are 1/4 size
static const int kRingFrames = 420; // ~7 s of 60 Hz
static const double kTurnWindowMs = 1500.0;
static const double kFarWindowMs = 3200.0;
static const UINT WM_SPIKE_TURNDONE = WM_APP + 0x51;
static const UINT WM_SPIKE_INIT = WM_APP + 0x52;

//--- config, log, time

struct Config {
    bool enabled = false;
    bool attached = false;
    bool noLayer = false;
    int rec = 1; // 0: no recorder, 2: record only a band across the middle (big windows) // baseline: plain SumatraPDF
                 // jumps, recorded the same way
    bool waitCover = false;
    int flushWait = 1;
    bool copyLock = false;
    int lagMs = 0; // test of the test: show the layer this late, so the check must report flashes
    int cw = 1600, ch = 1000;
    bool maximize = false;
    char out[MAX_PATH] = ".";
    char label[64] = "run";
};
static Config gCfg;
static bool gCfgRead = false;
static FILE* gLog = nullptr;
static LARGE_INTEGER gFreq;

static double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)gFreq.QuadPart;
}

static double QpcToMs(LONGLONG q) {
    return (double)q * 1000.0 / (double)gFreq.QuadPart;
}

static void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (gLog) {
        fputs(buf, gLog);
        fflush(gLog);
    }
    OutputDebugStringA(buf);
}

static void ReadConfig() {
    gCfgRead = true;
    QueryPerformanceFrequency(&gFreq);
    char env[1024];
    if (!GetEnvironmentVariableA("SPIKE_NOFLASH", env, sizeof(env))) {
        return;
    }
    gCfg.enabled = true;
    char* ctx = nullptr;
    for (char* tok = strtok_s(env, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
        char* eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char* v = eq + 1;
        if (!strcmp(tok, "layer")) {
            gCfg.attached = !strcmp(v, "attached");
            gCfg.noLayer = !strcmp(v, "none");
        } else if (!strcmp(tok, "wait")) {
            gCfg.waitCover = atoi(v) != 0;
        } else if (!strcmp(tok, "flush")) {
            gCfg.flushWait = atoi(v);
        } else if (!strcmp(tok, "rec")) {
            gCfg.rec = atoi(v);
        } else if (!strcmp(tok, "lag")) {
            gCfg.lagMs = atoi(v);
        } else if (!strcmp(tok, "copy")) {
            gCfg.copyLock = !strcmp(v, "lock");
        } else if (!strcmp(tok, "size")) {
            if (!strcmp(v, "max")) {
                gCfg.maximize = true;
            } else {
                sscanf_s(v, "%dx%d", &gCfg.cw, &gCfg.ch);
            }
        } else if (!strcmp(tok, "out")) {
            strcpy_s(gCfg.out, v);
        } else if (!strcmp(tok, "label")) {
            strcpy_s(gCfg.label, v);
        }
    }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", gCfg.out, gCfg.label);
    fopen_s(&gLog, path, "w");
    Log("=== %s: layer=%s wait=%d flush=%d copy=%s size=%s ===\n", gCfg.label, gCfg.attached ? "attached" : "ondemand",
        gCfg.waitCover, gCfg.flushWait, gCfg.copyLock ? "lock" : "ui", gCfg.maximize ? "max" : "fixed");
}

static double Median(std::vector<double> v) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static double Pct(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t i = (size_t)std::min<double>((double)v.size() - 1, p / 100.0 * (v.size() - 1) + 0.5);
    return v[i];
}

//--- shared state (gLock)

struct GpuImage {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

// copy request from the UI thread (copy=lock route, or upload of a memory copy)
struct CopyReq {
    bool pending = false;
    int which = 0; // 0 = old screen, 1 = new Spread
    HDC hdc = nullptr;
    Rect area;
    std::vector<uint8_t> pixels; // copy=ui: BGRA pixels of area
    double gpuMs = 0;
};

struct Turn {
    bool active = false;
    int id = 0;
    double t0 = 0;
    double dur = kCurlMs;
    Rect oldL, oldR, newL, newR;
    Rect oldArea, newArea;
    bool haveNew = false;
    bool windowShowsNew = false;
    double windowShowsNewAt = 0;
    // filled by the drawing thread
    double coverPresentedAt = 0, coveredAt = 0, revealAt = 0, landedAt = 0, endAt = 0;
    double uiWaitMs = 0, copyOldMs = 0, copyNewMs = 0, uploadMs = 0;
    std::vector<double> frameGaps;
};

static CRITICAL_SECTION gLock;
static HANDLE gWake, gCovered, gCopyDone, gReadyEvt;
static Turn gTurn;
static CopyReq gReq;
static bool gQuit = false;
static bool gReady = false;
static bool gDriverTurn = false;
static MainWindow* gWin = nullptr;
static HWND gMsgWnd = nullptr;
static int gCanvasW = 0, gCanvasH = 0;

//--- drawing thread

struct CB {
    float viewSize[2];
    float pagePos[2];
    float pageSize[2];
    float foldPoint[2];
    float foldNormal[2];
    float radius;
    float mode;
    float frontImg[4];
    float backImg[4];
    float backPagePos[2];
    float backHasImage;
    float pad0;
};

enum Mode {
    ModeFlat = 0,
    ModeCurl = 1,
    ModeShadow = 2,
    ModePaper = 3
};

static ComPtr<ID3D11Device> gDev;
static ComPtr<ID3D11DeviceContext> gCtx;
static ComPtr<IDXGIAdapter> gAdapter;
static ComPtr<IDXGISwapChain1> gSwap;
static HANDLE gFrameWait;
static ComPtr<IDCompositionDevice> gDcomp;
static ComPtr<IDCompositionTarget> gTarget;
static ComPtr<IDCompositionVisual> gVisual;
static ComPtr<ID3D11RenderTargetView> gRtv;
static ComPtr<ID3D11DepthStencilView> gDsv;
static ComPtr<ID3D11VertexShader> gVS;
static ComPtr<ID3D11PixelShader> gPS;
static ComPtr<ID3D11InputLayout> gLayout;
static ComPtr<ID3D11Buffer> gMeshVB, gMeshIB, gQuadVB, gQuadIB, gCBuf;
static UINT gMeshCount, gQuadCount;
static ComPtr<ID3D11SamplerState> gSamp;
static ComPtr<ID3D11BlendState> gBlend;
static ComPtr<ID3D11RasterizerState> gRast;
static ComPtr<ID3D11DepthStencilState> gDepth;
static GpuImage gImgOld, gImgNew;

#define CHECK(expr, what)                                               \
    do {                                                                \
        HRESULT hr_ = (expr);                                           \
        if (FAILED(hr_)) {                                              \
            Log("FAILED: %s (hr=0x%08lx)\n", what, (unsigned long)hr_); \
            return false;                                               \
        }                                                               \
    } while (0)

static void LogVideoMemory(const char* when) {
    ComPtr<IDXGIAdapter3> a3;
    if (!gAdapter || FAILED(gAdapter.As(&a3))) {
        return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
    Log("GPU memory (%s): %.1f MB local + %.1f MB shared\n", when, local.CurrentUsage / 1048576.0,
        shared.CurrentUsage / 1048576.0);
}

static bool MakeGrid(int gx, int gy, ComPtr<ID3D11Buffer>& vb, ComPtr<ID3D11Buffer>& ib, UINT& count) {
    std::vector<float> v;
    for (int y = 0; y <= gy; y++) {
        for (int x = 0; x <= gx; x++) {
            v.push_back((float)x / gx);
            v.push_back((float)y / gy);
        }
    }
    std::vector<uint32_t> idx;
    for (int y = 0; y < gy; y++) {
        for (int x = 0; x < gx; x++) {
            uint32_t i0 = y * (gx + 1) + x, i1 = i0 + 1, i2 = i0 + gx + 1, i3 = i2 + 1;
            uint32_t tri[6] = {i0, i1, i2, i1, i3, i2};
            idx.insert(idx.end(), tri, tri + 6);
        }
    }
    count = (UINT)idx.size();
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = (UINT)(v.size() * 4);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA d = {v.data()};
    CHECK(gDev->CreateBuffer(&bd, &d, &vb), "vertex buffer");
    bd.ByteWidth = (UINT)(idx.size() * 4);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    d.pSysMem = idx.data();
    CHECK(gDev->CreateBuffer(&bd, &d, &ib), "index buffer");
    return true;
}

// Shaders compiled at runtime by d3dcompiler_47.dll, only timed (the spike uses precompiled bytes).
static void TimeRuntimeShaderCompile() {
    double t0 = NowMs();
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) {
        Log("Shaders: d3dcompiler_47.dll not found\n");
        return;
    }
    auto compile = (pD3DCompile)GetProcAddress(dll, "D3DCompile");
    double t1 = NowMs();
    // the run script starts SumatraPDF from the repo root, where the HLSL source is
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    *strrchr(path, '\\') = 0;
    strcat_s(path, "\\..\\..\\src\\skeuomorph\\SpikeCurl.hlsl");
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f || !compile) {
        Log("Shaders: dll load %.1f ms, source not found (run from repo root to time compiling)\n", t1 - t0);
        if (f) {
            fclose(f);
        }
        return;
    }
    std::vector<char> src(200000);
    size_t n = fread(src.data(), 1, src.size(), f);
    fclose(f);
    ComPtr<ID3DBlob> vs, ps, err;
    compile(src.data(), n, "SpikeCurl.hlsl", nullptr, nullptr, "VS", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs,
            &err);
    compile(src.data(), n, "SpikeCurl.hlsl", nullptr, nullptr, "PS", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps,
            &err);
    double t2 = NowMs();
    Log("Shaders at runtime: load d3dcompiler_47.dll %.1f ms, compile both %.1f ms\n", t1 - t0, t2 - t1);
}

static bool CreateTarget() {
    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = gCanvasW;
    dd.Height = gCanvasH;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    CHECK(gDev->CreateTexture2D(&dd, nullptr, &depth), "depth texture");
    CHECK(gDev->CreateDepthStencilView(depth.Get(), nullptr, &gDsv), "depth view");
    ComPtr<ID3D11Texture2D> back;
    CHECK(gSwap->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    CHECK(gDev->CreateRenderTargetView(back.Get(), nullptr, &gRtv), "render target view");
    return true;
}

static bool SetupGraphics(HWND hwndCanvas) {
    double t0 = NowMs();
    D3D_FEATURE_LEVEL fl;
    CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &gDev, &fl, &gCtx),
          "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgiDev;
    CHECK(gDev.As(&dxgiDev), "IDXGIDevice");
    CHECK(dxgiDev->GetAdapter(&gAdapter), "GetAdapter");
    DXGI_ADAPTER_DESC ad;
    gAdapter->GetDesc(&ad);
    double t1 = NowMs();
    LogVideoMemory("before layer");

    ComPtr<IDXGIFactory2> factory;
    CHECK(gAdapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = gCanvasW;
    sd.Height = gCanvasH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    CHECK(factory->CreateSwapChainForComposition(gDev.Get(), &sd, nullptr, &gSwap), "CreateSwapChainForComposition");
    ComPtr<IDXGISwapChain2> swap2;
    CHECK(gSwap.As(&swap2), "IDXGISwapChain2");
    swap2->SetMaximumFrameLatency(1);
    gFrameWait = swap2->GetFrameLatencyWaitableObject();

    CHECK(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&gDcomp)), "DCompositionCreateDevice");
    // topmost=FALSE: the layer stays below child windows (form-field and annotation edit boxes)
    CHECK(gDcomp->CreateTargetForHwnd(hwndCanvas, FALSE, &gTarget), "CreateTargetForHwnd");
    CHECK(gDcomp->CreateVisual(&gVisual), "CreateVisual");
    CHECK(gTarget->SetRoot(gVisual.Get()), "SetRoot");
    if (gCfg.attached) {
        CHECK(gVisual->SetContent(gSwap.Get()), "SetContent");
    }
    CHECK(gDcomp->Commit(), "Commit");
    if (!CreateTarget()) {
        return false;
    }
    double t2 = NowMs();

    CHECK(gDev->CreateVertexShader(gSpikeVS, sizeof(gSpikeVS), nullptr, &gVS), "VS");
    CHECK(gDev->CreatePixelShader(gSpikePS, sizeof(gSpikePS), nullptr, &gPS), "PS");
    D3D11_INPUT_ELEMENT_DESC el = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    CHECK(gDev->CreateInputLayout(&el, 1, gSpikeVS, sizeof(gSpikeVS), &gLayout), "layout");
    double t3 = NowMs();

    if (!MakeGrid(48, 64, gMeshVB, gMeshIB, gMeshCount) || !MakeGrid(1, 1, gQuadVB, gQuadIB, gQuadCount)) {
        return false;
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(gDev->CreateBuffer(&bd, nullptr, &gCBuf), "constant buffer");
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(gDev->CreateSamplerState(&smp, &gSamp), "sampler");
    D3D11_BLEND_DESC bl = {};
    auto& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    CHECK(gDev->CreateBlendState(&bl, &gBlend), "blend");
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    CHECK(gDev->CreateRasterizerState(&rd, &gRast), "rasterizer");
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    CHECK(gDev->CreateDepthStencilState(&ds, &gDepth), "depth");
    double t4 = NowMs();
    Log("Setup on drawing thread: %ls; device %.1f ms, layer+swap chain %.1f ms, precompiled shaders %.2f ms, rest "
        "%.1f ms, total %.1f ms\n",
        ad.Description, t1 - t0, t2 - t1, t3 - t2, t4 - t3, t4 - t0);
    LogVideoMemory("after layer");
    Log("Swap chain %dx%d x2 buffers = %.1f MB held while the layer exists\n", gCanvasW, gCanvasH,
        2.0 * gCanvasW * gCanvasH * 4 / 1048576.0);
    TimeRuntimeShaderCompile();
    return true;
}

static bool EnsureImage(GpuImage& img, int w, int h, bool gdi) {
    if (img.tex && img.w == w && img.h == h) {
        return true;
    }
    img = GpuImage();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (gdi ? D3D11_BIND_RENDER_TARGET : 0);
    d.MiscFlags = gdi ? D3D11_RESOURCE_MISC_GDI_COMPATIBLE : 0;
    CHECK(gDev->CreateTexture2D(&d, nullptr, &img.tex), "image texture");
    CHECK(gDev->CreateShaderResourceView(img.tex.Get(), nullptr, &img.srv), "image view");
    img.w = w;
    img.h = h;
    return true;
}

// Waits until the GPU has finished the commands so far (to time copies).
static void GpuFinish() {
    ComPtr<ID3D11Query> ev;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    gDev->CreateQuery(&qd, &ev);
    gCtx->End(ev.Get());
    gCtx->Flush();
    BOOL done = FALSE;
    while (gCtx->GetData(ev.Get(), &done, sizeof(done), 0) == S_FALSE) {
    }
}

// copy=lock: GDI copies SumatraPDF's buffer straight into a GPU image. copy=ui: upload memory pixels.
static void ServiceCopy() {
    EnterCriticalSection(&gLock);
    if (!gReq.pending) {
        LeaveCriticalSection(&gLock);
        return;
    }
    CopyReq req;
    req.which = gReq.which;
    req.hdc = gReq.hdc;
    req.area = gReq.area;
    req.pixels.swap(gReq.pixels);
    gReq.pending = false;
    LeaveCriticalSection(&gLock);

    double t0 = NowMs();
    GpuImage& img = req.which == 0 ? gImgOld : gImgNew;
    int w = req.area.dx, h = req.area.dy;
    bool lockRoute = req.hdc != nullptr;
    EnsureImage(img, w, h, lockRoute);
    if (lockRoute) {
        ComPtr<IDXGISurface1> surf;
        HDC dc = nullptr;
        if (SUCCEEDED(img.tex.As(&surf)) && SUCCEEDED(surf->GetDC(FALSE, &dc))) {
            BitBlt(dc, 0, 0, w, h, req.hdc, req.area.x, req.area.y, SRCCOPY);
            surf->ReleaseDC(nullptr);
        } else {
            Log("FAILED: GetDC on GDI-compatible image\n");
        }
        SetEvent(gCopyDone); // the UI thread may use its buffer again
        GpuFinish();
    } else {
        gCtx->UpdateSubresource(img.tex.Get(), 0, nullptr, req.pixels.data(), w * 4, 0);
        GpuFinish();
    }
    double ms = NowMs() - t0;
    EnterCriticalSection(&gLock);
    if (req.which == 0) {
        gTurn.uploadMs = ms;
    } else {
        gTurn.haveNew = true;
    }
    LeaveCriticalSection(&gLock);
}

static CB BaseCB() {
    CB cb = {};
    cb.viewSize[0] = (float)gCanvasW;
    cb.viewSize[1] = (float)gCanvasH;
    cb.radius = 1;
    return cb;
}

static void SetPage(CB& cb, Rect r) {
    cb.pagePos[0] = (float)r.x;
    cb.pagePos[1] = (float)r.y;
    cb.pageSize[0] = (float)r.dx;
    cb.pageSize[1] = (float)r.dy;
}

static void SetImg(float* dst, Rect r) {
    dst[0] = (float)r.x;
    dst[1] = (float)r.y;
    dst[2] = (float)r.dx;
    dst[3] = (float)r.dy;
}

// Corner peel: the fold starts at the bottom corner, slanted, and straightens as it crosses the spine.
// At t=1 the radius is ~1 px and the page lies mirrored exactly on the new left page.
static void SetFold(CB& cb, double t, float pageW, float gap) {
    float e = (float)std::clamp(t, 0.0, 1.0);
    e = e * e * (3 - 2 * e);
    float angle = (1 - e) * 0.5f;
    float R = pageW * 0.10f * (1 - e) + 1.0f * e;
    float startX = pageW + 2, endX = (-gap - kPi * R) * 0.5f;
    cb.foldPoint[0] = startX + (endX - startX) * e;
    cb.foldPoint[1] = cb.pageSize[1];
    cb.foldNormal[0] = cosf(angle);
    cb.foldNormal[1] = sinf(angle);
    cb.radius = R;
}

static void Draw(const CB& cb, ID3D11ShaderResourceView* front, ID3D11ShaderResourceView* back, bool mesh) {
    gCtx->UpdateSubresource(gCBuf.Get(), 0, nullptr, &cb, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = {front, back};
    gCtx->PSSetShaderResources(0, 2, srvs);
    UINT stride = 8, off = 0;
    ID3D11Buffer* vb = mesh ? gMeshVB.Get() : gQuadVB.Get();
    gCtx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    gCtx->IASetIndexBuffer(mesh ? gMeshIB.Get() : gQuadIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    gCtx->DrawIndexed(mesh ? gMeshCount : gQuadCount, 0, 0);
}

enum class Scene {
    Empty,
    Curl,
    Landed
};

// One frame. Revealed parts are see-through once SumatraPDF really shows the new Spread, blank paper before.
static void RenderFrame(Scene scene, const Turn& st, double t, bool seeThrough) {
    float clear[4] = {0, 0, 0, 0};
    gCtx->OMSetRenderTargets(1, gRtv.GetAddressOf(), gDsv.Get());
    gCtx->ClearRenderTargetView(gRtv.Get(), clear);
    gCtx->ClearDepthStencilView(gDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (scene == Scene::Empty) {
        return;
    }
    D3D11_VIEWPORT vp = {0, 0, (float)gCanvasW, (float)gCanvasH, 0, 1};
    gCtx->RSSetViewports(1, &vp);
    gCtx->RSSetState(gRast.Get());
    gCtx->OMSetBlendState(gBlend.Get(), nullptr, 0xffffffff);
    gCtx->OMSetDepthStencilState(gDepth.Get(), 0);
    gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCtx->IASetInputLayout(gLayout.Get());
    gCtx->VSSetShader(gVS.Get(), nullptr, 0);
    gCtx->PSSetShader(gPS.Get(), nullptr, 0);
    gCtx->VSSetConstantBuffers(0, 1, gCBuf.GetAddressOf());
    gCtx->PSSetConstantBuffers(0, 1, gCBuf.GetAddressOf());
    gCtx->PSSetSamplers(0, 1, gSamp.GetAddressOf());

    if (scene == Scene::Landed) {
        CB cb = BaseCB();
        SetPage(cb, st.newL);
        if (st.haveNew) {
            cb.mode = ModeFlat;
            SetImg(cb.frontImg, st.newArea);
            Draw(cb, gImgNew.srv.Get(), nullptr, false);
        } else {
            cb.mode = ModePaper;
            Draw(cb, nullptr, nullptr, false);
        }
        if (!seeThrough) {
            SetPage(cb, st.newR);
            cb.mode = ModePaper;
            Draw(cb, nullptr, nullptr, false);
        }
        return;
    }

    // old left page stays until the Curl lands on it
    if (!st.oldL.IsEmpty()) {
        CB cb = BaseCB();
        cb.mode = ModeFlat;
        SetPage(cb, st.oldL);
        SetImg(cb.frontImg, st.oldArea);
        Draw(cb, gImgOld.srv.Get(), nullptr, false);
    }
    float gap = (float)(st.oldR.x - (st.newL.x + st.newL.dx));
    if (!seeThrough) {
        CB cb = BaseCB();
        cb.mode = ModePaper;
        SetPage(cb, st.newR);
        Draw(cb, nullptr, nullptr, false);
    }
    {
        CB cb = BaseCB();
        cb.mode = ModeShadow;
        SetPage(cb, st.oldR);
        SetFold(cb, t, (float)st.oldR.dx, gap);
        Draw(cb, nullptr, nullptr, false);
    }
    CB cb = BaseCB();
    cb.mode = ModeCurl;
    SetPage(cb, st.oldR);
    SetFold(cb, t, (float)st.oldR.dx, gap);
    SetImg(cb.frontImg, st.oldArea);
    SetImg(cb.backImg, st.newArea);
    cb.backPagePos[0] = (float)st.newL.x;
    cb.backPagePos[1] = (float)st.newL.y;
    cb.backHasImage = st.haveNew ? 1.0f : 0.0f;
    Draw(cb, gImgOld.srv.Get(), st.haveNew ? gImgNew.srv.Get() : nullptr, true);
}

static void SetLayerShown(bool show) {
    if (gCfg.attached) {
        return;
    }
    gVisual->SetContent(show ? gSwap.Get() : nullptr);
    gDcomp->Commit();
}

static DWORD WINAPI DrawingThread(void* arg) {
    HWND hwndCanvas = (HWND)arg;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = SetupGraphics(hwndCanvas);
    if (ok && gCfg.attached) {
        // one empty frame so the attached layer starts see-through
        WaitForSingleObject(gFrameWait, 1000);
        RenderFrame(Scene::Empty, gTurn, 0, true);
        gSwap->Present(1, 0);
    }
    EnterCriticalSection(&gLock);
    gReady = ok;
    LeaveCriticalSection(&gLock);
    SetEvent(gReadyEvt);
    if (!ok) {
        return 1;
    }

    bool animating = false, firstFrame = false, landedShown = false;
    int refreshesLeft = -1; // refreshes to wait after SumatraPDF shows the new Spread, -1 = not started
    bool seeThrough = false, pendingCover = false;
    double lastPresent = 0;
    int turnId = 0;
    while (true) {
        HANDLE hs[2] = {gWake, gFrameWait};
        DWORD r = WaitForMultipleObjects(animating ? 2 : 1, hs, FALSE, 1000);
        EnterCriticalSection(&gLock);
        bool quit = gQuit;
        LeaveCriticalSection(&gLock);
        if (quit) {
            break;
        }
        ServiceCopy();

        EnterCriticalSection(&gLock);
        Turn st = gTurn;
        LeaveCriticalSection(&gLock);
        if (!animating) {
            if (st.active && st.id != turnId) {
                turnId = st.id;
                animating = true;
                firstFrame = true;
                landedShown = false;
                seeThrough = false;
                pendingCover = false;
                refreshesLeft = -1;
            }
            continue; // wait for the frame-latency signal before drawing
        }
        if (r != WAIT_OBJECT_0 + 1) {
            continue;
        }

        // a frame signal means the compositor took the previous frame: the covering frame is on screen
        if (pendingCover) {
            pendingCover = false;
            EnterCriticalSection(&gLock);
            gTurn.coveredAt = NowMs();
            LeaveCriticalSection(&gLock);
            SetEvent(gCovered);
        }

        // see-through only some refreshes after SumatraPDF flushed the new Spread
        // (flush=-1, test of the test: see-through from the start)
        if (gCfg.flushWait < 0 && !seeThrough) {
            seeThrough = true;
        }
        if (st.windowShowsNew && !seeThrough) {
            if (refreshesLeft < 0) {
                refreshesLeft = gCfg.flushWait;
            } else if (refreshesLeft > 0) {
                refreshesLeft--;
            }
            if (refreshesLeft == 0) {
                seeThrough = true;
                EnterCriticalSection(&gLock);
                gTurn.revealAt = NowMs();
                LeaveCriticalSection(&gLock);
            }
        }

        if (firstFrame && gCfg.lagMs > 0) {
            Sleep(gCfg.lagMs);
        }
        double now = NowMs();
        double t = (now - st.t0) / st.dur;
        bool landed = t >= 1.0;
        bool timeout = now - st.t0 > 4000;
        bool done = (landed && landedShown && seeThrough) || timeout;
        Scene scene = done ? Scene::Empty : (landed ? Scene::Landed : Scene::Curl);
        RenderFrame(scene, st, t, seeThrough);
        gSwap->Present(1, 0);
        double pres = NowMs();
        if (lastPresent > 0 && !firstFrame) {
            EnterCriticalSection(&gLock);
            gTurn.frameGaps.push_back(pres - lastPresent);
            LeaveCriticalSection(&gLock);
        }
        lastPresent = pres;

        if (firstFrame) {
            firstFrame = false;
            SetLayerShown(true);
            EnterCriticalSection(&gLock);
            gTurn.coverPresentedAt = pres;
            LeaveCriticalSection(&gLock);
            pendingCover = true;
        }
        if (landed && !landedShown) {
            landedShown = true;
            EnterCriticalSection(&gLock);
            gTurn.landedAt = pres;
            LeaveCriticalSection(&gLock);
        }
        if (done) {
            SetLayerShown(false);
            animating = false;
            lastPresent = 0;
            EnterCriticalSection(&gLock);
            gTurn.endAt = NowMs();
            gTurn.active = false;
            LeaveCriticalSection(&gLock);
            if (timeout) {
                Log("turn %d: TIMEOUT, SumatraPDF never showed the new Spread\n", st.id);
            }
            // consume the frame signal for this last Present on the next turn's first wait
        }
    }
    return 0;
}

//--- capture thread: every frame the screen shows, as 1/4-size crops of the canvas

struct Frame {
    double t = 0; // when it was shown (ms, QPC)
    UINT accumulated = 0;
    std::vector<uint8_t> px; // BGRA
};

static std::vector<Frame> gRing;
static int gRingNext = 0, gRingCount = 0;
static int gCropX, gCropY, gCropW, gCropH, gSmallW, gSmallH, gCropScreenX, gCropScreenY;
static ComPtr<IDXGIOutput> gCapOutput;

static bool FindOutput(POINT pt) {
    ComPtr<IDXGIFactory1> f;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&f)), "factory");
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> o;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC d;
            o->GetDesc(&d);
            RECT rc = d.DesktopCoordinates;
            if (PtInRect(&rc, pt)) {
                gCapOutput = o;
                gCropX -= rc.left;
                gCropY -= rc.top;
                return true;
            }
            o.Reset();
        }
        a.Reset();
    }
    return false;
}

static void Downsample(const uint8_t* src, int pitch, std::vector<uint8_t>& dst) {
    for (int y = 0; y < gSmallH; y++) {
        for (int x = 0; x < gSmallW; x++) {
            uint32_t c[3] = {0, 0, 0};
            for (int dy = 0; dy < kDown; dy++) {
                const uint8_t* s = src + (size_t)(y * kDown + dy) * pitch + x * kDown * 4;
                for (int dx = 0; dx < kDown; dx++, s += 4) {
                    c[0] += s[0];
                    c[1] += s[1];
                    c[2] += s[2];
                }
            }
            uint8_t* d = &dst[((size_t)y * gSmallW + x) * 4];
            d[0] = (uint8_t)(c[0] / (kDown * kDown));
            d[1] = (uint8_t)(c[1] / (kDown * kDown));
            d[2] = (uint8_t)(c[2] / (kDown * kDown));
            d[3] = 255;
        }
    }
}

static void PushFrame(double t, UINT accumulated, const std::vector<uint8_t>& px) {
    EnterCriticalSection(&gLock);
    Frame& fr = gRing[gRingNext];
    fr.t = t;
    fr.accumulated = accumulated;
    fr.px = px;
    gRingNext = (gRingNext + 1) % kRingFrames;
    gRingCount = std::min(gRingCount + 1, kRingFrames);
    LeaveCriticalSection(&gLock);
}

// Fallback recorder: copies the screen as fast as it can and keeps each copy that changed.
// A copy shows the last frame the compositor put on screen, so polling faster than the refresh sees every frame.
static DWORD GdiPollCapture() {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = gCropW;
    bi.bmiHeader.biHeight = -gCropH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, dib);
    std::vector<uint8_t> tiny((size_t)gSmallW * gSmallH * 4), last;
    std::vector<double> copyMs, gaps;
    double prev = 0;
    while (true) {
        EnterCriticalSection(&gLock);
        bool quit = gQuit;
        LeaveCriticalSection(&gLock);
        if (quit) {
            break;
        }
        double t0 = NowMs();
        // the screen position of the crop; the DC is the virtual screen
        BitBlt(mem, 0, 0, gCropW, gCropH, screen, gCropScreenX, gCropScreenY, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        double t1 = NowMs();
        copyMs.push_back(t1 - t0);
        if (prev > 0) {
            gaps.push_back(t0 - prev);
        }
        prev = t0;
        Downsample((const uint8_t*)bits, gCropW * 4, tiny);
        if (tiny != last) {
            PushFrame((t0 + t1) * 0.5, 0, tiny);
            last = tiny;
        }
        if (copyMs.size() == 400) {
            Log("recorder: GDI screen copy %.2f ms median, %.2f ms p95; one copy every %.2f ms (p95 %.2f)\n",
                Median(copyMs), Pct(copyMs, 95), Median(gaps), Pct(gaps, 95));
        }
    }
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return 0;
}

static DWORD WINAPI CaptureThread(void*) {
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(gCapOutput->GetParent(IID_PPV_ARGS(&adapter)))) {
        return 1;
    }
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr,
                      &ctx);
    ComPtr<IDXGIOutput1> out1;
    gCapOutput.As(&out1);
    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = out1 ? out1->DuplicateOutput(dev.Get(), &dup) : E_FAIL;
    if (FAILED(hr)) {
        Log("DuplicateOutput failed (hr=0x%08lx): recording by polling the screen with GDI instead\n",
            (unsigned long)hr);
        return GdiPollCapture();
    }
    Log("recorder: Desktop Duplication (every composed frame)\n");
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = gCropW;
    sd.Height = gCropH;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    dev->CreateTexture2D(&sd, nullptr, &staging);
    std::vector<uint8_t> tiny((size_t)gSmallW * gSmallH * 4);

    while (true) {
        EnterCriticalSection(&gLock);
        bool quit = gQuit;
        LeaveCriticalSection(&gLock);
        if (quit) {
            break;
        }
        DXGI_OUTDUPL_FRAME_INFO fi;
        ComPtr<IDXGIResource> res;
        hr = dup->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (FAILED(hr)) {
            Log("capture: AcquireNextFrame hr=0x%08lx\n", (unsigned long)hr);
            break;
        }
        if (fi.LastPresentTime.QuadPart == 0) {
            dup->ReleaseFrame();
            continue; // only the mouse moved
        }
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        D3D11_BOX box = {(UINT)gCropX, (UINT)gCropY, 0, (UINT)(gCropX + gCropW), (UINT)(gCropY + gCropH), 1};
        ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < gSmallH; y++) {
                for (int x = 0; x < gSmallW; x++) {
                    uint32_t c[3] = {0, 0, 0};
                    for (int dy = 0; dy < kDown; dy++) {
                        const uint8_t* s =
                            (const uint8_t*)m.pData + (size_t)(y * kDown + dy) * m.RowPitch + x * kDown * 4;
                        for (int dx = 0; dx < kDown; dx++, s += 4) {
                            c[0] += s[0];
                            c[1] += s[1];
                            c[2] += s[2];
                        }
                    }
                    uint8_t* d = &tiny[((size_t)y * gSmallW + x) * 4];
                    d[0] = (uint8_t)(c[0] / (kDown * kDown));
                    d[1] = (uint8_t)(c[1] / (kDown * kDown));
                    d[2] = (uint8_t)(c[2] / (kDown * kDown));
                    d[3] = 255;
                }
            }
            ctx->Unmap(staging.Get(), 0);
        }
        dup->ReleaseFrame();
        EnterCriticalSection(&gLock);
        Frame& fr = gRing[gRingNext];
        fr.t = QpcToMs(fi.LastPresentTime.QuadPart);
        fr.accumulated = fi.AccumulatedFrames;
        fr.px = tiny;
        gRingNext = (gRingNext + 1) % kRingFrames;
        gRingCount = std::min(gRingCount + 1, kRingFrames);
        LeaveCriticalSection(&gLock);
    }
    return 0;
}

//--- analysis

static bool SavePng(const char* path, const uint8_t* px, int w, int h) {
    ComPtr<IWICImagingFactory> f;
    CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "WIC");
    ComPtr<IWICStream> st;
    CHECK(f->CreateStream(&st), "CreateStream");
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    CHECK(st->InitializeFromFilename(wpath, GENERIC_WRITE), "InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> enc;
    CHECK(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "CreateEncoder");
    CHECK(enc->Initialize(st.Get(), WICBitmapEncoderNoCache), "enc Initialize");
    ComPtr<IWICBitmapFrameEncode> fr;
    CHECK(enc->CreateNewFrame(&fr, nullptr), "CreateNewFrame");
    CHECK(fr->Initialize(nullptr), "frame Initialize");
    CHECK(fr->SetSize(w, h), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    CHECK(fr->SetPixelFormat(&fmt), "SetPixelFormat");
    CHECK(fr->WritePixels(h, w * 4, w * h * 4, (BYTE*)px), "WritePixels");
    CHECK(fr->Commit(), "frame Commit");
    CHECK(enc->Commit(), "enc Commit");
    return true;
}

static int PxDiff(const uint8_t* a, const uint8_t* b) {
    int d0 = abs(a[0] - b[0]), d1 = abs(a[1] - b[1]), d2 = abs(a[2] - b[2]);
    return std::max(d0, std::max(d1, d2));
}

struct Totals {
    int turns = 0, flashes = 0, oldBack = 0, gaps = 0, missed = 0;
    std::vector<double> coverMs, uiWait, copyOld, copyNew, upload, gaps60;
    int lateFrames = 0, frames = 0;
};
static Totals gTotals;
static FILE* gCsv = nullptr;

// Checks the frames around one turn. Old = last frame before the tap, New = last frame of the window.
// o/n = share of the pixels that differ between Old and New which match Old/New in this frame.
static void AnalyzeTurn(const Turn& st, double windowMs, const char* what) {
    std::vector<Frame> frames;
    Frame before;
    EnterCriticalSection(&gLock);
    for (int i = 0; i < gRingCount; i++) {
        int idx = (gRingNext - gRingCount + i + kRingFrames) % kRingFrames;
        const Frame& f = gRing[idx];
        if (f.t < st.t0) {
            before = f;
        } else if (f.t <= windowMs) {
            frames.push_back(f);
        }
    }
    LeaveCriticalSection(&gLock);
    if (before.px.empty() || frames.empty()) {
        int late = 0;
        for (double g : st.frameGaps) {
            late += g > 16.7 * 1.5;
        }
        gTotals.frames += (int)st.frameGaps.size();
        gTotals.lateFrames += late;
        gTotals.coverMs.push_back(st.coveredAt - st.t0);
        gTotals.uiWait.push_back(st.uiWaitMs);
        gTotals.copyOld.push_back(st.copyOldMs);
        gTotals.copyNew.push_back(st.copyNewMs);
        gTotals.upload.push_back(st.uploadMs);
        Log("turn %d (%s): not recorded | cover shown %.1f | UI waited %.1f | copy old %.1f new %.1f upload %.1f | "
            "%zu frames, %d late, worst gap %.1f ms\n",
            st.id, what, st.coveredAt - st.t0, st.uiWaitMs, st.copyOldMs, st.copyNewMs, st.uploadMs,
            st.frameGaps.size(), late, Pct(st.frameGaps, 100));
        return;
    }
    const Frame& after = frames.back(); // last frame before the window ends: layer gone, SumatraPDF only
    size_t n = (size_t)gSmallW * gSmallH;
    std::vector<uint8_t> mask(n);
    int informative = 0;
    for (size_t i = 0; i < n; i++) {
        mask[i] = PxDiff(&before.px[i * 4], &after.px[i * 4]) > 48;
        informative += mask[i];
    }
    if (informative < (int)(n / 50)) {
        Log("turn %d (%s): INVALID, old and new Spread look the same on screen\n", st.id, what);
        return;
    }

    double firstChange = -1, coverEnd = st.t0 + st.dur - 40, minO = 1;
    bool started = false, flash = false, oldBack = false, settled = false, gap = false;
    int missed = 0;
    std::vector<double> oVals, nVals;
    for (auto& f : frames) {
        int mo = 0, mn = 0;
        for (size_t i = 0; i < n; i++) {
            if (!mask[i]) {
                continue;
            }
            mo += PxDiff(&f.px[i * 4], &before.px[i * 4]) <= 24;
            mn += PxDiff(&f.px[i * 4], &after.px[i * 4]) <= 24;
        }
        double o = informative ? (double)mo / informative : 1, nn = informative ? (double)mn / informative : 1;
        oVals.push_back(o);
        nVals.push_back(nn);
        missed += f.accumulated > 1 ? (int)f.accumulated - 1 : 0;
        if (!started && o < 0.98) {
            started = true;
            firstChange = f.t;
        }
        if (f.t < coverEnd && nn > 0.85) {
            flash = true; // the new Spread was on screen before the Curl landed
        }
        if (started && f.t < coverEnd) {
            if (o > minO + 0.10) {
                oldBack = true;
            }
            minO = std::min(minO, o);
        }
        if (settled && nn < 0.985) {
            gap = true;
        }
        if (f.t >= coverEnd && nn >= 0.985) {
            settled = true;
        }
        if (gCsv) {
            fprintf(gCsv, "%s,%d,%s,%.1f,%u,%.3f,%.3f\n", gCfg.label, st.id, what, f.t - st.t0, f.accumulated, o, nn);
        }
    }

    // the edit-box turn: one frame mid-Curl at 1/4 size, to see whether the box stays above the Curl
    if (!strcmp(what, "editbox")) {
        for (auto& f : frames) {
            if (f.t >= st.t0 + 520) {
                char p2[MAX_PATH];
                sprintf_s(p2, "%s\\%s-editbox-midcurl.png", gCfg.out, gCfg.label);
                SavePng(p2, f.px.data(), gSmallW, gSmallH);
                break;
            }
        }
    }

    // filmstrip: every frame at 1/16 size, 10 per row
    int tw = gSmallW / 4, th = gSmallH / 4, cols = 10;
    int rows = ((int)frames.size() + 1 + cols - 1) / cols;
    std::vector<uint8_t> strip((size_t)tw * cols * th * rows * 4, 40);
    for (size_t k = 0; k <= frames.size(); k++) {
        const Frame& f = k == 0 ? before : frames[k - 1];
        int ox = (int)(k % cols) * tw, oy = (int)(k / cols) * th;
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                const uint8_t* s = &f.px[((size_t)(y * 4) * gSmallW + x * 4) * 4];
                uint8_t* d = &strip[((size_t)(oy + y) * tw * cols + ox + x) * 4];
                memcpy(d, s, 4);
            }
        }
    }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s-turn%02d.png", gCfg.out, gCfg.label, st.id);
    SavePng(path, strip.data(), tw * cols, th * rows);

    int late = 0;
    for (double g : st.frameGaps) {
        late += g > 16.7 * 1.5;
    }
    gTotals.turns++;
    gTotals.flashes += flash;
    gTotals.oldBack += oldBack;
    gTotals.gaps += gap;
    gTotals.missed += missed;
    gTotals.lateFrames += late;
    gTotals.frames += (int)st.frameGaps.size();
    gTotals.coverMs.push_back(st.coveredAt - st.t0);
    gTotals.uiWait.push_back(st.uiWaitMs);
    gTotals.copyOld.push_back(st.copyOldMs);
    gTotals.copyNew.push_back(st.copyNewMs);
    gTotals.upload.push_back(st.uploadMs);
    Log("turn %2d %-10s | first change %5.1f ms | cover shown %5.1f (presented %5.1f) | UI waited %4.1f | "
        "Sumatra shows new %6.1f | see-through %6.1f | landed %6.1f | layer gone %6.1f | copy old %4.1f new %4.1f "
        "upload %4.1f | %3zu frames, %d late, %d missed by recorder | FLASH %s, OLD BACK %s, GAP %s\n",
        st.id, what, firstChange < 0 ? -1.0 : firstChange - st.t0, st.coveredAt - st.t0, st.coverPresentedAt - st.t0,
        st.uiWaitMs, st.windowShowsNew ? st.windowShowsNewAt - st.t0 : -1.0, st.revealAt ? st.revealAt - st.t0 : -1.0,
        st.landedAt - st.t0, st.endAt - st.t0, st.copyOldMs, st.copyNewMs, st.uploadMs, frames.size(), late, missed,
        flash ? "YES" : "no", oldBack ? "YES" : "no", gap ? "YES" : "no");
}

//--- UI thread: screen copies

static Rect Union(Rect a, Rect b) {
    if (a.IsEmpty()) {
        return b;
    }
    if (b.IsEmpty()) {
        return a;
    }
    return a.Union(b);
}

// Visible pages of a Facing Spread, left and right.
static void SpreadRects(DisplayModel* dm, Rect& l, Rect& r) {
    l = r = Rect();
    for (int p = 1; p <= dm->PageCount(); p++) {
        PageInfo* pi = dm->GetPageInfo(p);
        if (!pi || pi->visibleRatio <= 0) {
            continue;
        }
        Rect s = pi->pageOnScreen;
        if (r.IsEmpty()) {
            r = s;
        } else if (s.x > r.x) {
            l = r;
            r = s;
        } else {
            l = s;
        }
    }
}

// Copies area of SumatraPDF's off-screen buffer into a GPU image. Returns ms the UI thread spent.
static double CopyScreen(int which, Rect area) {
    HDC src = gWin->buffer->GetDC();
    area = area.Intersect(Rect(0, 0, gCanvasW, gCanvasH));
    double t0 = NowMs();
    if (gCfg.copyLock) {
        EnterCriticalSection(&gLock);
        gReq.pending = true;
        gReq.which = which;
        gReq.hdc = src;
        gReq.area = area;
        LeaveCriticalSection(&gLock);
        ResetEvent(gCopyDone);
        SetEvent(gWake);
        WaitForSingleObject(gCopyDone, 200);
        return NowMs() - t0;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = area.dx;
    bi.bmiHeader.biHeight = -area.dy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(src);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, area.dx, area.dy, src, area.x, area.y, SRCCOPY);
    GdiFlush();
    std::vector<uint8_t> px((uint8_t*)bits, (uint8_t*)bits + (size_t)area.dx * area.dy * 4);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    EnterCriticalSection(&gLock);
    gReq.pending = true;
    gReq.which = which;
    gReq.hdc = nullptr;
    gReq.area = area;
    gReq.pixels.swap(px);
    LeaveCriticalSection(&gLock);
    SetEvent(gWake);
    return NowMs() - t0;
}

//--- hooks

static void SpikeEnsureMsgWnd();

void SpikeBeforeSpreadChange(MainWindow* win, DisplayModel* dm, int newStartPage) {
    if (!gCfg.enabled || win != gWin || !gDriverTurn || !gReady) {
        return;
    }
    EnterCriticalSection(&gLock);
    bool busy = gTurn.active;
    LeaveCriticalSection(&gLock);
    if (busy) {
        Log("turn skipped: a Curl is still in the air\n");
        return;
    }
    double t0 = NowMs();
    Rect l, r;
    SpreadRects(dm, l, r);
    // wait for the drawing thread to have read any previous request
    for (int i = 0; i < 50; i++) {
        EnterCriticalSection(&gLock);
        bool pending = gReq.pending;
        LeaveCriticalSection(&gLock);
        if (!pending) {
            break;
        }
        Sleep(1);
    }
    Rect area = Union(l, r);
    EnterCriticalSection(&gLock);
    int id = gTurn.id + 1;
    gTurn = Turn();
    gTurn.id = id;
    LeaveCriticalSection(&gLock);
    double copyMs = CopyScreen(0, area);

    EnterCriticalSection(&gLock);
    gTurn.active = !gCfg.noLayer;
    gTurn.endAt = gCfg.noLayer ? t0 + 1000 : 0;
    gTurn.t0 = t0;
    gTurn.oldL = l;
    gTurn.oldR = r;
    gTurn.newL = l; // until the posted task knows the new Spread
    gTurn.newR = r;
    gTurn.oldArea = area.Intersect(Rect(0, 0, gCanvasW, gCanvasH));
    gTurn.copyOldMs = copyMs;
    LeaveCriticalSection(&gLock);
    ResetEvent(gCovered);
    SetEvent(gWake);
    (void)newStartPage;
    // runs after this action's GoToPage calls, before SumatraPDF's repaint
    PostMessageW(gMsgWnd, WM_SPIKE_TURNDONE, 0, 0);
}

static void OnTurnDone() {
    DisplayModel* dm = gWin->AsFixed();
    Rect l, r;
    SpreadRects(dm, l, r);
    double waited = 0;
    if (gCfg.waitCover) {
        double t = NowMs();
        WaitForSingleObject(gCovered, 100);
        waited = NowMs() - t;
    }
    EnterCriticalSection(&gLock);
    gTurn.newL = l;
    gTurn.newR = r;
    gTurn.uiWaitMs = waited;
    LeaveCriticalSection(&gLock);
}

void SpikeAfterPaint(MainWindow* win, bool flushed) {
    if (!gCfgRead) {
        ReadConfig();
    }
    if (!gCfg.enabled) {
        return;
    }
    if (!gWin) {
        if (!win->AsFixed()) {
            return;
        }
        gWin = win;
        SpikeEnsureMsgWnd();
        PostMessageW(gMsgWnd, WM_SPIKE_INIT, 0, 0);
        return;
    }
    if (win != gWin || !flushed) {
        return;
    }
    EnterCriticalSection(&gLock);
    bool want = gTurn.active && !gTurn.windowShowsNew;
    Rect newL = gTurn.newL;
    LeaveCriticalSection(&gLock);
    if (!want) {
        return;
    }
    // SumatraPDF just put the new Spread (or "Rendering page N...") on screen
    DisplayModel* dm = win->AsFixed();
    Rect l, r;
    SpreadRects(dm, l, r);
    newL = l.IsEmpty() ? newL : l;
    double at = NowMs();
    double copyMs = CopyScreen(1, newL);
    EnterCriticalSection(&gLock);
    gTurn.windowShowsNew = true;
    gTurn.windowShowsNewAt = at;
    gTurn.newArea = newL.Intersect(Rect(0, 0, gCanvasW, gCanvasH));
    gTurn.copyNewMs = copyMs;
    LeaveCriticalSection(&gLock);
    SetEvent(gWake);
}

//--- the automatic test run (UI thread)

enum class Step {
    Bench,
    Next,
    Far,
    NextMenu,
    NextBox,
    NextEdit,
    Done
};
struct Planned {
    double delay; // ms after the previous step (and its check) finished
    Step step;
    int arg;
};
static std::vector<Planned> gPlan;
static size_t gPlanPos = 0;
static double gNextAt = 0;
static bool gInModal = false;
static HWND gEdit = nullptr, gBoxOwner = nullptr;
static double gPendingWindow = 0;
static const char* gPendingWhat = "";
static bool gHavePending = false;
static Step gModalStep = Step::Next;
static const UINT_PTR kDriverTimer = 1, kOpenModalTimer = 2, kCloseModalTimer = 3;

static void Bench() {
    DisplayModel* dm = gWin->AsFixed();
    Rect l, r;
    SpreadRects(dm, l, r);
    Rect spread = Union(l, r);
    Rect whole(0, 0, gCanvasW, gCanvasH);
    bool saved = gCfg.copyLock;
    for (int route = 0; route < 2; route++) {
        gCfg.copyLock = route == 1;
        for (int a = 0; a < 2; a++) {
            Rect area = a == 0 ? whole : spread;
            std::vector<double> ui, gpu;
            for (int i = 0; i < 10; i++) {
                ui.push_back(CopyScreen(0, area));
                Sleep(30);
                EnterCriticalSection(&gLock);
                gpu.push_back(gTurn.uploadMs);
                LeaveCriticalSection(&gLock);
            }
            Log("Screen copy %-37s %4dx%-4d: UI thread %.2f ms, drawing thread %.2f ms (median of 10)\n",
                route ? "GDI straight into GPU image (lock):" : "to memory on UI thread, then upload:", area.dx,
                area.dy, Median(ui), Median(gpu));
        }
    }
    gCfg.copyLock = saved;
}

static const char* StepName(Step s) {
    switch (s) {
        case Step::Far:
            return "far";
        case Step::NextMenu:
            return "menu";
        case Step::NextBox:
            return "dialog";
        case Step::NextEdit:
            return "editbox";
        default:
            return "near";
    }
}

static void StartTurn(Step s, int arg) {
    gDriverTurn = true;
    if (s == Step::Far) {
        gWin->ctrl->GoToPage(arg, false);
    } else {
        gWin->ctrl->GoToNextPage();
    }
    gDriverTurn = false;
    EnterCriticalSection(&gLock);
    bool started = gTurn.active || (gCfg.noLayer && gTurn.t0 > NowMs() - 100);
    LeaveCriticalSection(&gLock);
    if (!started) {
        Log("step %s: no turn started\n", StepName(s));
        return;
    }
    gHavePending = true;
    gPendingWindow = s == Step::Far ? kFarWindowMs : kTurnWindowMs;
    if (!gEdit) {
        gPendingWhat = StepName(s);
    }
}

static void Finish() {
    Log("\nSUMMARY %s: %d turns | flashes %d | old Spread back %d | gaps at removal %d | frames missed by recorder "
        "%d\n",
        gCfg.label, gTotals.turns, gTotals.flashes, gTotals.oldBack, gTotals.gaps, gTotals.missed);
    Log("  cover on screen after tap: median %.1f ms, max %.1f | UI waited median %.1f max %.1f | copy old median "
        "%.1f, new %.1f, upload %.1f\n",
        Median(gTotals.coverMs), Pct(gTotals.coverMs, 100), Median(gTotals.uiWait), Pct(gTotals.uiWait, 100),
        Median(gTotals.copyOld), Median(gTotals.copyNew), Median(gTotals.upload));
    Log("  Curl frames: %d, late (>1.5 refreshes) %d\n", gTotals.frames, gTotals.lateFrames);
    if (gCsv) {
        fclose(gCsv);
        gCsv = nullptr;
    }
    EnterCriticalSection(&gLock);
    gQuit = true;
    LeaveCriticalSection(&gLock);
    SetEvent(gWake);
    PostMessageW(gWin->hwndFrame, WM_CLOSE, 0, 0);
}

// a menu or dialog opens 150 ms into the Curl and closes 700 ms later
static void OpenModal() {
    gInModal = true;
    SetTimer(gMsgWnd, kCloseModalTimer, 700, nullptr);
    if (gModalStep == Step::NextMenu) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"spike menu: the Curl must keep playing");
        AppendMenuW(m, MF_STRING, 2, L"second item");
        RECT wr;
        GetWindowRect(gWin->hwndFrame, &wr);
        TrackPopupMenu(m, TPM_RETURNCMD, wr.right + 20, wr.top + 80, 0, gMsgWnd, nullptr);
        DestroyMenu(m);
    } else {
        // closed from a helper thread: posts OK to the dialog after 700 ms
        CreateThread(
            nullptr, 0,
            [](void*) -> DWORD {
                for (int i = 0; i < 100; i++) {
                    Sleep(10);
                    HWND box = FindWindowW(L"#32770", L"spike dialog");
                    if (box && i >= 70) {
                        PostMessageW(box, WM_COMMAND, IDOK, 0);
                        return 0;
                    }
                }
                Log("dialog closer: dialog not found\n");
                return 0;
            },
            nullptr, 0, nullptr);
        MessageBoxW(gBoxOwner, L"The Curl must keep playing behind this dialog.", L"spike dialog", MB_OK);
    }
    gInModal = false;
    Log("modal %s closed\n", gModalStep == Step::NextMenu ? "menu" : "dialog");
}

static void CloseModal() {
    EndMenu();
}

static void DriverTick() {
    if (gInModal) {
        return;
    }
    double now = NowMs();
    if (gHavePending) {
        EnterCriticalSection(&gLock);
        Turn st = gTurn;
        LeaveCriticalSection(&gLock);
        // check frames up to shortly after the layer went away
        bool settled = !st.active && now > st.endAt + 300;
        if (!settled && now < st.t0 + 5000) {
            return;
        }
        gHavePending = false;
        AnalyzeTurn(st, settled ? st.endAt + 250 : now, gPendingWhat);
        if (gEdit) {
            DestroyWindow(gEdit);
            gEdit = nullptr;
        }
        if (gPlanPos < gPlan.size()) {
            gNextAt = NowMs() + gPlan[gPlanPos].delay;
        }
        return;
    }
    if (gPlanPos >= gPlan.size() || now < gNextAt) {
        return;
    }
    Planned p = gPlan[gPlanPos++];
    switch (p.step) {
        case Step::Bench:
            Bench();
            break;
        case Step::Next:
        case Step::Far:
            StartTurn(p.step, p.arg);
            break;
        case Step::NextEdit: {
            Rect l, r;
            SpreadRects(gWin->AsFixed(), l, r);
            gEdit = CreateWindowExW(0, L"EDIT", L"edit box: must stay above the Curl",
                                    WS_CHILD | WS_VISIBLE | WS_BORDER, r.x + r.dx / 8, r.y + r.dy * 3 / 5, r.dx * 3 / 4,
                                    r.dy / 8, gWin->hwndCanvas, nullptr, GetModuleHandleW(nullptr), nullptr);
            Sleep(10);
            gNextAt = NowMs() + 400; // the turn follows once the box is on screen
            gPlan.insert(gPlan.begin() + gPlanPos, Planned{0, Step::Next, 0});
            gPendingWhat = "editbox";
            return;
        }
        case Step::NextMenu:
        case Step::NextBox:
            StartTurn(p.step, p.arg);
            gModalStep = p.step;
            SetTimer(gMsgWnd, kOpenModalTimer, 150, nullptr);
            break;
        case Step::Done:
            Finish();
            return;
    }
    if (!gHavePending && gPlanPos < gPlan.size()) {
        gNextAt = NowMs() + gPlan[gPlanPos].delay;
    }
}

static void OnInit() {
    // size and place the window so the recorder's crop is known
    HWND frame = gWin->hwndFrame;
    if (gCfg.maximize) {
        ShowWindow(frame, SW_MAXIMIZE);
    } else {
        RECT fr, cr;
        GetWindowRect(frame, &fr);
        GetClientRect(gWin->hwndCanvas, &cr);
        int extraW = (fr.right - fr.left) - cr.right, extraH = (fr.bottom - fr.top) - cr.bottom;
        ShowWindow(frame, SW_RESTORE);
        SetWindowPos(frame, HWND_TOPMOST, 20, 20, gCfg.cw + extraW, gCfg.ch + extraH, SWP_SHOWWINDOW);
    }
    SetForegroundWindow(frame);
    DisplayModel* dm = gWin->AsFixed();
    if (dm && dm->GetDisplayMode() != DisplayMode::Facing) {
        gWin->ctrl->SetDisplayMode(DisplayMode::Facing);
    }
    // let SumatraPDF settle at the new size
    MSG m;
    double until = NowMs() + 800;
    while (NowMs() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }

    RECT cr;
    GetClientRect(gWin->hwndCanvas, &cr);
    gCanvasW = cr.right;
    gCanvasH = cr.bottom;
    POINT pt = {0, 0};
    ClientToScreen(gWin->hwndCanvas, &pt);
    gCropX = gCropScreenX = pt.x;
    gCropY = gCropScreenY = pt.y;
    gCropW = (gCanvasW / kDown) * kDown;
    gCropH = (gCanvasH / kDown) * kDown;
    if (gCfg.rec == 2) {
        // a band through the middle of the Spread: enough to see flashes, cheap enough to keep up at 4K
        int band = (gCanvasH / 5 / kDown) * kDown;
        gCropY += (gCanvasH - band) / 2;
        gCropScreenY = gCropY;
        gCropH = band;
    }
    gSmallW = gCropW / kDown;
    gSmallH = gCropH / kDown;
    Log("Canvas %dx%d at screen %d,%d\n", gCanvasW, gCanvasH, pt.x, pt.y);
    POINT c = {pt.x + gCanvasW / 2, pt.y + gCanvasH / 2};
    if (!FindOutput(c)) {
        Log("FAILED: no output for the window\n");
        return;
    }
    gRing.resize(kRingFrames);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s-frames.csv", gCfg.out, gCfg.label);
    fopen_s(&gCsv, path, "w");
    if (gCsv) {
        fprintf(gCsv, "run,turn,kind,ms_after_tap,accumulated,match_old,match_new\n");
    }

    // planned moment for Direct3D setup: first Paged View paint, on the drawing thread
    double t0 = NowMs();
    CreateThread(nullptr, 0, DrawingThread, gWin->hwndCanvas, 0, nullptr);
    double t1 = NowMs();
    WaitForSingleObject(gReadyEvt, 5000);
    Log("UI thread: starting the drawing thread took %.2f ms; ready %.1f ms later\n", t1 - t0, NowMs() - t0);
    if (gCfg.rec) {
        CreateThread(nullptr, 0, CaptureThread, nullptr, 0, nullptr);
    }

    // a hidden owner far right, so the test dialog doesn't land on the recorded area
    RECT wr;
    GetWindowRect(frame, &wr);
    gBoxOwner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, wr.right + 300, wr.top + 200, 10, 10,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    double gapMs = 300;
    gPlan.push_back({1000, Step::Bench, 0});
    for (int i = 0; i < 10; i++) {
        gPlan.push_back({gapMs, Step::Next, 0});
    }
    gPlan.push_back({gapMs, Step::NextMenu, 0});
    gPlan.push_back({gapMs, Step::NextBox, 0});
    gPlan.push_back({gapMs, Step::NextEdit, 0});
    gPlan.push_back({gapMs, Step::Far, 191});
    gPlan.push_back({800, Step::Far, 11});
    gPlan.push_back({800, Step::Far, 197});
    gPlan.push_back({800, Step::Far, 21});
    gPlan.push_back({800, Step::Far, 199});
    gPlan.push_back({gapMs, Step::Done, 0});
    gNextAt = NowMs() + gPlan[0].delay;
    SetTimer(gMsgWnd, kDriverTimer, 15, nullptr);
}

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SPIKE_TURNDONE) {
        OnTurnDone();
        return 0;
    }
    if (msg == WM_SPIKE_INIT) {
        OnInit();
        return 0;
    }
    if (msg == WM_TIMER) {
        if (wp == kDriverTimer) {
            DriverTick();
        } else if (wp == kOpenModalTimer) {
            KillTimer(hwnd, wp);
            OpenModal();
        } else if (wp == kCloseModalTimer) {
            KillTimer(hwnd, wp);
            CloseModal();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Message window for the spike's posted tasks, created on first use from the UI thread.
static struct SpikeInit {
    SpikeInit() {
        InitializeCriticalSection(&gLock);
        gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        gCovered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        gCopyDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        gReadyEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
} gSpikeInit;

static void SpikeEnsureMsgWnd() {
    if (gMsgWnd) {
        return;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SpikeNoFlashMsg";
    RegisterClassW(&wc);
    gMsgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

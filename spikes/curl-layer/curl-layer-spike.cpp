// Throwaway spike for "Can a Curl layer run smoothly on this PC?" (never merged).
// Draws bent, textured pages with Direct3D 11 on a see-through DirectComposition
// layer over a plain GDI window, and measures frame times.
//
//   curl-layer-spike.exe [--warp] [--adapter N] [--size WxH] [--seconds S] [--out DIR] [--label NAME]
//
// No --size: maximized window. --warp: Windows' software renderer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static const float kPi = 3.14159265f;
static const double kCurlMs = 700.0;   // normal Curl
static const double kBundleMs = 910.0; // Bundle
static const int kMaxCurls = 5;        // at most 5 in the air
static const int kBundleSheets = 18;   // 18 px at most, ~1 px per drawn sheet

//--- logging and timing

static FILE* gOut = nullptr;

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (!gOut) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(gOut, fmt, ap);
    va_end(ap);
    fflush(gOut);
}

#define CHECK(expr, what)                                                       \
    do {                                                                        \
        HRESULT hr_ = (expr);                                                   \
        if (FAILED(hr_)) {                                                      \
            Log("FAILED: %s (hr=0x%08lx)\n", what, (unsigned long)hr_);         \
            return false;                                                       \
        }                                                                       \
    } while (0)

static LARGE_INTEGER gFreq;

static double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)gFreq.QuadPart;
}

//--- options

struct Options {
    bool warp = false;
    int adapter = -1;
    int clientW = 0;
    int clientH = 0;
    double seconds = 4.0;
    std::string outDir = ".";
    std::string label = "run";
};
static Options gOpt;

static void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool hasNext = i + 1 < argc;
        if (a == "--warp") {
            gOpt.warp = true;
        } else if (a == "--adapter" && hasNext) {
            gOpt.adapter = atoi(argv[++i]);
        } else if (a == "--size" && hasNext) {
            sscanf(argv[++i], "%dx%d", &gOpt.clientW, &gOpt.clientH);
        } else if (a == "--seconds" && hasNext) {
            gOpt.seconds = atof(argv[++i]);
        } else if (a == "--out" && hasNext) {
            gOpt.outDir = argv[++i];
        } else if (a == "--label" && hasNext) {
            gOpt.label = argv[++i];
        }
    }
}

//--- graphics state

static HWND gHwnd;
static int W, H;
static double gRefreshMs = 1000.0 / 60.0;

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
static ComPtr<ID3D11Buffer> gMeshVB, gMeshIB, gQuadVB, gQuadIB, gCB;
static UINT gMeshIndexCount, gQuadIndexCount;
static ComPtr<ID3D11SamplerState> gSamp;
static ComPtr<ID3D11BlendState> gBlend;
static ComPtr<ID3D11RasterizerState> gRast;
static ComPtr<ID3D11DepthStencilState> gDepthOn, gDepthOff;

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
};
static Tex gSnapOld, gSnapNew;
static std::vector<Tex> gPageTex; // front + back for each Curl in the air

// Spread layout: two pages side by side, spine at the window's centre.
static float gPageX, gPageY, gPageW, gPageH;

// Matches cbuffer CB in the shader.
struct CB {
    float viewSize[2];
    float pagePos[2];
    float pageSize[2];
    float foldPoint[2];
    float foldNormal[2];
    float radius;
    float zBase;
    float mode;
    float alpha;
    float persp;
    float pad0;
    float uvFront[4];
    float uvBack[4];
};

enum DrawMode { ModeFlat = 0, ModeCurl = 1, ModeShadow = 2, ModeSheet = 3 };

static const char* kShader = R"(
cbuffer CB : register(b0) {
    float2 viewSize; float2 pagePos;
    float2 pageSize; float2 foldPoint;
    float2 foldNormal; float radius; float zBase;
    float mode; float alpha; float persp; float pad0;
    float4 uvFront; float4 uvBack;
};
Texture2D texFront : register(t0);
Texture2D texBack : register(t1);
SamplerState samp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 nrm : NORMAL;
    float2 pagePx : TEXCOORD1;
};

// Bend the page around a cylinder whose axis is the fold line.
VSOut VS(float2 uv : TEXCOORD0) {
    float2 p = uv * pageSize;
    float3 P = float3(p, 0);
    float3 N = float3(0, 0, 1);
    if (mode == 1 || mode == 3) {
        float s = dot(p - foldPoint, foldNormal);
        if (s > 0) {
            float phi = s / radius;
            float2 base = p - foldNormal * s;
            if (phi < 3.14159265) {
                P = float3(base + foldNormal * (radius * sin(phi)), radius * (1 - cos(phi)));
                N = float3(-foldNormal * sin(phi), cos(phi));
            } else {
                P = float3(base - foldNormal * (s - 3.14159265 * radius), 2 * radius);
                N = float3(0, 0, -1);
            }
        }
    }
    float3 world = float3(pagePos + P.xy, P.z + zBase);
    float2 c = viewSize * 0.5;
    float k = persp / max(persp - world.z, 1.0);
    float2 sp = c + (world.xy - c) * k;
    VSOut o;
    o.pos = float4(sp.x / viewSize.x * 2 - 1, 1 - sp.y / viewSize.y * 2, saturate(0.5 - world.z * 0.0005), 1);
    o.uv = uv;
    o.nrm = N;
    o.pagePx = p;
    return o;
}

float4 PS(VSOut i, bool front : SV_IsFrontFace) : SV_Target {
    if (mode == 2) {
        float s = dot(i.pagePx - foldPoint, foldNormal);
        float a = s > 0 ? 0.35 * exp(-s / (radius * 2)) : 0;
        return float4(0, 0, 0, a * alpha);
    }
    float3 L = normalize(float3(0.3, -0.4, 1));
    float shade = 0.72 + 0.28 * abs(dot(normalize(i.nrm), L));
    float3 col;
    if (mode == 3) {
        col = float3(0.96, 0.95, 0.92);
    } else if (front || mode == 0) {
        col = texFront.Sample(samp, uvFront.xy + i.uv * uvFront.zw).rgb;
    } else {
        float2 u = float2(1 - i.uv.x, i.uv.y);
        col = texBack.Sample(samp, uvBack.xy + u * uvBack.zw).rgb;
    }
    if (mode == 0) {
        shade = 1;
    }
    return float4(col * shade * alpha, alpha);
}
)";

//--- fake page images

static uint32_t Rng(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

// Paper with grey "text lines", like a page of a book.
static void PaintPage(uint32_t* px, int stride, int x0, int y0, int w, int h, uint32_t seed) {
    const uint32_t paper = 0xFFF7F4EC;
    const uint32_t ink = 0xFF3A3A3A;
    for (int y = 0; y < h; y++) {
        std::fill(px + (y0 + y) * stride + x0, px + (y0 + y) * stride + x0 + w, paper);
    }
    int margin = w / 10;
    int lineH = std::max(4, h / 48);
    uint32_t s = seed * 7919u + 1;
    int titleH = lineH * 3;
    for (int y = margin; y < margin + titleH; y++) {
        int len = w / 2 + (int)(seed % 7) * w / 40;
        std::fill(px + (y0 + y) * stride + x0 + margin, px + (y0 + y) * stride + x0 + std::min(w - margin, margin + len), ink);
    }
    for (int ly = margin + titleH * 2; ly + lineH < h - margin; ly += lineH) {
        int len = (w - 2 * margin) - (int)(Rng(s) % (uint32_t)std::max(1, w / 4));
        for (int y = ly; y < ly + lineH / 2; y++) {
            std::fill(px + (y0 + y) * stride + x0 + margin, px + (y0 + y) * stride + x0 + margin + len, ink);
        }
    }
}

static bool MakeTex(int w, int h, const uint32_t* px, Tex& out) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {px, (UINT)w * 4, 0};
    CHECK(gDev->CreateTexture2D(&d, px ? &init : nullptr, &out.tex), "CreateTexture2D");
    CHECK(gDev->CreateShaderResourceView(out.tex.Get(), nullptr, &out.srv), "CreateShaderResourceView");
    return true;
}

static void LayoutSpread() {
    float margin = 24;
    gPageH = H - 2 * margin;
    gPageW = gPageH * 0.72f;
    if (2 * gPageW > W - 2 * margin) {
        gPageW = (W - 2 * margin) / 2;
        gPageH = gPageW / 0.72f;
    }
    gPageX = W * 0.5f;
    gPageY = (H - gPageH) * 0.5f;
}

// Window-size copies of the screen before and after a jump, plus page images for Curls.
static bool MakeTextures() {
    std::vector<uint32_t> snap((size_t)W * H);
    int pw = (int)gPageW, ph = (int)gPageH, px0 = (int)(gPageX - gPageW), py0 = (int)gPageY;
    for (int s = 0; s < 2; s++) {
        std::fill(snap.begin(), snap.end(), 0xFF6E6E6E);
        PaintPage(snap.data(), W, px0, py0, pw, ph, 100 + s * 2);
        PaintPage(snap.data(), W, px0 + pw, py0, pw, ph, 101 + s * 2);
        if (!MakeTex(W, H, snap.data(), s == 0 ? gSnapOld : gSnapNew)) {
            return false;
        }
    }
    std::vector<uint32_t> page((size_t)pw * ph);
    gPageTex.resize(kMaxCurls * 2);
    for (int i = 0; i < kMaxCurls * 2; i++) {
        PaintPage(page.data(), pw, 0, 0, pw, ph, 200 + i);
        if (!MakeTex(pw, ph, page.data(), gPageTex[i])) {
            return false;
        }
    }
    return true;
}

//--- setup

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        // Stand-in for SumatraPDF's own GDI drawing under the layer.
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH a = CreateSolidBrush(RGB(40, 90, 160));
        HBRUSH b = CreateSolidBrush(RGB(230, 120, 40));
        for (int y = 0; y < rc.bottom; y += 64) {
            for (int x = 0; x < rc.right; x += 64) {
                RECT r = {x, y, x + 64, y + 64};
                FillRect(dc, &r, ((x + y) / 64) % 2 ? a : b);
            }
        }
        DeleteObject(a);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, 20, 20, "GDI window underneath (stand-in for SumatraPDF)", 47);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool PumpMessages() {
    MSG m;
    while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return true;
}

static bool CreateWin() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "CurlLayerSpike";
    RegisterClassA(&wc);

    DWORD style = WS_OVERLAPPEDWINDOW;
    int ww = CW_USEDEFAULT, wh = CW_USEDEFAULT;
    if (gOpt.clientW > 0) {
        RECT r = {0, 0, gOpt.clientW, gOpt.clientH};
        AdjustWindowRectEx(&r, style, FALSE, 0);
        ww = r.right - r.left;
        wh = r.bottom - r.top;
    }
    gHwnd = CreateWindowExA(0, wc.lpszClassName, "Curl layer spike", style, 40, 40, ww, wh, nullptr, nullptr,
                            wc.hInstance, nullptr);
    if (!gHwnd) {
        Log("FAILED: CreateWindow\n");
        return false;
    }
    ShowWindow(gHwnd, gOpt.clientW > 0 ? SW_SHOW : SW_SHOWMAXIMIZED);
    SetForegroundWindow(gHwnd);
    UpdateWindow(gHwnd);
    PumpMessages();

    RECT rc;
    GetClientRect(gHwnd, &rc);
    W = rc.right;
    H = rc.bottom;
    return true;
}

static void ListAdapters() {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
        return;
    }
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        Log("  adapter %u: %ls (%llu MB dedicated)\n", i, d.Description,
            (unsigned long long)(d.DedicatedVideoMemory >> 20));
        a.Reset();
    }
}

static bool CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<IDXGIAdapter1> chosen;
    D3D_DRIVER_TYPE type = gOpt.warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    if (gOpt.adapter >= 0) {
        ComPtr<IDXGIFactory1> f;
        CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&f)), "CreateDXGIFactory1");
        CHECK(f->EnumAdapters1(gOpt.adapter, &chosen), "EnumAdapters1");
        type = D3D_DRIVER_TYPE_UNKNOWN;
    }
    D3D_FEATURE_LEVEL fl;
    CHECK(D3D11CreateDevice(chosen.Get(), type, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &gDev, &fl, &gCtx),
          "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgiDev;
    CHECK(gDev.As(&dxgiDev), "IDXGIDevice");
    CHECK(dxgiDev->GetAdapter(&gAdapter), "GetAdapter");
    DXGI_ADAPTER_DESC ad;
    gAdapter->GetDesc(&ad);
    Log("Device: %ls, feature level %x\n", ad.Description, (unsigned)fl);
    return true;
}

static bool CreateLayer() {
    ComPtr<IDXGIFactory2> factory;
    CHECK(gAdapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = W;
    sd.Height = H;
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

    ComPtr<IDXGIDevice> dxgiDev;
    CHECK(gDev.As(&dxgiDev), "IDXGIDevice");
    CHECK(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&gDcomp)), "DCompositionCreateDevice");
    CHECK(gDcomp->CreateTargetForHwnd(gHwnd, TRUE, &gTarget), "CreateTargetForHwnd");
    CHECK(gDcomp->CreateVisual(&gVisual), "CreateVisual");
    CHECK(gVisual->SetContent(gSwap.Get()), "SetContent");
    CHECK(gTarget->SetRoot(gVisual.Get()), "SetRoot");
    CHECK(gDcomp->Commit(), "Commit");

    ComPtr<ID3D11Texture2D> back;
    CHECK(gSwap->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    CHECK(gDev->CreateRenderTargetView(back.Get(), nullptr, &gRtv), "CreateRenderTargetView");

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = W;
    dd.Height = H;
    dd.MipLevels = 1;
    dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.SampleDesc.Count = 1;
    dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    CHECK(gDev->CreateTexture2D(&dd, nullptr, &depth), "depth texture");
    CHECK(gDev->CreateDepthStencilView(depth.Get(), nullptr, &gDsv), "CreateDepthStencilView");
    return true;
}

static bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), "spike.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
    if (FAILED(hr)) {
        Log("Shader %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    return true;
}

// A grid of (u,v) points; the vertex shader bends it.
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

static bool CreatePipeline() {
    ComPtr<ID3DBlob> vs, ps;
    if (!Compile("VS", "vs_4_0", vs) || !Compile("PS", "ps_4_0", ps)) {
        return false;
    }
    CHECK(gDev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gVS), "VS");
    CHECK(gDev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &gPS), "PS");
    D3D11_INPUT_ELEMENT_DESC el = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    CHECK(gDev->CreateInputLayout(&el, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &gLayout), "layout");

    if (!MakeGrid(48, 64, gMeshVB, gMeshIB, gMeshIndexCount) || !MakeGrid(1, 1, gQuadVB, gQuadIB, gQuadIndexCount)) {
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(CB);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CHECK(gDev->CreateBuffer(&bd, nullptr, &gCB), "constant buffer");

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    CHECK(gDev->CreateSamplerState(&sd, &gSamp), "sampler");

    // Premultiplied alpha, so empty parts of the layer stay see-through.
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
    CHECK(gDev->CreateDepthStencilState(&ds, &gDepthOn), "depth on");
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    CHECK(gDev->CreateDepthStencilState(&ds, &gDepthOff), "depth off");
    return true;
}

//--- drawing

static CB BaseCB() {
    CB cb = {};
    cb.viewSize[0] = (float)W;
    cb.viewSize[1] = (float)H;
    cb.alpha = 1;
    cb.persp = 3.0f * std::max(W, H);
    cb.radius = 1;
    float full[4] = {0, 0, 1, 1};
    memcpy(cb.uvFront, full, sizeof(full));
    memcpy(cb.uvBack, full, sizeof(full));
    return cb;
}

// Corner peel: the fold starts at the bottom corner, slanted, and straightens as it nears the spine.
static void SetFold(CB& cb, double t) {
    float e = (float)std::clamp(t, 0.0, 1.0);
    e = e * e * (3 - 2 * e);
    float angle = (1 - e) * 0.55f;
    float R = gPageW * (0.10f - 0.06f * e) + 4;
    float startX = gPageW + 2, endX = -kPi * R * 0.5f;
    cb.foldPoint[0] = startX + (endX - startX) * e;
    cb.foldPoint[1] = gPageH;
    cb.foldNormal[0] = cosf(angle);
    cb.foldNormal[1] = sinf(angle);
    cb.radius = R;
}

static void SetRect(CB& cb, float x, float y, float w, float h) {
    cb.pagePos[0] = x;
    cb.pagePos[1] = y;
    cb.pageSize[0] = w;
    cb.pageSize[1] = h;
}

enum class Geo { Quad, Mesh };

static void Draw(const CB& cb, ID3D11ShaderResourceView* front, ID3D11ShaderResourceView* back, Geo geo) {
    gCtx->UpdateSubresource(gCB.Get(), 0, nullptr, &cb, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = {front, back};
    gCtx->PSSetShaderResources(0, 2, srvs);
    UINT stride = 8, off = 0;
    bool mesh = geo == Geo::Mesh;
    ID3D11Buffer* vb = mesh ? gMeshVB.Get() : gQuadVB.Get();
    gCtx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    gCtx->IASetIndexBuffer(mesh ? gMeshIB.Get() : gQuadIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    gCtx->DrawIndexed(mesh ? gMeshIndexCount : gQuadIndexCount, 0, 0);
}

struct Scenario {
    const char* name;
    int curls;
    bool background; // full-window copies of the old and new Spread under the Curls
    bool bundle;
    const char* shot;
};

static void RenderFrame(const Scenario& sc, double ms) {
    float clear[4] = {0, 0, 0, 0};
    gCtx->OMSetRenderTargets(1, gRtv.GetAddressOf(), gDsv.Get());
    gCtx->ClearRenderTargetView(gRtv.Get(), clear);
    gCtx->ClearDepthStencilView(gDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    gCtx->RSSetViewports(1, &vp);
    gCtx->RSSetState(gRast.Get());
    gCtx->OMSetBlendState(gBlend.Get(), nullptr, 0xffffffff);
    gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gCtx->IASetInputLayout(gLayout.Get());
    gCtx->VSSetShader(gVS.Get(), nullptr, 0);
    gCtx->PSSetShader(gPS.Get(), nullptr, 0);
    gCtx->VSSetConstantBuffers(0, 1, gCB.GetAddressOf());
    gCtx->PSSetConstantBuffers(0, 1, gCB.GetAddressOf());
    gCtx->PSSetSamplers(0, 1, gSamp.GetAddressOf());

    // New Spread over the whole window, the old Spread's right half still on top of it.
    gCtx->OMSetDepthStencilState(gDepthOff.Get(), 0);
    if (sc.background) {
        CB cb = BaseCB();
        cb.mode = ModeFlat;
        SetRect(cb, 0, 0, (float)W, (float)H);
        Draw(cb, gSnapNew.srv.Get(), nullptr, Geo::Quad);
        SetRect(cb, W * 0.5f, 0, W * 0.5f, (float)H);
        cb.uvFront[0] = 0.5f;
        cb.uvFront[2] = 0.5f;
        Draw(cb, gSnapOld.srv.Get(), nullptr, Geo::Quad);
    }

    // Shadows on the revealed page, then the Curls themselves.
    std::vector<double> prog(sc.curls);
    for (int i = 0; i < sc.curls; i++) {
        prog[i] = fmod(ms / kCurlMs + (double)i / sc.curls, 1.0);
    }
    for (int i = 0; i < sc.curls; i++) {
        CB cb = BaseCB();
        cb.mode = ModeShadow;
        SetRect(cb, gPageX, gPageY, gPageW, gPageH);
        SetFold(cb, prog[i]);
        Draw(cb, nullptr, nullptr, Geo::Quad);
    }
    gCtx->OMSetDepthStencilState(gDepthOn.Get(), 0);
    for (int i = 0; i < sc.curls; i++) {
        CB cb = BaseCB();
        cb.mode = ModeCurl;
        SetRect(cb, gPageX, gPageY, gPageW, gPageH);
        SetFold(cb, prog[i]);
        cb.zBase = (float)(prog[i] * 4.0);
        Draw(cb, gPageTex[i * 2].srv.Get(), gPageTex[i * 2 + 1].srv.Get(), Geo::Mesh);
    }

    // Bundle: a textured top sheet with plain paper sheets fanned beneath it.
    if (sc.bundle) {
        double t = fmod(ms / kBundleMs, 1.0);
        for (int j = 0; j < kBundleSheets; j++) {
            CB cb = BaseCB();
            cb.mode = (float)(j == 0 ? ModeCurl : ModeSheet);
            SetRect(cb, gPageX, gPageY, gPageW, gPageH);
            SetFold(cb, t - j * 0.2 / kBundleSheets);
            cb.zBase = -(float)j;
            Draw(cb, gPageTex[0].srv.Get(), gPageTex[1].srv.Get(), Geo::Mesh);
        }
    }
}

//--- measuring

static double Pct(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t i = (size_t)std::min<double>((double)v.size() - 1, p / 100.0 * (v.size() - 1) + 0.5);
    return v[i];
}

static bool SavePng(const std::string& path, const uint32_t* px, int w, int h) {
    ComPtr<IWICImagingFactory> f;
    CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "WIC");
    ComPtr<IWICStream> st;
    CHECK(f->CreateStream(&st), "CreateStream");
    std::wstring wpath(path.begin(), path.end());
    CHECK(st->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE), "InitializeFromFilename");
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

// What the user sees: the screen under the window, halved in size.
static void CaptureScreen(const std::string& name) {
    POINT p = {0, 0};
    ClientToScreen(gHwnd, &p);
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, W, H, screen, p.x, p.y, SRCCOPY | CAPTUREBLT);
    GdiFlush();

    int w2 = W / 2, h2 = H / 2;
    std::vector<uint32_t> out((size_t)w2 * h2);
    const uint8_t* src = (const uint8_t*)bits;
    for (int y = 0; y < h2; y++) {
        for (int x = 0; x < w2; x++) {
            uint32_t c[3] = {0, 0, 0};
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t* s = src + ((size_t)(y * 2 + dy) * W + (x * 2 + dx)) * 4;
                    c[0] += s[0];
                    c[1] += s[1];
                    c[2] += s[2];
                }
            }
            out[(size_t)y * w2 + x] = 0xFF000000 | ((c[2] / 4) << 16) | ((c[1] / 4) << 8) | (c[0] / 4);
        }
    }
    std::string path = gOpt.outDir + "\\" + gOpt.label + "-" + name + ".png";
    if (SavePng(path, out.data(), w2, h2)) {
        Log("  screenshot: %s\n", path.c_str());
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

struct GpuQuery {
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool pending = false;
};

static void ReadGpu(GpuQuery& q, std::vector<double>& gpu, bool wait) {
    if (!q.pending) {
        return;
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT flags = wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
    while (gCtx->GetData(q.disjoint.Get(), &dj, sizeof(dj), flags) == S_FALSE) {
        if (!wait) {
            return;
        }
    }
    UINT64 b = 0, e = 0;
    while (gCtx->GetData(q.begin.Get(), &b, sizeof(b), 0) == S_FALSE) {
    }
    while (gCtx->GetData(q.end.Get(), &e, sizeof(e), 0) == S_FALSE) {
    }
    q.pending = false;
    if (dj.Disjoint || dj.Frequency == 0) {
        return;
    }
    gpu.push_back((double)(e - b) * 1000.0 / (double)dj.Frequency);
}

static bool RunScenario(const Scenario& sc) {
    const int kQueries = 6;
    GpuQuery qs[kQueries];
    for (auto& q : qs) {
        D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        gDev->CreateQuery(&d, &q.disjoint);
        d.Query = D3D11_QUERY_TIMESTAMP;
        gDev->CreateQuery(&d, &q.begin);
        gDev->CreateQuery(&d, &q.end);
    }

    std::vector<double> intervals, cpu, gpu;
    double start = NowMs(), last = -1;
    bool skipNext = true, shotDone = sc.shot == nullptr;
    int frame = 0;
    while (true) {
        if (!PumpMessages()) {
            return false;
        }
        if (NowMs() - start > gOpt.seconds * 1000.0) {
            break;
        }
        WaitForSingleObjectEx(gFrameWait, 1000, TRUE);
        double t = NowMs();
        if (last >= 0 && !skipNext) {
            intervals.push_back(t - last);
        }
        skipNext = false;
        last = t;
        double el = t - start;

        GpuQuery& q = qs[frame % kQueries];
        ReadGpu(q, gpu, true);
        gCtx->Begin(q.disjoint.Get());
        gCtx->End(q.begin.Get());
        RenderFrame(sc, el);
        gCtx->End(q.end.Get());
        gCtx->End(q.disjoint.Get());
        q.pending = true;
        gSwap->Present(1, 0);
        cpu.push_back(NowMs() - t);
        frame++;

        for (auto& other : qs) {
            ReadGpu(other, gpu, false);
        }
        if (!shotDone && el > 1500) {
            CaptureScreen(sc.shot);
            shotDone = true;
            skipNext = true;
        }
    }
    for (auto& q : qs) {
        ReadGpu(q, gpu, true);
    }

    double sum = 0;
    int missed = 0;
    for (double v : intervals) {
        sum += v;
        missed += v > gRefreshMs * 1.5 ? 1 : 0;
    }
    double fps = intervals.empty() ? 0 : 1000.0 * intervals.size() / sum;
    Log("%-22s | %5.1f fps | frame ms median %5.1f  p95 %5.1f  p99 %5.1f  max %6.1f | late frames %3d/%-4zu (%4.1f%%) "
        "| GPU ms median %5.2f  p95 %5.2f | CPU ms median %4.2f\n",
        sc.name, fps, Pct(intervals, 50), Pct(intervals, 95), Pct(intervals, 99), Pct(intervals, 100), missed,
        intervals.size(), intervals.empty() ? 0.0 : 100.0 * missed / intervals.size(), Pct(gpu, 50), Pct(gpu, 95),
        Pct(cpu, 50));
    return true;
}

// Copying SumatraPDF's off-screen buffer (a GDI bitmap) into a GPU texture, as a Curl's picture.
static void MeasureSnapshot() {
    HDC wdc = GetDC(gHwnd);
    HDC srcDC = CreateCompatibleDC(wdc);
    HBITMAP srcBmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ oldSrc = SelectObject(srcDC, srcBmp);
    BitBlt(srcDC, 0, 0, W, H, wdc, 0, 0, SRCCOPY);

    HDC dibDC = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dibDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldDib = SelectObject(dibDC, dib);

    Tex t;
    MakeTex(W, H, nullptr, t);
    ComPtr<ID3D11Query> ev;
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    gDev->CreateQuery(&qd, &ev);

    std::vector<double> gdi, up;
    for (int i = 0; i < 12; i++) {
        double t0 = NowMs();
        BitBlt(dibDC, 0, 0, W, H, srcDC, 0, 0, SRCCOPY);
        GdiFlush();
        double t1 = NowMs();
        gCtx->UpdateSubresource(t.tex.Get(), 0, nullptr, bits, W * 4, 0);
        gCtx->End(ev.Get());
        gCtx->Flush();
        BOOL done = FALSE;
        while (gCtx->GetData(ev.Get(), &done, sizeof(done), 0) == S_FALSE) {
        }
        double t2 = NowMs();
        gdi.push_back(t1 - t0);
        up.push_back(t2 - t1);
    }
    Log("Snapshot copy (%dx%d): GDI buffer -> memory %.2f ms, memory -> GPU %.2f ms (median of 12)\n", W, H,
        Pct(gdi, 50), Pct(up, 50));

    // Faster route: GDI copies straight into a GPU image that GDI can draw on.
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = W;
    d.Height = H;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    ComPtr<ID3D11Texture2D> gdiTex;
    ComPtr<IDXGISurface1> surf;
    std::vector<double> direct;
    if (SUCCEEDED(gDev->CreateTexture2D(&d, nullptr, &gdiTex)) && SUCCEEDED(gdiTex.As(&surf))) {
        for (int i = 0; i < 12; i++) {
            double t0 = NowMs();
            HDC sdc = nullptr;
            if (FAILED(surf->GetDC(FALSE, &sdc))) {
                break;
            }
            BitBlt(sdc, 0, 0, W, H, srcDC, 0, 0, SRCCOPY);
            surf->ReleaseDC(nullptr);
            gCtx->End(ev.Get());
            gCtx->Flush();
            BOOL done = FALSE;
            while (gCtx->GetData(ev.Get(), &done, sizeof(done), 0) == S_FALSE) {
            }
            direct.push_back(NowMs() - t0);
        }
    }
    Log("Snapshot copy, direct route: GDI buffer -> GPU %.2f ms (median of %zu)\n", Pct(direct, 50), direct.size());

    SelectObject(dibDC, oldDib);
    DeleteObject(dib);
    DeleteDC(dibDC);
    SelectObject(srcDC, oldSrc);
    DeleteObject(srcBmp);
    DeleteDC(srcDC);
    ReleaseDC(gHwnd, wdc);
}

// The layer is shown only while Curls play: cost of taking it off screen and back.
static void MeasureShowHide() {
    std::vector<double> hide, show;
    for (int i = 0; i < 10; i++) {
        double t0 = NowMs();
        gVisual->SetContent(nullptr);
        gDcomp->Commit();
        gDcomp->WaitForCommitCompletion();
        double t1 = NowMs();
        gVisual->SetContent(gSwap.Get());
        gDcomp->Commit();
        gDcomp->WaitForCommitCompletion();
        double t2 = NowMs();
        hide.push_back(t1 - t0);
        show.push_back(t2 - t1);
    }
    Log("Layer hide %.2f ms, show %.2f ms (median of 10, until the compositor has it)\n", Pct(hide, 50),
        Pct(show, 50));
}

static void LogVideoMemory() {
    ComPtr<IDXGIAdapter3> a3;
    if (FAILED(gAdapter.As(&a3))) {
        return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, nonLocal = {};
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
    Log("GPU memory used by this process: %llu MB local + %llu MB shared\n",
        (unsigned long long)(local.CurrentUsage >> 20), (unsigned long long)(nonLocal.CurrentUsage >> 20));
}

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&gFreq);
    ParseArgs(argc, argv);
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        printf("SetProcessDpiAwarenessContext failed: %lu\n", GetLastError());
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::string outPath = gOpt.outDir + "\\" + gOpt.label + ".txt";
    gOut = fopen(outPath.c_str(), "w");

    DWM_TIMING_INFO ti = {sizeof(ti)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) {
        gRefreshMs = (double)ti.qpcRefreshPeriod * 1000.0 / (double)gFreq.QuadPart;
    }
    Log("=== %s ===\n", gOpt.label.c_str());
    ListAdapters();

    double t0 = NowMs();
    if (!CreateWin()) {
        return 1;
    }
    Log("Window client %dx%d, screen refresh %.2f ms (%.0f Hz)\n", W, H, gRefreshMs, 1000.0 / gRefreshMs);

    double t1 = NowMs();
    if (!CreateDevice()) {
        return 1;
    }
    double t2 = NowMs();
    if (!CreateLayer()) {
        return 1;
    }
    double t3 = NowMs();
    if (!CreatePipeline()) {
        return 1;
    }
    double t4 = NowMs();
    LayoutSpread();
    if (!MakeTextures()) {
        return 1;
    }
    double t5 = NowMs();
    Log("Setup: window %.0f ms, D3D device %.0f ms, layer %.0f ms, shaders+pipeline %.0f ms, page images %.0f ms\n",
        t1 - t0, t2 - t1, t3 - t2, t4 - t3, t5 - t4);
    (void)t0;

    LogVideoMemory();
    MeasureSnapshot();
    MeasureShowHide();

    Scenario scenarios[] = {
        {"see-through, 1 Curl", 1, false, false, "see-through"},
        {"1 Curl", 1, true, false, nullptr},
        {"5 Curls", 5, true, false, "5-curls"},
        {"5 Curls + Bundle", 5, true, true, "5-curls-bundle"},
    };
    for (auto& sc : scenarios) {
        if (!RunScenario(sc)) {
            break;
        }
    }
    LogVideoMemory();

    if (gOut) {
        fclose(gOut);
    }
    DestroyWindow(gHwnd);
    return 0;
}

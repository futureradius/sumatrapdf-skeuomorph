/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Timer.h"
#include "base/File.h"

#include "gui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"

#include "skeuomorph/PageTurnLogic.h"
#include "skeuomorph/CurlD3D.h"
#include "skeuomorph/CurlLayer.h"
#include "skeuomorph/ScreenCopy.h"
#include "skeuomorph/FlashRecorder.h"
#include "skeuomorph/CurlRenderer.h"
#include "skeuomorph/CurlShaders.h"

using namespace pageturn;

// One drawing thread per window. The UI thread only queues commands; the
// drawing thread owns Direct3D, the layer and every Present:
//
//   UI thread                         drawing thread
//   CurlCopyOld  -- copy (waits) -->  old Spread -> picture
//   CurlStart    -- Start ------->    first frame, Present, show layer
//     waits for "covered"  <--------  next frame signal: it's on screen
//   SumatraPDF repaints the new Spread under the layer
//   CurlWindowShowsNew -- copy, ShowsNew -->  landing page -> picture;
//                                     uncovered parts see-through one refresh
//                                     later; landed + see-through: layer goes
//
// Idle, it sleeps on its wake event. While a Curl plays it waits on the swap
// chain's frame-latency handle, draws from the clock, then Present(1, 0). No
// timers, so Curls keep playing while a menu or dialog is open.

// ADR 0005: usually 9-12 ms, ~35 ms at 4K
constexpr int kCoverWaitMs = 100;
constexpr int kLandWaitMs = 50;
// the layer never stays longer than this, whatever SumatraPDF does
constexpr double kStuckMs = 4000;
// idle: wake now and then to notice a window that died without CurlDestroy
constexpr int kIdleWaitMs = 1000;
constexpr int kFrameWaitMs = 100;
constexpr int kJoinMs = 2000;
// refreshes between SumatraPDF's flush and going see-through (ADR 0005)
constexpr int kSeeThroughRefreshes = 1;
// a frame is late when its gap is over 1.5 refreshes
constexpr double kLateFactor = 1.5;

// the plain cylinder of build step 4 (spec 3.3); corner lead and shadow come later
constexpr float kPi = 3.14159265f;
constexpr float kRadiusPerWidth = 0.17f;
constexpr float kShrinkFrom = 0.6f; // the radius shrinks to 0 over the last 40%
constexpr float kSingleFadeFrom = 0.7f;
constexpr int kGridX = 48;
constexpr int kGridY = 64;

// screen copies: the next Curl's old Spread, the flying Curl's, its landing page
enum Slot {
    kSlotNextOld = 0,
    kSlotOld,
    kSlotLanding,
    kSlotCount
};

// matches cbuffer CB in Curl.hlsl
struct Consts {
    float viewSize[2];
    float pagePos[2];
    float pageSize[2];
    float foldPoint[2];
    float foldNormal[2];
    float radius;
    float mode;
    float frontImg[4];
    float backImg[4];
    float mirrorX2;
    float backY;
    float backScaleY;
    float alpha;
    float paper[4];
};
static_assert(sizeof(Consts) % 16 == 0, "constant buffers come in 16 byte rows");

enum Mode {
    kModeFlat = 0,
    kModeCurl = 1,
    kModePaper = 2
};

// everything a Curl needs, copied into the drawing thread
struct TurnData {
    int id = 0;
    double t0 = 0;
    CurlShape shape = CurlShape::Spread;
    Sweep sweep = Sweep::ToLeft;
    int durationMs = 0;
    CurlSpread from;
    CurlSpread to;
    Color paper = 0;
    Size canvas;
};

enum class CmdKind {
    Start,
    ShowsNew,
    LandAll,
    Resize,
    Quit
};

struct Cmd {
    CmdKind kind = CmdKind::Quit;
    TurnData turn;
    CurlSpread now;           // ShowsNew
    bool haveLanding = false; // ShowsNew: the landing page was copied
    Size size;                // Resize
    const char* why = "";     // LandAll, Resize: for the log
};

enum class Load {
    Loading,
    Loaded,
    Failed
};

struct Renderer {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    Size startSize;
    ThreadHandle thread = nullptr;
    HANDLE wake = nullptr;
    HANDLE covered = nullptr;
    HANDLE gone = nullptr;
    ScreenCopyDesk* desk = nullptr;

    // shared, under mutex
    Mutex mutex;
    Vec<Cmd> cmds;
    Load load = Load::Loading;
    int started = 0;   // last Curl queued
    int coveredId = 0; // its first frame is on screen
    int goneId = 0;    // Curls up to this one landed and the layer went
    CurlTimes times;
    int uiWaitId = 0;
    double uiWaitMs = 0;

    // UI thread only
    CurlSpread from;
    bool haveFrom = false;
    CurlShape shape = CurlShape::Spread;
    Sweep sweep = Sweep::ToLeft;
};

static Vec<Renderer*> gRenderers;

double CurlNowMs() {
    static TimeStamp start = TimeGet();
    return TimeSinceInMs(start);
}

static Rect UnionOf(Rect a, Rect b) {
    if (a.IsEmpty()) {
        return b;
    }
    if (b.IsEmpty()) {
        return a;
    }
    return a.Union(b);
}

static void Post(Renderer* r, const Cmd& c) {
    r->mutex.Lock();
    VecAppend(r->cmds, c);
    r->mutex.Unlock();
    SetEvent(r->wake);
}

//--- drawing thread: Direct3D

struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    CurlLayer* layer = nullptr;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> meshVB;
    ComPtr<ID3D11Buffer> meshIB;
    ComPtr<ID3D11Buffer> quadVB;
    ComPtr<ID3D11Buffer> quadIB;
    ComPtr<ID3D11Buffer> consts;
    UINT meshCount = 0;
    UINT quadCount = 0;
    ComPtr<ID3D11SamplerState> samp;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rast;
    ComPtr<ID3D11DepthStencilState> depth;
    CurlPicture pics[kSlotCount];
};

// d3d11.dll is loaded by hand, nothing is linked
static PFN_D3D11_CREATE_DEVICE LoadD3D11() {
    static PFN_D3D11_CREATE_DEVICE fn = [] {
        HMODULE dll = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dll ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(dll, "D3D11CreateDevice") : nullptr;
    }();
    return fn;
}

// default adapter (the discrete GPU wakes slowly, ADR 0001); software (WARP)
// if there's no hardware one
static bool MakeDevice(Gpu& g, bool* warp) {
    PFN_D3D11_CREATE_DEVICE create = LoadD3D11();
    if (!create) {
        return false;
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    for (D3D_DRIVER_TYPE type : types) {
        HRESULT hr = create(nullptr, type, nullptr, flags, levels, (UINT)dimofi(levels), D3D11_SDK_VERSION, &g.dev,
                            nullptr, &g.ctx);
        if (SUCCEEDED(hr)) {
            *warp = type == D3D_DRIVER_TYPE_WARP;
            return true;
        }
    }
    return false;
}

// a grid of gx x gy cells over 0..1, as triangles
static bool MakeGrid(Gpu& g, int gx, int gy, ComPtr<ID3D11Buffer>& vb, ComPtr<ID3D11Buffer>& ib, UINT* count) {
    Vec<float> v;
    for (int y = 0; y <= gy; y++) {
        for (int x = 0; x <= gx; x++) {
            VecAppend(v, (float)x / gx);
            VecAppend(v, (float)y / gy);
        }
    }
    Vec<u32> idx;
    for (int y = 0; y < gy; y++) {
        for (int x = 0; x < gx; x++) {
            u32 i0 = (u32)(y * (gx + 1) + x);
            u32 i1 = i0 + 1;
            u32 i2 = i0 + (u32)gx + 1;
            u32 i3 = i2 + 1;
            u32 tri[6] = {i0, i1, i2, i1, i3, i2};
            VecAppendN(idx, tri, 6);
        }
    }
    *count = (UINT)len(idx);

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = (UINT)(len(v) * sizeof(float));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA d{};
    d.pSysMem = v.els;
    if (FAILED(g.dev->CreateBuffer(&bd, &d, &vb))) {
        return false;
    }
    bd.ByteWidth = (UINT)(len(idx) * sizeof(u32));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    d.pSysMem = idx.els;
    return SUCCEEDED(g.dev->CreateBuffer(&bd, &d, &ib));
}

static bool MakePipeline(Gpu& g) {
    ID3D11Device* dev = g.dev.Get();
    if (FAILED(dev->CreateVertexShader(gCurlVS, sizeof(gCurlVS), nullptr, &g.vs)) ||
        FAILED(dev->CreatePixelShader(gCurlPS, sizeof(gCurlPS), nullptr, &g.ps))) {
        return false;
    }
    D3D11_INPUT_ELEMENT_DESC el = {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    if (FAILED(dev->CreateInputLayout(&el, 1, gCurlVS, sizeof(gCurlVS), &g.inputLayout))) {
        return false;
    }
    if (!MakeGrid(g, kGridX, kGridY, g.meshVB, g.meshIB, &g.meshCount) ||
        !MakeGrid(g, 1, 1, g.quadVB, g.quadIB, &g.quadCount)) {
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Consts);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g.consts))) {
        return false;
    }

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&smp, &g.samp))) {
        return false;
    }

    // premultiplied alpha, like the swap chain
    D3D11_BLEND_DESC bl{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = bl.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(dev->CreateBlendState(&bl, &g.blend))) {
        return false;
    }

    // both faces: the back of the turning page shows the landing page
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &g.rast))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    return SUCCEEDED(dev->CreateDepthStencilState(&ds, &g.depth));
}

static TempStr AdapterNameTemp(Gpu& g) {
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(g.dev.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) {
        return StrL("?");
    }
    return ToUtf8Temp(WStr((const WCHAR*)desc.Description));
}

static bool Setup(Renderer* r, Gpu& g, bool* warp, const char** why) {
    if (!MakeDevice(g, warp)) {
        *why = "no Direct3D 11 device";
        return false;
    }
    g.layer = NewCurlLayer(g.dev.Get(), r->hwnd, r->startSize, why);
    if (!g.layer) {
        return false;
    }
    if (!MakePipeline(g)) {
        *why = "Direct3D pipeline failed";
        return false;
    }
    return true;
}

//--- drawing thread: one Curl

// where the pieces of a Curl are, canvas px
struct Geometry {
    Rect sheet;         // old page that turns over: front old page, back landing page
    Rect stays;         // landing side: the old screen until the sheet covers it
    Rect landing;       // new page the sheet lands on; empty in Single Page
    Rect revealed;      // new page uncovered as the sheet lifts
    float mirrorX2 = 0; // twice the spine's x: the sheet lands mirrored around it
};

static Geometry GeometryOf(const TurnData& t) {
    Geometry g;
    bool toLeft = t.sweep != Sweep::ToRight;
    if (t.shape == CurlShape::Single) {
        // the page turns over its own edge, its back is blank paper
        g.sheet = t.from.left;
        g.revealed = t.to.left;
        g.mirrorX2 = toLeft ? 2.0f * g.sheet.x : 2.0f * g.sheet.Right();
        return g;
    }

    // forward in Facing: the right page curls over to the left
    Rect fromLead = toLeft ? t.from.right : t.from.left;
    Rect fromStay = toLeft ? t.from.left : t.from.right;
    g.sheet = fromLead.IsEmpty() ? fromStay : fromLead;
    g.landing = toLeft ? t.to.left : t.to.right;
    g.revealed = toLeft ? t.to.right : t.to.left;
    g.stays = UnionOf(fromLead.IsEmpty() ? Rect() : fromStay, g.landing);
    if (g.landing.IsEmpty()) {
        g.mirrorX2 = toLeft ? 2.0f * g.sheet.x : 2.0f * g.sheet.Right();
    } else {
        g.mirrorX2 = toLeft ? (float)(g.landing.Right() + g.sheet.x) : (float)(g.sheet.Right() + g.landing.x);
    }
    return g;
}

// the Curl in the air, drawing thread only
struct Flight {
    TurnData turn;
    Geometry geo;
    bool animating = false;
    bool needCover = false;    // the next Present is the covering frame
    bool pendingCover = false; // the next frame signal says it's on screen
    bool showsNew = false;
    bool seeThrough = false;
    bool landedShown = false;
    bool haveLanding = false;
    int refreshesLeft = -1;
    double lastPresent = 0;
    double refreshMs = 1000.0 / 60;
    CurlTimes times;
};

enum class Scene {
    Empty,
    Curl,
    Landed
};

enum class LandHow {
    Done,
    AtOnce,
    TimedOut
};

static double RefreshMs(HWND hwnd) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        return 1000.0 / 60;
    }
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) {
        return 1000.0 / 60;
    }
    return 1000.0 / dm.dmDisplayFrequency;
}

static void SetRect4(float* dst, Rect r) {
    dst[0] = (float)r.x;
    dst[1] = (float)r.y;
    dst[2] = (float)r.dx;
    dst[3] = (float)r.dy;
}

static void SetPicture(float* dst, const CurlPicture& pic) {
    if (!pic.ok) {
        SetRect4(dst, Rect());
        return;
    }
    SetRect4(dst, pic.area);
}

static Consts BaseConsts(Gpu& g, const TurnData& t, Rect page) {
    Consts c{};
    Size view = CurlLayerSize(g.layer);
    c.viewSize[0] = (float)view.dx;
    c.viewSize[1] = (float)view.dy;
    c.pagePos[0] = (float)page.x;
    c.pagePos[1] = (float)page.y;
    c.pageSize[0] = (float)page.dx;
    c.pageSize[1] = (float)page.dy;
    c.radius = 1;
    c.alpha = 1;
    c.paper[0] = GetRed(t.paper) / 255.0f;
    c.paper[1] = GetGreen(t.paper) / 255.0f;
    c.paper[2] = GetBlue(t.paper) / 255.0f;
    c.paper[3] = 1;
    return c;
}

static void Draw(Gpu& g, const Consts& c, const CurlPicture* front, const CurlPicture* back, bool mesh) {
    g.ctx->UpdateSubresource(g.consts.Get(), 0, nullptr, &c, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = {front && front->ok ? front->srv.Get() : nullptr,
                                         back && back->ok ? back->srv.Get() : nullptr};
    g.ctx->PSSetShaderResources(0, 2, srvs);
    UINT stride = 2 * sizeof(float);
    UINT offset = 0;
    ID3D11Buffer* vb = mesh ? g.meshVB.Get() : g.quadVB.Get();
    g.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g.ctx->IASetIndexBuffer(mesh ? g.meshIB.Get() : g.quadIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    g.ctx->DrawIndexed(mesh ? g.meshCount : g.quadCount, 0, 0);
}

// a flat, pixel-exact piece of a screen copy
static void DrawFlat(Gpu& g, const TurnData& t, Rect r, const CurlPicture& pic) {
    if (r.IsEmpty()) {
        return;
    }
    Consts c = BaseConsts(g, t, r);
    c.mode = kModeFlat;
    SetPicture(c.frontImg, pic);
    Draw(g, c, &pic, nullptr, false);
}

static void DrawPaper(Gpu& g, const TurnData& t, Rect r) {
    if (r.IsEmpty()) {
        return;
    }
    Consts c = BaseConsts(g, t, r);
    c.mode = kModePaper;
    Draw(g, c, nullptr, nullptr, false);
}

// the cylinder: the fold sweeps from the free edge past the spine while the
// radius shrinks, so the sheet lands flat and mirrored on the other side
static void DrawSheet(Gpu& g, const Flight& f, float t) {
    const Geometry& geo = f.geo;
    if (geo.sheet.IsEmpty()) {
        return;
    }
    float w = (float)geo.sheet.dx;
    float p = 0.5f - 0.5f * cosf(kPi * t);
    float radius = kRadiusPerWidth * w;
    if (t > kShrinkFrom) {
        radius *= (1 - t) / (1 - kShrinkFrom);
    }
    radius = std::max(radius, 1.0f);

    // in sheet px; mirrored for a sweep to the right
    bool toLeft = f.turn.sweep != Sweep::ToRight;
    float spine = geo.mirrorX2 / 2;
    float halfGap = toLeft ? geo.sheet.x - spine : spine - geo.sheet.Right();
    float startX = w + 1;
    float endX = -halfGap - kPi * radius / 2;
    float x = startX + (endX - startX) * p;

    Consts c = BaseConsts(g, f.turn, geo.sheet);
    c.mode = kModeCurl;
    c.foldPoint[0] = toLeft ? x : w - x;
    c.foldNormal[0] = toLeft ? 1.0f : -1.0f;
    c.radius = radius;
    const CurlPicture& front = g.pics[kSlotOld];
    SetPicture(c.frontImg, front);
    const CurlPicture* back = nullptr;
    if (f.haveLanding && !geo.landing.IsEmpty()) {
        back = &g.pics[kSlotLanding];
        SetPicture(c.backImg, *back);
        c.mirrorX2 = geo.mirrorX2;
        c.backY = (float)geo.landing.y;
        c.backScaleY = (float)geo.landing.dy / std::max(geo.sheet.dy, 1);
    }
    // Single Page: once over, the page fades away
    if (f.turn.shape == CurlShape::Single && t > kSingleFadeFrom) {
        c.alpha = std::max(0.0f, 1 - (t - kSingleFadeFrom) / (1 - kSingleFadeFrom));
    }
    Draw(g, c, &front, back, true);
}

// Uncovered parts are blank paper until SumatraPDF shows the new Spread, then
// see-through. Landed: the landing page is the copy of what's beneath, so the
// last frame matches SumatraPDF's own picture
static void RenderFrame(Gpu& g, const Flight& f, float t, Scene scene) {
    ID3D11RenderTargetView* rtv = CurlLayerTarget(g.layer);
    float clear[4] = {0, 0, 0, 0};
    g.ctx->OMSetRenderTargets(1, &rtv, CurlLayerDepth(g.layer));
    g.ctx->ClearRenderTargetView(rtv, clear);
    g.ctx->ClearDepthStencilView(CurlLayerDepth(g.layer), D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (scene == Scene::Empty) {
        return;
    }

    Size view = CurlLayerSize(g.layer);
    D3D11_VIEWPORT vp = {0, 0, (float)view.dx, (float)view.dy, 0, 1};
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(g.rast.Get());
    g.ctx->OMSetBlendState(g.blend.Get(), nullptr, 0xffffffff);
    g.ctx->OMSetDepthStencilState(g.depth.Get(), 0);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(g.inputLayout.Get());
    g.ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g.ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, g.consts.GetAddressOf());
    g.ctx->PSSetConstantBuffers(0, 1, g.consts.GetAddressOf());
    g.ctx->PSSetSamplers(0, 1, g.samp.GetAddressOf());

    const Geometry& geo = f.geo;
    if (scene == Scene::Curl) {
        DrawFlat(g, f.turn, geo.stays, g.pics[kSlotOld]);
    }
    if (!f.seeThrough) {
        DrawPaper(g, f.turn, geo.revealed);
    }
    if (scene == Scene::Curl) {
        DrawSheet(g, f, t);
        return;
    }
    if (geo.landing.IsEmpty()) {
        return;
    }
    if (f.haveLanding) {
        DrawFlat(g, f.turn, geo.landing, g.pics[kSlotLanding]);
    } else {
        DrawPaper(g, f.turn, geo.landing);
    }
}

// the drawing thread's times -> shared, keeping the UI thread's wait
static void Publish(Renderer* r, const Flight& f) {
    r->mutex.Lock();
    r->times = f.times;
    r->times.uiWaitMs = r->uiWaitId == f.times.id ? r->uiWaitMs : 0;
    r->mutex.Unlock();
}

// SKEUOMORPH_PERF_LOG=<file>: the same line per Curl appended there, also in
// Release builds, for the speed check (tests/skeuomorph-speed.ts)
static Str PerfLogPath() {
    static Str path = [] {
        WCHAR buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"SKEUOMORPH_PERF_LOG", buf, dimof(buf));
        return n > 0 && n < dimof(buf) ? str::Dup(ToUtf8Temp(WStr(buf))) : Str();
    }();
    return path;
}

static void AppendPerfLog(Str path, Str line) {
    file::FileHandle h = file::OpenReadWrite(path, true);
    if (h == file::kInvalidFileHandle) {
        return;
    }
    file::SeekEnd(h);
    file::WriteAll(h, line);
    file::Close(h);
}

// one line per Curl, beside the per-turn line (another prefix, so the
// tests/skeuomorph-*.ts parsers don't count it):
//   skeuomorph-curl: #3 landed after 702 ms, 42 frames, 0 late, covered 12.8 ms, UI waited 12.1 ms
//   skeuomorph-curl: #4 landed at once (resize) after 310 ms, 18 frames, ...
static void LogFlight(Renderer* r, const Flight& f, LandHow how, const char* why) {
    Str perfPath = PerfLogPath();
    if (!IS_DEBUG && len(perfPath) == 0) {
        return;
    }
    r->mutex.Lock();
    double uiWait = r->uiWaitId == f.times.id ? r->uiWaitMs : 0;
    r->mutex.Unlock();
    const CurlTimes& tm = f.times;
    TempStr what = StrL("TIMEOUT");
    if (how == LandHow::Done) {
        what = StrL("landed");
    } else if (how == LandHow::AtOnce) {
        what = fmt("landed at once (%s)", Str(why));
    }
    double covered = tm.covered > 0 ? tm.covered - tm.start : -1;
    TempStr line = fmt("skeuomorph-curl: #%d %s after %d ms, %d frames, %d late, covered %.1f ms, UI waited %.1f ms\n",
                       tm.id, what, (int)(tm.gone - tm.start), tm.frames, tm.lateFrames, covered, uiWait);
    if (IS_DEBUG) {
        logf("%s", line);
    }
    if (len(perfPath) > 0) {
        AppendPerfLog(perfPath, line);
    }
}

// the layer goes now; tell a waiting UI thread. why: for the log, landed at once
static void Land(Renderer* r, Gpu& g, Flight& f, LandHow how, const char* why = "") {
    if (!f.animating) {
        return;
    }
    if (CurlLayerShown(g.layer)) {
        CurlLayerShow(g.layer, false);
    }
    f.animating = false;
    g.pics[kSlotOld] = CurlPicture();
    g.pics[kSlotLanding] = CurlPicture();
    f.times.gone = CurlNowMs();
    f.times.timedOut = how == LandHow::TimedOut;
    f.times.landedAtOnce = how == LandHow::AtOnce;
    Publish(r, f);
    LogFlight(r, f, how, why);

    r->mutex.Lock();
    r->coveredId = std::max(r->coveredId, f.turn.id);
    r->goneId = std::max(r->goneId, f.turn.id);
    r->mutex.Unlock();
    SetEvent(r->covered);
    SetEvent(r->gone);
}

// one Curl at a time in this step: a new one lands the one in the air and
// starts from the old Spread copied at its own tap
static void StartFlight(Renderer* r, Gpu& g, Flight& f, const TurnData& turn) {
    bool replacing = f.animating;
    if (replacing) {
        f.animating = false;
        f.times.gone = CurlNowMs();
        f.times.landedAtOnce = true;
        LogFlight(r, f, LandHow::AtOnce, "next Curl");
        r->mutex.Lock();
        r->goneId = std::max(r->goneId, f.turn.id);
        r->mutex.Unlock();
    }

    // the swap chain follows the canvas; its buffers are undefined until the next Present
    Size size = turn.canvas;
    if (!size.IsEmpty() && !(size == CurlLayerSize(g.layer))) {
        if (CurlLayerShown(g.layer)) {
            CurlLayerShow(g.layer, false);
        }
        CurlLayerResize(g.layer, size);
    }

    // the next Curl must never fly this one's old Spread
    // test of the test (FlashRecorder): the first frame comes late
    if (int lagMs = FlashLayerLagMs()) {
        Sleep((DWORD)lagMs);
    }
    std::swap(g.pics[kSlotOld], g.pics[kSlotNextOld]);
    g.pics[kSlotNextOld].ok = false;
    f = Flight();
    f.turn = turn;
    f.geo = GeometryOf(turn);
    f.animating = true;
    f.needCover = true;
    f.refreshMs = RefreshMs(r->hwnd);
    f.times.id = turn.id;
    f.times.start = turn.t0;
    Publish(r, f);
}

static void OnShowsNew(Flight& f, const Cmd& c) {
    if (!f.animating) {
        return;
    }
    f.turn.to = c.now;
    f.geo = GeometryOf(f.turn);
    f.haveLanding = c.haveLanding;
    if (!f.showsNew) {
        f.showsNew = true;
        f.times.showsNew = CurlNowMs();
    }
}

// false: the device is gone, stop drawing
static bool DrawFrame(Renderer* r, Gpu& g, Flight& f) {
    double now = CurlNowMs();
    if (f.pendingCover) {
        // the compositor took the covering frame: it's on screen
        f.pendingCover = false;
        f.times.covered = now;
        r->mutex.Lock();
        r->coveredId = std::max(r->coveredId, f.turn.id);
        r->mutex.Unlock();
        Publish(r, f);
        SetEvent(r->covered);
    }

    if (f.showsNew && !f.seeThrough) {
        if (f.refreshesLeft < 0) {
            f.refreshesLeft = kSeeThroughRefreshes;
        } else if (f.refreshesLeft > 0) {
            f.refreshesLeft--;
        }
        if (f.refreshesLeft == 0) {
            f.seeThrough = true;
            f.times.seeThrough = now;
        }
    }

    double elapsed = now - f.turn.t0;
    float t = (float)std::min(1.0, elapsed / std::max(f.turn.durationMs, 1));
    bool landed = t >= 1;
    bool done = landed && f.landedShown && f.seeThrough;
    if (done || elapsed > kStuckMs) {
        // this frame signal is used up: present a see-through frame, or the
        // latency handle never signals again
        RenderFrame(g, f, t, Scene::Empty);
        CurlLayerPresent(g.layer);
        Land(r, g, f, done ? LandHow::Done : LandHow::TimedOut);
        return true;
    }

    RenderFrame(g, f, t, landed ? Scene::Landed : Scene::Curl);
    if (!CurlLayerPresent(g.layer)) {
        Land(r, g, f, LandHow::AtOnce, "Present failed");
        return false;
    }
    double presented = CurlNowMs();
    if (f.lastPresent > 0) {
        f.times.frames++;
        if (presented - f.lastPresent > kLateFactor * f.refreshMs) {
            f.times.lateFrames++;
        }
    }
    f.lastPresent = presented;

    if (f.needCover) {
        f.needCover = false;
        if (!CurlLayerShown(g.layer)) {
            CurlLayerShow(g.layer, true);
        }
        f.times.coverShown = presented;
        f.pendingCover = true;
    }
    if (landed && !f.landedShown) {
        f.landedShown = true;
        f.times.landed = presented;
    }
    return true;
}

static void TakeCmds(Renderer* r, Vec<Cmd>& out) {
    r->mutex.Lock();
    out = r->cmds;
    VecClear(r->cmds);
    r->mutex.Unlock();
}

// a frame signal taken with nothing left to draw (a Curl landed at once just
// then): give it back with a see-through frame, or the latency handle never
// signals again and every later Curl times out
static void ReturnFrame(Gpu& g, const Flight& f) {
    if (!CurlLayerTarget(g.layer)) {
        return;
    }
    RenderFrame(g, f, 0, Scene::Empty);
    CurlLayerPresent(g.layer);
}

static void DrawLoop(Renderer* r, Gpu& g) {
    Flight f;
    Vec<Cmd> cmds;
    while (true) {
        HANDLE handles[2] = {r->wake, CurlLayerFrameWait(g.layer)};
        DWORD n = f.animating ? 2 : 1;
        DWORD res = WaitForMultipleObjects(n, handles, FALSE, f.animating ? kFrameWaitMs : kIdleWaitMs);
        bool frameSignal = res == WAIT_OBJECT_0 + 1;
        if (!IsWindow(r->hwnd)) {
            return;
        }

        // copies first: the UI thread is waiting for them
        ScreenCopyServe(r->desk, g.dev.Get(), g.pics, kSlotCount);

        TakeCmds(r, cmds);
        for (const Cmd& c : cmds) {
            switch (c.kind) {
                case CmdKind::Quit:
                    Land(r, g, f, LandHow::AtOnce, "window closed");
                    return;
                case CmdKind::LandAll:
                    Land(r, g, f, LandHow::AtOnce, c.why);
                    break;
                case CmdKind::Resize:
                    Land(r, g, f, LandHow::AtOnce, c.why);
                    CurlLayerResize(g.layer, c.size);
                    break;
                case CmdKind::Start:
                    StartFlight(r, g, f, c.turn);
                    break;
                case CmdKind::ShowsNew:
                    OnShowsNew(f, c);
                    break;
            }
        }
        if (!f.animating) {
            if (frameSignal) {
                ReturnFrame(g, f);
            }
            continue;
        }

        // only a frame signal may draw: then Present doesn't block
        if (!frameSignal) {
            if (CurlNowMs() - f.turn.t0 > kStuckMs) {
                Land(r, g, f, LandHow::TimedOut);
            }
            continue;
        }
        if (!DrawFrame(r, g, f)) {
            logf("skeuomorph-layer: Present failed, Curls are off in this window\n");
            return;
        }
    }
}

static void DrawThread(Renderer* r) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Gpu* g = new Gpu();
    const char* why = nullptr;
    bool warp = false;
    TimeStamp t = TimeGet();
    bool ok = Setup(r, *g, &warp, &why);
    if (ok) {
        Str soft = warp ? StrL(" (WARP)") : StrL("");
        logf("skeuomorph-layer: loaded in %.1f ms on %s%s\n", TimeSinceInMs(t), AdapterNameTemp(*g), soft);
    } else {
        logf("skeuomorph-layer: not loaded (%s); Page Turns are instant jumps\n", Str(why ? why : "?"));
    }
    r->mutex.Lock();
    r->load = ok ? Load::Loaded : Load::Failed;
    r->mutex.Unlock();

    if (ok) {
        DrawLoop(r, *g);
    }

    r->mutex.Lock();
    r->load = Load::Failed;
    r->goneId = r->started;
    r->mutex.Unlock();
    SetEvent(r->covered);
    SetEvent(r->gone);

    DeleteCurlLayer(g->layer);
    delete g;
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}

//--- UI thread

// sent messages are handled while waiting, in case tearing the layer down
// needs the window's thread
static bool JoinPumping(ThreadHandle h, int timeoutMs) {
    TimeStamp start = TimeGet();
    while (true) {
        int left = timeoutMs - (int)TimeSinceInMs(start);
        if (left <= 0) {
            return false;
        }
        DWORD res = MsgWaitForMultipleObjects(1, &h, FALSE, (DWORD)left, QS_SENDMESSAGE);
        if (res == WAIT_OBJECT_0) {
            return true;
        }
        if (res != WAIT_OBJECT_0 + 1) {
            return false;
        }
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

static void StopRenderer(Renderer* r) {
    if (r->thread) {
        Cmd c;
        c.kind = CmdKind::Quit;
        Post(r, c);
        if (!JoinPumping(r->thread, kJoinMs)) {
            // still running and using r: leave it be rather than free it under its feet
            logf("skeuomorph-layer: drawing thread didn't stop\n");
            return;
        }
        SafeCloseThreadHandle(&r->thread);
    }
    DeleteScreenCopyDesk(r->desk);
    CloseHandle(r->wake);
    CloseHandle(r->covered);
    CloseHandle(r->gone);
    delete r;
}

// also stops the drawing threads of closed windows
static Renderer* FindRenderer(MainWindow* win) {
    Renderer* found = nullptr;
    for (int i = len(gRenderers) - 1; i >= 0; i--) {
        Renderer* r = gRenderers[i];
        // a new window can get a closed one's address
        if (!IsMainWindowValid(r->win) || r->win->hwndCanvas != r->hwnd) {
            VecRemoveAt(gRenderers, i);
            StopRenderer(r);
            continue;
        }
        if (r->win == win) {
            found = r;
        }
    }
    return found;
}

static bool IsLoaded(Renderer* r) {
    r->mutex.Lock();
    bool loaded = r->load == Load::Loaded;
    r->mutex.Unlock();
    return loaded;
}

static bool InAir(Renderer* r) {
    r->mutex.Lock();
    bool inAir = r->goneId < r->started;
    r->mutex.Unlock();
    return inAir;
}

static Size CanvasSize(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return {rc.right - rc.left, rc.bottom - rc.top};
}

// lazy setup at the first Paged View paint with the switch on: starts the
// drawing thread, which loads Direct3D and the layer (~0.1 ms here)
void CurlEnsure(MainWindow* win) {
    if (!win || !win->hwndCanvas || FindRenderer(win)) {
        return;
    }
    auto r = new Renderer();
    r->win = win;
    r->hwnd = win->hwndCanvas;
    r->startSize = CanvasSize(r->hwnd);
    r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->covered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->gone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->desk = NewScreenCopyDesk(r->wake);
    VecAppend(gRenderers, r);

    r->thread = StartThread(MkFunc0<Renderer>(DrawThread, r), StrL("CurlDrawing"));
    if (!r->thread) {
        // never retried: Page Turns stay instant jumps in this window
        r->load = Load::Failed;
        logf("skeuomorph-layer: not loaded (no drawing thread)\n");
    }
}

bool CurlIsLoaded(MainWindow* win) {
    Renderer* r = FindRenderer(win);
    return r && IsLoaded(r);
}

bool CurlInAir(MainWindow* win) {
    Renderer* r = FindRenderer(win);
    return r && InAir(r);
}

// hook 3: win->buffer still holds the last painted frame. The whole width of
// the canvas, so a side with no page (the cover) has its background too
void CurlCopyOld(MainWindow* win, CurlSpread from) {
    Renderer* r = FindRenderer(win);
    if (!r || !IsLoaded(r)) {
        return;
    }
    Rect area = UnionOf(from.left, from.right);
    if (area.IsEmpty()) {
        return;
    }
    area.x = 0;
    area.dx = ScreenCopySize(win).dx;
    ScreenCopyAsk(r->desk, win, area, kSlotNextOld);
    r->from = from;
    r->haveFrom = true;
}

void CurlStart(MainWindow* win, const CurlTurn& turn) {
    Renderer* r = FindRenderer(win);
    if (!r || !IsLoaded(r) || !r->haveFrom) {
        return;
    }
    r->haveFrom = false;
    r->shape = turn.shape;
    r->sweep = turn.sweep;

    Cmd c;
    c.kind = CmdKind::Start;
    TurnData& t = c.turn;
    t.t0 = CurlNowMs();
    t.shape = turn.shape;
    t.sweep = turn.sweep;
    t.durationMs = turn.durationMs;
    t.from = r->from;
    t.to = turn.to;
    t.paper = turn.paper;
    t.canvas = ScreenCopySize(win);
    r->mutex.Lock();
    t.id = ++r->started;
    r->mutex.Unlock();
    Post(r, c);

    // ADR 0005: SumatraPDF repaints only once the Curl covers the screen,
    // or the new Spread would flash for a frame now and then
    int id = t.id;
    while (!FlashSkipsCoverWait()) {
        int left = kCoverWaitMs - (int)(CurlNowMs() - t.t0);
        if (left <= 0) {
            break;
        }
        WaitForSingleObject(r->covered, (DWORD)left);
        r->mutex.Lock();
        bool covered = r->coveredId >= id;
        r->mutex.Unlock();
        if (covered) {
            break;
        }
    }
    r->mutex.Lock();
    r->uiWaitId = id;
    r->uiWaitMs = CurlNowMs() - t.t0;
    r->times.uiWaitMs = r->times.id == id ? r->uiWaitMs : r->times.uiWaitMs;
    r->mutex.Unlock();
}

// hook 6, flushed: the screen shows SumatraPDF's new Spread or its message.
// Copied again at every flushed paint while the Curl is in the air, so a
// better picture ("Rendering page N..." -> the page) switches in at once
void CurlWindowShowsNew(MainWindow* win, CurlSpread now) {
    Renderer* r = FindRenderer(win);
    if (!r || !IsLoaded(r) || !InAir(r)) {
        return;
    }
    Rect landing;
    if (r->shape == CurlShape::Spread) {
        landing = r->sweep == Sweep::ToRight ? now.right : now.left;
    }
    Cmd c;
    c.kind = CmdKind::ShowsNew;
    c.now = now;
    c.haveLanding = !landing.IsEmpty() && ScreenCopyAsk(r->desk, win, landing, kSlotLanding);
    Post(r, c);
}

// lands everything at once: the layer disappears now
void CurlLandAll(MainWindow* win, const char* why) {
    Renderer* r = FindRenderer(win);
    if (!r || !InAir(r)) {
        return;
    }
    Cmd c;
    c.kind = CmdKind::LandAll;
    c.why = why;
    Post(r, c);

    // short: the layer should be gone before SumatraPDF's next paint
    double start = CurlNowMs();
    while (InAir(r)) {
        int left = kLandWaitMs - (int)(CurlNowMs() - start);
        if (left <= 0) {
            break;
        }
        WaitForSingleObject(r->gone, (DWORD)left);
    }
}

void CurlLandAllWindows(const char* why) {
    // CurlLandAll may drop closed windows' renderers from gRenderers
    Vec<MainWindow*> wins;
    for (Renderer* r : gRenderers) {
        VecAppend(wins, r->win);
    }
    for (MainWindow* win : wins) {
        CurlLandAll(win, why);
    }
}

// the canvas changed size: lands, then fits the swap chain to it
void CurlResize(MainWindow* win) {
    Renderer* r = FindRenderer(win);
    if (!r) {
        return;
    }
    constexpr const char* why = "resize";
    CurlLandAll(win, why);
    Cmd c;
    c.kind = CmdKind::Resize;
    c.size = CanvasSize(r->hwnd);
    c.why = why;
    Post(r, c);
}

// the window is going (WM_NCDESTROY): stops and joins the drawing thread
void CurlDestroy(MainWindow* win) {
    for (int i = 0; i < len(gRenderers); i++) {
        Renderer* r = gRenderers[i];
        if (r->win != win) {
            continue;
        }
        VecRemoveAt(gRenderers, i);
        StopRenderer(r);
        return;
    }
}

bool CurlLastTimes(MainWindow* win, CurlTimes* out) {
    Renderer* r = FindRenderer(win);
    if (!r) {
        return false;
    }
    r->mutex.Lock();
    *out = r->times;
    r->mutex.Unlock();
    return true;
}

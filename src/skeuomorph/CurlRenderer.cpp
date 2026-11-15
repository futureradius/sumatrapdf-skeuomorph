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
#include "skeuomorph/PageEdges.h"
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
//
// Every tap starts its own Curl: up to 5 are in the air, all drawn in each
// frame. A backward tap sends the newest back from where it is instead.

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

// the corner peel (spec 3.3), as in the prototype
constexpr float kPi = 3.14159265f;
constexpr float kRadiusPerWidth = 0.17f;
constexpr float kShrinkFrom = 0.6f; // the radius shrinks to 0 over the last 40%
constexpr float kMinRadius = 0.01f;
constexpr float kLeadDeg = 35;
constexpr float kLeadPow = 1.5f;
constexpr float kShadow = 0.3f;
constexpr float kShading = 0.3f;
constexpr float kDepth = 0.7f;
// prototype: camera = max(view height, 1.2 x sheet reach) x (14 - 12.6 x depth), ~5.2 x
constexpr float kCamPerHeight = 14 - 12.6f * kDepth;
constexpr float kCamPerReach = 1.2f;
// the shadow is 8 faint copies in a ring: a soft edge
constexpr int kShadowCopies = 8;
constexpr float kShadowPerCopy = 0.16f;
// several Curls in the air: fewer, darker copies each, or the GPU falls
// behind (5 sheets x 8 copies: late frames at 1638x1000)
constexpr int kMinShadowCopies = 3;
// Single Page: once over, the page fades away
constexpr float kSingleFadeFrom = 0.55f;
// pages of different sizes (#31): the view glides from the old Spread's fit
// to the new one's during the Curl. Page Edges tuck under over the first 15%
// of the Curl and slide out over as long after landing; a page beneath fades
// in or out over 35% where it sticks out past the sheet above it
constexpr float kEdgeTuck = 0.15f;
constexpr float kUnderFade = 0.35f;
// same size within 1%; a comic double spread: twice as wide (4%), as high (3%)
constexpr float kSameSize = 0.01f;
constexpr float kSpreadWide = 0.04f;
constexpr float kSpreadHigh = 0.03f;
// views this close are the same: no glide
constexpr float kSameZoom = 0.002f;
constexpr float kSamePx = 0.5f;
constexpr int kGridX = 48;
constexpr int kGridY = 64;
// a Bundle's sheets (spec 3.5): alternate a little in tone, darken along the
// free edge, and only the top and bottom ones cast a shadow, 30% stronger
constexpr float kSheetTint = 0.975f;
constexpr float kFreeEdgeDark = 0.45f;
constexpr float kBundleShadow = 1.3f;
// depth nudge between sheets lying on each other, px: keeps their order
constexpr float kStackNudge = 0.05f;
// Page Edges drawn by the layer: one row of column colours per side
constexpr int kEdgeCols = 256;

// screen copies land here, then move into their Curl: the next Curl's old
// Spread, and the newest Curl's landing page
enum Slot {
    kSlotNextOld = 0,
    kSlotLanding,
    kSlotCount
};
// CurlTimesOf looks this many Curls back
constexpr int kTimesKept = 16;
// a copy must end this long before the next frame is due
constexpr double kCopyMarginMs = 2;

// matches cbuffer CB in Curl.hlsl
struct Consts {
    float viewSize[2];
    float mode;
    float alpha;
    float paper[4];
    float rect[4];
    float frontImg[4];
    float backImg[4];
    float startRect[4];
    float endRect[4];
    float spineX;
    float dirX;
    float progress;
    float lead;
    float radius;
    float camDist;
    float shadowOff[2];
    float shadowAlpha;
    float shading;
    float backdropX;   // kModeBackdrop: a column of plain background; < 0: none
    float backdropY;   // or, without one, a row; < 0: none
    float frontSrc[4]; // canvas rect of the front face's picture (kModeImage: rect's)
    float backSrc[4];
    float keep[4];  // kModeImage: opaque inside; empty: none
    float look[4];  // kModeCurl: lift, depth nudge at start, at end, tint
    float stack[4]; // kModeCurl: free edge darkening; kModeEdges: -, -, page edge x, side
};
static_assert(sizeof(Consts) == 240, "matches cbuffer CB, in 16 byte rows");

enum Mode {
    kModeFlat = 0,
    kModeCurl = 1,
    kModePaper = 2,
    kModeShadow = 3,
    kModeBackdrop = 4,
    kModeImage = 5,
    kModeEdges = 6
};

// everything a Curl needs, copied into the drawing thread
struct TurnData {
    int id = 0; // the command: a new Curl, or one sent back
    int clockId = 0;
    bool back = false;
    TurnKind kind = TurnKind::Curl;
    Sheets sheets;
    int firstPage = 0;
    int splitOf = 0;
    int peeled = 0;
    Landed across;
    int dpi = 0;
    Pace pace;
    double t0 = 0;
    CurlShape shape = CurlShape::Spread;
    Sweep sweep = Sweep::ToLeft;
    bool forward = true;
    int durationMs = 0;
    CurlSpread from;
    CurlSpread to;
    Color paper = 0;
    int edgesPx = 0;
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
    int started = 0;   // last Start queued
    int coveredId = 0; // the frame showing it is on screen
    int goneId = 0;    // Starts up to this one are done: landed, or sent back again
    CurlTimes times[kTimesKept];
    int lastTimes = 0;

    // UI thread only
    CurlSpread from;
    bool haveFrom = false;
    CurlShape shape = CurlShape::Spread;
    Sweep sweep = Sweep::ToLeft;
    bool forward = true;
    bool glide = false;      // the whole new screen is copied
    bool split = false;      // so is a split-off sheet's: its Bundle lands under it
    bool landingCopy = true; // sent back: SumatraPDF shows the old Spread, nothing to copy
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

static float Lerp(float a, float b, float f) {
    return a + (b - a) * f;
}

static float SmoothStep(float a, float b, float x) {
    x = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return x * x * (3 - 2 * x);
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
    ComPtr<ID3D11RasterizerState> rastClipped; // shadows: only on paper
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11Texture2D> edgeTex; // Page Edges' column colours: row 0 left, 1 right
    ComPtr<ID3D11ShaderResourceView> edgeSrv;
    CurlPicture inbox[kSlotCount];
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
    rd.ScissorEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &g.rastClipped))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(dev->CreateDepthStencilState(&ds, &g.depth))) {
        return false;
    }
    ds.DepthEnable = FALSE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(dev->CreateDepthStencilState(&ds, &g.noDepth))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kEdgeCols;
    td.Height = 2;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g.edgeTex))) {
        return false;
    }
    return SUCCEEDED(dev->CreateShaderResourceView(g.edgeTex.Get(), nullptr, &g.edgeSrv));
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

// what the turning sheet shows on a face
enum class Face {
    Paper,   // blank paper
    Old,     // the old Spread's copy
    Landing, // the landing page's copy, once SumatraPDF shows it
};

enum class Fade {
    None,
    Out, // Single Page forward: once over, the page fades away
    In,  // Single Page backward: the page comes back from where it faded
};

// where the pieces of a Curl are, canvas px. The sheet lies flat at start,
// turns over the spine and lies flat at end, mirrored:
//
//   Facing forward         Single Page forward    Single Page backward
//   [land][sheet]          |[sheet]               [start]|[end]
//    end <- start           end <-| start                |  old page under it
//         spine                 spine (left edge)      spine (left edge)
struct Geometry {
    Rect start;
    Rect end;
    Face startFace = Face::Old;
    Face endFace = Face::Landing;
    float spineX = 0;
    float dirX = 1; // +1: start is right of the spine
    Fade fade = Fade::None;
    Rect stays;         // the old screen, until the sheet covers it
    Rect revealed;      // new page uncovered as the sheet lifts: blank paper until SumatraPDF shows it
    Rect landing;       // landed: the landing page's copy; empty: nothing
    Rect paperBelow[2]; // where the sheet's shadow may fall: paper, never background
};

static Rect MirrorRect(Rect r, float spineX) {
    r.x = (int)lroundf(2 * spineX) - r.Right();
    return r;
}

// the old screen beside the landing page keeps its Page Edges until the sheet
// lands, so they change when the paper does
static Rect WithEdges(Rect r, int edgesPx, Sweep sweep) {
    if (r.IsEmpty() || edgesPx <= 0) {
        return r;
    }
    r.dx += edgesPx;
    if (sweep != Sweep::ToRight) {
        r.x -= edgesPx;
    }
    return r;
}

// halfway between the start side's page and the end side's, at their inner
// edges; with one of them, at its inner edge
static float SpineX(Rect startSide, Rect endSide, bool toLeft) {
    float startEdge = (float)(toLeft ? startSide.x : startSide.Right());
    float endEdge = (float)(toLeft ? endSide.Right() : endSide.x);
    if (startSide.IsEmpty()) {
        return endEdge;
    }
    if (endSide.IsEmpty()) {
        return startEdge;
    }
    return (startEdge + endEdge) / 2;
}

// the part of r on the end side: the old screen there must not cover the
// turning page, nor the start side once that page lifts. Pages of different
// sizes can reach past the spine
static Rect EndSide(Rect r, float spineX, Rect fromLead, bool toLeft) {
    if (r.IsEmpty()) {
        return r;
    }
    int spine = toLeft ? (int)floorf(spineX) : (int)ceilf(spineX);
    if (!fromLead.IsEmpty()) {
        spine = toLeft ? std::min(spine, fromLead.x) : std::max(spine, fromLead.Right());
    }
    if (toLeft && r.Right() > spine) {
        r.dx = std::max(0, spine - r.x);
    } else if (!toLeft && r.x < spine) {
        r.dx = std::max(0, r.Right() - spine);
        r.x = spine;
    }
    return r;
}

// the parts of r outside hole: up to 4, the rest empty
static void Outside(Rect r, Rect hole, Rect (&out)[4]) {
    for (Rect& o : out) {
        o = Rect();
    }
    if (r.IsEmpty()) {
        return;
    }
    Rect h = r.Intersect(hole);
    if (h.IsEmpty()) {
        out[0] = r;
        return;
    }
    out[0] = Rect(r.x, r.y, r.dx, h.y - r.y);
    out[1] = Rect(r.x, h.Bottom(), r.dx, r.Bottom() - h.Bottom());
    out[2] = Rect(r.x, h.y, h.x - r.x, h.dy);
    out[3] = Rect(h.Right(), h.y, r.Right() - h.Right(), h.dy);
}

// Single Page: the page turns over its own edge (left, or right in
// Right-to-Left Reading); its back is blank paper. A backward turn plays the
// same Curl mirrored: the page comes back over that edge onto the old one
static Geometry SingleGeometry(const TurnData& t) {
    Geometry g;
    bool toLeft = t.sweep != Sweep::ToRight;
    g.dirX = toLeft ? 1.0f : -1.0f;
    g.revealed = t.to.left;
    if (t.forward) {
        g.start = t.from.left;
        g.spineX = (float)(toLeft ? g.start.x : g.start.Right());
        g.end = MirrorRect(g.start, g.spineX);
        g.startFace = Face::Old;
        g.endFace = Face::Paper;
        g.fade = Fade::Out;
        g.paperBelow[0] = g.revealed.Intersect(t.from.left);
        return g;
    }
    g.end = t.to.left;
    g.spineX = (float)(toLeft ? g.end.Right() : g.end.x);
    g.start = MirrorRect(g.end, g.spineX);
    g.startFace = Face::Paper;
    g.endFace = Face::Landing;
    g.fade = Fade::In;
    g.stays = t.from.left;
    g.landing = t.to.left;
    g.paperBelow[0] = g.stays.Intersect(g.landing);
    return g;
}

static Geometry GeometryOf(const TurnData& t) {
    if (t.shape == CurlShape::Single) {
        return SingleGeometry(t);
    }

    // forward in Facing: the right page curls over to the left; the back of
    // the turning page is the new Spread's first page
    Geometry g;
    bool toLeft = t.sweep != Sweep::ToRight;
    g.dirX = toLeft ? 1.0f : -1.0f;
    Rect fromLead = toLeft ? t.from.right : t.from.left;
    Rect fromStay = toLeft ? t.from.left : t.from.right;
    g.landing = toLeft ? t.to.left : t.to.right;
    g.revealed = toLeft ? t.to.right : t.to.left;
    g.spineX = SpineX(fromLead.IsEmpty() ? g.revealed : fromLead, g.landing.IsEmpty() ? fromStay : g.landing, toLeft);
    g.stays = EndSide(WithEdges(UnionOf(fromStay, g.landing), t.edgesPx, t.sweep), g.spineX, fromLead, toLeft);
    g.paperBelow[0] = fromStay;
    g.paperBelow[1] = g.revealed;

    // a side with no page (beside a wide page): like Single Page, a blank
    // sheet fades in there, or fades away once over
    g.start = fromLead;
    if (fromLead.IsEmpty()) {
        g.start = g.landing.IsEmpty() ? Rect() : MirrorRect(g.landing, g.spineX);
        g.startFace = Face::Paper;
        g.fade = Fade::In;
    }
    g.end = g.landing;
    if (g.landing.IsEmpty()) {
        g.end = MirrorRect(g.start, g.spineX);
        g.endFace = Face::Paper;
        g.fade = Fade::Out;
    }
    return g;
}

//--- pages of different sizes: the view glides with the turn (#31)
//
// Pages keep their real sizes. Each Spread's resting view is a camera on a
// shared world: page units, the spine at x = 0, the Spread's middle at y = 0.
// During the Curl the camera moves from the old view to the new one, so the
// old pages (from the old screen copy) and the new ones (from SumatraPDF's new
// screen) show scaled side by side, and arrives as the sheet lands:
//
//      old view               mid-Curl                 new view
//   +-----------+         +-----------+         +-----------+
//   |   [A4]|   |   -->   |  [A4][A0] |   -->   |   [ A0  ] |
//   +-----------+         +-----------+         +-----------+

// screen = world * z + a
struct Cam {
    float z = 1;
    float ax = 0;
    float ay = 0;
};

enum ScreenSide {
    kLeft = 0,
    kRight = 1
};

enum class EdgeMode {
    Crossfade, // same size, or a comic double spread
    Tuck,      // under the page as the turn starts, out again after landing
};

struct Glide {
    bool valid = false;
    bool on = false;
    Cam from;
    Cam to;
    // per screen side: the old and the new page, world; and in their copies
    RectF oldPage[2];
    RectF newPage[2];
    Rect oldSrc[2];
    Rect newSrc[2];
    int oldEdges[2] = {};
    int newEdges[2] = {};
    EdgeMode edges[2] = {EdgeMode::Crossfade, EdgeMode::Crossfade};
    int startSide = kRight; // where the sheet lies before the turn
    RectF front;            // the sheet before the turn, world
    RectF back;             // and after it
    Face frontFace = Face::Old;
    Face backFace = Face::Landing;
    Fade fade = Fade::None;
    float endT = 1; // the layer goes at this share of the Curl
};

static bool IsEmptyF(RectF r) {
    return r.dx <= 0 || r.dy <= 0;
}

static RectF ToWorld(const Cam& c, Rect r) {
    if (r.IsEmpty()) {
        return RectF();
    }
    return RectF((r.x - c.ax) / c.z, (r.y - c.ay) / c.z, r.dx / c.z, r.dy / c.z);
}

static RectF ToScreen(const Cam& c, RectF w) {
    return RectF(w.x * c.z + c.ax, w.y * c.z + c.ay, w.dx * c.z, w.dy * c.z);
}

static RectF MirrorWorld(RectF w) {
    return RectF(-(w.x + w.dx), w.y, w.dx, w.dy);
}

static bool Near(float a, float b, float tolerance) {
    return b > 0 && fabsf(a / b - 1) < tolerance;
}

static bool SameSize(RectF a, RectF b) {
    return Near(a.dx, b.dx, kSameSize) && Near(a.dy, b.dy, kSameSize);
}

static EdgeMode EdgeModeOf(RectF a, RectF b) {
    if (IsEmptyF(a) || IsEmptyF(b) || SameSize(a, b)) {
        return EdgeMode::Crossfade;
    }
    bool doubled = Near(a.dx, 2 * b.dx, kSpreadWide) || Near(b.dx, 2 * a.dx, kSpreadWide);
    if (Near(a.dy, b.dy, kSpreadHigh) && doubled) {
        return EdgeMode::Crossfade;
    }
    return EdgeMode::Tuck;
}

// a Spread by screen side, and its resting view. Single Page puts its page
// on the page's side of the spine: the spine is the edge it turns over
struct Sides {
    Rect page[2];
    int edges[2] = {};
    Cam cam;
    bool ok = false;
};

static Sides SidesOf(const CurlSpread& s, CurlShape shape, bool spineRight) {
    Sides d;
    d.edges[kLeft] = s.edgesLeft;
    d.edges[kRight] = s.edgesRight;
    float spine = 0;
    if (shape == CurlShape::Single) {
        d.page[spineRight ? kLeft : kRight] = s.left;
        spine = (float)(spineRight ? s.left.Right() : s.left.x);
    } else {
        d.page[kLeft] = s.left;
        d.page[kRight] = s.right;
        spine = s.spineX;
    }
    Rect all = UnionOf(d.page[kLeft], d.page[kRight]);
    d.cam.z = s.zoom;
    d.cam.ax = spine;
    d.cam.ay = all.y + all.dy / 2.0f;
    d.ok = s.zoom > 0 && !all.IsEmpty();
    return d;
}

// on: the view moves or a page changes size; otherwise the screen copies
// stay where they are (GeometryOf)
static Glide GlideOf(const TurnData& t) {
    Glide gl;
    bool spineRight = t.forward == (t.sweep == Sweep::ToRight);
    Sides a = SidesOf(t.from, t.shape, spineRight);
    Sides b = SidesOf(t.to, t.shape, spineRight);
    if (!a.ok || !b.ok) {
        return gl;
    }
    gl.valid = true;
    gl.from = a.cam;
    gl.to = b.cam;
    bool sameView = fabsf(logf(b.cam.z / a.cam.z)) < kSameZoom && fabsf(b.cam.ax - a.cam.ax) < kSamePx &&
                    fabsf(b.cam.ay - a.cam.ay) < kSamePx;
    bool resized = false;
    for (int i = kLeft; i <= kRight; i++) {
        gl.oldPage[i] = ToWorld(a.cam, a.page[i]);
        gl.newPage[i] = ToWorld(b.cam, b.page[i]);
        gl.oldSrc[i] = a.page[i];
        gl.newSrc[i] = b.page[i];
        gl.oldEdges[i] = a.edges[i];
        gl.newEdges[i] = b.edges[i];
        gl.edges[i] = EdgeModeOf(gl.oldPage[i], gl.newPage[i]);
        bool both = !IsEmptyF(gl.oldPage[i]) && !IsEmptyF(gl.newPage[i]);
        resized = resized || (both && !SameSize(gl.oldPage[i], gl.newPage[i]));
    }
    gl.on = !sameView || resized;

    // the sheet: the old page on the start side turns over onto the end side,
    // where it's the new page. A side with no page: blank paper fading in or out
    gl.startSide = t.sweep == Sweep::ToRight ? kLeft : kRight;
    int endSide = 1 - gl.startSide;
    gl.front = gl.oldPage[gl.startSide];
    gl.back = gl.newPage[endSide];
    if (IsEmptyF(gl.front) && !IsEmptyF(gl.back)) {
        gl.front = MirrorWorld(gl.back);
        gl.frontFace = Face::Paper;
        gl.fade = Fade::In;
    } else if (IsEmptyF(gl.back)) {
        gl.back = MirrorWorld(gl.front);
        gl.backFace = Face::Paper;
        gl.fade = Fade::Out;
    }
    bool tuck = gl.edges[kLeft] == EdgeMode::Tuck || gl.edges[kRight] == EdgeMode::Tuck;
    gl.endT = tuck ? 1 + kEdgeTuck : 1;
    return gl;
}

// the view at e (0 old .. 1 new): zoom evenly in log scale around the one
// point that stays put on screen; equal zooms: a plain pan
static Cam CamAt(const Glide& gl, float e) {
    if (e <= 0) {
        return gl.from;
    }
    if (e >= 1) {
        return gl.to;
    }
    Cam c;
    float zoomLog = logf(gl.to.z / gl.from.z);
    c.z = gl.from.z * expf(zoomLog * e);
    if (fabsf(zoomLog) < 1e-4f) {
        c.ax = Lerp(gl.from.ax, gl.to.ax, e);
        c.ay = Lerp(gl.from.ay, gl.to.ay, e);
        return c;
    }
    float dz = gl.from.z - gl.to.z;
    float px = (gl.to.ax - gl.from.ax) / dz;
    float py = (gl.to.ay - gl.from.ay) / dz;
    c.ax = gl.from.ax + px * (gl.from.z - c.z);
    c.ay = gl.from.ay + py * (gl.from.z - c.z);
    return c;
}

// a Curl in the air, drawing thread only
struct Flight {
    TurnData turn; // turn.id: its last command (a new Curl, or sent back)
    Geometry geo;
    Glide glide;
    CurlPicture oldPic;  // the old Spread, whole canvas
    CurlPicture landPic; // the landing page, once SumatraPDF shows it
    CurlPicture toPic;   // sent back: the new Spread, whole canvas
    bool back = false;   // sent back: heading to where it started
    Sheets sheets;       // a Curl: one; a Bundle: 2-14
    // a Bundle that taps the other way turned partly back: per sheet, 0 on
    // with the Bundle, -1 back from where it was, +1 on again; ownT its progress
    signed char dir[kMaxSheets] = {};
    float ownT[kMaxSheets] = {};
    int peeled = 0; // leaves turned back
    float t = 0;    // time progress, 0 .. EndT
    double advancedAt = 0;
    bool showsNew = false;
    bool seeThrough = false;
    bool restShown = false; // a frame with it at rest was presented
    int refreshesLeft = -1;
    double lastPresent = 0;
    double refreshMs = 1000.0 / 60;
    CurlTimes times;
};

// the Curls in the air, oldest first; all turn the same way. A glide flies
// alone. Each frame shares one cover hand-off
struct Stage {
    Vec<Flight*> flights;
    Pace pace;
    int lastStart = 0;    // the last Start command taken
    int needCover = 0;    // the next Present shows this Start
    int pendingCover = 0; // the next frame signal says it's on screen
    // whole-canvas pictures of Curls that left the air, for the next copies:
    // making a new one takes several ms, enough for a late frame
    CurlPicture spares[kMaxInAir + 1];
    int nSpares = 0;
    double lastPresent = 0;
    double refreshMs = 1000.0 / 60;
    double copyMsPerMpx = 1; // measured: about 0.9 here
};

enum class Scene {
    Empty,
    Curl
};

enum class LandHow {
    Done,
    AtOnce,
    TimedOut,
    SentBack
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

static void SetRect4F(float* dst, RectF r) {
    dst[0] = r.x;
    dst[1] = r.y;
    dst[2] = r.dx;
    dst[3] = r.dy;
}

static void SetPicture(float* dst, const CurlPicture& pic) {
    if (!pic.ok) {
        SetRect4(dst, Rect());
        return;
    }
    SetRect4(dst, pic.area);
}

static Consts BaseConsts(Gpu& g, const TurnData& t, Rect r) {
    Consts c{};
    Size view = CurlLayerSize(g.layer);
    c.viewSize[0] = (float)view.dx;
    c.viewSize[1] = (float)view.dy;
    SetRect4(c.rect, r);
    c.radius = 1;
    c.camDist = 1; // flat pieces lie on the page plane: no perspective
    c.alpha = 1;
    c.paper[0] = GetRed(t.paper) / 255.0f;
    c.paper[1] = GetGreen(t.paper) / 255.0f;
    c.paper[2] = GetBlue(t.paper) / 255.0f;
    c.paper[3] = 1;
    c.look[3] = 1;
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

// a flat, pixel-exact piece of a screen copy; see-through by 1 - alpha
static void DrawFlat(Gpu& g, const TurnData& t, Rect r, const CurlPicture& pic, float alpha = 1) {
    if (r.IsEmpty() || alpha <= 0) {
        return;
    }
    Consts c = BaseConsts(g, t, r);
    c.mode = kModeFlat;
    c.alpha = alpha;
    SetPicture(c.frontImg, pic);
    Draw(g, c, &pic, nullptr, false);
}

// plain background over r, as the old screen had it at the same height
static void DrawBackdrop(Gpu& g, const TurnData& t, Rect r, const CurlPicture& pic, PointF at, float alpha) {
    if (r.IsEmpty() || alpha <= 0) {
        return;
    }
    Consts c = BaseConsts(g, t, r);
    c.mode = kModeBackdrop;
    c.alpha = alpha;
    c.backdropX = at.x;
    c.backdropY = at.y;
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

static float SheetAlpha(Fade fade, float t) {
    if (fade == Fade::Out) {
        return 1 - SmoothStep(kSingleFadeFrom, 1, t);
    }
    if (fade == Fade::In) {
        return 1 - SmoothStep(kSingleFadeFrom, 1, 1 - t);
    }
    return 1;
}

// the picture a face shows, or null for blank paper. The next Curl's old
// Spread holds this one's landing page, as SumatraPDF last showed it
static const CurlPicture* FacePicture(const Flight& f, const Flight* next, Face face) {
    if (face == Face::Old) {
        return &f.oldPic;
    }
    if (face != Face::Landing) {
        return nullptr;
    }
    if (next && next->oldPic.ok) {
        return &next->oldPic;
    }
    if (f.landPic.ok) {
        return &f.landPic;
    }
    return f.toPic.ok ? &f.toPic : nullptr;
}

// a glide's new screen
static const CurlPicture* NewPicture(const Flight& f) {
    if (f.landPic.ok) {
        return &f.landPic;
    }
    return f.toPic.ok ? &f.toPic : nullptr;
}

// several Curls in the air: each sheet is drawn in two halves, one per side
// of the spine; empty clip: no clip
static void SetClip(Gpu& g, Rect clip) {
    if (clip.IsEmpty()) {
        g.ctx->RSSetState(g.rast.Get());
        return;
    }
    D3D11_RECT sc = {clip.x, clip.y, clip.Right(), clip.Bottom()};
    g.ctx->RSSetState(g.rastClipped.Get());
    g.ctx->RSSetScissorRects(1, &sc);
}

// the shadow of the lifted paper, on the paper below only
static void DrawShadow(Gpu& g, const Rect (&paperBelow)[2], Consts c, Rect clip, int copies) {
    if (c.shadowAlpha <= 0) {
        return;
    }
    Size view = CurlLayerSize(g.layer);
    Rect viewRect(0, 0, view.dx, view.dy);
    if (!clip.IsEmpty()) {
        viewRect = viewRect.Intersect(clip);
    }
    c.mode = kModeShadow;
    g.ctx->RSSetState(g.rastClipped.Get());
    g.ctx->OMSetDepthStencilState(g.noDepth.Get(), 0);
    for (Rect below : paperBelow) {
        below = below.Intersect(viewRect);
        if (below.IsEmpty()) {
            continue;
        }
        D3D11_RECT sc = {below.x, below.y, below.Right(), below.Bottom()};
        g.ctx->RSSetScissorRects(1, &sc);
        for (int k = 0; k < copies; k++) {
            float a = 2 * kPi * k / copies;
            c.shadowOff[0] = cosf(a);
            c.shadowOff[1] = sinf(a);
            Draw(g, c, nullptr, nullptr, true);
        }
    }
    SetClip(g, clip);
    g.ctx->OMSetDepthStencilState(g.depth.Get(), 0);
}

// where the turning sheet lies and what its faces show, canvas px. A Bundle
// draws several: fromTop 0 is the top one where the turn starts
struct SheetPlace {
    int shadowCopies = kShadowCopies;
    Sheets sheets;
    int fromTop = 0;
    float shadow = 1; // strength; 0: casts none
    const CurlPicture* turnedPic = nullptr;
    RectF start; // flat before the turn
    RectF end;   // and after it, mirrored over the spine
    float spineX = 0;
    float dirX = 1;
    Fade fade = Fade::None;
    const CurlPicture* front = nullptr; // null: blank paper
    const CurlPicture* back = nullptr;
    RectF frontSrc; // the front face in its picture
    RectF backSrc;
    Rect paperBelow[2];
};

enum class Pass {
    Shadow,
    Sheet,
    Both
};

// the corner peel: the fold line starts through the outer bottom corner,
// slanted, and straightens as it sweeps to the spine; the cylinder shrinks
// over the last 40%, so the sheet lands flat on the other side. A Bundle's
// sheets are stiffer and wrap round one axis, the lower ones further out
static void DrawSheetAt(Gpu& g, const TurnData& turn, const SheetPlace& sp, float t, Rect clip,
                        Pass pass = Pass::Both) {
    if (sp.start.dx <= 0 || sp.end.dx <= 0) {
        return;
    }
    float p = EaseInOut(t);
    float w = Lerp(sp.start.dx, sp.end.dx, p);
    float h = Lerp(sp.start.dy, sp.end.dy, p);
    float gapStart = sp.dirX > 0 ? sp.start.x - sp.spineX : sp.spineX - (sp.start.x + sp.start.dx);
    float reach = gapStart + w;

    Consts c = BaseConsts(g, turn, Rect());
    c.mode = kModeCurl;
    SetRect4F(c.startRect, sp.start);
    SetRect4F(c.endRect, sp.end);
    SetRect4F(c.frontSrc, sp.frontSrc);
    SetRect4F(c.backSrc, sp.backSrc);
    c.spineX = sp.spineX;
    c.dirX = sp.dirX;
    c.progress = p;
    const Sheets& s = sp.sheets;
    Lift lift = SheetLift(s, p, sp.fromTop);
    float bend = kRadiusPerWidth * RadiusFactor(s) * std::min(w, h) * (1 - SmoothStep(kShrinkFrom, 1, p));
    c.lead = kLeadDeg * LeadFactor(s) * kPi / 180 * powf(1 - p, kLeadPow);
    c.radius = bend + kMinRadius + lift.radius;
    c.camDist = std::max(c.viewSize[1], kCamPerReach * reach) * kCamPerHeight;
    c.shading = kShading;
    c.alpha = SheetAlpha(sp.fade, t);
    c.shadowAlpha = kShadow * kShadowPerCopy * c.alpha * kShadowCopies / sp.shadowCopies * sp.shadow;
    // lying on each other, the top sheet wins where the turn starts, the
    // bottom one (landing last) where it ends
    c.look[0] = lift.base;
    c.look[1] = kStackNudge * (s.n - 1 - sp.fromTop);
    c.look[2] = kStackNudge * sp.fromTop;
    if (s.n > 1) {
        c.look[3] = sp.fromTop % 2 ? kSheetTint : 1;
        c.stack[0] = kFreeEdgeDark;
    }
    SetPicture(c.frontImg, sp.front ? *sp.front : CurlPicture());
    SetPicture(c.backImg, sp.back ? *sp.back : CurlPicture());
    if (pass != Pass::Sheet && sp.shadow > 0) {
        DrawShadow(g, sp.paperBelow, c, clip, sp.shadowCopies);
    }
    if (pass != Pass::Shadow) {
        Draw(g, c, sp.front, sp.back, true);
    }
}

// a sheet of a Bundle: on with the Bundle, or turned by taps (back from where
// it was, or on again)
static float SheetTNow(const Flight& f, int k) {
    if (f.dir[k] != 0) {
        return f.ownT[k];
    }
    return SheetT(f.sheets, std::min(f.t, 1.0f), k);
}

static bool GoesBack(const Flight& f, int k) {
    return f.dir[k] < 0;
}

// sheet k of a Bundle: the top one shows the old page, the last one landing
// the landing page; the ones between are blank paper. Once taps turned some
// back, the page SumatraPDF shows lies on top of each side: the last sheet
// landing shows it on its back, the first going back on its front
static SheetPlace SheetOfStack(const SheetPlace& place, const Flight& f, int k) {
    int n = f.sheets.n;
    SheetPlace sp = place;
    sp.sheets = f.sheets;
    sp.fromTop = k;
    if (n == 1) {
        return sp;
    }
    int lastOn = -1;
    int firstBack = n;
    for (int i = 0; i < n; i++) {
        if (GoesBack(f, i)) {
            firstBack = std::min(firstBack, i);
        } else {
            lastOn = i;
        }
    }
    bool turned = firstBack < n || f.peeled;
    sp.front = k == 0 ? place.front : nullptr;
    sp.back = !turned && k == n - 1 ? place.back : nullptr;
    if (turned) {
        sp.front = k == firstBack ? place.turnedPic : sp.front;
        sp.back = k == lastOn ? place.turnedPic : nullptr;
    }
    sp.shadow = k == 0 || k == n - 1 ? kBundleShadow : 0;
    return sp;
}

static RectF MirrorF(RectF r, float spineX) {
    return RectF(2 * spineX - (r.x + r.dx), r.y, r.dx, r.dy);
}

static RectF LerpRectF(RectF a, RectF b, float f) {
    return RectF(Lerp(a.x, b.x, f), Lerp(a.y, b.y, f), Lerp(a.dx, b.dx, f), Lerp(a.dy, b.dy, f));
}

// a Curl's sheet, or a Bundle's sheets: the top and bottom ones' shadows
// first, so they fall on the paper below and not on the stack. stepped: pages
// of different sizes, the sheets between step evenly from the top page's
// size to the bottom one's
static void DrawStack(Gpu& g, const Flight& f, const SheetPlace& place, Rect clip, bool stepped = false) {
    int n = f.sheets.n;
    if (n == 1) {
        DrawSheetAt(g, f.turn, place, std::min(f.t, 1.0f), clip);
        return;
    }
    auto sheetAt = [&](int k) {
        SheetPlace sp = SheetOfStack(place, f, k);
        if (stepped) {
            sp.start = LerpRectF(place.start, MirrorF(place.end, place.spineX), (float)k / n);
            sp.end = LerpRectF(MirrorF(place.start, place.spineX), place.end, (float)(k + 1) / n);
        }
        return sp;
    };
    for (int k : {0, n - 1}) {
        DrawSheetAt(g, f.turn, sheetAt(k), SheetTNow(f, k), clip, Pass::Shadow);
    }
    for (int k = 0; k < n; k++) {
        DrawSheetAt(g, f.turn, sheetAt(k), SheetTNow(f, k), clip, Pass::Sheet);
    }
}

// the sheet where the screen copies have it: the view doesn't move. A Bundle
// that taps turned partly back shows on top of each side the page SumatraPDF
// shows: turnedPic, the whole screen copied after the last such tap
static void DrawSheet(Gpu& g, const Flight& f, const Flight* next, const CurlPicture* turnedPic, Rect clip,
                      int shadowCopies) {
    const Geometry& geo = f.geo;
    if (geo.start.IsEmpty() || geo.end.IsEmpty()) {
        return;
    }
    SheetPlace sp;
    sp.shadowCopies = shadowCopies;
    sp.start = ToRectF(geo.start);
    sp.end = ToRectF(geo.end);
    sp.spineX = geo.spineX;
    sp.dirX = geo.dirX;
    sp.fade = geo.fade;
    sp.front = FacePicture(f, next, geo.startFace);
    sp.back = FacePicture(f, next, geo.endFace);
    sp.turnedPic = turnedPic;
    sp.frontSrc = sp.start;
    sp.backSrc = sp.end;
    sp.paperBelow[0] = geo.paperBelow[0];
    sp.paperBelow[1] = geo.paperBelow[1];
    DrawStack(g, f, sp, clip);
}

// plain background in the old screen: x, a column in the middle of the margin
// beside the old page and its Page Edges; else y, a row above or below them
// (a Spread as wide as the window); -1: none on screen
static PointF BackdropAt(const TurnData& t) {
    Rect page = UnionOf(t.from.left, t.from.right);
    int left = page.x - t.edgesPx;
    if (left >= 1) {
        return PointF((float)(left / 2), -1);
    }
    int right = page.Right() + t.edgesPx;
    if (right < t.canvas.dx) {
        return PointF((float)((right + t.canvas.dx) / 2), -1);
    }
    if (page.y >= 1) {
        return PointF(-1, (float)(page.y / 2));
    }
    if (page.Bottom() < t.canvas.dy) {
        return PointF(-1, (float)((page.Bottom() + t.canvas.dy) / 2));
    }
    return PointF(-1, -1);
}

// Single Page, all but the turning sheet. Pages of different sizes put the new
// page and its Page Edges elsewhere than the old: what's around the old page
// (background, Page Edges, a wider page's sides) stays as it was, then changes
// to the new screen while the sheet settles, the last part of the turn
// (spec 3.3: the forward sheet fades away then; backward it lands):
//
//   forward:  old page = the sheet; under it the new page (blank paper until
//             SumatraPDF shows it) and, beyond the new page, background;
//             around the old page, the old screen, fading as the sheet does
//   backward: the old page under the incoming sheet, where the new page will
//             lie; the new page's parts beyond it, background until the sheet
//             covers them; everything else, the old screen, fading out
//
// Several Curls in the air: the bottom is the newest one's (forward) or the
// oldest one's (backward); every Curl adds its old screen around the page
enum class Part {
    Bottom,
    Around
};

// the newest Curl's new page where the sheet lifted off: blank paper until
// SumatraPDF shows it, then see-through. Sent back, SumatraPDF shows the old
// Spread there: the new one comes from its copy
static void DrawRevealed(Gpu& g, const Flight& f, Rect r) {
    if (f.seeThrough && !f.back) {
        return;
    }
    if (f.toPic.ok) {
        DrawFlat(g, f.turn, r, f.toPic);
        return;
    }
    DrawPaper(g, f.turn, r);
}

static void DrawSingleAround(Gpu& g, const Flight& f, Part part) {
    const TurnData& turn = f.turn;
    const CurlPicture& old = f.oldPic;
    Rect oldPage = turn.from.left;
    Rect newPage = turn.to.left;
    Size view = CurlLayerSize(g.layer);
    Rect canvas(0, 0, view.dx, view.dy);
    float settle = SheetAlpha(Fade::Out, f.t); // 1 until the sheet settles, then 0
    PointF backdropAt = BackdropAt(turn);
    Rect parts[4];
    if (turn.forward) {
        if (part == Part::Bottom) {
            DrawRevealed(g, f, newPage);
            Outside(oldPage, newPage, parts);
            for (Rect r : parts) {
                DrawBackdrop(g, turn, r, old, backdropAt, settle);
            }
            return;
        }
        Outside(canvas, oldPage, parts);
        for (Rect r : parts) {
            DrawFlat(g, turn, r, old, settle);
        }
        return;
    }
    Rect under = newPage.Intersect(oldPage);
    if (part == Part::Bottom) {
        DrawFlat(g, turn, under, old);
        Outside(newPage, oldPage, parts);
        for (Rect r : parts) {
            DrawBackdrop(g, turn, r, old, backdropAt, 1);
        }
        return;
    }
    Outside(canvas, under, parts);
    for (Rect r : parts) {
        DrawFlat(g, turn, r, old, settle);
    }
}

// pic's src stretched over dst, smooth; see-through by 1 - alpha outside keep.
// No picture: blank paper. dst == src: pixel-exact
static void DrawImage(Gpu& g, const TurnData& t, RectF dst, const CurlPicture* pic, RectF src, float alpha,
                      RectF keep = RectF()) {
    if (IsEmptyF(dst) || alpha <= 0) {
        return;
    }
    Consts c = BaseConsts(g, t, Rect());
    c.mode = kModeImage;
    c.alpha = alpha;
    SetRect4F(c.rect, dst);
    SetRect4F(c.frontSrc, src);
    SetRect4F(c.keep, keep);
    SetPicture(c.frontImg, pic && pic->ok ? *pic : CurlPicture());
    Draw(g, c, pic, nullptr, false);
}

// a page's Page Edges, T px thick beside its outer side, pulled out to share
// out of T (0: tucked under the page). The strip is the copy's own, so it's
// pixel-exact when the view is at rest and out = 1
static void DrawEdgeStrip(Gpu& g, const TurnData& t, const Cam& cam, RectF world, Rect src, int side, int T, float out,
                          float alpha, const CurlPicture* pic) {
    float w = T * out;
    if (IsEmptyF(world) || src.IsEmpty() || w < 0.5f || alpha <= 0 || !pic || !pic->ok) {
        return;
    }
    RectF at = ToScreen(cam, world);
    RectF dst(side == kRight ? at.x + at.dx : at.x - w, at.y, w, at.dy);
    // the part still out: the outer one
    float srcX = side == kRight ? src.Right() + T - w : (float)(src.x - T);
    DrawImage(g, t, dst, pic, RectF(srcX, (float)src.y, w, (float)src.dy), alpha);
}

// one side's Page Edges at turn progress q (past 1: landed). Tuck: the old
// page's go under it as the turn starts, the new page's come out after
// landing. Crossfade: as the page under the sheet shows (start side, the new
// page comes in) or goes (end side, the old page is covered)
static void DrawSideEdges(Gpu& g, const Flight& f, const Cam& cam, int side, float q) {
    const Glide& gl = f.glide;
    const CurlPicture* oldPic = &f.oldPic;
    const CurlPicture* newPic = NewPicture(f);
    auto drawOld = [&](float out, float a) {
        DrawEdgeStrip(g, f.turn, cam, gl.oldPage[side], gl.oldSrc[side], side, gl.oldEdges[side], out, a, oldPic);
    };
    auto drawNew = [&](float out, float a) {
        DrawEdgeStrip(g, f.turn, cam, gl.newPage[side], gl.newSrc[side], side, gl.newEdges[side], out, a, newPic);
    };
    if (gl.edges[side] == EdgeMode::Tuck) {
        if (q < 1) {
            drawOld(1 - SmoothStep(0, kEdgeTuck, q), 1);
        } else {
            drawNew(SmoothStep(0, kEdgeTuck, q - 1), 1);
        }
        return;
    }
    float qc = std::min(q, 1.0f);
    float aNew = side == gl.startSide ? SmoothStep(0, kUnderFade, qc) : SmoothStep(1 - kUnderFade, 1, qc);
    drawOld(1, 1 - aNew);
    drawNew(1, aNew);
}

static Rect Rounded(RectF r) {
    if (IsEmptyF(r)) {
        return Rect();
    }
    int x0 = (int)lroundf(r.x);
    int y0 = (int)lroundf(r.y);
    return Rect(x0, y0, (int)lroundf(r.x + r.dx) - x0, (int)lroundf(r.y + r.dy) - y0);
}

// the strip beside a resting page where its Page Edges go
static Rect EdgeRoom(Rect page, int side, int T) {
    if (page.IsEmpty() || T <= 0) {
        return Rect();
    }
    return Rect(side == kRight ? page.Right() : page.x - T, page.y, T, page.dy);
}

// a glide frame at turn progress q, 0 .. gl.endT. Everything is drawn: the
// view differs from SumatraPDF's until the sheet lands. Then SumatraPDF's new
// screen, with the new Page Edges sliding out from under their pages:
//
//   background  ->  Page Edges  ->  pages beneath  ->  shadow  ->  sheet
static void RenderGlide(Gpu& g, const Flight& f) {
    float q = f.t;
    const Glide& gl = f.glide;
    const TurnData& turn = f.turn;
    const CurlPicture* oldPic = &f.oldPic;
    const CurlPicture* newPic = NewPicture(f);
    float qc = std::min(q, 1.0f);
    Cam cam = CamAt(gl, 0.5f - 0.5f * cosf(kPi * qc));
    Size view = CurlLayerSize(g.layer);
    Rect canvas(0, 0, view.dx, view.dy);
    PointF backdropAt = BackdropAt(turn);
    DrawBackdrop(g, turn, canvas, *oldPic, backdropAt, 1);

    bool landed = q >= 1;
    if (landed && newPic) {
        DrawFlat(g, turn, canvas, *newPic);
        for (int i = kLeft; i <= kRight; i++) {
            DrawBackdrop(g, turn, EdgeRoom(gl.newSrc[i], i, gl.newEdges[i]), *oldPic, backdropAt, 1);
        }
    }
    for (int i = kLeft; i <= kRight; i++) {
        DrawSideEdges(g, f, cam, i, q);
    }
    if (landed) {
        if (!newPic) {
            for (int i = kLeft; i <= kRight; i++) {
                DrawImage(g, turn, ToScreen(cam, gl.newPage[i]), nullptr, RectF(), 1);
            }
        }
        return;
    }

    // pages beneath, opaque under the sheet's resting outline. Where one
    // sticks out past it, the new page fades in as the sheet lifts off, the
    // old one fades out as the sheet lands on it
    int startSide = gl.startSide;
    int endSide = 1 - startSide;
    RectF frontAt = ToScreen(cam, gl.front);
    RectF backAt = ToScreen(cam, gl.back);
    RectF revealedAt = ToScreen(cam, gl.newPage[startSide]);
    RectF staysAt = ToScreen(cam, gl.oldPage[endSide]);
    RectF keepFront = gl.frontFace == Face::Old ? frontAt : RectF();
    RectF keepBack = gl.backFace == Face::Landing ? backAt : RectF();
    DrawImage(g, turn, revealedAt, newPic, ToRectF(gl.newSrc[startSide]), SmoothStep(0, kUnderFade, q), keepFront);
    DrawImage(g, turn, staysAt, oldPic, ToRectF(gl.oldSrc[endSide]), 1 - SmoothStep(1 - kUnderFade, 1, q), keepBack);

    SheetPlace sp;
    sp.start = frontAt;
    sp.end = backAt;
    sp.spineX = cam.ax;
    sp.dirX = startSide == kRight ? 1.0f : -1.0f;
    sp.fade = gl.fade;
    sp.front = gl.frontFace == Face::Old ? oldPic : nullptr;
    sp.back = gl.backFace == Face::Landing ? newPic : nullptr;
    sp.frontSrc = ToRectF(gl.oldSrc[startSide]);
    sp.backSrc = ToRectF(gl.newSrc[endSide]);
    sp.paperBelow[0] = Rounded(revealedAt);
    sp.paperBelow[1] = Rounded(staysAt);
    DrawStack(g, f, sp, Rect(), true);
}

// the share of the Curl when the layer goes: a glide stays up while the new
// Page Edges slide out
static float EndT(const Flight& f) {
    return f.glide.on ? f.glide.endT : 1;
}

// landed, or back where it started
// sheets taps turned are home: back where they lifted, or landed
static bool TurnedHome(const Flight& f) {
    for (int k = 0; k < f.sheets.n; k++) {
        bool home = f.dir[k] < 0 ? f.ownT[k] <= 0 : f.dir[k] == 0 || f.ownT[k] >= 1;
        if (!home) {
            return false;
        }
    }
    return true;
}

static bool AtRest(const Flight& f) {
    return f.back ? f.t <= 0 : f.t >= EndT(f) && TurnedHome(f);
}

// one Curl at rest, before the layer goes: what SumatraPDF shows (or will),
// so the last frame matches its picture. Landed: the landing page is the copy
// of what's beneath; back: the old screen
static void RenderRest(Gpu& g, const Flight& f) {
    const Geometry& geo = f.geo;
    if (f.back) {
        Size view = CurlLayerSize(g.layer);
        DrawFlat(g, f.turn, Rect(0, 0, view.dx, view.dy), f.oldPic);
        return;
    }
    if (f.turn.shape == CurlShape::Spread) {
        DrawRevealed(g, f, geo.revealed);
    } else if (f.turn.forward) {
        DrawRevealed(g, f, f.turn.to.left);
    }
    if (geo.landing.IsEmpty()) {
        return;
    }
    const CurlPicture* land = FacePicture(f, nullptr, Face::Landing);
    if (land) {
        DrawFlat(g, f.turn, geo.landing, *land);
    } else {
        DrawPaper(g, f.turn, geo.landing);
    }
}

// taps the other way turned every sheet of this Bundle back: it now heads
// home, the way the next page turns
static bool AllBack(const Flight& f) {
    for (int k = 0; k < f.sheets.n; k++) {
        if (!GoesBack(f, k)) {
            return false;
        }
    }
    return f.sheets.n > 1;
}

// the side its sheets come to lie on
static int EndSideOf(const Flight& f) {
    bool toLeft = f.geo.dirX > 0;
    return toLeft != AllBack(f) ? kLeft : kRight;
}

// what lies under the sheets, per side: the old screen of the oldest turn
// landing there; else the newest turn's new page it lifts off. A sheet split
// off a Bundle goes back the other way, so then both sides have an old screen
static void DrawBottom(Gpu& g, const Stage& st) {
    const Flight& newest = *VecLast(st.flights);
    const Flight& oldest = *st.flights[0];
    if (newest.turn.shape == CurlShape::Spread) {
        const Flight* lands[2] = {};
        for (const Flight* f : st.flights) {
            int side = EndSideOf(*f);
            if (!lands[side]) {
                lands[side] = f;
            }
        }
        for (int side = kLeft; side <= kRight; side++) {
            if (lands[side]) {
                continue;
            }
            for (int i = len(st.flights) - 1; i >= 0; i--) {
                const Flight& f = *st.flights[i];
                if (EndSideOf(f) != side) {
                    DrawRevealed(g, f, f.geo.revealed);
                    break;
                }
            }
        }
        for (const Flight* f : lands) {
            if (f) {
                DrawFlat(g, f->turn, AllBack(*f) ? f->geo.start : f->geo.stays, f->oldPic);
            }
        }
        return;
    }
    DrawSingleAround(g, newest.turn.forward ? newest : oldest, Part::Bottom);
    for (int i = len(st.flights) - 1; i >= 0; i--) {
        DrawSingleAround(g, *st.flights[i], Part::Around);
    }
}

// the newest split-off sheet's copy of the whole screen, once SumatraPDF shows it
static const CurlPicture* SplitLanding(const Stage& st) {
    for (int i = len(st.flights) - 1; i >= 0; i--) {
        const Flight& f = *st.flights[i];
        if (f.turn.splitOf) {
            return f.landPic.ok ? &f.landPic : nullptr;
        }
    }
    return nullptr;
}

// as in a book: on the side read pages lie on, a later page lies on top; on
// the other side, an earlier one. So each sheet is drawn in two halves, split
// at the spine, each on a fresh depth buffer (a Bundle's sheets share one):
//
//      read side      |    other side        (left to right: read on the left)
//   page 3, 5, 7 ..   |   .. 7, 5, 3         (drawn in this order)
static void DrawSheets(Gpu& g, const Stage& st) {
    int n = len(st.flights);
    const Flight* next[kMaxInAir + 1] = {};
    for (int i = 0; i + 1 < n && i < kMaxInAir; i++) {
        next[i] = st.flights[i + 1];
    }
    const CurlPicture* splitLanding = SplitLanding(st);
    int casters = 0;
    for (const Flight* f : st.flights) {
        casters += f->sheets.n > 1 ? 2 : 1;
    }
    int copies = std::max(kMinShadowCopies, kShadowCopies / casters);
    if (n == 1) {
        DrawSheet(g, *st.flights[0], nullptr, splitLanding, Rect(), copies);
        return;
    }

    int order[kMaxInAir + 1];
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    auto byPage = [&](int a, int b) { return st.flights[a]->turn.firstPage < st.flights[b]->turn.firstPage; };
    std::sort(order, order + n, byPage);

    const TurnData& newest = VecLast(st.flights)->turn;
    bool r2l = newest.forward == (newest.sweep == Sweep::ToRight);
    Size view = CurlLayerSize(g.layer);
    int spine = ClampI((int)lroundf(VecLast(st.flights)->geo.spineX), 0, view.dx);
    Rect left(0, 0, spine, view.dy);
    Rect right(spine, 0, view.dx - spine, view.dy);
    Rect readSide = r2l ? right : left;
    Rect otherSide = r2l ? left : right;
    ID3D11DepthStencilView* depth = CurlLayerDepth(g.layer);
    for (int k = 0; k < n; k++) {
        int i = order[k];
        g.ctx->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 1.0f, 0);
        DrawSheet(g, *st.flights[i], next[i], splitLanding, readSide, copies);
    }
    for (int k = n - 1; k >= 0; k--) {
        int i = order[k];
        g.ctx->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, 1.0f, 0);
        DrawSheet(g, *st.flights[i], next[i], splitLanding, otherSide, copies);
    }
    SetClip(g, Rect());
}

static bool BundleInAir(const Stage& st) {
    for (const Flight* f : st.flights) {
        if (f->turn.kind == TurnKind::Bundle) {
            return true;
        }
    }
    return false;
}

static int EdgesOf(const CurlSpread& s, int side) {
    return side == kLeft ? s.edgesLeft : s.edgesRight;
}

static Rect PageBeside(const CurlSpread& s, CurlShape shape, int side) {
    if (shape == CurlShape::Single) {
        return s.left;
    }
    return side == kLeft ? s.left : s.right;
}

// one side's Page Edges, thick px, in the column colours SumatraPDF paints
static void DrawEdgeColumns(Gpu& g, const TurnData& t, Rect page, int side, int thick) {
    if (page.IsEmpty() || thick <= 0) {
        return;
    }
    Rect r(side == kRight ? page.Right() : page.x - thick, page.y, thick, page.dy);
    Consts c = BaseConsts(g, t, r);
    c.mode = kModeEdges;
    c.stack[2] = (float)(side == kRight ? page.Right() : page.x);
    c.stack[3] = side == kRight ? 1.0f : -1.0f;
    g.ctx->UpdateSubresource(g.consts.Get(), 0, nullptr, &c, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = {g.edgeSrv.Get(), nullptr};
    g.ctx->PSSetShaderResources(0, 2, srvs);
    UINT stride = 2 * sizeof(float);
    UINT offset = 0;
    ID3D11Buffer* vb = g.quadVB.Get();
    g.ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    g.ctx->IASetIndexBuffer(g.quadIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    g.ctx->DrawIndexed(g.quadCount, 0, 0);
}

static void UploadEdgeColumns(Gpu& g, const TurnData& t, const int (&thick)[2]) {
    u8 rows[2][kEdgeCols * 4] = {};
    for (int side = kLeft; side <= kRight; side++) {
        for (int col = 0; col < std::min(thick[side], kEdgeCols); col++) {
            Color c = EdgeColumnColor(col, thick[side], t.dpi, t.paper);
            u8* px = &rows[side][col * 4];
            px[0] = GetRed(c);
            px[1] = GetGreen(c);
            px[2] = GetBlue(c);
            px[3] = 255;
        }
    }
    g.ctx->UpdateSubresource(g.edgeTex.Get(), 0, nullptr, rows, kEdgeCols * 4, 0);
}

// while a Bundle is in the air the layer draws the Page Edges (spec 3.5):
// what SumatraPDF shows once everything lands, plus what each turn in the air
// still carries. The start side shrinks as a Bundle's bottom sheet lifts, the
// end side grows as it lands. Background first, over the room beside the pages
static void DrawMovingEdges(Gpu& g, const Stage& st) {
    if (!BundleInAir(st)) {
        return;
    }
    const Flight& newest = *VecLast(st.flights);
    const Flight& oldest = *st.flights[0];
    const CurlSpread& end = newest.back ? newest.turn.from : newest.turn.to;
    float px[2] = {(float)EdgesOf(end, kLeft), (float)EdgesOf(end, kRight)};
    bool any[2] = {px[kLeft] > 0, px[kRight] > 0};
    for (const Flight* f : st.flights) {
        int endSide = EndSideOf(*f);
        float t = std::min(f->t, 1.0f);
        for (int side = kLeft; side <= kRight; side++) {
            int from = EdgesOf(f->turn.from, side);
            int to = EdgesOf(f->turn.to, side);
            float done = side == endSide ? EdgesArrive(t) : EdgesDepart(f->sheets, t);
            px[side] += EdgesOffTarget(from, to, done, f->back);
            any[side] = any[side] || from > 0 || to > 0;
        }
    }

    const TurnData& turn = oldest.turn;
    int room = turn.edgesPx;
    int thick[2] = {};
    Rect page[2];
    for (int side = kLeft; side <= kRight; side++) {
        thick[side] = ClampI((int)lroundf(px[side]), 0, room);
        page[side] = PageBeside(end, turn.shape, side);
        if (page[side].IsEmpty()) {
            page[side] = PageBeside(turn.from, turn.shape, side);
        }
    }
    UploadEdgeColumns(g, newest.turn, thick);
    PointF backdropAt = BackdropAt(turn);
    for (int side = kLeft; side <= kRight; side++) {
        Rect p = page[side];
        if (!any[side] || p.IsEmpty()) {
            continue;
        }
        Rect strip(side == kRight ? p.Right() : p.x - room, p.y, room, p.dy);
        DrawBackdrop(g, turn, strip, oldest.oldPic, backdropAt, 1);
        DrawEdgeColumns(g, newest.turn, p, side, thick[side]);
    }
}

// Uncovered parts are blank paper until SumatraPDF shows the new Spread, then
// see-through
static void RenderFrame(Gpu& g, const Stage& st, Scene scene) {
    ID3D11RenderTargetView* rtv = CurlLayerTarget(g.layer);
    float clear[4] = {0, 0, 0, 0};
    g.ctx->OMSetRenderTargets(1, &rtv, CurlLayerDepth(g.layer));
    g.ctx->ClearRenderTargetView(rtv, clear);
    g.ctx->ClearDepthStencilView(CurlLayerDepth(g.layer), D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (scene == Scene::Empty || len(st.flights) == 0) {
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

    const Flight& newest = *VecLast(st.flights);
    bool alone = len(st.flights) == 1;
    if (alone && newest.back && AtRest(newest)) {
        RenderRest(g, newest);
        return;
    }
    if (newest.glide.on) {
        RenderGlide(g, newest);
        return;
    }
    if (alone && AtRest(newest)) {
        RenderRest(g, newest);
        return;
    }
    DrawBottom(g, st);
    DrawMovingEdges(g, st);
    DrawSheets(g, st);
}

static CurlTimes& TimesSlot(Renderer* r, int id) {
    return r->times[id % kTimesKept];
}

// the drawing thread's times -> shared, keeping the UI thread's wait
static void Publish(Renderer* r, const Flight& f) {
    r->mutex.Lock();
    CurlTimes& slot = TimesSlot(r, f.times.id);
    double uiWait = slot.id == f.times.id ? slot.uiWaitMs : 0;
    slot = f.times;
    slot.uiWaitMs = uiWait;
    r->lastTimes = std::max(r->lastTimes, f.times.id);
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
//   skeuomorph-curl: #5 sent back after 200 ms, 12 frames, ...   (goes on as #6)
static void LogFlight(Renderer* r, const Flight& f, LandHow how, const char* why) {
    Str perfPath = PerfLogPath();
    if (!IS_DEBUG && len(perfPath) == 0) {
        return;
    }
    r->mutex.Lock();
    CurlTimes& slot = TimesSlot(r, f.times.id);
    double uiWait = slot.id == f.times.id ? slot.uiWaitMs : 0;
    r->mutex.Unlock();
    const CurlTimes& tm = f.times;
    TempStr what = StrL("TIMEOUT");
    if (how == LandHow::Done) {
        what = StrL("landed");
    } else if (how == LandHow::AtOnce) {
        what = fmt("landed at once (%s)", Str(why));
    } else if (how == LandHow::SentBack) {
        what = StrL("sent back");
    }
    double covered = tm.covered > 0 ? tm.covered - tm.start : -1;
    // a Bundle: ", 14 sheets" (", 14 sheets, 3 back" when taps turned 3 back)
    TempStr sheets = StrL("");
    if (f.sheets.n > 1) {
        int back = 0;
        for (int k = 0; k < f.sheets.n; k++) {
            back += GoesBack(f, k) ? 1 : 0;
        }
        sheets = back == 0 ? fmt(", %d sheets", f.sheets.n) : fmt(", %d sheets, %d back", f.sheets.n, back);
    }
    TempStr line =
        fmt("skeuomorph-curl: #%d %s after %d ms, %d frames, %d late, covered %.1f ms, UI waited %.1f ms%s\n", tm.id,
            what, (int)(tm.gone - tm.start), tm.frames, tm.lateFrames, covered, uiWait, sheets);
    if (IS_DEBUG && how != LandHow::SentBack) {
        // where the Spreads were, canvas px, for tests/skeuomorph-curl-look.ts:
        //   skeuomorph-rects: #3 from 10,20,300,400 to 5,20,310,400 edges 12 glide 1
        Rect o = UnionOf(f.turn.from.left, f.turn.from.right);
        Rect n = UnionOf(f.turn.to.left, f.turn.to.right);
        logf("skeuomorph-rects: #%d from %d,%d,%d,%d to %d,%d,%d,%d edges %d glide %d\n", tm.id, o.x, o.y, o.dx, o.dy,
             n.x, n.y, n.dx, n.dy, f.turn.edgesPx, f.glide.on ? 1 : 0);
    }
    if (IS_DEBUG) {
        logf("%s", line);
    }
    if (len(perfPath) > 0) {
        AppendPerfLog(perfPath, line);
    }
}

// Starts up to goneId are done: none in the air but newer ones
static void PublishGone(Renderer* r, const Stage& st) {
    int gone = st.lastStart;
    for (Flight* f : st.flights) {
        gone = std::min(gone, f->turn.id - 1);
    }
    r->mutex.Lock();
    r->goneId = std::max(r->goneId, gone);
    if (len(st.flights) == 0) {
        r->coveredId = std::max(r->coveredId, st.lastStart);
    }
    r->mutex.Unlock();
    SetEvent(r->covered);
    SetEvent(r->gone);
}

static void Take(CurlPicture& dst, CurlPicture& src) {
    dst = std::move(src);
    src = CurlPicture();
}

static void KeepSpare(Stage& st, CurlPicture& pic) {
    if (!pic.tex || st.nSpares >= (int)dimof(st.spares)) {
        pic = CurlPicture();
        return;
    }
    pic.ok = false;
    Take(st.spares[st.nSpares++], pic);
}

// the next old Spread is copied into a spare
static void Refill(Gpu& g, Stage& st) {
    if (!g.inbox[kSlotNextOld].tex && st.nSpares > 0) {
        Take(g.inbox[kSlotNextOld], st.spares[--st.nSpares]);
    }
}

// a Curl leaves the air; its whole-canvas pictures become spares
static void Finish(Renderer* r, Gpu& g, Stage& st, int i, LandHow how, const char* why) {
    Flight* f = st.flights[i];
    f->times.gone = CurlNowMs();
    f->times.timedOut = how == LandHow::TimedOut;
    f->times.landedAtOnce = how == LandHow::AtOnce;
    Publish(r, *f);
    LogFlight(r, *f, how, why);
    KeepSpare(st, f->oldPic);
    KeepSpare(st, f->toPic);
    Refill(g, st);
    VecRemoveAt(st.flights, i);
    delete f;
}

// everything lands now and the layer goes; tell a waiting UI thread. why: for
// the log, landed at once
static void LandAll(Renderer* r, Gpu& g, Stage& st, LandHow how, const char* why = "") {
    while (len(st.flights) > 0) {
        Finish(r, g, st, 0, how, why);
    }
    if (CurlLayerShown(g.layer)) {
        CurlLayerShow(g.layer, false);
    }
    // nothing in the air: the memory goes
    g.inbox[kSlotLanding] = CurlPicture();
    for (int i = 0; i < st.nSpares; i++) {
        st.spares[i] = CurlPicture();
    }
    st.nSpares = 0;
    st.lastPresent = 0;
    PublishGone(r, st);
}

static bool SameWay(const TurnData& a, const TurnData& b) {
    return a.shape == b.shape && a.sweep == b.sweep && a.forward == b.forward;
}

// a turn the next one shares the air with: the same way, or a Bundle whose
// sheets all go back, the way the next one turns
static bool FliesWith(const Flight& f, const TurnData& next) {
    if (SameWay(f.turn, next)) {
        return true;
    }
    bool opposite = f.turn.shape == next.shape && f.turn.sweep != next.sweep && f.turn.forward != next.forward;
    return opposite && AllBack(f);
}

// sent back (or on again) from where it is. Heading back, SumatraPDF shows the
// old Spread: the new one comes from this tap's copy. New times, new id
static void SendBack(Renderer* r, Gpu& g, Stage& st, Flight& f, const TurnData& turn) {
    f.times.gone = CurlNowMs();
    Publish(r, f);
    LogFlight(r, f, LandHow::SentBack, "");

    f.back = !f.back;
    if (f.back && g.inbox[kSlotNextOld].ok) {
        std::swap(f.toPic, g.inbox[kSlotNextOld]);
    }
    g.inbox[kSlotNextOld].ok = false;
    f.turn.id = turn.id;
    f.turn.t0 = turn.t0;
    f.showsNew = false;
    f.seeThrough = false;
    f.refreshesLeft = -1;
    f.restShown = false;
    f.lastPresent = 0;
    f.times = CurlTimes();
    f.times.id = turn.id;
    f.times.start = turn.t0;
    st.needCover = turn.id;
    Publish(r, f);
}

static Flight* FindFlight(Stage& st, int clockId) {
    for (Flight* f : st.flights) {
        if (f->turn.clockId == clockId) {
            return f;
        }
    }
    return nullptr;
}

// taps the other way turned peeled of a Bundle's leaves back: as many of its
// sheets turn back from where they are, from its last sheet up; fewer, and
// the last ones turned back go on again. Like SendBack, the tap's cover
// hand-off rides on this Bundle
static void Peel(Gpu& g, Stage& st, Flight& f, const TurnData& turn) {
    int n = f.sheets.n;
    int want = SheetsBack(f.sheets, turn.peeled);
    int back = 0;
    for (int k = 0; k < n; k++) {
        back += GoesBack(f, k) ? 1 : 0;
    }
    for (int k = n - 1; k >= 0 && back < want; k--) {
        if (!GoesBack(f, k)) {
            f.ownT[k] = SheetTNow(f, k);
            f.dir[k] = -1;
            back++;
        }
    }
    for (int k = 0; k < n && back > want; k++) {
        if (GoesBack(f, k)) {
            f.dir[k] = 1;
            back--;
        }
    }
    f.peeled = turn.peeled;
    f.turn.id = turn.id;
    f.turn.t0 = turn.t0;
    f.turn.splitOf = turn.splitOf;
    f.turn.to.edgesLeft = turn.to.edgesLeft;
    f.turn.to.edgesRight = turn.to.edgesRight;
    f.showsNew = false;
    f.seeThrough = false;
    f.refreshesLeft = -1;
    f.restShown = false;
    g.inbox[kSlotNextOld].ok = false;
    st.needCover = turn.id;
}

// every tap starts its own Curl or Bundle, from the old Spread copied at the
// tap. Only turns the same way share the air (and sheets split off a Bundle,
// going back), a glide flies alone, and past 5 the oldest lands at once. So
// does a turn whose pages the new one crosses
static void OnStart(Renderer* r, Gpu& g, Stage& st, const TurnData& turn) {
    st.pace = turn.pace;
    Flight* sent = turn.back ? FindFlight(st, turn.clockId) : nullptr;
    if (sent) {
        st.lastStart = turn.id;
        SendBack(r, g, st, *sent, turn);
        return;
    }
    Flight* bundle = turn.splitOf ? FindFlight(st, turn.splitOf) : nullptr;
    if (bundle) {
        st.lastStart = turn.id;
        Peel(g, st, *bundle, turn);
        return;
    }

    for (int k = 0; k < turn.across.n; k++) {
        for (int i = len(st.flights) - 1; i >= 0; i--) {
            if (st.flights[i]->turn.clockId == turn.across.ids[k]) {
                Finish(r, g, st, i, LandHow::AtOnce, "jump across");
            }
        }
    }

    // pages sent back home lie where this one lifts: they'd hide it
    for (int i = len(st.flights) - 1; i >= 0; i--) {
        if (st.flights[i]->back && AtRest(*st.flights[i])) {
            Finish(r, g, st, i, LandHow::Done, "");
        }
    }

    // the swap chain follows the canvas; its buffers are undefined until the next Present
    Size size = turn.canvas;
    bool resize = !size.IsEmpty() && !(size == CurlLayerSize(g.layer));
    if (len(st.flights) > 0) {
        const Flight& last = *VecLast(st.flights);
        if (resize || last.glide.on || GlideOf(turn).on || !FliesWith(last, turn)) {
            LandAll(r, g, st, LandHow::AtOnce, "next Curl");
        }
    }
    if (len(st.flights) >= kMaxInAir) {
        Finish(r, g, st, 0, LandHow::AtOnce, "sixth Curl");
    }
    if (resize) {
        if (CurlLayerShown(g.layer)) {
            CurlLayerShow(g.layer, false);
        }
        CurlLayerResize(g.layer, size);
    }

    // set only now: landing the others above must not count this Start as done
    st.lastStart = turn.id;

    // test of the test (FlashRecorder): the first frame comes late
    if (int lagMs = FlashLayerLagMs()) {
        Sleep((DWORD)lagMs);
    }
    auto f = new Flight();
    Take(f->oldPic, g.inbox[kSlotNextOld]);
    Refill(g, st);
    f->turn = turn;
    f->geo = GeometryOf(turn);
    f->glide = GlideOf(turn);
    f->sheets = turn.sheets;
    f->advancedAt = turn.t0;
    f->refreshMs = RefreshMs(r->hwnd);
    f->times.id = turn.id;
    f->times.start = turn.t0;
    // the one before lands on the page this one's copy shows
    if (len(st.flights) > 0 && f->oldPic.ok) {
        VecLast(st.flights)->landPic = CurlPicture();
    }
    VecAppend(st.flights, f);
    st.needCover = turn.id;
    Publish(r, *f);
    PublishGone(r, st);
}

// SumatraPDF shows the newest command's Spread: for a new Curl, its landing
// page; sent back, its old Spread
static void OnShowsNew(Gpu& g, Stage& st, const Cmd& c) {
    Flight* f = nullptr;
    for (Flight* x : st.flights) {
        if (!f || x->turn.id > f->turn.id) {
            f = x;
        }
    }
    if (!f) {
        return;
    }
    if (!f->back) {
        f->turn.to = c.now;
        f->geo = GeometryOf(f->turn);
        // glide or not was decided at the start; only the places follow the layout
        Glide gl = GlideOf(f->turn);
        if (gl.valid) {
            gl.on = f->glide.on;
            f->glide = gl;
        }
        if (c.haveLanding && g.inbox[kSlotLanding].ok) {
            std::swap(f->landPic, g.inbox[kSlotLanding]);
        }
    }
    g.inbox[kSlotLanding].ok = false;
    if (!f->showsNew) {
        f->showsNew = true;
        f->times.showsNew = CurlNowMs();
    }
}

// the layer never stays longer than this, whatever SumatraPDF does
static double StuckMs(const Flight& f) {
    return std::max(kStuckMs, f.turn.durationMs * (double)f.glide.endT + 1000);
}

static bool IsStuck(const Stage& st, double now) {
    for (Flight* f : st.flights) {
        if (now - f->turn.t0 > StuckMs(*f)) {
            return true;
        }
    }
    return false;
}

// every Curl in the air moves on at the current length, so a Curl sped up by
// fast tapping keeps its progress; sent back, it moves back
static void Advance(Stage& st, double now) {
    for (Flight* f : st.flights) {
        int lenMs = std::max(st.pace.LengthMs(f->turn.kind, (i64)now), 1);
        double dt = now - f->advancedAt;
        f->advancedAt = now;
        if (dt <= 0) {
            continue;
        }
        float step = (float)(dt / lenMs);
        f->t = f->back ? std::max(0.0f, f->t - step) : std::min(EndT(*f), f->t + step);
        for (int k = 0; k < f->sheets.n; k++) {
            f->ownT[k] = std::clamp(f->ownT[k] + f->dir[k] * step, 0.0f, 1.0f);
        }
    }
}

static bool Rested(const Flight& f) {
    return AtRest(f) && f.restShown;
}

// Curls at rest leave the air: the oldest once landed (the next one's copy
// has its page), any once back where it started. The last one waits until
// SumatraPDF shows the final Spread
static void DropRested(Renderer* r, Gpu& g, Stage& st) {
    for (int i = len(st.flights) - 1; i >= 0 && len(st.flights) > 1; i--) {
        Flight* f = st.flights[i];
        if (!f->back || !Rested(*f)) {
            continue;
        }
        // the newest: its old Spread is the new Spread of the one before
        if (i == len(st.flights) - 1) {
            Take(st.flights[i - 1]->toPic, f->oldPic);
        }
        Finish(r, g, st, i, LandHow::Done, "");
    }
    while (len(st.flights) > 1 && !st.flights[0]->back && Rested(*st.flights[0])) {
        Finish(r, g, st, 0, LandHow::Done, "");
    }
    for (Flight* f : st.flights) {
        if (!Rested(*f)) {
            return;
        }
    }
    if (len(st.flights) > 0 && VecLast(st.flights)->seeThrough) {
        while (len(st.flights) > 0) {
            Finish(r, g, st, 0, LandHow::Done, "");
        }
    }
}

// uncovered parts go see-through one refresh after SumatraPDF shows the new Spread
static void CountToSeeThrough(Flight& f, double now) {
    if (!f.showsNew || f.seeThrough) {
        return;
    }
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

// the compositor took the covering frame: it's on screen
static void NoteCovered(Renderer* r, Stage& st, double now) {
    if (!st.pendingCover) {
        return;
    }
    int id = st.pendingCover;
    st.pendingCover = 0;
    r->mutex.Lock();
    r->coveredId = std::max(r->coveredId, id);
    r->mutex.Unlock();
    for (Flight* f : st.flights) {
        if (f->turn.id == id) {
            f->times.covered = now;
            Publish(r, *f);
        }
    }
    SetEvent(r->covered);
}

// false: the device is gone, stop drawing
static bool DrawFrame(Renderer* r, Gpu& g, Stage& st) {
    double now = CurlNowMs();
    NoteCovered(r, st, now);
    for (Flight* f : st.flights) {
        CountToSeeThrough(*f, now);
    }
    Advance(st, now);
    DropRested(r, g, st);
    bool stuck = IsStuck(st, now);
    if (len(st.flights) == 0 || stuck) {
        // this frame signal is used up: present a see-through frame, or the
        // latency handle never signals again
        RenderFrame(g, st, Scene::Empty);
        CurlLayerPresent(g.layer);
        LandAll(r, g, st, stuck ? LandHow::TimedOut : LandHow::Done);
        return true;
    }
    PublishGone(r, st);

    RenderFrame(g, st, Scene::Curl);
    if (!CurlLayerPresent(g.layer)) {
        LandAll(r, g, st, LandHow::AtOnce, "Present failed");
        return false;
    }
    double presented = CurlNowMs();
    st.lastPresent = presented;
    st.refreshMs = st.flights[0]->refreshMs;
    for (Flight* f : st.flights) {
        if (f->lastPresent > 0) {
            f->times.frames++;
            if (presented - f->lastPresent > kLateFactor * f->refreshMs) {
                f->times.lateFrames++;
            }
        }
        f->lastPresent = presented;
        if (AtRest(*f) && !f->restShown) {
            f->restShown = true;
            f->times.landed = presented;
        }
    }

    if (st.needCover) {
        if (!CurlLayerShown(g.layer)) {
            CurlLayerShow(g.layer, true);
        }
        for (Flight* f : st.flights) {
            if (f->turn.id == st.needCover) {
                f->times.coverShown = presented;
            }
        }
        st.pendingCover = st.needCover;
        st.needCover = 0;
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
static void ReturnFrame(Gpu& g, const Stage& st) {
    if (!CurlLayerTarget(g.layer)) {
        return;
    }
    RenderFrame(g, st, Scene::Empty);
    CurlLayerPresent(g.layer);
}

// copies the old Spread or a landing page while the UI thread waits;
// learns how long a copy takes
static void ServeCopy(Renderer* r, Gpu& g, Stage& st) {
    int px = ScreenCopyPendingPx(r->desk);
    if (px <= 0) {
        return;
    }
    double t0 = CurlNowMs();
    ScreenCopyServe(r->desk, g.dev.Get(), g.inbox, kSlotCount);
    double ms = CurlNowMs() - t0;
    st.copyMsPerMpx = ms / (px / 1e6);
}

// a copy takes about a frame at 4K: one that would run into the next frame
// waits for it, at most this long; 0: copy now
static double CopyWaitMs(const Stage& st, int px) {
    if (px <= 0 || st.lastPresent <= 0) {
        return 0;
    }
    double now = CurlNowMs();
    double due = st.lastPresent + st.refreshMs;
    double copyMs = px / 1e6 * st.copyMsPerMpx;
    if (now + copyMs + kCopyMarginMs < due) {
        return 0;
    }
    return std::max(0.0, due - now) + kCopyMarginMs;
}

static void DrawLoop(Renderer* r, Gpu& g) {
    Stage st;
    Vec<Cmd> cmds;
    while (true) {
        bool animating = len(st.flights) > 0;
        HANDLE handles[2] = {r->wake, CurlLayerFrameWait(g.layer)};
        DWORD n = animating ? 2 : 1;
        DWORD res = WaitForMultipleObjects(n, handles, FALSE, animating ? kFrameWaitMs : kIdleWaitMs);
        bool frameSignal = res == WAIT_OBJECT_0 + 1;
        if (!IsWindow(r->hwnd)) {
            for (Flight* f : st.flights) {
                delete f;
            }
            return;
        }

        TakeCmds(r, cmds);
        for (const Cmd& c : cmds) {
            switch (c.kind) {
                case CmdKind::Quit:
                    LandAll(r, g, st, LandHow::AtOnce, "window closed");
                    return;
                case CmdKind::LandAll:
                    LandAll(r, g, st, LandHow::AtOnce, c.why);
                    break;
                case CmdKind::Resize:
                    LandAll(r, g, st, LandHow::AtOnce, c.why);
                    CurlLayerResize(g.layer, c.size);
                    break;
                case CmdKind::Start:
                    OnStart(r, g, st, c.turn);
                    break;
                case CmdKind::ShowsNew:
                    OnShowsNew(g, st, c);
                    break;
            }
        }
        if (len(st.flights) == 0) {
            ServeCopy(r, g, st);
            if (frameSignal) {
                ReturnFrame(g, st);
            }
            continue;
        }

        // the UI thread waits for a copy (its Start or ShowsNew comes after
        // it). One that would run into the next frame waits until it's out
        if (!frameSignal) {
            double waitMs = CopyWaitMs(st, ScreenCopyPendingPx(r->desk));
            if (waitMs > 0) {
                frameSignal = WaitForSingleObject(CurlLayerFrameWait(g.layer), (DWORD)waitMs) == WAIT_OBJECT_0;
            }
        }

        // only a frame signal may draw: then Present doesn't block
        if (frameSignal && !DrawFrame(r, g, st)) {
            logf("skeuomorph-layer: Present failed, Curls are off in this window\n");
            ServeCopy(r, g, st);
            return;
        }
        ServeCopy(r, g, st);
        if (!frameSignal && IsStuck(st, CurlNowMs())) {
            LandAll(r, g, st, LandHow::TimedOut);
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

// hook 3: win->buffer still holds the last painted frame. The whole canvas,
// so a side with no page (the cover) has its background too, and so does a
// taller landing page (pages of different sizes)
void CurlCopyOld(MainWindow* win, CurlSpread from) {
    Renderer* r = FindRenderer(win);
    if (!r || !IsLoaded(r)) {
        return;
    }
    if (UnionOf(from.left, from.right).IsEmpty()) {
        return;
    }
    Rect area(Point(), ScreenCopySize(win));
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
    r->forward = turn.forward;
    r->landingCopy = !turn.back;

    Cmd c;
    c.kind = CmdKind::Start;
    TurnData& t = c.turn;
    t.clockId = turn.clockId;
    t.back = turn.back;
    t.kind = turn.kind;
    t.sheets = turn.sheets;
    t.firstPage = turn.firstPage;
    t.splitOf = turn.splitOf;
    t.peeled = turn.peeled;
    t.across = turn.across;
    t.dpi = turn.dpi;
    t.pace = turn.pace;
    t.t0 = CurlNowMs();
    t.shape = turn.shape;
    t.sweep = turn.sweep;
    t.forward = turn.forward;
    t.durationMs = turn.durationMs;
    t.from = r->from;
    t.to = turn.to;
    t.paper = turn.paper;
    t.edgesPx = turn.edgesPx;
    t.canvas = ScreenCopySize(win);
    r->glide = GlideOf(t).on;
    r->split = turn.splitOf != 0;
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
    CurlTimes& slot = TimesSlot(r, id);
    if (slot.id != id) {
        slot = CurlTimes();
        slot.id = id;
    }
    slot.uiWaitMs = CurlNowMs() - t.t0;
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
    // Single Page: only a backward turn lands a page
    Rect landing;
    if (r->shape == CurlShape::Spread) {
        landing = r->sweep == Sweep::ToRight ? now.right : now.left;
    } else if (!r->forward) {
        landing = now.left;
    }
    // a glide shows the new pages scaled in a moving view, then the new
    // screen; a split-off sheet's Bundle lands on the other page
    if (r->glide || r->split) {
        landing = Rect(Point(), ScreenCopySize(win));
    }
    if (!r->landingCopy) {
        landing = Rect();
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
    *out = r->times[r->lastTimes % kTimesKept];
    r->mutex.Unlock();
    return true;
}

bool CurlTimesOf(MainWindow* win, int id, CurlTimes* out) {
    Renderer* r = FindRenderer(win);
    if (!r || id <= 0) {
        return false;
    }
    r->mutex.Lock();
    *out = TimesSlot(r, id);
    r->mutex.Unlock();
    return out->id == id;
}

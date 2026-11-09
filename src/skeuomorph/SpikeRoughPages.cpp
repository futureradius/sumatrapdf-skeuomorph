// Throwaway spike for "How fast do Rough Pages arrive, and how much memory do Curls need?" (#20). Never merged.
//
// Off unless the environment variable SPIKE_ROUGH is set, e.g.
//   SPIKE_ROUGH=mode=tap,rough=1,size=1920x1200,out=C:\x,label=A
//
//   mode=tap|hold   tap = a Page Turn every 77 ms (fastest tapping); hold = this PC's key repeat (delay, then rate)
//   rough=0|1       1 = one worker thread draws Rough Pages from a background copy of the document (Clone()),
//                   as decided in #18; 0 = no Rough Pages (to compare SumatraPDF's landing and memory)
//   size=WxH|max    canvas size
//   bursts=N taps=N bursts per run and taps per burst (hold: taps = key repeats)
//   gpu=1           also allocate the worst-case GPU images (5 Curls + a Bundle) and measure GPU memory
//
// The Curls are not drawn: only their timing is modelled (Curl look-and-feel decisions in #8 and #15):
// 700 ms, fast taps shorten every Curl in the air to gap x 4 (never below 310 ms), at most 5 in the air (the oldest
// lands at once). From that, every page gets a "visible" window, the time it is on a Curl face, revealed under a
// Curl, or lying on the left until the next Curl lands on it. The run records when each page first has a picture:
// sharp (SumatraPDF showed its Spread fully drawn) or Rough (the worker finished it), and writes
// <out>\<label>.txt and <label>-pages.csv.

#include "base/Base.h"
#include "base/Win.h"
#include "base/Pixmap.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "gui/UIModels.h"
#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "RenderCache.h"
#include "EngineMupdf.h"
#include "MainWindow.h"
#include "SumatraPDF.h"
#include "SpikeRoughPages.h"

#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN8
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#pragma pop_macro("NTDDI_VERSION")
#include <psapi.h>
#include <mmsystem.h>

#include <vector>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

using Microsoft::WRL::ComPtr;

static const double kNormalMs = 700.0;
static const double kMinMs = 310.0;
static const int kMaxInAir = 5;
static const double kTapGapMs = 77.0;
static const double kInf = 1e18;
static const UINT WM_SPIKE_INIT = WM_APP + 0x61;
static const UINT WM_SPIKE_TAP = WM_APP + 0x62;
static const UINT_PTR kTickTimer = 1;

//--- config, log, time

struct Config {
    bool enabled = false;
    bool hold = false;
    bool rough = true;
    bool maximize = false;
    bool gpu = false;
    bool keepStore = true; // store=0: empty the copy's MuPDF cache after each Rough Page
    float scale = 0.5f;    // Rough Page size relative to the page on screen (0.5 = half width, half height)
    int cw = 1920, ch = 1200;
    int bursts = 3;
    int taps = 24;
    char out[MAX_PATH] = ".";
    char label[64] = "run";
};

static Config gCfg;
static bool gCfgRead = false;
static FILE* gLog = nullptr;
static LARGE_INTEGER gFreq;

static double NowMs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)gFreq.QuadPart;
}

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (gLog) {
        fputs(buf, gLog);
        fflush(gLog);
    }
}

static void ReadConfig() {
    gCfgRead = true;
    QueryPerformanceFrequency(&gFreq);
    char v[1024];
    DWORD n = GetEnvironmentVariableA("SPIKE_ROUGH", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) {
        return;
    }
    gCfg.enabled = true;
    char* ctx = nullptr;
    for (char* tok = strtok_s(v, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx)) {
        char* eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        const char* k = tok;
        const char* val = eq + 1;
        if (!strcmp(k, "mode")) {
            gCfg.hold = !strcmp(val, "hold");
        } else if (!strcmp(k, "rough")) {
            gCfg.rough = atoi(val) != 0;
        } else if (!strcmp(k, "scale")) {
            gCfg.scale = (float)atof(val);
        } else if (!strcmp(k, "store")) {
            gCfg.keepStore = atoi(val) != 0;
        } else if (!strcmp(k, "gpu")) {
            gCfg.gpu = atoi(val) != 0;
        } else if (!strcmp(k, "size")) {
            if (!strcmp(val, "max")) {
                gCfg.maximize = true;
            } else {
                sscanf_s(val, "%dx%d", &gCfg.cw, &gCfg.ch);
            }
        } else if (!strcmp(k, "bursts")) {
            gCfg.bursts = atoi(val);
        } else if (!strcmp(k, "taps")) {
            gCfg.taps = atoi(val);
        } else if (!strcmp(k, "out")) {
            strcpy_s(gCfg.out, val);
        } else if (!strcmp(k, "label")) {
            strcpy_s(gCfg.label, val);
        }
    }
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", gCfg.out, gCfg.label);
    fopen_s(&gLog, path, "w");
}

static double Pct(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

static double PrivateMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
    return pmc.PrivateUsage / (1024.0 * 1024.0);
}

static double PeakPrivateMB() {
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.PeakPagefileUsage / (1024.0 * 1024.0);
}

//--- shared state (gLock)

struct PageState {
    int burst = -1;
    double vs = -1;       // first moment the page needs a picture (on a Curl face or revealed under one)
    double ve = kInf;     // covered or gone (kInf: still showing)
    double sharpT = kInf; // SumatraPDF showed it fully drawn
    double roughT = kInf; // Rough Page finished
    double roughMs = 0;   // time to draw the Rough Page
    bool final = false;   // on the Spread a burst landed on
    bool failed = false;
    Pixmap* rough = nullptr;
};

struct Curl {
    int burst = 0;
    double tap = 0;
    double p = 0;
    double lastT = 0;
    bool landed = false;
    bool cut = false; // landed at once because a 6th went up
    bool halfDone = false;
    double tLand = 0;
    int oldL = 0, oldR = 0, newL = 0, newR = 0;
};

static CRITICAL_SECTION gLock;
static HANDLE gWake;
static std::vector<PageState> gPages;
static std::vector<Curl> gCurls;
static double gDur = kNormalMs;
static double gLastTap = -1e9;
static int gBurst = -1;
static bool gIgnoreTurns = true;
static MainWindow* gWin = nullptr;
static HWND gMsgWnd = nullptr;
static int gPageCount = 0;
static float gZoom = 1;
static int gRotation = 0;

// worker
static EngineBase* gCopy = nullptr;
static AbortCookie* gCookie = nullptr;
static int gDrawing = 0;
static bool gAbortAsked = false;
static bool gQuit = false;
static double gCloneMs = 0, gPrivBeforeClone = 0, gPrivAfterClone = 0;
static int gRoughDone = 0, gRoughAborted = 0, gRoughFailed = 0;
static i64 gRoughBytes = 0, gRoughBytesPeak = 0;
static int gRoughHeld = 0, gRoughHeldPeak = 0;
static std::vector<double> gRenderMs;
static HANDLE gWorkerReady;
static HANDLE gWorkerThread = nullptr;

static void FreeRough(PageState& ps) {
    if (ps.rough) {
        gRoughBytes -= (i64)ps.rough->stride * ps.rough->height;
        gRoughHeld--;
        FreePixmap(ps.rough);
        ps.rough = nullptr;
    }
}

// Moves every Curl in the air on to time t (under gLock).
static void Advance(double t) {
    for (Curl& c : gCurls) {
        if (c.landed) {
            continue;
        }
        double p = c.p + (t - c.lastT) / gDur;
        if (!c.halfDone && p >= 0.5) {
            c.halfDone = true;
            double th = c.lastT + (0.5 - c.p) * gDur;
            gPages[c.oldR].ve = std::min(gPages[c.oldR].ve, th);
        }
        if (p >= 1) {
            c.landed = true;
            c.tLand = c.lastT + (1 - c.p) * gDur;
            // the landing page covers the old left page
            if (c.oldL > 0) {
                gPages[c.oldL].ve = std::min(gPages[c.oldL].ve, c.tLand);
            }
            p = 1;
        }
        c.p = p;
        c.lastT = t;
    }
    // tapping stopped: Curls ease back to normal speed
    if (t - gLastTap > gDur) {
        gDur = kNormalMs;
    }
    // Rough Pages that are off screen are thrown away; so are those SumatraPDF has drawn sharp
    for (int p = 1; p <= gPageCount; p++) {
        PageState& ps = gPages[p];
        if (ps.rough && (t >= ps.ve || ps.sharpT < kInf)) {
            FreeRough(ps);
        }
    }
}

static int InAir() {
    int n = 0;
    for (Curl& c : gCurls) {
        n += c.landed ? 0 : 1;
    }
    return n;
}

static bool Wanted(int p, double t) {
    PageState& ps = gPages[p];
    return ps.vs >= 0 && ps.vs <= t && t < ps.ve && ps.sharpT == kInf && ps.roughT == kInf && !ps.failed;
}

// oldest need first (the oldest Curl's faces), then page order
static int NextWanted(double t) {
    int best = 0;
    for (int p = 1; p <= gPageCount; p++) {
        if (!Wanted(p, t)) {
            continue;
        }
        if (!best || gPages[p].vs < gPages[best].vs) {
            best = p;
        }
    }
    return best;
}

//--- worker thread: one Rough Page at a time from the background copy

static DWORD WINAPI Worker(void* arg) {
    EngineBase* src = (EngineBase*)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    gPrivBeforeClone = PrivateMB();
    double t0 = NowMs();
    gCopy = src->Clone();
    gCloneMs = NowMs() - t0;
    gPrivAfterClone = PrivateMB();
    src->Release();
    SetEvent(gWorkerReady);
    if (!gCopy) {
        return 0;
    }
    while (!gQuit) {
        EnterCriticalSection(&gLock);
        double t = NowMs();
        Advance(t);
        int page = NextWanted(t);
        float zoom = gZoom * gCfg.scale;
        int rot = gRotation;
        if (page) {
            gDrawing = page;
            gAbortAsked = false;
        }
        LeaveCriticalSection(&gLock);
        if (!page) {
            WaitForSingleObject(gWake, 5);
            continue;
        }
        double r0 = NowMs();
        AbortCookie* cookie = nullptr;
        RenderPageArgs args(page, zoom, rot, nullptr, RenderTarget::View, &cookie);
        // the UI thread reads gCookie under gLock to abort; the engine sets it at the start of RenderPage
        EnterCriticalSection(&gLock);
        LeaveCriticalSection(&gLock);
        gCookie = nullptr;
        args.cookie_out = &gCookie;
        Pixmap* bmp = gCopy->RenderPage(args);
        if (!gCfg.keepStore) {
            if (EngineMupdf* em = AsEngineMupdf(gCopy)) {
                fz_empty_store(em->Ctx());
            }
        }
        double r1 = NowMs();
        EnterCriticalSection(&gLock);
        AbortCookie* c = gCookie;
        gCookie = nullptr;
        bool aborted = gAbortAsked;
        gDrawing = 0;
        PageState& ps = gPages[page];
        if (aborted || !Wanted(page, r1)) {
            gRoughAborted++;
            FreePixmap(bmp);
        } else if (!bmp) {
            ps.failed = true;
            gRoughFailed++;
        } else {
            ps.roughT = r1;
            ps.roughMs = r1 - r0;
            ps.rough = bmp;
            gRoughBytes += (i64)bmp->stride * bmp->height;
            gRoughHeld++;
            gRoughBytesPeak = std::max(gRoughBytesPeak, gRoughBytes);
            gRoughHeldPeak = std::max(gRoughHeldPeak, gRoughHeld);
            gRoughDone++;
            gRenderMs.push_back(r1 - r0);
        }
        LeaveCriticalSection(&gLock);
        delete c;
    }
    return 0;
}

// UI thread, every tick: abort the Rough Page being drawn if nobody needs it any more
static void CheckAbort() {
    EnterCriticalSection(&gLock);
    double t = NowMs();
    Advance(t);
    if (gDrawing && !gAbortAsked) {
        PageState& ps = gPages[gDrawing];
        if (t >= ps.ve || ps.sharpT < kInf) {
            gAbortAsked = true;
            if (gCookie) {
                gCookie->Abort();
            }
        }
    }
    LeaveCriticalSection(&gLock);
}

//--- hooks

void SpikeBeforeSpreadChange(MainWindow* win, DisplayModel* dm, int newStartPage) {
    if (!gCfg.enabled || win != gWin || gIgnoreTurns || gBurst < 0) {
        return;
    }
    int oldL = dm->CurrentPageNo();
    if (newStartPage <= oldL) {
        return; // only forward turns in this test
    }
    double t = NowMs();
    EnterCriticalSection(&gLock);
    Advance(t);
    double gap = t - gLastTap;
    gDur = gap < kNormalMs ? std::max(kMinMs, std::min(kNormalMs, gap * 4)) : kNormalMs;
    gLastTap = t;
    if (InAir() >= kMaxInAir) {
        for (Curl& c : gCurls) {
            if (!c.landed) {
                if (!c.halfDone) {
                    c.halfDone = true;
                    gPages[c.oldR].ve = std::min(gPages[c.oldR].ve, t);
                }
                c.landed = true;
                c.cut = true;
                c.tLand = t;
                c.p = 1;
                if (c.oldL > 0) {
                    gPages[c.oldL].ve = std::min(gPages[c.oldL].ve, t);
                }
                break;
            }
        }
    }
    Curl c;
    c.burst = gBurst;
    c.tap = c.lastT = t;
    c.oldL = oldL;
    c.oldR = oldL + 1 <= gPageCount ? oldL + 1 : 0;
    c.newL = newStartPage;
    c.newR = newStartPage + 1 <= gPageCount ? newStartPage + 1 : 0;
    if (c.oldR) {
        // the front of the lifting page
        PageState& f = gPages[c.oldR];
        if (f.vs < 0) {
            f.vs = t;
            f.burst = gBurst;
        }
    }
    // the back of the lifting page, and the page revealed under it
    for (int p : {c.newL, c.newR}) {
        if (p > 0) {
            PageState& ps = gPages[p];
            ps.vs = t;
            ps.burst = gBurst;
            ps.ve = kInf;
        }
    }
    gCurls.push_back(c);
    LeaveCriticalSection(&gLock);
    SetEvent(gWake);
}

static void MarkSharpIfDrawn(MainWindow* win) {
    DisplayModel* dm = win->AsFixed();
    if (!dm || !gRenderCache || !gRenderCache->VisibleTargetTilesReady(dm)) {
        return;
    }
    double t = NowMs();
    EnterCriticalSection(&gLock);
    for (int p = 1; p <= gPageCount; p++) {
        PageInfo* pi = dm->GetPageInfo(p);
        if (pi && pi->visibleRatio > 0 && gPages[p].sharpT == kInf) {
            gPages[p].sharpT = t;
        }
    }
    LeaveCriticalSection(&gLock);
}

static void EnsureMsgWnd();

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
        EnsureMsgWnd();
        PostMessageW(gMsgWnd, WM_SPIKE_INIT, 0, 0);
        return;
    }
    if (win == gWin && flushed && !gPages.empty()) {
        MarkSharpIfDrawn(win);
    }
}

//--- GPU memory: allocate the worst-case pictures of 5 Curls + a Bundle

static void MeasureGpu(int canvasW, int canvasH, int pageW, int pageH) {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        Log("GPU: no device (0x%08x)\n", (unsigned)hr);
        return;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    dev.As(&dxgiDev);
    ComPtr<IDXGIAdapter> ad;
    dxgiDev->GetAdapter(&ad);
    ComPtr<IDXGIAdapter3> ad3;
    ad.As(&ad3);
    DXGI_ADAPTER_DESC desc;
    ad->GetDesc(&desc);
    auto usage = [&](double& local, double& nonlocal) {
        DXGI_QUERY_VIDEO_MEMORY_INFO a = {}, b = {};
        ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &a);
        ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &b);
        local = a.CurrentUsage / (1024.0 * 1024.0);
        nonlocal = b.CurrentUsage / (1024.0 * 1024.0);
    };
    std::vector<ComPtr<ID3D11Texture2D>> keep;
    double planned = 0;
    auto alloc = [&](int w, int h, DXGI_FORMAT fmt, UINT bind, UINT misc, int count, const char* what) {
        double mb = 0;
        for (int i = 0; i < count; i++) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = bind;
            td.MiscFlags = misc;
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex))) {
                Log("GPU: texture %dx%d failed\n", w, h);
                return;
            }
            // touch it so the driver really commits the memory
            float clear[4] = {1, 1, 1, 1};
            if (bind & D3D11_BIND_RENDER_TARGET) {
                ComPtr<ID3D11RenderTargetView> rtv;
                dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
                ctx->ClearRenderTargetView(rtv.Get(), clear);
            }
            keep.push_back(tex);
            mb += (double)w * h * 4 / (1024.0 * 1024.0);
        }
        planned += mb;
        Log("  %-58s %2d x %4dx%-4d = %6.1f MB\n", what, count, w, h, mb);
    };
    double l0, n0, l1, n1;
    ctx->Flush();
    Sleep(200);
    usage(l0, n0);
    Log("GPU worst case, canvas %dx%d, page %dx%d, adapter %ls\n", canvasW, canvasH, pageW, pageH, desc.Description);
    UINT rt = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    UINT gdi = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    // the layer's own buffers (2 swap-chain buffers + depth), as in #19
    alloc(canvasW, canvasH, DXGI_FORMAT_B8G8R8A8_UNORM, rt, 0, 2, "layer: swap chain buffers");
    alloc(canvasW, canvasH, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL, 0, 1, "layer: depth buffer");
    // per Curl/Bundle in the air: copy of the old Spread (2 pages) + the landing page (#19)
    alloc(pageW * 2, pageH, DXGI_FORMAT_B8G8R8A8_UNORM, rt, gdi, 6,
          "screen copy of the old Spread, per Curl/Bundle (6)");
    alloc(pageW, pageH, DXGI_FORMAT_B8G8R8A8_UNORM, rt, gdi, 6, "screen copy of the landing page, per Curl/Bundle (6)");
    // Rough Pages: half width, half height; at most 10 faces (5 Curls x 2)
    alloc(pageW / 2, pageH / 2, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 0, 10,
          "Rough Pages (10 faces)");
    ctx->Flush();
    Sleep(300);
    usage(l1, n1);
    Log("  planned %.1f MB; measured GPU memory +%.1f MB local, +%.1f MB shared (now %.1f / %.1f)\n", planned, l1 - l0,
        n1 - n0, l1, n1);
    keep.clear();
}

//--- driver

enum class St {
    Settle,
    Burst,
    Drain,
    Done
};
static St gSt = St::Settle;
static int gBurstNo = 0;
static double gStateAt = 0;
static int gBurstStart[16];
static double gBurstFirstTap[16], gBurstLastTap[16];
static int gTapsPosted = 0;
static int gRepeatDelayMs = 500;
static double gRepeatGapMs = 33.3;
static HANDLE gTapThread = nullptr;

static DWORD WINAPI TapThread(void*) {
    timeBeginPeriod(1);
    double t0 = NowMs();
    for (int i = 0; i < gCfg.taps; i++) {
        double at;
        if (gCfg.hold) {
            at = i == 0 ? 0 : gRepeatDelayMs + (i - 1) * gRepeatGapMs;
        } else {
            at = i * kTapGapMs;
        }
        for (;;) {
            double left = t0 + at - NowMs();
            if (left <= 0) {
                break;
            }
            Sleep(left > 3 ? (DWORD)(left - 2) : 0);
        }
        PostMessageW(gMsgWnd, WM_SPIKE_TAP, 0, 0);
    }
    timeEndPeriod(1);
    return 0;
}

static bool SpreadSharp(int startPage) {
    EnterCriticalSection(&gLock);
    bool ok = gPages[startPage].sharpT < kInf && (startPage + 1 > gPageCount || gPages[startPage + 1].sharpT < kInf);
    LeaveCriticalSection(&gLock);
    return ok;
}

static void WriteResults();

static void StartBurstAt(int page) {
    gIgnoreTurns = true;
    gWin->ctrl->GoToPage(page, false);
    gIgnoreTurns = false;
    gSt = St::Settle;
    gStateAt = NowMs();
}

static void Tick() {
    CheckAbort();
    double t = NowMs();
    DisplayModel* dm = gWin->AsFixed();
    switch (gSt) {
        case St::Settle:
            if (t - gStateAt > 1500 && (SpreadSharp(dm->CurrentPageNo()) || t - gStateAt > 6000)) {
                // the starting Spread was shown by SumatraPDF before the burst
                EnterCriticalSection(&gLock);
                int s = dm->CurrentPageNo();
                for (int p = s; p <= std::min(s + 1, gPageCount); p++) {
                    gPages[p].vs = -1;
                }
                gBurst = gBurstNo;
                LeaveCriticalSection(&gLock);
                gBurstStart[gBurstNo] = s;
                gBurstFirstTap[gBurstNo] = 0;
                gTapsPosted = 0;
                gSt = St::Burst;
                gStateAt = t;
                gTapThread = CreateThread(nullptr, 0, TapThread, nullptr, 0, nullptr);
            }
            break;
        case St::Burst:
            if (WaitForSingleObject(gTapThread, 0) == WAIT_OBJECT_0) {
                CloseHandle(gTapThread);
                gTapThread = nullptr;
                gSt = St::Drain;
                gStateAt = t;
            }
            break;
        case St::Drain: {
            EnterCriticalSection(&gLock);
            bool air = InAir() > 0;
            LeaveCriticalSection(&gLock);
            int s = dm->CurrentPageNo();
            if ((!air && SpreadSharp(s) && t - gStateAt > 500) || t - gStateAt > 10000) {
                EnterCriticalSection(&gLock);
                for (int p = s; p <= std::min(s + 1, gPageCount); p++) {
                    gPages[p].final = true;
                }
                gBurst = -1;
                LeaveCriticalSection(&gLock);
                gBurstNo++;
                if (gBurstNo >= gCfg.bursts || s + 2 * gCfg.taps + 4 > gPageCount) {
                    gSt = St::Done;
                    WriteResults();
                    return;
                }
                StartBurstAt(s + 4);
            }
            break;
        }
        case St::Done:
            break;
    }
}

static void OnTap() {
    double t = NowMs();
    if (gBurstFirstTap[gBurstNo] == 0) {
        gBurstFirstTap[gBurstNo] = t;
    }
    gBurstLastTap[gBurstNo] = t;
    gTapsPosted++;
    gWin->ctrl->GoToNextPage();
}

static void PumpFor(double ms) {
    MSG m;
    double until = NowMs() + ms;
    while (NowMs() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
}

static void OnInit() {
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
    if (dm->GetDisplayMode() != DisplayMode::Facing) {
        gWin->ctrl->SetDisplayMode(DisplayMode::Facing);
    }
    PumpFor(1000);
    dm = gWin->AsFixed();
    RECT cr;
    GetClientRect(gWin->hwndCanvas, &cr);
    gPageCount = dm->PageCount();
    gPages.resize(gPageCount + 2);
    gZoom = dm->GetZoomReal(1);
    gRotation = dm->GetRotation();
    PageInfo* pi = dm->GetPageInfo(dm->CurrentPageNo());
    int pageW = pi ? pi->pageOnScreen.dx : 0, pageH = pi ? pi->pageOnScreen.dy : 0;

    UINT speed = 31, delay = 1;
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    gRepeatDelayMs = 250 * (delay + 1);
    // documented range: 0 = ~2.5 repeats/s, 31 = ~30 repeats/s
    gRepeatGapMs = 1000.0 / (2.5 + speed * (27.5 / 31.0));

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    Log("Rough Page spike: %s, rough=%d, canvas %dx%d, page %dx%d at zoom %.3f, %d pages, %u logical processors\n",
        gCfg.hold ? "hold a key" : "tap every 77 ms", gCfg.rough ? 1 : 0, (int)cr.right, (int)cr.bottom, pageW, pageH,
        gZoom, gPageCount, si.dwNumberOfProcessors);
    if (gCfg.hold) {
        Log("Key repeat on this PC: delay %d ms, then every %.1f ms (speed %u)\n", gRepeatDelayMs, gRepeatGapMs, speed);
    }
    Str fp = dm->GetEngine()->FilePath();
    Log("File: %.*s\n", fp.len, fp.s ? fp.s : "");
    Log("Private memory before the copy: %.1f MB\n", PrivateMB());

    if (gCfg.gpu) {
        MeasureGpu(cr.right, cr.bottom, pageW, pageH);
    }
    if (gCfg.rough) {
        EngineBase* e = dm->GetEngine();
        e->AddRef();
        gWorkerThread = CreateThread(nullptr, 0, Worker, e, 0, nullptr);
        WaitForSingleObject(gWorkerReady, 30000);
        Log("Background copy (Clone): %.1f ms, private memory %.1f -> %.1f MB (+%.1f)\n", gCloneMs, gPrivBeforeClone,
            gPrivAfterClone, gPrivAfterClone - gPrivBeforeClone);
    }
    gBurstNo = 0;
    StartBurstAt(1);
    SetTimer(gMsgWnd, kTickTimer, 10, nullptr);
}

static void WriteResults() {
    KillTimer(gMsgWnd, kTickTimer);
    EnterCriticalSection(&gLock);
    double now = NowMs();
    Advance(now);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s-pages.csv", gCfg.out, gCfg.label);
    FILE* csv = nullptr;
    fopen_s(&csv, path, "w");
    if (csv) {
        fprintf(
            csv,
            "burst,page,final,visible_from_ms,visible_ms,sharp_after_ms,rough_after_ms,rough_draw_ms,covered_pct\n");
    }
    std::vector<double> allCover;
    int allPast = 0, allShown = 0, allSharp = 0;
    for (int b = 0; b < gBurstNo; b++) {
        double t0 = gBurstFirstTap[b], t1 = gBurstLastTap[b];
        int taps = 0, cut = 0, maxAir = 0;
        double lastLand = t1;
        std::vector<double> durs;
        for (Curl& c : gCurls) {
            if (c.burst != b) {
                continue;
            }
            taps++;
            cut += c.cut ? 1 : 0;
            lastLand = std::max(lastLand, c.tLand);
            durs.push_back(c.tLand - c.tap);
            int air = 0;
            for (Curl& d : gCurls) {
                air += (d.tap <= c.tap && d.tLand > c.tap) ? 1 : 0;
            }
            maxAir = std::max(maxAir, air);
        }
        int past = 0, shown = 0, sharp = 0, roughInBurst = 0;
        std::vector<double> cover, wait, vis;
        int finalPage = 0;
        double finalSharp = 0, finalRough = kInf;
        for (int p = 1; p <= gPageCount; p++) {
            PageState& ps = gPages[p];
            if (ps.burst != b || ps.vs < 0) {
                continue;
            }
            if (ps.roughT >= t0 && ps.roughT <= lastLand) {
                roughInBurst++;
            }
            if (ps.final) {
                finalPage = finalPage ? finalPage : p;
                finalSharp = std::max(finalSharp, ps.sharpT - t1);
                finalRough = std::min(finalRough, ps.roughT);
            }
            double ve = std::min(ps.ve, lastLand + 1000);
            double pic = std::min(ps.sharpT, ps.roughT);
            double v = std::max(0.0, ve - ps.vs);
            double cov = v > 0 ? std::max(0.0, ve - std::max(ps.vs, pic)) / v : 0;
            if (csv) {
                fprintf(csv, "%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%.1f,%.0f\n", b, p, ps.final ? 1 : 0, ps.vs - t0, v,
                        ps.sharpT < kInf ? ps.sharpT - ps.vs : -1, ps.roughT < kInf ? ps.roughT - ps.vs : -1,
                        ps.roughMs, ps.final ? -1 : cov * 100);
            }
            if (ps.final) {
                continue;
            }
            past++;
            vis.push_back(v);
            cover.push_back(cov * 100);
            if (pic < ve) {
                shown++;
                wait.push_back(pic - ps.vs);
                sharp += ps.sharpT < ve && ps.sharpT <= ps.roughT ? 1 : 0;
            }
        }
        allPast += past;
        allShown += shown;
        allSharp += sharp;
        allCover.insert(allCover.end(), cover.begin(), cover.end());
        double span = lastLand - t0;
        Log("\nBurst %d: from page %d, %d taps in %.0f ms (gap %.0f ms), Curls %.0f-%.0f ms, at most %d in the air, %d "
            "landed "
            "at once\n",
            b + 1, gBurstStart[b], taps, t1 - t0, taps > 1 ? (t1 - t0) / (taps - 1) : 0, Pct(durs, 0), Pct(durs, 1),
            maxAir, cut);
        Log("  flipped-past pages: %d; with a picture while showing: %d (sharp %d, Rough %d); blank paper all along: "
            "%d\n",
            past, shown, sharp, shown - sharp, past - shown);
        Log(
            "  each page shows for %.0f ms (median), picture arrives %.0f ms (median) / %.0f ms (90%%) after it first "
            "shows; time covered by a picture: %.0f%% (median), %.0f%% (mean)\n",
            Pct(vis, 0.5), Pct(wait, 0.5), Pct(wait, 0.9), Pct(cover, 0.5), cover.empty() ? 0 : [&] {
                double s = 0;
                for (double x : cover) {
                    s += x;
                }
                return s / cover.size();
            }());
        Log("  Rough Pages finished during the burst: %d in %.0f ms = %.1f per second\n", roughInBurst, span,
            span > 0 ? roughInBurst * 1000.0 / span : 0);
        Log("  landed Spread (page %d): sharp %.0f ms after the last tap; Rough Page %s\n", finalPage, finalSharp,
            finalRough < kInf ? "arrived first" : "not needed / not in time");
    }
    double mean = 0;
    for (double x : allCover) {
        mean += x;
    }
    mean = allCover.empty() ? 0 : mean / allCover.size();
    Log("\nAll bursts: %d flipped-past pages, %d (%.0f%%) showed a picture (%d sharp), %d blank all along; time "
        "covered "
        "%.0f%% (mean)\n",
        allPast, allShown, allPast ? allShown * 100.0 / allPast : 0, allSharp, allPast - allShown, mean);
    Log("Rough Pages: %d drawn, %d thrown away unfinished or unneeded, %d failed; draw time median %.0f ms, 90%% %.0f "
        "ms, max %.0f ms\n",
        gRoughDone, gRoughAborted, gRoughFailed, Pct(gRenderMs, 0.5), Pct(gRenderMs, 0.9), Pct(gRenderMs, 1));
    Log("Rough Pages held at once: at most %d, %.1f MB (CPU memory)\n", gRoughHeldPeak,
        gRoughBytesPeak / (1024.0 * 1024.0));
    Log("Private memory at the end %.1f MB, peak %.1f MB\n", PrivateMB(), PeakPrivateMB());
    if (csv) {
        fclose(csv);
    }
    gQuit = true;
    LeaveCriticalSection(&gLock);
    SetEvent(gWake);
    if (gWorkerThread) {
        WaitForSingleObject(gWorkerThread, 10000);
        double a = PrivateMB();
        if (EngineMupdf* em = AsEngineMupdf(gCopy)) {
            fz_empty_store(em->Ctx());
        }
        double b = PrivateMB();
        gCopy->Release();
        gCopy = nullptr;
        double c = PrivateMB();
        Log("Releasing the copy: private memory %.1f MB, after emptying its MuPDF cache %.1f MB, after closing it %.1f "
            "MB\n",
            a, b, c);
    }
    if (gLog) {
        fclose(gLog);
        gLog = nullptr;
    }
    ExitProcess(0);
}

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SPIKE_INIT) {
        OnInit();
        return 0;
    }
    if (msg == WM_SPIKE_TAP) {
        OnTap();
        return 0;
    }
    if (msg == WM_TIMER && wp == kTickTimer) {
        Tick();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static struct SpikeInit {
    SpikeInit() {
        InitializeCriticalSection(&gLock);
        gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        gWorkerReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
} gSpikeInit;

static void EnsureMsgWnd() {
    if (gMsgWnd) {
        return;
    }
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SpikeRoughMsg";
    RegisterClassW(&wc);
    gMsgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

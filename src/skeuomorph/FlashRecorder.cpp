/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "base/Pixmap.h"

#include "gui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "PngOptimizer.h"

#include "skeuomorph/PageTurnLogic.h"
#include "skeuomorph/CurlD3D.h"
#include "skeuomorph/CurlRenderer.h"
#include "skeuomorph/FlashRecorder.h"

#if IS_DEBUG

// Test-only flash recorder (spec 5.7), ported from spike/no-flash. Off unless
//   SKEUOMORPH_FLASH=out=C:\dir      on; failing turns also get a filmstrip PNG
//                                    there, every frame a line in flash-frames.csv
//   SKEUOMORPH_FLASH=nowait,lag=50   test of the test: SumatraPDF repaints before
//                                    the Curl covers it, the layer 50 ms late
//
//   capture thread   Desktop Duplication: every frame the screen shows, the canvas
//                    cut out and shrunk (GPU mips) into a ring of ~5 s
//   UI thread        FlashTap (hook 3), FlashCurlStarted: one record per Curl; a
//                    timer ends it when the layer went + 300 ms, or at the next tap
//   analysis thread  per Curl: did anything flash? One log line:
//     skeuomorph-flash: #3 ok, Curl #3, 46 frames, 0 missed, cover on screen 11.8 ms, UI waited 10.9 ms
//     skeuomorph-flash: #4 FLASH (flicker 61% for 3 frames at 17 ms), Curl #4, ...
//
// A flash is any of:
//   flicker     soon after the tap, pixels leave a picture and come back within
//               6 frames: the new Spread (or background, a blank page) showed
//               before the Curl covered it
//   old back    the old Spread comes back after the Curl started covering it
//   new early   most of the new Spread shows in the first 40% of the Curl
//   neither     a frame shows mostly neither the old nor the new Spread
//   gap         after the Curl landed, a frame isn't the new Spread
// The last four need a clean start and end on screen: not for fast taps (another
// Curl in the air at the tap) nor Curls the next tap replaced.

constexpr int kRingFrames = 300; // 5 s at 60 Hz
constexpr int kMaxSmallDx = 420; // recorded frames are at most this wide
constexpr int kAcquireMs = 100;
constexpr double kSettleMs = 300; // after the layer went: SumatraPDF alone
constexpr double kFrameLagMs = 100;
constexpr double kGiveUpMs = 8000;
constexpr UINT kPollMs = 50;

// pixel colour distance (max of B, G, R): same <= kSame < kDiffers <= other
constexpr int kSame = 24;
constexpr int kDiffers = 48;
constexpr int kMaxBlipFrames = 6;
// a flicker starts this soon after the tap, before the Curl covers the screen
// (ADR 0005: at most ~35 ms at 4K). Later, a fast Curl's own shadow and
// shading pass over pixels within kMaxBlipFrames; the old Spread coming back
// and gaps at the end have their own checks
constexpr double kFlickerWindowMs = 150;
constexpr double kFlickerShare = 0.08; // of the canvas
constexpr double kMinMaskShare = 0.02; // old and new must differ this much to judge
constexpr double kStartedBelow = 0.98;
constexpr double kOldBackRise = 0.10;
constexpr double kNewPeak = 0.6;
constexpr double kNewDrop = 0.15;
constexpr double kEarlyPart = 0.4;
constexpr double kEarlyNew = 0.5;
constexpr double kNeitherShare = 0.5;
constexpr double kSettledNew = 0.985;
constexpr double kGapNew = 0.97;

struct FlashConfig {
    bool on = false;
    bool noWait = false;
    int lagMs = 0;
    Str outDir;
};

static FlashConfig ReadConfig() {
    FlashConfig c;
    WCHAR buf[1024];
    DWORD n = GetEnvironmentVariableW(L"SKEUOMORPH_FLASH", buf, dimof(buf));
    if (n == 0 || n >= dimof(buf)) {
        return c;
    }
    c.on = true;
    Str rest = str::Dup(ToUtf8Temp(WStr(buf)));
    while (rest.len > 0) {
        Str opt, after;
        if (!str::CutChar(rest, ',', &opt, &after)) {
            opt = rest;
            after = Str();
        }
        rest = after;
        if (str::StartsWith(opt, StrL("out="))) {
            c.outDir = str::Dup(Str(opt.s + 4, opt.len - 4));
        } else if (str::Eq(opt, StrL("nowait"))) {
            c.noWait = true;
        } else if (str::StartsWith(opt, StrL("lag="))) {
            c.lagMs = std::max(0, ParseInt(Str(opt.s + 4, opt.len - 4)));
        }
    }
    return c;
}

static const FlashConfig& Config() {
    static FlashConfig c = ReadConfig();
    return c;
}

//--- shared between the threads, under rec.mutex

struct RecFrame {
    double t = 0; // shown at, CurlNowMs() clock
    int missed = 0;
};

// one Curl to judge: frames shown from tapMs to endMs
struct TurnJob {
    int n = 0;
    int curlId = 0;
    bool fastTap = false;
    bool replaced = false;
    bool gaveUp = false;
    double tapMs = 0;
    double endMs = 0;
    CurlTimes times;
};

struct Recorder {
    Mutex mutex;
    HANDLE jobReady = nullptr;
    Rect crop; // the canvas, screen px
    Size thumb;
    int gen = 0;
    u8* ring = nullptr;
    RecFrame info[kRingFrames];
    int next = 0;
    int count = 0;
    Vec<TurnJob> jobs;
};

static Recorder* gRec = nullptr;

static int PxDiff(const u8* a, const u8* b) {
    int d0 = abs(a[0] - b[0]);
    int d1 = abs(a[1] - b[1]);
    int d2 = abs(a[2] - b[2]);
    return std::max(d0, std::max(d1, d2));
}

static bool SameRect(Rect a, Rect b) {
    return a.x == b.x && a.y == b.y && a.dx == b.dx && a.dy == b.dy;
}

// QPC ticks (Desktop Duplication's frame time) -> CurlNowMs()'s clock
static double QpcToCurlMs(LONGLONG q) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double nowMs = CurlNowMs();
    return nowMs - (double)(now.QuadPart - q) * 1000.0 / (double)freq.QuadPart;
}

//--- capture thread

static PFN_D3D11_CREATE_DEVICE LoadCreateDevice() {
    static PFN_D3D11_CREATE_DEVICE fn = [] {
        HMODULE dll = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dll ? (PFN_D3D11_CREATE_DEVICE)GetProcAddress(dll, "D3D11CreateDevice") : nullptr;
    }();
    return fn;
}

struct Capture {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D> crop; // mip 0: the canvas at full size
    ComPtr<ID3D11ShaderResourceView> cropSrv;
    ComPtr<ID3D11Texture2D> staging; // mip `level` of crop
    Rect screen;
    Rect onOutput; // the canvas on the output's desktop image
    int level = 0;
    Size thumb;
};

// the output showing pt; a factory from a throwaway device, so dxgi.lib isn't linked
static bool FindOutput(POINT pt, ComPtr<IDXGIAdapter1>& adapter, ComPtr<IDXGIOutput1>& out, RECT* desk) {
    PFN_D3D11_CREATE_DEVICE create = LoadCreateDevice();
    ComPtr<ID3D11Device> dev;
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, nullptr))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> first;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(dev.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&first)) ||
        FAILED(first->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) {
            return false;
        }
        for (UINT j = 0;; j++) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            if (!PtInRect(&d.DesktopCoordinates, pt)) {
                continue;
            }
            adapter = a;
            *desk = d.DesktopCoordinates;
            return SUCCEEDED(o.As(&out));
        }
    }
}

static bool SetupCapture(Capture& c, Rect screen, const char** why) {
    c = Capture();
    c.screen = screen;
    POINT pt = {screen.x + screen.dx / 2, screen.y + screen.dy / 2};
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> out;
    RECT desk{};
    if (!FindOutput(pt, adapter, out, &desk)) {
        *why = "no screen shows the canvas";
        return false;
    }
    PFN_D3D11_CREATE_DEVICE create = LoadCreateDevice();
    if (FAILED(create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &c.dev,
                      nullptr, &c.ctx))) {
        *why = "no Direct3D device";
        return false;
    }
    if (FAILED(out->DuplicateOutput(c.dev.Get(), &c.dup))) {
        *why = "Desktop Duplication refused";
        return false;
    }

    Rect deskRect(desk);
    Rect vis = screen.Intersect(deskRect);
    if (vis.IsEmpty()) {
        *why = "canvas off screen";
        return false;
    }
    c.onOutput = {vis.x - desk.left, vis.y - desk.top, vis.dx, vis.dy};
    while ((vis.dx >> c.level) > kMaxSmallDx) {
        c.level++;
    }
    c.thumb = {std::max(vis.dx >> c.level, 1), std::max(vis.dy >> c.level, 1)};

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)vis.dx;
    td.Height = (UINT)vis.dy;
    td.MipLevels = (UINT)c.level + 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(c.dev->CreateTexture2D(&td, nullptr, &c.crop)) ||
        FAILED(c.dev->CreateShaderResourceView(c.crop.Get(), nullptr, &c.cropSrv))) {
        *why = "no crop texture";
        return false;
    }
    td.Width = (UINT)c.thumb.dx;
    td.Height = (UINT)c.thumb.dy;
    td.MipLevels = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.MiscFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(c.dev->CreateTexture2D(&td, nullptr, &c.staging))) {
        *why = "no staging texture";
        return false;
    }
    return true;
}

// a new crop: the ring starts over at the new size
static void ResetRing(Recorder* rec, Size thumb) {
    rec->mutex.Lock();
    free(rec->ring);
    rec->ring = (u8*)malloc((size_t)kRingFrames * thumb.dx * thumb.dy * 4);
    rec->thumb = rec->ring ? thumb : Size();
    rec->next = 0;
    rec->count = 0;
    rec->gen++;
    rec->mutex.Unlock();
}

static void PushFrame(Recorder* rec, const u8* px, double t, int missed) {
    rec->mutex.Lock();
    if (rec->ring) {
        size_t frameBytes = (size_t)rec->thumb.dx * rec->thumb.dy * 4;
        memcpy(rec->ring + (size_t)rec->next * frameBytes, px, frameBytes);
        rec->info[rec->next] = {t, missed};
        rec->next = (rec->next + 1) % kRingFrames;
        rec->count = std::min(rec->count + 1, kRingFrames);
    }
    rec->mutex.Unlock();
}

// one shown frame -> the ring; false: the duplication is lost, set it up again
static bool GrabFrame(Recorder* rec, Capture& c, Vec<u8>& px) {
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = c.dup->AcquireNextFrame(kAcquireMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return true;
    }
    if (FAILED(hr)) {
        return false;
    }
    // only the mouse moved
    if (fi.LastPresentTime.QuadPart == 0) {
        c.dup->ReleaseFrame();
        return true;
    }

    ComPtr<ID3D11Texture2D> tex;
    D3D11_TEXTURE2D_DESC td{};
    if (FAILED(res.As(&tex))) {
        c.dup->ReleaseFrame();
        return true;
    }
    tex->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        // HDR desktop: not supported
        c.dup->ReleaseFrame();
        return false;
    }
    D3D11_BOX box = {(UINT)c.onOutput.x, (UINT)c.onOutput.y, 0, (UINT)c.onOutput.Right(), (UINT)c.onOutput.Bottom(), 1};
    c.ctx->CopySubresourceRegion(c.crop.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
    c.ctx->GenerateMips(c.cropSrv.Get());
    c.ctx->CopySubresourceRegion(c.staging.Get(), 0, 0, 0, 0, c.crop.Get(), (UINT)c.level, nullptr);

    D3D11_MAPPED_SUBRESOURCE m{};
    bool mapped = SUCCEEDED(c.ctx->Map(c.staging.Get(), 0, D3D11_MAP_READ, 0, &m));
    if (mapped) {
        int rowBytes = c.thumb.dx * 4;
        for (int y = 0; y < c.thumb.dy; y++) {
            memcpy(px.els + (size_t)y * rowBytes, (const u8*)m.pData + (size_t)y * m.RowPitch, rowBytes);
        }
        c.ctx->Unmap(c.staging.Get(), 0);
    }
    c.dup->ReleaseFrame();
    if (mapped) {
        int missed = fi.AccumulatedFrames > 1 ? (int)fi.AccumulatedFrames - 1 : 0;
        PushFrame(rec, px.els, QpcToCurlMs(fi.LastPresentTime.QuadPart), missed);
    }
    return true;
}

static void CaptureThread(Recorder* rec) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Capture c;
    Vec<u8> px;
    bool ready = false;
    const char* lastWhy = nullptr;
    while (true) {
        rec->mutex.Lock();
        Rect want = rec->crop;
        rec->mutex.Unlock();
        if (want.IsEmpty()) {
            Sleep(kAcquireMs);
            continue;
        }

        if (!ready || !SameRect(want, c.screen)) {
            const char* why = nullptr;
            ready = SetupCapture(c, want, &why);
            if (!ready) {
                if (why != lastWhy) {
                    logf("skeuomorph-flash: can't record (%s)\n", Str(why));
                    lastWhy = why;
                }
                c.screen = want;
                Sleep(kAcquireMs * 5);
                continue;
            }
            lastWhy = nullptr;
            VecResize(px, c.thumb.dx * c.thumb.dy * 4);
            ResetRing(rec, c.thumb);
            logf("skeuomorph-flash: recording canvas %dx%d at %d,%d (frames %dx%d)\n", want.dx, want.dy, want.x, want.y,
                 c.thumb.dx, c.thumb.dy);
        }
        if (!GrabFrame(rec, c, px)) {
            // mode change, secure desktop, HDR: set up again
            ready = false;
            c.dup.Reset();
            Sleep(kAcquireMs);
        }
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}

//--- analysis thread

// frames of one turn, copied out of the ring
struct TurnFrames {
    Size thumb;
    int n = 0;
    bool haveBefore = false; // frame 0: the last one shown before the tap
    RecFrame* info = nullptr;
    u8* px = nullptr;
};

static const u8* FramePx(const TurnFrames& f, int k) {
    return f.px + (size_t)k * f.thumb.dx * f.thumb.dy * 4;
}

static void FreeFrames(TurnFrames& f) {
    free(f.info);
    free(f.px);
    f = TurnFrames();
}

static bool CopyFrames(Recorder* rec, const TurnJob& job, TurnFrames& out) {
    rec->mutex.Lock();
    int first = -1;
    int last = -1;
    bool before = false;
    for (int i = 0; i < rec->count; i++) {
        int idx = (rec->next - rec->count + i + kRingFrames) % kRingFrames;
        double t = rec->info[idx].t;
        if (t < job.tapMs) {
            first = i;
            before = true;
        } else if (t <= job.endMs) {
            if (first < 0) {
                first = i;
            }
            last = i;
        }
    }
    if (first < 0 || last < 0 || !rec->ring) {
        rec->mutex.Unlock();
        return false;
    }

    out.thumb = rec->thumb;
    out.n = last - first + 1;
    out.haveBefore = before;
    size_t frameBytes = (size_t)out.thumb.dx * out.thumb.dy * 4;
    out.info = (RecFrame*)malloc(sizeof(RecFrame) * out.n);
    out.px = (u8*)malloc(frameBytes * out.n);
    if (!out.info || !out.px) {
        rec->mutex.Unlock();
        FreeFrames(out);
        return false;
    }
    for (int k = 0; k < out.n; k++) {
        int idx = (rec->next - rec->count + first + k + kRingFrames) % kRingFrames;
        out.info[k] = rec->info[idx];
        memcpy(out.px + frameBytes * k, rec->ring + frameBytes * idx, frameBytes);
    }
    rec->mutex.Unlock();
    return true;
}

// per frame against the old and the new Spread, share of the pixels that differ between them
struct FrameScore {
    double old = -1;
    double nu = -1;
    double neither = -1;
    int flickerPx = 0; // pixels that leave this frame's picture and come back
};

struct Verdict {
    bool judgedRefs = false;
    int flickerPx = 0;
    int flickerAt = -1;
    int flickerLen = 0;
    bool oldBack = false;
    bool newCovered = false;
    bool newEarly = false;
    bool neither = false;
    bool gap = false;
    int missed = 0;
};

// pixels that leave frame k's colour at k+1 and come back within kMaxBlipFrames,
// for k + 1 shown within kFlickerWindowMs of the tap
static void ScoreFlicker(const TurnFrames& f, double tapMs, FrameScore* sc, int* maxLen) {
    int nPx = f.thumb.dx * f.thumb.dy;
    for (int k = 0; k + 2 < f.n; k++) {
        if (f.info[k + 1].t - tapMs > kFlickerWindowMs) {
            break;
        }
        const u8* a = FramePx(f, k);
        const u8* b = FramePx(f, k + 1);
        for (int p = 0; p < nPx; p++) {
            if (PxDiff(b + p * 4, a + p * 4) <= kDiffers) {
                continue;
            }
            for (int e = k + 2; e < f.n && e <= k + 1 + kMaxBlipFrames; e++) {
                int d = PxDiff(FramePx(f, e) + p * 4, a + p * 4);
                if (d <= kSame) {
                    sc[k].flickerPx++;
                    *maxLen = std::max(*maxLen, e - k - 1);
                    break;
                }
                if (d <= kDiffers) {
                    break;
                }
            }
        }
    }
}

// old = the frame before the tap, nu = the last one (SumatraPDF alone), or null
static bool ScoreRefs(const TurnFrames& f, const u8* old, const u8* nu, FrameScore* sc) {
    int nPx = f.thumb.dx * f.thumb.dy;
    Vec<u8> mask;
    VecResize(mask, nPx);
    int informative = 0;
    for (int p = 0; p < nPx; p++) {
        bool differs = !nu || PxDiff(old + p * 4, nu + p * 4) > kDiffers;
        mask[p] = differs ? 1 : 0;
        informative += mask[p];
    }
    if (nu && informative < nPx * kMinMaskShare) {
        return false;
    }
    for (int k = 0; k < f.n; k++) {
        const u8* px = FramePx(f, k);
        int mo = 0;
        int mn = 0;
        for (int p = 0; p < nPx; p++) {
            if (!mask[p]) {
                continue;
            }
            mo += PxDiff(px + p * 4, old + p * 4) <= kSame;
            mn += nu && PxDiff(px + p * 4, nu + p * 4) <= kSame;
        }
        sc[k].old = (double)mo / informative;
        if (nu) {
            sc[k].nu = (double)mn / informative;
            sc[k].neither = 1 - sc[k].old - sc[k].nu;
        }
    }
    return true;
}

static Verdict Judge(const TurnJob& job, const TurnFrames& f, FrameScore* sc) {
    Verdict v;
    for (int k = 0; k < f.n; k++) {
        v.missed += f.info[k].missed;
    }
    ScoreFlicker(f, job.tapMs, sc, &v.flickerLen);
    for (int k = 0; k < f.n; k++) {
        if (sc[k].flickerPx > v.flickerPx) {
            v.flickerPx = sc[k].flickerPx;
            v.flickerAt = k;
        }
    }

    // the old and new Spread on screen only for a clean start (and end)
    if (job.fastTap || !f.haveBefore) {
        return v;
    }
    bool settledTurn = !job.replaced && !job.gaveUp && job.times.gone > 0;
    const u8* old = FramePx(f, 0);
    const u8* nu = settledTurn ? FramePx(f, f.n - 1) : nullptr;
    if (!ScoreRefs(f, old, nu, sc)) {
        return v;
    }
    v.judgedRefs = true;

    const CurlTimes& tm = job.times;
    double curlMs = tm.landed > 0 ? tm.landed - tm.start : 0;
    bool started = false;
    bool settled = false;
    double minOld = 1;
    double peakNew = 0;
    for (int k = 1; k < f.n; k++) {
        const FrameScore& s = sc[k];
        double t = f.info[k].t;
        if (s.old < kStartedBelow) {
            started = true;
        }
        if (started && !settled) {
            v.oldBack |= s.old > minOld + kOldBackRise;
            minOld = std::min(minOld, s.old);
        }
        if (!nu) {
            continue;
        }
        if (s.nu >= kNewPeak) {
            peakNew = std::max(peakNew, s.nu);
        }
        if (!settled && peakNew > 0 && s.nu < peakNew - kNewDrop) {
            v.newCovered = true;
        }
        if (curlMs > 0 && t - tm.start < kEarlyPart * curlMs && s.nu > kEarlyNew) {
            v.newEarly = true;
        }
        if (!settled && s.neither > kNeitherShare) {
            v.neither = true;
        }
        if (settled && t > tm.landed && s.nu < kGapNew) {
            v.gap = true;
        }
        if (s.nu >= kSettledNew) {
            settled = true;
        }
    }
    return v;
}

static bool Flashed(const Verdict& v, Size thumb) {
    bool flicker = v.flickerPx >= thumb.dx * thumb.dy * kFlickerShare;
    return flicker || v.oldBack || v.newCovered || v.newEarly || v.neither || v.gap;
}

static TempStr WhatFlashedTemp(const Verdict& v, const TurnFrames& f, double tapMs) {
    str::Builder b;
    auto add = [&b](Str s) {
        if (len(b) > 0) {
            b.Append(StrL("; "));
        }
        b.Append(s);
    };
    int nPx = f.thumb.dx * f.thumb.dy;
    if (v.flickerPx >= nPx * kFlickerShare) {
        double at = f.info[v.flickerAt + 1].t - tapMs;
        add(fmt("flicker %d%% for %d frames at %.0f ms", v.flickerPx * 100 / nPx, v.flickerLen, at));
    }
    if (v.oldBack) {
        add(StrL("old Spread back"));
    }
    if (v.newCovered) {
        add(StrL("new Spread shown, then covered"));
    }
    if (v.newEarly) {
        add(StrL("new Spread before the Curl"));
    }
    if (v.neither) {
        add(StrL("neither old nor new"));
    }
    if (v.gap) {
        add(StrL("gap after landing"));
    }
    return str::DupTemp(ToStr(b));
}

static TempStr NotesTemp(const TurnJob& job, const TurnFrames& f, const Verdict& v) {
    const CurlTimes& tm = job.times;
    double cover = tm.covered > 0 ? tm.covered - tm.start : -1;
    TempStr s = fmt("Curl #%d, %d frames, %d missed, cover on screen %.1f ms, UI waited %.1f ms", job.curlId, f.n,
                    v.missed, cover, tm.uiWaitMs);
    if (job.fastTap) {
        s = fmt("%s, fast tap", s);
    }
    if (job.replaced) {
        s = fmt("%s, replaced", s);
    }
    if (job.gaveUp) {
        s = fmt("%s, gave up waiting", s);
    }
    if (!v.judgedRefs) {
        s = fmt("%s, flicker only", s);
    }
    return s;
}

static void WriteCsv(const TurnJob& job, const TurnFrames& f, const FrameScore* sc) {
    static file::FileHandle h = file::kInvalidFileHandle;
    static bool tried = false;
    if (!tried) {
        tried = true;
        TempStr path = path::JoinTemp(Config().outDir, StrL("flash-frames.csv"));
        h = file::OpenReadWrite(path, true);
        if (h != file::kInvalidFileHandle && file::SeekEnd(h) == 0) {
            file::WriteAll(h, StrL("turn,frame,ms_after_tap,missed,old,new,neither,flicker_px\n"));
        }
    }
    if (h == file::kInvalidFileHandle) {
        return;
    }
    str::Builder b;
    for (int k = 0; k < f.n; k++) {
        const FrameScore& s = sc[k];
        b.Append(fmt("%d,%d,%.1f,%d,%.3f,%.3f,%.3f,%d\n", job.n, k, f.info[k].t - job.tapMs, f.info[k].missed, s.old,
                     s.nu, s.neither, s.flickerPx));
    }
    file::WriteAll(h, ToStr(b));
}

// every frame at 1/4 of the recorded size, 10 per row
static void WriteFilmstrip(const TurnJob& job, const TurnFrames& f) {
    constexpr int kCols = 10;
    constexpr int kShrink = 4;
    int tw = std::max(f.thumb.dx / kShrink, 1);
    int th = std::max(f.thumb.dy / kShrink, 1);
    int rows = (f.n + kCols - 1) / kCols;
    Pixmap pm;
    pm.width = tw * kCols;
    pm.height = th * rows;
    pm.stride = pm.width * 4;
    pm.data = (u8*)malloc((size_t)pm.stride * pm.height);
    if (!pm.data) {
        return;
    }
    memset(pm.data, 40, (size_t)pm.stride * pm.height);
    for (int k = 0; k < f.n; k++) {
        int ox = (k % kCols) * tw;
        int oy = (k / kCols) * th;
        const u8* src = FramePx(f, k);
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                const u8* s = src + ((size_t)(y * kShrink) * f.thumb.dx + x * kShrink) * 4;
                u8* d = pm.data + (size_t)(oy + y) * pm.stride + (ox + x) * 4;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            }
        }
    }
    Str png = EncodePngFromPixmap(&pm);
    free(pm.data);
    if (len(png) == 0) {
        return;
    }
    TempStr path = path::JoinTemp(Config().outDir, fmt("flash-turn%02d.png", job.n));
    file::WriteFile(path, png);
    str::Free(png);
}

static void AnalyzeTurn(Recorder* rec, const TurnJob& job) {
    TurnFrames f;
    if (!CopyFrames(rec, job, f) || f.n < 2) {
        logf("skeuomorph-flash: #%d not checked (no frames recorded), Curl #%d\n", job.n, job.curlId);
        FreeFrames(f);
        return;
    }
    Vec<FrameScore> sc;
    VecResize(sc, f.n);
    for (FrameScore& s : sc) {
        s = FrameScore();
    }
    Verdict v = Judge(job, f, sc.els);
    bool flashed = Flashed(v, f.thumb);
    TempStr notes = NotesTemp(job, f, v);
    if (flashed) {
        logf("skeuomorph-flash: #%d FLASH (%s), %s\n", job.n, WhatFlashedTemp(v, f, job.tapMs), notes);
    } else {
        logf("skeuomorph-flash: #%d ok, %s\n", job.n, notes);
    }
    if (Config().outDir) {
        WriteCsv(job, f, sc.els);
        if (flashed) {
            WriteFilmstrip(job, f);
        }
    }
    FreeFrames(f);
}

static void AnalysisThread(Recorder* rec) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (true) {
        WaitForSingleObject(rec->jobReady, INFINITE);
        while (true) {
            rec->mutex.Lock();
            bool have = len(rec->jobs) > 0;
            TurnJob job;
            if (have) {
                job = VecPopAt(rec->jobs, 0);
            }
            rec->mutex.Unlock();
            if (!have) {
                break;
            }
            AnalyzeTurn(rec, job);
        }
    }
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}

//--- UI thread

struct Pending {
    TurnJob job;
    double cutMs = 0; // the next action came then
};

static MainWindow* gWin = nullptr;
static Vec<Pending> gPending;
static double gTapMs = 0;
static bool gTapInAir = false;
static int gTurns = 0;
static int gLastCurlId = 0;
static UINT_PTR gTimer = 0;

static void UpdateCrop(MainWindow* win) {
    RECT rc{};
    GetClientRect(win->hwndCanvas, &rc);
    POINT tl = {0, 0};
    ClientToScreen(win->hwndCanvas, &tl);
    Rect crop(tl.x, tl.y, rc.right - rc.left, rc.bottom - rc.top);
    gRec->mutex.Lock();
    gRec->crop = crop;
    gRec->mutex.Unlock();
}

// the turn's frames are complete once SumatraPDF alone shows the new Spread
// (layer gone + kSettleMs), or the next action came
static double EndOf(Pending& p, double now) {
    double end = p.job.times.gone > 0 ? p.job.times.gone + kSettleMs : 0;
    if (p.cutMs > 0 && (end == 0 || p.cutMs < end)) {
        end = p.cutMs;
        p.job.replaced = p.job.times.gone == 0 || p.cutMs < p.job.times.gone;
    }
    if (end == 0 && now - p.job.tapMs > kGiveUpMs) {
        end = now;
        p.job.gaveUp = true;
    }
    return end;
}

static void CALLBACK PollTurns(HWND, UINT, UINT_PTR, DWORD) {
    if (!IsMainWindowValid(gWin)) {
        VecClear(gPending);
    }
    CurlTimes t;
    bool have = len(gPending) > 0 && CurlLastTimes(gWin, &t);
    double now = CurlNowMs();
    for (int i = 0; i < len(gPending);) {
        Pending& p = gPending[i];
        if (have && t.id == p.job.curlId) {
            p.job.times = t;
        }
        double end = EndOf(p, now);
        if (end <= 0 || now < end + kFrameLagMs) {
            i++;
            continue;
        }
        p.job.endMs = end;
        gRec->mutex.Lock();
        VecAppend(gRec->jobs, p.job);
        gRec->mutex.Unlock();
        SetEvent(gRec->jobReady);
        VecRemoveAt(gPending, i);
    }
    if (len(gPending) == 0) {
        KillTimer(nullptr, gTimer);
        gTimer = 0;
    }
}

static bool IsOn() {
    return Config().on;
}

// at every Paged View paint with the switch on: starts recording the first window seen
void FlashWatch(MainWindow* win) {
    if (!IsOn() || !win || !win->hwndCanvas) {
        return;
    }
    if (!gRec) {
        gRec = new Recorder();
        gRec->jobReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        gWin = win;
        if (Config().outDir) {
            dir::CreateAll(Config().outDir);
        }
        UpdateCrop(win);
        StartThread(MkFunc0<Recorder>(CaptureThread, gRec), StrL("FlashCapture"));
        StartThread(MkFunc0<Recorder>(AnalysisThread, gRec), StrL("FlashAnalysis"));
        return;
    }
    if (win == gWin) {
        UpdateCrop(win);
    }
}

// hook 3, every action: a new tap ends the turn before it
void FlashTap(MainWindow* win) {
    if (!IsOn() || !gRec || win != gWin) {
        return;
    }
    UpdateCrop(win);
    gTapMs = CurlNowMs();
    gTapInAir = CurlInAir(win);
    for (Pending& p : gPending) {
        if (p.cutMs == 0) {
            p.cutMs = gTapMs;
        }
    }
}

// after CurlStart: with the cover wait the Curl's times are out; without
// (nowait) its id comes next
void FlashCurlStarted(MainWindow* win) {
    if (!IsOn() || !gRec || win != gWin) {
        return;
    }
    CurlTimes t;
    if (!CurlLastTimes(win, &t)) {
        return;
    }
    int id = t.id;
    if (id <= gLastCurlId) {
        if (!CurlInAir(win)) {
            return;
        }
        id = gLastCurlId + 1;
        t = CurlTimes();
    }
    gLastCurlId = id;

    Pending p;
    p.job.n = ++gTurns;
    p.job.curlId = id;
    p.job.fastTap = gTapInAir;
    p.job.tapMs = gTapMs;
    p.job.times = t;
    VecAppend(gPending, p);
    if (!gTimer) {
        gTimer = SetTimer(nullptr, 0, kPollMs, PollTurns);
    }
}

bool FlashSkipsCoverWait() {
    return IsOn() && Config().noWait;
}

int FlashLayerLagMs() {
    return IsOn() ? Config().lagMs : 0;
}

#else

void FlashWatch(MainWindow*) {}
void FlashTap(MainWindow*) {}
void FlashCurlStarted(MainWindow*) {}

bool FlashSkipsCoverWait() {
    return false;
}

int FlashLayerLagMs() {
    return 0;
}

#endif

/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Pixmap.h"
#include "base/Timer.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include "gui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineMupdf.h"
#include "DisplayModel.h"
#include "RenderCache.h"
#include "PdfDarkMode.h"
#include "SumatraPDF.h"

#include "skeuomorph/RoughPages.h"

// spec 5.6: half the page's on-screen size, long side at most ~600 px
constexpr float kRoughScale = 0.5f;
constexpr float kRoughMaxPx = 600;
// finished pictures nobody wishes for any more: the newest kept
constexpr int kShelfMax = 16;

// what the pictures look like: a change throws the finished ones away
struct Look {
    DisplayModel* dm = nullptr; // only compared, never followed
    int rotation = 0;
    float zoom = 0;
    bool grayscale = false;
    int brightness = 0;
    int contrast = 0;
    Color textCol = 0;
    Color bgCol = 0;
    Color linkCol = 0;
    bool legacyRecolor = false;
    DarkModeProfile dark;
};

static bool SameLook(const Look& a, const Look& b) {
    return a.dm == b.dm && a.rotation == b.rotation && a.zoom == b.zoom && a.grayscale == b.grayscale &&
           a.brightness == b.brightness && a.contrast == b.contrast && a.textCol == b.textCol && a.bgCol == b.bgCol &&
           a.linkCol == b.linkCol && a.legacyRecolor == b.legacyRecolor && a.dark.hash == b.dark.hash;
}

struct Finished {
    int page = 0;
    Pixmap* bmp = nullptr;
};

struct RoughPages {
    LONG refs = 1; // the owner's; the worker holds one more while it runs
    Mutex mutex;
    HANDLE wake = nullptr;
    HWND hwnd = nullptr; // repainted when a page it shows is ready
    bool quit = false;
    bool started = false;

    // UI thread -> worker
    Look look;
    int gen = 1;               // bumped on every new look: finished ones go
    int copyId = 0;            // bumped when the copy must change
    EngineBase* src = nullptr; // AddRef'd: the copy is cloned from it

    // drawing thread -> worker
    RoughWant wants[kMaxRoughWishes];
    int nWants = 0;

    // UI thread -> worker: the Spread on screen until SumatraPDF draws it sharp
    RoughWant shown[2];
    int nShown = 0;

    // worker
    int drawing = 0;
    bool abortAsked = false;
    AbortCookie* cookie = nullptr; // set by the engine while it renders
    Vec<int> failed;

    // worker -> drawing and UI thread, oldest first
    Vec<Finished> finished;
};

static void Release(RoughPages* rp) {
    if (InterlockedDecrement(&rp->refs) > 0) {
        return;
    }
    for (Finished& f : rp->finished) {
        FreePixmap(f.bmp);
    }
    if (rp->src) {
        rp->src->Release();
    }
    CloseHandle(rp->wake);
    delete rp;
}

// under mutex
static void AbortDrawing(RoughPages* rp) {
    if (!rp->drawing || rp->abortAsked) {
        return;
    }
    rp->abortAsked = true;
    if (rp->cookie) {
        rp->cookie->Abort();
    }
}

static bool OnScreen(RoughPages* rp, int page) {
    for (int i = 0; i < rp->nShown; i++) {
        if (rp->shown[i].page == page) {
            return true;
        }
    }
    return false;
}

static bool Wished(RoughPages* rp, int page) {
    for (int i = 0; i < rp->nWants; i++) {
        if (rp->wants[i].page == page) {
            return true;
        }
    }
    return OnScreen(rp, page);
}

static Finished* FindFinished(RoughPages* rp, int page) {
    for (Finished& f : rp->finished) {
        if (f.page == page) {
            return &f;
        }
    }
    return nullptr;
}

// under mutex: the oldest ones nobody wishes for go, beyond kShelfMax
static void Trim(RoughPages* rp) {
    int extra = len(rp->finished) - kShelfMax;
    for (int i = 0; i < len(rp->finished) && extra > 0;) {
        if (Wished(rp, rp->finished[i].page)) {
            i++;
            continue;
        }
        FreePixmap(rp->finished[i].bmp);
        VecRemoveAt(rp->finished, i);
        extra--;
    }
}

static bool Wanted(RoughPages* rp, RoughWant w) {
    return !FindFinished(rp, w.page) && !VecContains(rp->failed, w.page);
}

// under mutex: the first wish not finished and not failed; the Curls' first,
// then the Spread on screen
static RoughWant NextWant(RoughPages* rp) {
    for (int i = 0; i < rp->nWants; i++) {
        if (Wanted(rp, rp->wants[i])) {
            return rp->wants[i];
        }
    }
    for (int i = 0; i < rp->nShown; i++) {
        if (Wanted(rp, rp->shown[i])) {
            return rp->shown[i];
        }
    }
    return RoughWant();
}

// the colours SumatraPDF's own render thread gives its pages (RenderCache.cpp)
static Pixmap* Grayscale(Pixmap* bmp) {
    int bpp = PixmapBytesPerPixel(bmp->format);
    for (int y = 0; y < bmp->height; y++) {
        u8* p = bmp->data + ((size_t)y * bmp->stride);
        for (int x = 0; x < bmp->width; x++, p += bpp) {
            u8 gray = (u8)((54 * (u32)p[2] + 183 * (u32)p[1] + 19 * (u32)p[0] + 128) >> 8);
            p[0] = gray;
            p[1] = gray;
            p[2] = gray;
        }
    }
    return bmp;
}

static Pixmap* Adjust(Pixmap* bmp, int brightness, int contrast) {
    if (brightness == 0 && contrast == 0) {
        return bmp;
    }
    float c = (float)contrast / 100.f;
    float factor = c >= 0 ? 1.f + 3.f * c : 1.f + c;
    float offset = (float)brightness * 127.5f / 100.f;
    u8 lut[256];
    for (int v = 0; v < 256; v++) {
        float r = ((float)v - 127.5f) * factor + 127.5f + offset;
        lut[v] = (u8)limitValue((int)(r + 0.5f), 0, 255);
    }
    int bpp = PixmapBytesPerPixel(bmp->format);
    for (int y = 0; y < bmp->height; y++) {
        u8* p = bmp->data + ((size_t)y * bmp->stride);
        for (int x = 0; x < bmp->width; x++, p += bpp) {
            p[0] = lut[p[0]];
            p[1] = lut[p[1]];
            p[2] = lut[p[2]];
        }
    }
    return bmp;
}

// a Rough Page, BGRA without alpha, or null
static Pixmap* Draw(RoughPages* rp, EngineBase* copy, RoughWant w, const Look& look) {
    RectF box = copy->Transform(copy->PageMediabox(w.page), w.page, 1.0f, look.rotation);
    float longSide = std::max(box.dx, box.dy);
    if (longSide <= 0 || w.zoom <= 0) {
        return nullptr;
    }
    float zoom = std::min(w.zoom * kRoughScale, kRoughMaxPx / longSide);

    RenderPageArgs args(w.page, zoom, look.rotation, nullptr, RenderTarget::View, &rp->cookie);
    if (look.dark.mode != PageColorMode::Normal) {
        args.darkProfile = &look.dark;
    }
    Pixmap* bmp = PixmapToBgra(copy->RenderPage(args));
    if (!bmp || !bmp->data) {
        FreePixmap(bmp);
        return nullptr;
    }
    if (look.grayscale) {
        bmp = Grayscale(bmp);
    }
    bmp = Adjust(bmp, look.brightness, look.contrast);
    bool recolor = args.darkProfile ? DarkModeProfileUsesLegacyPostProcess(args.darkProfile) : look.legacyRecolor;
    if (recolor) {
        const DarkModeProfile* p = args.darkProfile;
        RecolorPixmap(bmp, p ? p->foreground : look.textCol, p ? p->pageBackground : look.bgCol,
                      p ? p->linkColor : look.linkCol);
    }
    return bmp;
}

static void Worker(RoughPages* rp) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    EngineBase* copy = nullptr;
    int copyId = 0;
    for (;;) {
        rp->mutex.Lock();
        if (rp->quit) {
            rp->mutex.Unlock();
            break;
        }
        EngineBase* src = nullptr;
        bool newCopy = copyId != rp->copyId;
        if (newCopy) {
            copyId = rp->copyId;
            src = rp->src;
            rp->src = nullptr;
        }
        RoughWant w = newCopy ? RoughWant() : NextWant(rp);
        Look look = rp->look;
        int gen = rp->gen;
        rp->drawing = w.page;
        rp->abortAsked = false;
        rp->mutex.Unlock();

        // the copy follows the document on screen: closed when it leaves
        if (newCopy) {
            if (copy) {
                copy->Release();
                copy = nullptr;
            }
            if (src) {
                TimeStamp t = TimeGet();
                copy = src->Clone();
                src->Release();
                logf("skeuomorph-rough: background copy %s in %.0f ms\n", copy ? StrL("made") : StrL("failed"),
                     TimeSinceInMs(t));
            }
            continue;
        }
        if (!w.page || !copy) {
            WaitForSingleObject(rp->wake, INFINITE);
            continue;
        }

        TimeStamp t = TimeGet();
        Pixmap* bmp = Draw(rp, copy, w, look);
        // spec 5.6: the copy's image cache is emptied after each Rough Page
        if (EngineMupdf* em = AsEngineMupdf(copy)) {
            fz_empty_store(em->Ctx());
        }
        double ms = TimeSinceInMs(t);

        rp->mutex.Lock();
        AbortCookie* cookie = rp->cookie;
        rp->cookie = nullptr;
        bool keep = !rp->abortAsked && gen == rp->gen && copyId == rp->copyId && Wished(rp, w.page);
        rp->drawing = 0;
        bool failed = keep && !bmp;
        if (failed) {
            VecAppend(rp->failed, w.page);
        }
        bool repaint = false;
        if (keep && bmp) {
            VecAppend(rp->finished, Finished{w.page, bmp});
            bmp = nullptr;
            Trim(rp);
            repaint = OnScreen(rp, w.page);
        }
        HWND hwnd = rp->hwnd;
        rp->mutex.Unlock();
        // SumatraPDF's painting shows it until the sharp page is ready
        if (repaint) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        delete cookie;

        Str what = keep ? StrL("done") : StrL("dropped");
        if (failed) {
            what = StrL("failed");
        }
        FreePixmap(bmp);
        logf("skeuomorph-rough: page %d, %s, %.0f ms\n", w.page, what, ms);
    }
    if (copy) {
        copy->Release();
    }
    Release(rp);
}

RoughPages* NewRoughPages(HWND hwnd) {
    auto rp = new RoughPages();
    rp->hwnd = hwnd;
    rp->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return rp;
}

// the worker finishes its page, then frees everything
void ReleaseRoughPages(RoughPages* rp) {
    if (!rp) {
        return;
    }
    rp->mutex.Lock();
    rp->quit = true;
    AbortDrawing(rp);
    rp->mutex.Unlock();
    SetEvent(rp->wake);
    Release(rp);
}

// under mutex: a new look; the finished pictures and wishes go
static void NewLook(RoughPages* rp, const Look& look) {
    bool newDoc = look.dm != rp->look.dm;
    rp->look = look;
    rp->gen++;
    rp->nWants = 0;
    rp->nShown = 0;
    VecClear(rp->failed);
    for (Finished& f : rp->finished) {
        FreePixmap(f.bmp);
    }
    VecClear(rp->finished);
    AbortDrawing(rp);
    if (!newDoc) {
        return;
    }
    rp->copyId++;
    if (rp->src) {
        rp->src->Release();
        rp->src = nullptr;
    }
    if (look.dm) {
        rp->src = look.dm->GetEngine();
        rp->src->AddRef();
    }
}

static void StartWorker(RoughPages* rp) {
    if (rp->started) {
        return;
    }
    rp->started = true;
    InterlockedIncrement(&rp->refs);
    ThreadHandle h = StartThread(MkFunc0<RoughPages>(Worker, rp), StrL("RoughPages"));
    if (!h) {
        Release(rp);
        return;
    }
    SafeCloseThreadHandle(&h);
}

// at every paint of a Paged View with the switch on and the layer loaded.
// EPUB and other reflowed documents lay out pages anew in the copy: none there
void RoughShow(RoughPages* rp, DisplayModel* dm) {
    if (!rp || !dm || dm->GetEngine()->isReflowable) {
        RoughHide(rp);
        return;
    }
    Look look;
    look.dm = dm;
    look.rotation = dm->GetRotation();
    look.zoom = dm->GetZoomVirtual();
    look.grayscale = AtomicBoolGet(&gRenderCache->grayscalePageColors);
    look.brightness = dm->brightness;
    look.contrast = dm->contrast;
    look.textCol = gRenderCache->textColor;
    look.bgCol = gRenderCache->backgroundColor;
    look.linkCol = gRenderCache->linkColor;
    EngineBase* engine = dm->GetEngine();
    look.legacyRecolor = EngineUsesDocumentColorsFollowTheme(engine) && !EngineUsesReflowThemeCss(engine);
    BuildViewDarkModeProfile(engine, &look.dark);

    rp->mutex.Lock();
    bool changed = !SameLook(look, rp->look);
    if (changed) {
        NewLook(rp, look);
    }
    rp->mutex.Unlock();
    if (changed) {
        StartWorker(rp);
        SetEvent(rp->wake);
    }
}

// the document left the screen: the copy closes
void RoughHide(RoughPages* rp) {
    if (!rp) {
        return;
    }
    rp->mutex.Lock();
    bool shown = rp->look.dm != nullptr;
    if (shown) {
        NewLook(rp, Look());
    }
    rp->mutex.Unlock();
    if (shown) {
        SetEvent(rp->wake);
    }
}

// the drawing thread never waits for SumatraPDF's painting (RoughPaint):
// busy, it tries again next frame. 0: busy
int RoughGeneration(RoughPages* rp) {
    if (!rp->mutex.TryLock()) {
        return 0;
    }
    int gen = rp->gen;
    rp->mutex.Unlock();
    return gen;
}

// replaces the wishes: a page no longer wished is aborted
bool RoughWish(RoughPages* rp, const RoughWant* wants, int n, RoughWait wait) {
    n = std::min(n, kMaxRoughWishes);
    if (wait == RoughWait::Block) {
        rp->mutex.Lock();
    } else if (!rp->mutex.TryLock()) {
        return false;
    }
    bool same = n == rp->nWants;
    for (int i = 0; same && i < n; i++) {
        same = wants[i].page == rp->wants[i].page;
    }
    for (int i = 0; i < n; i++) {
        rp->wants[i] = wants[i];
    }
    rp->nWants = n;
    if (rp->drawing && !Wished(rp, rp->drawing)) {
        AbortDrawing(rp);
    }
    Trim(rp);
    rp->mutex.Unlock();
    if (!same) {
        SetEvent(rp->wake);
    }
    return true;
}

// a finished one, read in place: locked until RoughReturn. Null: none, not locked
Pixmap* RoughBorrow(RoughPages* rp, int page) {
    if (!rp->mutex.TryLock()) {
        return nullptr;
    }
    Finished* f = FindFinished(rp, page);
    if (!f) {
        rp->mutex.Unlock();
        return nullptr;
    }
    return f->bmp;
}

void RoughReturn(RoughPages* rp) {
    rp->mutex.Unlock();
}

// UI thread, at every paint: the pages of the Spread on screen SumatraPDF
// hasn't drawn sharp yet
void RoughKeep(RoughPages* rp, const RoughWant* shown, int n) {
    n = std::min(n, (int)dimof(rp->shown));
    rp->mutex.Lock();
    bool same = n == rp->nShown;
    for (int i = 0; i < n; i++) {
        same = same && shown[i].page == rp->shown[i].page;
        rp->shown[i] = shown[i];
    }
    rp->nShown = n;
    rp->mutex.Unlock();
    if (!same) {
        SetEvent(rp->wake);
    }
}

// UI thread: page's Rough Page stretched over pageRect, inside clip
bool RoughPaint(RoughPages* rp, DisplayModel* dm, HDC hdc, int page, Rect pageRect, Rect clip) {
    bool ok = false;
    rp->mutex.Lock();
    Finished* f = rp->look.dm == dm ? FindFinished(rp, page) : nullptr;
    if (f) {
        int saved = SaveDC(hdc);
        IntersectClipRect(hdc, clip.x, clip.y, clip.Right(), clip.Bottom());
        ok = BlitPixmap(f->bmp, hdc, pageRect);
        RestoreDC(hdc, saved);
    }
    rp->mutex.Unlock();
    return ok;
}

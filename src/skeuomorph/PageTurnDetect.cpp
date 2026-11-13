/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Timer.h"
#include "base/UITask.h"
#include "gui/UIModels.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "Theme.h"
#include "MainWindow.h"

#include "skeuomorph/PageTurnLogic.h"
#include "skeuomorph/PageTurnHooks.h"
#include "skeuomorph/PageEdges.h"
#include "skeuomorph/CurlRenderer.h"
#include "skeuomorph/FlashRecorder.h"
#include "skeuomorph/PageTurnDetect.h"

using namespace pageturn;

// One action (key, wheel notch, command) = one Page Turn:
//
//   message --> GoToPage --> hook 3: first call of the action:
//           |                        gate, old Spread, Post(FinishTurn);
//           |                        gate passed: copy the old Spread
//           +-> GoToPage --> hook 3: ignored, same action
//   FinishTurn (posted, runs after the message): final Spread -> DecideTurn;
//           a Curl: start it, wait until it covers the screen
//   WM_PAINT --> hook 6: fingerprint of what the reader now sees;
//           flushed: the Curl layer may go see-through

struct WindowTurns {
    MainWindow* win = nullptr;
    Fingerprint atPaint;
    Spread spreadAtPaint;

    // an action is under way: its first hook call posted FinishTurn
    bool pending = false;
    // only compared, never followed: it may be freed before FinishTurn
    DisplayModel* pendingDm = nullptr;
    Spread from;
    Skip gate = Skip::None;

    TurnClock clock{0};
};

static Vec<WindowTurns*> gWindowTurns;

enum class Lookup {
    Find,
    Create
};

// also drops the state of closed windows
static WindowTurns* FindTurns(MainWindow* win, Lookup lookup) {
    WindowTurns* found = nullptr;
    for (int i = len(gWindowTurns) - 1; i >= 0; i--) {
        WindowTurns* wt = gWindowTurns[i];
        if (!IsMainWindowValid(wt->win)) {
            VecRemoveAt(gWindowTurns, i);
            delete wt;
            continue;
        }
        if (wt->win == win) {
            found = wt;
        }
    }
    if (found || lookup == Lookup::Find || !IsMainWindowValid(win)) {
        return found;
    }

    found = new WindowTurns();
    found->win = win;
    VecAppend(gWindowTurns, found);
    return found;
}

// monotonic ms for TurnClock
static i64 NowMs() {
    static TimeStamp start = TimeGet();
    return (i64)TimeSinceInMs(start);
}

static Fingerprint FingerprintOf(DisplayModel* dm) {
    Fingerprint f;
    if (!dm) {
        return f;
    }
    f.dm = (uintptr_t)dm;
    f.displayMode = (int)dm->GetDisplayMode();
    f.facingStepOne = dm->facingStepOne;
    f.inPresentation = dm->InPresentation();
    f.r2l = dm->GetDisplayR2L();
    f.layoutGeneration = dm->layoutGeneration;
    f.pageCount = dm->PageCount();
    return f;
}

// the row tables also know wide pages shown alone; SpreadOf doesn't
static Spread SpreadNow(DisplayModel* dm) {
    int p = dm->CurrentPageNo();
    return {dm->FirstPageInRow(p), dm->LastPageInRow(p)};
}

// a wide page alone across both columns ("landscape as spread": comics,
// image folders)
static bool IsWideAlone(DisplayModel* dm, int pageNo) {
    if (pageNo < 1 || !dm->ShouldTreatLandscapeAsSpread()) {
        return false;
    }
    if (dm->FirstPageInRow(pageNo) != dm->LastPageInRow(pageNo)) {
        return false;
    }
    // FirstPageInRow filled the flags
    return pageNo <= len(dm->spreadFlags) && dm->spreadFlags[pageNo - 1] != 0;
}

// the Spread's pages as on screen, canvas px; a lone page in Facing or Book
// View goes to its side of the spine
static CurlSpread SpreadOnScreen(DisplayModel* dm) {
    CurlSpread s;
    if (!dm || !dm->pagesInfo) {
        return s;
    }
    Rect shown[2];
    int shownNo = 0;
    int n = 0;
    for (int p = 1; p <= dm->PageCount() && n < 2; p++) {
        PageInfo* pi = dm->GetPageInfo(p);
        if (!pi || !pi->isShown || pi->visibleRatio <= 0 || pi->pageOnScreen.IsEmpty()) {
            continue;
        }
        shownNo = p;
        shown[n++] = pi->pageOnScreen;
    }
    if (n == 0) {
        return s;
    }
    s.zoom = dm->GetZoomReal(shownNo);
    EdgesOnScreen(dm, &s.edgesLeft, &s.edgesRight);
    if (IsSingle(dm->GetDisplayMode())) {
        s.left = shown[0];
        return s;
    }
    if (n == 2) {
        bool inOrder = shown[0].x <= shown[1].x;
        s.left = inOrder ? shown[0] : shown[1];
        s.right = inOrder ? shown[1] : shown[0];
        return s;
    }
    // a picture printed across the spine: Curls fold it there, half per side
    Rect r = shown[0];
    if (IsWideAlone(dm, shownNo)) {
        int half = r.dx / 2;
        s.left = Rect(r.x, r.y, half, r.dy);
        s.right = Rect(r.x + half, r.y, r.dx - half, r.dy);
        return s;
    }
    bool rightOfSpine = r.x + r.dx / 2 >= dm->GetViewPort().dx / 2;
    (rightOfSpine ? s.right : s.left) = r;
    return s;
}

static void StartCurl(MainWindow* win, DisplayModel* dm, const Turn& t, int lenMs) {
    CurlTurn c;
    c.shape = IsSingle(dm->GetDisplayMode()) ? CurlShape::Single : CurlShape::Spread;
    c.sweep = t.sweep;
    c.forward = t.forward;
    c.durationMs = lenMs;
    c.to = SpreadOnScreen(dm);
    c.edgesPx = EdgesRoomOnScreen(dm);
    ThemePageRenderColors(c.paper);
    CurlStart(win, c);
}

static bool IsHidden(MainWindow* win) {
    HWND hwnd = win->hwndFrame;
    return IsIconic(hwnd) || !IsWindowVisible(hwnd);
}

#if IS_DEBUG
// one line per Page Turn, for tests/skeuomorph-*.ts:
//   skeuomorph: Curl forward, 1 page, sweep left, 700 ms, 1 in air, 0 landed [1-1 -> 2-2]
//   skeuomorph: Bundle 18 px backward, 39 pages, sweep right, 910 ms, 2 in air, 0 landed [40-40 -> 1-1]
//   skeuomorph: nothing (display mode switch) [1-1 -> 1-2]
// "in air" counts this turn; "landed" ones this turn made land early or that
// finished since the last turn
static void LogTurn(const Turn& t, Spread from, Spread to, int lenMs, int inAir, int landed) {
    TempStr spreads = fmt("[%d-%d -> %d-%d]", from.first, from.last, to.first, to.last);
    if (t.kind == TurnKind::Nothing) {
        logf("skeuomorph: nothing (%s) %s\n", SkipName(t.why), spreads);
        return;
    }

    TempStr what = StrL("Curl");
    if (t.kind == TurnKind::Bundle) {
        what = fmt("Bundle %d px", WholePx(BundlePx(t.pages)));
    }
    Str dir = t.forward ? StrL("forward") : StrL("backward");
    Str sweep = t.sweep == Sweep::ToLeft ? StrL("left") : StrL("right");
    Str pages = t.pages == 1 ? StrL("page") : StrL("pages");
    logf("skeuomorph: %s %s, %d %s, sweep %s, %d ms, %d in air, %d landed %s\n", what, dir, t.pages, pages, sweep,
         lenMs, inAir, landed, spreads);
}
#else
static void LogTurn(const Turn&, Spread, Spread, int, int, int) {}
#endif

// everything in the air lands now: the layer goes and the clock forgets it,
// so the next Curl is a whole one, not a fast tap
static void LandEverything(WindowTurns* wt, MainWindow* win, const char* why) {
    if (wt) {
        Landed landed;
        wt->clock.LandAll(&landed);
    }
    CurlLandAll(win, why);
}

// runs after the action's message: everything GoToPage did is in place
static void FinishTurn(MainWindow* win) {
    WindowTurns* wt = FindTurns(win, Lookup::Find);
    if (!wt || !wt->pending) {
        return;
    }
    wt->pending = false;

    // the document closed or the tab changed: nothing turned in this window
    DisplayModel* dm = win->AsFixed();
    if (!dm || dm != wt->pendingDm) {
        LandEverything(wt, win, "document changed");
        return;
    }

    // zoom, resize, restore: the Spread stayed, not a page change. A Curl in
    // the air lands if the view changed under it; a mere scroll keeps it
    Spread to = SpreadNow(dm);
    if (to.first == wt->from.first && to.last == wt->from.last) {
        if (wt->gate != Skip::None) {
            LandEverything(wt, win, "view changed");
        }
        return;
    }

    Reading reading = dm->GetDisplayR2L() ? Reading::RightToLeft : Reading::LeftToRight;
    Turn t = DecideTurn(wt->gate, wt->from, to, reading);
    if (t.kind == TurnKind::Nothing) {
        LogTurn(t, wt->from, to, 0, wt->clock.InAir(), 0);
        LandEverything(wt, win, SkipName(t.why).s);
        return;
    }

    i64 now = NowMs();
    Landed landed;
    wt->clock.SetDuration(PageTurnDurationMs());
    wt->clock.Start(now, t.kind, &landed);
    int lenMs = wt->clock.LengthMs(t.kind, now);
    LogTurn(t, wt->from, to, lenMs, wt->clock.InAir(), landed.n);

    // Bundles are still instant jumps in build step 4 (they come in step 7)
    if (t.kind != TurnKind::Curl) {
        LandEverything(wt, win, "Bundle");
        return;
    }
    StartCurl(win, dm, t, lenMs);
    FlashCurlStarted(win);
}

void DetectBeforeSpread(MainWindow* win, DisplayModel* dm) {
    WindowTurns* wt = FindTurns(win, Lookup::Create);
    if (!wt || wt->pending) {
        return;
    }

    FlashTap(win);

    TurnGate g;
    g.switchOn = gSettings->skeuomorphPageTurn;
    g.pagedView = !IsContinuous(dm->GetDisplayMode());
    g.layerLoaded = CurlIsLoaded(win);
    g.syncing = dm->syncingWithEngineLayout;
    g.stepsOnePage = dm->StepsOnePage();
    g.hidden = IsHidden(win);
    g.atHook = FingerprintOf(dm);
    g.atPaint = wt->atPaint;

    // SetDisplayMode & co. re-pair the pages before GoToPage: the reader last
    // saw the painted Spread, not dm's rows
    wt->gate = CheckGate(g);
    wt->from = g.atHook == g.atPaint ? SpreadNow(dm) : wt->spreadAtPaint;
    wt->pendingDm = dm;
    wt->pending = true;

    // the view changed since the last paint (tab switch, display mode, Manga
    // Mode): what's in the air lands before anything is drawn
    if (g.atHook != g.atPaint) {
        LandEverything(wt, win, "view changed");
    }

    // win->buffer still shows the old Spread; later it's gone
    if (wt->gate == Skip::None) {
        CurlCopyOld(win, SpreadOnScreen(dm));
    }
    uitask::Post(MkFunc0<MainWindow>(FinishTurn, win), "PageTurnFinish");
}

// an unflushed paint left the old screen up: keep the old fingerprint
void DetectAfterPaint(MainWindow* win, bool flushed) {
    DisplayModel* dm = win->AsFixed();
    // the Curl layer is set up at the first Paged View paint with the switch on
    if (dm && gSettings->skeuomorphPageTurn && !IsContinuous(dm->GetDisplayMode())) {
        CurlEnsure(win);
        FlashWatch(win);
    }
    if (!flushed) {
        return;
    }
    WindowTurns* wt = FindTurns(win, Lookup::Create);
    if (!wt) {
        return;
    }
    wt->atPaint = FingerprintOf(dm);
    wt->spreadAtPaint = dm && dm->pagesInfo ? SpreadNow(dm) : Spread{};
    CurlWindowShowsNew(win, SpreadOnScreen(dm));
}

// a dead dm's address can come back for the next document; it must read as
// "opening". Also called when trimming margins re-lays dm out; the next
// paint restores the fingerprint
void DetectForget(MainWindow* win, DisplayModel* dm) {
    WindowTurns* wt = FindTurns(win, Lookup::Find);
    LandEverything(wt, win, "document closed");
    if (!wt) {
        return;
    }
    if (wt->atPaint.dm == (uintptr_t)dm) {
        wt->atPaint = {};
        wt->spreadAtPaint = {};
    }
    if (wt->pendingDm == dm) {
        wt->pendingDm = nullptr;
    }
}

// hook 4, outside an action: zoom, rotation, Manga Mode, Presentation, a new
// canvas size. Inside one (GoToPage re-lays out every Page Turn), FinishTurn
// decides; the Curl it starts comes after, so a turn never lands its own Curl
void DetectBeforeRelayout(MainWindow* win) {
    WindowTurns* wt = FindTurns(win, Lookup::Find);
    if (wt && wt->pending) {
        return;
    }
    LandEverything(wt, win, "relayout");
}

// before SumatraPDF paints: the window shows another document or view than
// the one under the Curl (tab switch, closed document, Manga Mode). Lands
// before the paint, not after
void DetectBeforePaint(MainWindow* win) {
    if (!CurlInAir(win)) {
        return;
    }
    WindowTurns* wt = FindTurns(win, Lookup::Find);
    if (wt && FingerprintOf(win->AsFixed()) == wt->atPaint) {
        return;
    }
    LandEverything(wt, win, "view changed");
}

// a window change (resize, minimize, DPI): see WindowChanges.cpp
void DetectLandAll(MainWindow* win, const char* why) {
    LandEverything(FindTurns(win, Lookup::Find), win, why);
}

// the switch turned off
void DetectLandAllWindows(const char* why) {
    for (WindowTurns* wt : gWindowTurns) {
        Landed landed;
        wt->clock.LandAll(&landed);
    }
    CurlLandAllWindows(why);
}

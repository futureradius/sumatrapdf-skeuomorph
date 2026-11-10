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
#include "MainWindow.h"

#include "skeuomorph/PageTurnLogic.h"
#include "skeuomorph/PageTurnHooks.h"
#include "skeuomorph/PageTurnDetect.h"

using namespace pageturn;

// One action (key, wheel notch, command) = one Page Turn:
//
//   message --> GoToPage --> hook 3: first call of the action:
//           |                        gate, old Spread, Post(FinishTurn)
//           +-> GoToPage --> hook 3: ignored, same action
//   FinishTurn (posted, runs after the message): final Spread -> DecideTurn
//   WM_PAINT --> hook 6: fingerprint of what the reader now sees

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
        return;
    }

    // zoom, resize, restore: the Spread stayed, not a page change
    Spread to = SpreadNow(dm);
    if (to.first == wt->from.first && to.last == wt->from.last) {
        return;
    }

    Reading reading = dm->GetDisplayR2L() ? Reading::RightToLeft : Reading::LeftToRight;
    Turn t = DecideTurn(wt->gate, wt->from, to, reading);
    if (t.kind == TurnKind::Nothing) {
        LogTurn(t, wt->from, to, 0, wt->clock.InAir(), 0);
        return;
    }

    i64 now = NowMs();
    Landed landed;
    wt->clock.SetDuration(PageTurnDurationMs());
    wt->clock.Start(now, t.kind, &landed);
    LogTurn(t, wt->from, to, wt->clock.LengthMs(t.kind, now), wt->clock.InAir(), landed.n);
}

void DetectBeforeSpread(MainWindow* win, DisplayModel* dm) {
    WindowTurns* wt = FindTurns(win, Lookup::Create);
    if (!wt || wt->pending) {
        return;
    }

    TurnGate g;
    g.switchOn = gSettings->skeuomorphPageTurn;
    g.pagedView = !IsContinuous(dm->GetDisplayMode());
    // no Curl layer before build step 4
    g.layerLoaded = true;
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
    uitask::Post(MkFunc0<MainWindow>(FinishTurn, win), "PageTurnFinish");
}

// an unflushed paint left the old screen up: keep the old fingerprint
void DetectAfterPaint(MainWindow* win, bool flushed) {
    if (!flushed) {
        return;
    }
    WindowTurns* wt = FindTurns(win, Lookup::Create);
    if (!wt) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    wt->atPaint = FingerprintOf(dm);
    wt->spreadAtPaint = dm && dm->pagesInfo ? SpreadNow(dm) : Spread{};
}

// a dead dm's address can come back for the next document; it must read as
// "opening". Also called when trimming margins re-lays dm out; the next
// paint restores the fingerprint
void DetectForget(MainWindow* win, DisplayModel* dm) {
    WindowTurns* wt = FindTurns(win, Lookup::Find);
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

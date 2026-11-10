/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "Settings.h"
#include "AppSettings.h"

#include "skeuomorph/PageTurnLogic.h"
#include "skeuomorph/PageTurnDetect.h"
#include "skeuomorph/PageEdges.h"
#include "skeuomorph/CurlRenderer.h"
#include "skeuomorph/WindowChanges.h"
#include "skeuomorph/PageTurnHooks.h"

// View / Skeuomorph Page Turn; saved at once so it survives a crash
void ToggleSkeuomorphPageTurn() {
    gSettings->skeuomorphPageTurn = !gSettings->skeuomorphPageTurn;
    ScheduleSaveSettings();
    DetectLandAllWindows("switch off");
    EdgesRelayoutAll();
}

// normal Curl length; PageTurnDuration clamped so a typo can't freeze or skip the Curl
int PageTurnDurationMs() {
    return limitValue(gSettings->pageTurnDuration, 100, 5000);
}

// hook 3: GoToPage is about to swap the Spread
void PageTurnBeforeSpread(MainWindow* win, DisplayModel* dm, int pageNo) {
    DetectBeforeSpread(win, dm);
    EdgesBeforeSpread(dm, pageNo);
}

// hook 4: room for Page Edges and the cover flag; zoom, rotation & co. land Curls
void PageTurnBeforeRelayout(MainWindow* win, DisplayModel* dm) {
    DetectBeforeRelayout(win);
    EdgesBeforeRelayout(dm);
}

// hook 5: resting Page Edges, after the background, before the pages
void PageEdgesPaint(MainWindow*, HDC hdc, DisplayModel* dm) {
    EdgesPaint(hdc, dm);
}

// hook 6: flushed = the screen now shows SumatraPDF's own picture
void PageTurnAfterPaint(MainWindow* win, bool flushed) {
    DetectAfterPaint(win, flushed);
    if (gSettings->skeuomorphPageTurn) {
        WatchWindowChanges(win);
    }
}

// hook 8: dm is going away, a new one may get its address
void PageTurnForget(MainWindow* win, DisplayModel* dm) {
    DetectForget(win, dm);
    EdgesForget(dm);
}

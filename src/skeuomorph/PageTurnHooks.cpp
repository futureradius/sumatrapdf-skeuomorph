/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "Settings.h"
#include "AppSettings.h"

#include "skeuomorph/PageTurnDetect.h"
#include "skeuomorph/PageTurnHooks.h"

// View / Skeuomorph Page Turn; saved at once so it survives a crash
void ToggleSkeuomorphPageTurn() {
    gSettings->skeuomorphPageTurn = !gSettings->skeuomorphPageTurn;
    ScheduleSaveSettings();
}

// normal Curl length; PageTurnDuration clamped so a typo can't freeze or skip the Curl
int PageTurnDurationMs() {
    return limitValue(gSettings->pageTurnDuration, 100, 5000);
}

// hook 3: GoToPage is about to swap the Spread
void PageTurnBeforeSpread(MainWindow* win, DisplayModel* dm, int) {
    DetectBeforeSpread(win, dm);
}

// hook 4: room for Page Edges comes in build step 3
void PageTurnBeforeRelayout(MainWindow*, DisplayModel*) {}

// hook 6: flushed = the screen now shows SumatraPDF's own picture
void PageTurnAfterPaint(MainWindow* win, bool flushed) {
    DetectAfterPaint(win, flushed);
}

// hook 8: dm is going away, a new one may get its address
void PageTurnForget(MainWindow* win, DisplayModel* dm) {
    DetectForget(win, dm);
}

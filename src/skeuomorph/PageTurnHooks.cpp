/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "Settings.h"
#include "AppSettings.h"

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

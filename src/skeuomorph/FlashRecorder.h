/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// test-only: records what the screen shows during each Curl and logs whether
// it flashed. Debug builds only, and only with SKEUOMORPH_FLASH set
struct MainWindow;

void FlashWatch(MainWindow* win);
void FlashTap(MainWindow* win);
void FlashCurlStarted(MainWindow* win);

bool FlashSkipsCoverWait();
int FlashLayerLagMs();

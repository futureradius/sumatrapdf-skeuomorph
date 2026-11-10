/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// one action = one Page Turn: per-window state between the hooks
struct MainWindow;
struct DisplayModel;

void DetectBeforeSpread(MainWindow* win, DisplayModel* dm);
void DetectAfterPaint(MainWindow* win, bool flushed);
void DetectForget(MainWindow* win, DisplayModel* dm);

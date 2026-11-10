/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// the only header original files include; one function per hook point
struct MainWindow;
struct DisplayModel;

void ToggleSkeuomorphPageTurn();
int PageTurnDurationMs();

void PageTurnBeforeSpread(MainWindow* win, DisplayModel* dm, int pageNo);
void PageTurnBeforeRelayout(MainWindow* win, DisplayModel* dm);
void PageEdgesPaint(MainWindow* win, HDC hdc, DisplayModel* dm);
void PageTurnAfterPaint(MainWindow* win, bool flushed);
void PageTurnForget(MainWindow* win, DisplayModel* dm);

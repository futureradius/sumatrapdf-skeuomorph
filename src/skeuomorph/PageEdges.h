/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Page Edges at rest: the room beside the Spread, the cover beside the spine,
// and drawing them in SumatraPDF's own painting
struct DisplayModel;

void EdgesBeforeRelayout(DisplayModel* dm);
void EdgesBeforeSpread(DisplayModel* dm, int pageNo);
void EdgesForget(DisplayModel* dm);
void EdgesRelayoutAll();
void EdgesPaint(HDC hdc, DisplayModel* dm);

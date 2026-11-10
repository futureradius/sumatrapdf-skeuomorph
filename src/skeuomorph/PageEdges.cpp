/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
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
#include "skeuomorph/PageEdges.h"

using namespace pageturn;

// Page Edges at rest, in a Paged View with the switch on:
//
//   |<- margin ->|<- room ->|  page  | page  |<- room ->|<- margin ->|
//                    ||||||                    ||||||||
//                    read                       unread
//
// The room is the whole book's thickness on each side, added to windowMargin
// at the start of every Relayout (hook 4), so Fit Page shrinks the pages and
// a fixed zoom keeps them. The Page Edges are painted into it (hook 5).

// prototype colours for white paper; other paper is tinted the same way
constexpr Color kEdgeNear = MkRgb(0xf1, 0xec, 0xe0);
constexpr Color kEdgeOuter = MkRgb(0xd6, 0xcf, 0xbd);
constexpr Color kFineLine = MkRgb(110, 100, 80);
constexpr float kFineLineAlpha = 0.28f;
constexpr Color kRimLine = MkRgb(80, 70, 55);
constexpr float kRimLineAlpha = 0.55f;

// the room hook 4 left in a document's windowMargin
struct RoomOf {
    DisplayModel* dm = nullptr;
    Room room;
};

static Vec<RoomOf*> gRooms;

static int FindRoom(DisplayModel* dm) {
    for (int i = 0; i < len(gRooms); i++) {
        if (gRooms[i]->dm == dm) {
            return i;
        }
    }
    return -1;
}

static bool EdgesOn(DisplayModel* dm) {
    return gSettings->skeuomorphPageTurn && !IsContinuous(dm->GetDisplayMode());
}

static Layout LayoutOf(DisplayModel* dm) {
    DisplayMode mode = dm->GetDisplayMode();
    if (IsSingle(mode)) {
        return Layout::Single;
    }
    return IsBookView(mode) ? Layout::Book : Layout::Facing;
}

// screen px on each side of the Spread; none in Presentation
static int RoomOnScreen(DisplayModel* dm) {
    if (!EdgesOn(dm) || dm->InPresentation()) {
        return 0;
    }
    return ScaleByDpi(RoomPx(dm->PageCount(), LayoutOf(dm)), dm->uiDpi);
}

// sets the room to px; a document that never had room keeps its margin
// untouched, so the switch off is exactly the original
static void SetRoom(DisplayModel* dm, int px) {
    int i = FindRoom(dm);
    if (i < 0 && px == 0) {
        return;
    }
    if (i < 0) {
        RoomOf* r = new RoomOf();
        r->dm = dm;
        VecAppend(gRooms, r);
        i = len(gRooms) - 1;
    }

    RoomOf* r = gRooms[i];
    WindowMargin& m = dm->windowMargin;
    r->room = ApplyRoom(r->room, m.left, m.right, px);
    m.left = r->room.left;
    m.right = r->room.right;
    if (px == 0) {
        VecRemoveAt(gRooms, i);
        delete r;
    }
}

// hook 4: every zoom, resize, mode switch and Spread change passes here,
// before fit zoom is computed
void EdgesBeforeRelayout(DisplayModel* dm) {
    dm->pageBesideSpine = EdgesOn(dm);
    SetRoom(dm, RoomOnScreen(dm));
}

// dm dies, or trimming margins re-lays it out right after: give the margin
// back, so a new dm at the same address starts clean
void EdgesForget(DisplayModel* dm) {
    SetRoom(dm, 0);
}

// the switch changed: room and cover move at once, like a zoom change
void EdgesRelayoutAll() {
    for (MainWindow* win : gWindows) {
        DisplayModel* dm = win->AsFixed();
        if (!dm || !dm->pagesInfo || IsContinuous(dm->GetDisplayMode())) {
            continue;
        }
        dm->RelayoutKeepingView();
    }
}

static u8 MixU8(u8 a, u8 b, float f) {
    return (u8)lroundf(a + (b - a) * f);
}

static Color Mix(Color a, Color b, float f) {
    return MkRgb(MixU8(GetRed(a), GetRed(b), f), MixU8(GetGreen(a), GetGreen(b), f), MixU8(GetBlue(a), GetBlue(b), f));
}

// paper x tint, channel by channel: white paper gets the tint itself
static Color Tint(Color paper, Color tint) {
    auto ch = [](u8 p, u8 t) { return (u8)((p * t + 127) / 255); };
    return MkRgb(ch(GetRed(paper), GetRed(tint)), ch(GetGreen(paper), GetGreen(tint)),
                 ch(GetBlue(paper), GetBlue(tint)));
}

enum class Side {
    Left,
    Right
};

// fine 1 px columns beside the page's outer edge, full page height, a little
// darker toward the outside; cut off at the window, never squeezed
static void PaintSide(HDC hdc, Rect page, Side side, int thick, int dpi, Color paper, Rect clip) {
    Color colNear = Tint(paper, kEdgeNear);
    Color colOuter = Tint(paper, kEdgeOuter);
    for (int col = 0; col < thick; col++) {
        Color c = Mix(colNear, colOuter, (col + 0.5f) / thick);
        EdgeLine line = EdgeLineAt(col, thick, dpi);
        if (line == EdgeLine::Fine) {
            c = Mix(c, kFineLine, kFineLineAlpha);
        } else if (line == EdgeLine::Rim) {
            c = Mix(c, kRimLine, kRimLineAlpha);
        }

        int x = side == Side::Right ? page.x + page.dx + col : page.x - 1 - col;
        Rect r = Rect(x, page.y, 1, page.dy).Intersect(clip);
        if (r.IsEmpty()) {
            continue;
        }
        RECT rc = ToRECT(r);
        SetDCBrushColor(hdc, c);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(DC_BRUSH));
    }
}

// hook 5: after the background, before the pages
void EdgesPaint(HDC hdc, DisplayModel* dm) {
    int whole = RoomOnScreen(dm);
    if (whole <= 0 || !dm->pagesInfo) {
        return;
    }

    // the Spread on screen, and its outermost pages
    Spread at;
    Rect leftPage;
    Rect rightPage;
    for (int pageNo = 1; pageNo <= dm->PageCount(); pageNo++) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || !pi->isShown || pi->pos.IsEmpty()) {
            continue;
        }
        Rect r = pi->pageOnScreen;
        if (at.first == 0) {
            at = {pageNo, pageNo};
            leftPage = rightPage = r;
            continue;
        }
        at.last = pageNo;
        if (r.x < leftPage.x) {
            leftPage = r;
        }
        if (r.x + r.dx > rightPage.x + rightPage.dx) {
            rightPage = r;
        }
    }
    if (at.first == 0) {
        return;
    }

    Reading reading = dm->GetDisplayR2L() ? Reading::RightToLeft : Reading::LeftToRight;
    EdgesPx e = RestingEdges(dm->PageCount(), at, LayoutOf(dm), reading, whole);
    Color paper;
    ThemePageRenderColors(paper);
    Rect clip(Point(), dm->GetViewPort().Size());
    COLORREF prevBrush = GetDCBrushColor(hdc);
    PaintSide(hdc, leftPage, Side::Left, e.left, dm->uiDpi, paper, clip);
    PaintSide(hdc, rightPage, Side::Right, e.right, dm->uiDpi, paper, clip);
    SetDCBrushColor(hdc, prevBrush);
}

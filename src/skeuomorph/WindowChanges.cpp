/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"

#include "gui/UIModels.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"

#include "skeuomorph/PageTurnLogic.h"
#include "skeuomorph/PageTurnDetect.h"
#include "skeuomorph/CurlRenderer.h"
#include "skeuomorph/WindowChanges.h"

// Everything in the air lands at once when the document area changes (spec
// 3.11). Moving the window, focus, menus, dialogs and covering windows send
// none of these, so Curls keep playing then:
//
//   canvas  WM_SIZE        resize, maximize, fullscreen, Presentation: land,
//                          fit the layer to the new size
//           WM_SHOWWINDOW  hidden (Favorites tab): land
//           WM_PAINT       another document or view than under the Curl
//                          (tab switch, closed document): land before painting
//           WM_NCDESTROY   stop the drawing thread
//   frame   WM_SIZE        minimized: land
//           WM_DPICHANGED  land
//
// Zoom, rotation and Manga Mode re-lay out: hook 4 (DetectBeforeRelayout).

struct CanvasWatch {
    MainWindow* win = nullptr;
    Size size;
};

static UINT_PTR WatchId() {
    static UINT_PTR id = NextSubclassId();
    return id;
}

static Size ClientSize(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return {rc.right - rc.left, rc.bottom - rc.top};
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto w = (CanvasWatch*)ref;
    switch (msg) {
        case WM_SIZE: {
            // SumatraPDF re-lays out and paints in its own handler, after this
            Size size = {LOWORD(lp), HIWORD(lp)};
            if (!(size == w->size)) {
                w->size = size;
                DetectLandAll(w->win, "resize");
                CurlResize(w->win);
            }
            break;
        }
        case WM_SHOWWINDOW:
            if (!wp) {
                DetectLandAll(w->win, "hidden");
            }
            break;
        case WM_PAINT:
            DetectBeforePaint(w->win);
            break;
        case WM_NCDESTROY:
            CurlDestroy(w->win);
            RemoveWindowSubclass(hwnd, CanvasProc, id);
            delete w;
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK FrameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto win = (MainWindow*)ref;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                DetectLandAll(win, "minimize");
            }
            break;
        case WM_DPICHANGED:
            DetectLandAll(win, "DPI change");
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, FrameProc, id);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// once per window, with the switch on; cheap to call at every paint
void WatchWindowChanges(MainWindow* win) {
    if (!win || !win->hwndCanvas || !win->hwndFrame) {
        return;
    }
    UINT_PTR id = WatchId();
    DWORD_PTR ref = 0;
    if (GetWindowSubclass(win->hwndCanvas, CanvasProc, id, &ref)) {
        return;
    }

    auto w = new CanvasWatch();
    w->win = win;
    w->size = ClientSize(win->hwndCanvas);
    if (!SetWindowSubclass(win->hwndCanvas, CanvasProc, id, (DWORD_PTR)w)) {
        delete w;
        return;
    }
    SetWindowSubclass(win->hwndFrame, FrameProc, id, (DWORD_PTR)win);
}

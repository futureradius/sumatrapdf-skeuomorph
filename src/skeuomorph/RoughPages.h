/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Rough Pages (spec 5.6, ADR 0002): small pictures of pages the Curls show
// that SumatraPDF hasn't drawn sharp. One worker thread per window draws them,
// one at a time at below-normal priority, from a background copy of the
// document (Clone(), as PageThumbnails.cpp), never through the render queue:
//
//   UI thread       RoughShow(rp, dm)    the copy follows dm (made on the worker)
//                   RoughHide(rp)        the copy closes
//   drawing thread  RoughWish(rp, ...)   pages wanted, best first, at most 10
//                   RoughBorrow(rp, page)  a finished one, BGRA, until RoughReturn
//   UI thread       RoughKeep(rp, ...)   the Spread on screen, until it's sharp
//                   RoughPaint(rp, ...)  SumatraPDF shows it there meanwhile
//   worker          a page dropped from the wishes is aborted, its result thrown away
//
// Include base/Pixmap.h first.
struct RoughPages;
struct DisplayModel;

constexpr int kMaxRoughWishes = 10;

struct RoughWant {
    int page = 0;
    float zoom = 0; // the page on screen: px per page unit
};

RoughPages* NewRoughPages(HWND hwnd);
void ReleaseRoughPages(RoughPages* rp);

// UI thread
void RoughShow(RoughPages* rp, DisplayModel* dm);
void RoughHide(RoughPages* rp);
void RoughKeep(RoughPages* rp, const RoughWant* shown, int n);
bool RoughPaint(RoughPages* rp, DisplayModel* dm, HDC hdc, int page, Rect pageRect, Rect clip);

// drawing thread
int RoughGeneration(RoughPages* rp);
enum class RoughWait {
    Try, // the lock is busy: false
    Block
};
bool RoughWish(RoughPages* rp, const RoughWant* wants, int n, RoughWait wait);
Pixmap* RoughBorrow(RoughPages* rp, int page);
void RoughReturn(RoughPages* rp);

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
//                   RoughTake(rp, page)  a finished one, BGRA; the caller owns it
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

RoughPages* NewRoughPages();
void ReleaseRoughPages(RoughPages* rp);

// UI thread
void RoughShow(RoughPages* rp, DisplayModel* dm);
void RoughHide(RoughPages* rp);

// drawing thread
int RoughGeneration(RoughPages* rp);
void RoughWish(RoughPages* rp, const RoughWant* wants, int n);
Pixmap* RoughTake(RoughPages* rp, int page);

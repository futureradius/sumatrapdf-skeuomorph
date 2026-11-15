/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#if IS_DEBUG

#include "skeuomorph/PageTurnLogic.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

using namespace pageturn;

static bool Near(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

static bool SameSpread(Spread a, int first, int last) {
    return a.first == first && a.last == last;
}

static void SpreadOfTest() {
    utassert(SameSpread(SpreadOf(5, 10, Layout::Single), 5, 5));
    utassert(SameSpread(SpreadOf(0, 10, Layout::Single), 1, 1));
    utassert(SameSpread(SpreadOf(11, 10, Layout::Single), 10, 10));
    utassert(SameSpread(SpreadOf(1, 0, Layout::Single), 0, 0));

    utassert(SameSpread(SpreadOf(1, 10, Layout::Facing), 1, 2));
    utassert(SameSpread(SpreadOf(4, 10, Layout::Facing), 3, 4));
    utassert(SameSpread(SpreadOf(9, 9, Layout::Facing), 9, 9));

    // lone cover, then pairs, then a lone last page
    utassert(SameSpread(SpreadOf(1, 10, Layout::Book), 1, 1));
    utassert(SameSpread(SpreadOf(2, 10, Layout::Book), 2, 3));
    utassert(SameSpread(SpreadOf(3, 10, Layout::Book), 2, 3));
    utassert(SameSpread(SpreadOf(9, 10, Layout::Book), 8, 9));
    utassert(SameSpread(SpreadOf(10, 10, Layout::Book), 10, 10));
    utassert(SameSpread(SpreadOf(2, 2, Layout::Book), 2, 2));
    utassert(SameSpread(SpreadOf(1, 1, Layout::Book), 1, 1));
}

static void ExpectTurn(Turn t, TurnKind kind, bool forward, Sweep sweep, int pages) {
    utassert(t.kind == kind);
    utassert(t.why == Skip::None);
    utassert(t.forward == forward);
    utassert(t.sweep == sweep);
    utassert(t.pages == pages);
}

static void ClassifyTest() {
    auto ltr = Reading::LeftToRight;
    auto rtl = Reading::RightToLeft;
    auto single = [](int p) { return SpreadOf(p, 10, Layout::Single); };
    auto facing = [](int p) { return SpreadOf(p, 10, Layout::Facing); };
    auto book = [](int p) { return SpreadOf(p, 10, Layout::Book); };

    ExpectTurn(ClassifyTurn(single(3), single(4), ltr), TurnKind::Curl, true, Sweep::ToLeft, 1);
    ExpectTurn(ClassifyTurn(single(3), single(2), ltr), TurnKind::Curl, false, Sweep::ToRight, 1);
    ExpectTurn(ClassifyTurn(single(3), single(5), ltr), TurnKind::Bundle, true, Sweep::ToLeft, 2);
    ExpectTurn(ClassifyTurn(single(9), single(1), ltr), TurnKind::Bundle, false, Sweep::ToRight, 8);

    // right-to-left mirrors the sweep, not the direction
    ExpectTurn(ClassifyTurn(single(3), single(4), rtl), TurnKind::Curl, true, Sweep::ToRight, 1);
    ExpectTurn(ClassifyTurn(single(3), single(2), rtl), TurnKind::Curl, false, Sweep::ToLeft, 1);
    ExpectTurn(ClassifyTurn(facing(1), facing(7), rtl), TurnKind::Bundle, true, Sweep::ToRight, 6);
    ExpectTurn(ClassifyTurn(book(6), book(1), rtl), TurnKind::Bundle, false, Sweep::ToLeft, 5);

    ExpectTurn(ClassifyTurn(facing(1), facing(3), ltr), TurnKind::Curl, true, Sweep::ToLeft, 2);
    ExpectTurn(ClassifyTurn(facing(10), facing(7), ltr), TurnKind::Curl, false, Sweep::ToRight, 2);
    ExpectTurn(ClassifyTurn(facing(1), facing(5), ltr), TurnKind::Bundle, true, Sweep::ToLeft, 4);
    ExpectTurn(ClassifyTurn(SpreadOf(7, 9, Layout::Facing), SpreadOf(9, 9, Layout::Facing), ltr), TurnKind::Curl, true,
               Sweep::ToLeft, 2);

    // opening the cover and closing it, the lone last page
    ExpectTurn(ClassifyTurn(book(1), book(2), ltr), TurnKind::Curl, true, Sweep::ToLeft, 1);
    ExpectTurn(ClassifyTurn(book(3), book(1), ltr), TurnKind::Curl, false, Sweep::ToRight, 1);
    ExpectTurn(ClassifyTurn(book(1), book(4), ltr), TurnKind::Bundle, true, Sweep::ToLeft, 3);
    ExpectTurn(ClassifyTurn(book(8), book(10), ltr), TurnKind::Curl, true, Sweep::ToLeft, 2);
    ExpectTurn(ClassifyTurn(book(10), book(9), ltr), TurnKind::Curl, false, Sweep::ToRight, 2);

    // a wide page alone across both columns is a Spread of its own
    ExpectTurn(ClassifyTurn({1, 2}, {3, 3}, ltr), TurnKind::Curl, true, Sweep::ToLeft, 2);
    ExpectTurn(ClassifyTurn({3, 3}, {4, 5}, ltr), TurnKind::Curl, true, Sweep::ToLeft, 1);
    ExpectTurn(ClassifyTurn({4, 5}, {3, 3}, ltr), TurnKind::Curl, false, Sweep::ToRight, 1);

    // the same Spread is nothing, even if the page inside it changed
    Turn t = ClassifyTurn(facing(3), facing(4), ltr);
    utassert(t.kind == TurnKind::Nothing && t.why == Skip::SameSpread && t.sweep == Sweep::None);
    t = ClassifyTurn(single(4), single(4), ltr);
    utassert(t.kind == TurnKind::Nothing && t.why == Skip::SameSpread);
    t = ClassifyTurn({}, single(4), ltr);
    utassert(t.kind == TurnKind::Nothing && t.why == Skip::Opening);

    t = DecideTurn(Skip::DisplayModeSwitch, single(1), single(2), ltr);
    utassert(t.kind == TurnKind::Nothing && t.why == Skip::DisplayModeSwitch);
    t = DecideTurn(Skip::None, single(1), single(2), ltr);
    utassert(t.kind == TurnKind::Curl);
}

static Fingerprint GoodPrint() {
    Fingerprint fp;
    fp.dm = 0x1000;
    fp.displayMode = 3;
    fp.layoutGeneration = 2;
    fp.pageCount = 40;
    return fp;
}

static void FingerprintTest() {
    Fingerprint a = GoodPrint();
    Fingerprint b = GoodPrint();
    utassert(a == b && !(a != b));
    utassert(FingerprintSkip(a, b) == Skip::None);

    b.dm = 0;
    utassert(a != b);
    utassert(FingerprintSkip(a, b) == Skip::Opening);
    b = GoodPrint();
    b.dm = 0x2000;
    utassert(FingerprintSkip(a, b) == Skip::Opening);

    b = GoodPrint();
    b.displayMode = 4;
    utassert(FingerprintSkip(a, b) == Skip::DisplayModeSwitch);
    b = GoodPrint();
    b.inPresentation = true;
    utassert(FingerprintSkip(a, b) == Skip::DisplayModeSwitch);
    b = GoodPrint();
    b.facingStepOne = true;
    utassert(FingerprintSkip(a, b) == Skip::StepOnePage);
    b = GoodPrint();
    b.r2l = true;
    utassert(FingerprintSkip(a, b) == Skip::MangaToggle);
    b = GoodPrint();
    b.pageCount = 41;
    b.layoutGeneration = 3;
    utassert(FingerprintSkip(a, b) == Skip::EpubRenumbering);
    b = GoodPrint();
    b.layoutGeneration = 3;
    utassert(FingerprintSkip(a, b) == Skip::Relayout);
}

static TurnGate GoodGate() {
    TurnGate g;
    g.switchOn = true;
    g.pagedView = true;
    g.layerLoaded = true;
    g.atHook = GoodPrint();
    g.atPaint = GoodPrint();
    return g;
}

static void GateTest() {
    TurnGate g = GoodGate();
    utassert(CheckGate(g) == Skip::None);

    g.switchOn = false;
    g.pagedView = false;
    g.hidden = true;
    utassert(CheckGate(g) == Skip::SwitchOff);
    g = GoodGate();
    g.pagedView = false;
    utassert(CheckGate(g) == Skip::Continuous);
    g = GoodGate();
    g.hidden = true;
    utassert(CheckGate(g) == Skip::Minimized);
    g = GoodGate();
    g.syncing = true;
    utassert(CheckGate(g) == Skip::EpubRenumbering);
    g = GoodGate();
    g.stepsOnePage = true;
    utassert(CheckGate(g) == Skip::StepOnePage);
    g = GoodGate();
    g.layerLoaded = false;
    utassert(CheckGate(g) == Skip::NoLayer);

    // a changed fingerprint says more than a missing layer
    g.atPaint.displayMode = 1;
    utassert(CheckGate(g) == Skip::DisplayModeSwitch);
    g = GoodGate();
    g.atPaint = {};
    utassert(CheckGate(g) == Skip::Opening);
}

static void NamesTest() {
    utassert(str::Eq(SkipName(Skip::None), StrL("")));
    utassert(str::Eq(SkipName(Skip::SameSpread), StrL("same Spread")));
    utassert(str::Eq(SkipName(Skip::SwitchOff), StrL("switch off")));
    utassert(str::Eq(SkipName(Skip::Continuous), StrL("continuous view")));
    utassert(str::Eq(SkipName(Skip::Minimized), StrL("minimized")));
    utassert(str::Eq(SkipName(Skip::Opening), StrL("opening")));
    utassert(str::Eq(SkipName(Skip::DisplayModeSwitch), StrL("display mode switch")));
    utassert(str::Eq(SkipName(Skip::StepOnePage), StrL("step one page")));
    utassert(str::Eq(SkipName(Skip::MangaToggle), StrL("Manga Mode toggle")));
    utassert(str::Eq(SkipName(Skip::EpubRenumbering), StrL("EPUB renumbering")));
    utassert(str::Eq(SkipName(Skip::Relayout), StrL("relayout")));
    utassert(str::Eq(SkipName(Skip::NoLayer), StrL("no layer")));
    utassert(str::Eq(TurnKindName(TurnKind::Nothing), StrL("nothing")));
    utassert(str::Eq(TurnKindName(TurnKind::Curl), StrL("Curl")));
    utassert(str::Eq(TurnKindName(TurnKind::Bundle), StrL("Bundle")));
}

static void SizesTest() {
    // whole book: sqrt(pages x 0.12 x 46), 4..46 px; spec examples 10, 22, 46
    utassert(BookPx(0) == 0);
    utassert(Near(BookPx(1), 4));
    utassert(Near(BookPx(20), 10.507f));
    utassert(WholePx(BookPx(20)) == 10);
    utassert(WholePx(BookPx(89)) == 22);
    utassert(WholePx(BookPx(40)) == 14);
    utassert(WholePx(BookPx(383)) == 45);
    utassert(Near(BookPx(400), 46));
    utassert(Near(BookPx(100000), 46));
    utassert(WholePx(BookPx(3)) == 4);
    utassert(WholePx(-1) == 0);

    // split by share of the pages off the Spread
    auto ltr = Reading::LeftToRight;
    auto rtl = Reading::RightToLeft;
    EdgesPx e = SplitEdges(800, {1, 2}, ltr);
    utassert(e.read == 0 && e.unread == 46 && e.left == 0 && e.right == 46);
    e = SplitEdges(800, {799, 800}, ltr);
    utassert(e.read == 46 && e.unread == 0 && e.left == 46 && e.right == 0);
    e = SplitEdges(800, {399, 400}, ltr);
    utassert(e.read == 23 && e.unread == 23);
    e = SplitEdges(800, {3, 4}, ltr);
    utassert(e.read == 1 && e.unread == 45);
    e = SplitEdges(800, {797, 798}, ltr);
    utassert(e.read == 45 && e.unread == 1);
    e = SplitEdges(800, {1, 2}, rtl);
    utassert(e.unread == 46 && e.left == 46 && e.right == 0);
    e = SplitEdges(800, {599, 600}, rtl);
    utassert(e.read == 34 && e.unread == 12 && e.left == 12 && e.right == 34);

    // Book View: lone cover and lone last page have paper on one side only
    e = SplitEdges(40, {1, 1}, ltr);
    utassert(e.read == 0 && e.unread == 14);
    e = SplitEdges(40, {40, 40}, ltr);
    utassert(e.read == 14 && e.unread == 0);
    e = SplitEdges(3, {2, 2}, ltr);
    utassert(e.read == 2 && e.unread == 2);
    e = SplitEdges(1, {1, 1}, ltr);
    utassert(e.read == 0 && e.unread == 0 && e.left == 0 && e.right == 0);
    e = SplitEdges(2, {1, 2}, ltr);
    utassert(e.read == 0 && e.unread == 0);
    e = SplitEdges(10, {}, ltr);
    utassert(e.read == 0 && e.unread == 0);

    // Bundles: 0.70 px per page, at most 18 px (about 26 pages)
    utassert(BundlePx(0) == 0);
    utassert(Near(BundlePx(2), 1.4f));
    utassert(Near(BundlePx(10), 7));
    utassert(WholePx(BundlePx(10)) == 7);
    utassert(Near(BundlePx(25), 17.5f));
    utassert(Near(BundlePx(26), 18));
    utassert(Near(BundlePx(500), 18));

    // sheets: about 1 per 1.1 px, 2..14
    utassert(BundleSheets(BundlePx(2)) == 2);
    utassert(BundleSheets(BundlePx(4)) == 3);
    utassert(BundleSheets(BundlePx(10)) == 6);
    utassert(BundleSheets(14) == 13);
    utassert(BundleSheets(BundlePx(26)) == 14);
    utassert(BundleSheets(0) == 2);
}

static void LengthTest() {
    TurnClock c(700);
    utassert(c.CurlMs(0) == 700);
    utassert(c.LengthMs(TurnKind::Curl, 0) == 700);
    utassert(c.LengthMs(TurnKind::Bundle, 0) == 910);
    utassert(c.LengthMs(TurnKind::Nothing, 0) == 0);
    c.SetDuration(1000);
    utassert(c.CurlMs(0) == 1000);
    utassert(c.LengthMs(TurnKind::Bundle, 0) == 1300);

    // slow taps keep the normal length
    TurnClock slow(700);
    slow.Start(0, TurnKind::Curl, nullptr);
    slow.Start(700, TurnKind::Curl, nullptr);
    slow.Start(1500, TurnKind::Curl, nullptr);
    utassert(slow.CurlMs(1500) == 700);
}

// length after two taps gapMs apart
static int FastMs(int durationMs, int gapMs) {
    TurnClock c(durationMs);
    c.Start(1000, TurnKind::Curl, nullptr);
    c.Start(1000 + gapMs, TurnKind::Curl, nullptr);
    return c.CurlMs(1000 + gapMs);
}

static void FastTapTest() {
    // gap x 4, never below 310, never above normal
    utassert(FastMs(700, 100) == 400);
    utassert(FastMs(700, 150) == 600);
    utassert(FastMs(700, 50) == 310);
    utassert(FastMs(700, 0) == 310);
    utassert(FastMs(700, 175) == 700);
    utassert(FastMs(700, 300) == 700);
    utassert(FastMs(5000, 500) == 2000);
    utassert(FastMs(5000, 4999) == 5000);
    // below 310 the normal length stays
    utassert(FastMs(200, 50) == 200);
    utassert(FastMs(100, 10) == 100);
    utassert(FastMs(310, 10) == 310);

    // Bundles count as a tap and speed up in step
    TurnClock c(700);
    c.Start(0, TurnKind::Bundle, nullptr);
    c.Start(100, TurnKind::Curl, nullptr);
    utassert(c.CurlMs(100) == 400);
    utassert(c.LengthMs(TurnKind::Bundle, 100) == 520);
    c.Start(200, TurnKind::Bundle, nullptr);
    utassert(c.CurlMs(200) == 400);

    // a steady rhythm keeps its length
    TurnClock r(700);
    for (int t = 0; t <= 1000; t += 120) {
        r.Start(t, TurnKind::Curl, nullptr);
    }
    utassert(r.CurlMs(960) == 480);
}

static void EaseBackTest() {
    // fast at 100 ms: 400 is held for 400 ms, then eases linearly to 700 over 700 ms
    TurnClock c(700);
    c.Start(0, TurnKind::Curl, nullptr);
    c.Start(100, TurnKind::Curl, nullptr);
    utassert(c.CurlMs(300) == 400);
    utassert(c.CurlMs(500) == 400);
    utassert(c.CurlMs(850) == 550);
    utassert(c.CurlMs(1200) == 700);
    utassert(c.CurlMs(100000) == 700);
    int prev = 0;
    bool rising = true;
    for (int t = 100; t <= 1300; t += 10) {
        int ms = c.CurlMs(t);
        rising &= ms >= prev;
        prev = ms;
    }
    utassert(rising);

    // a tap slower than the eased length doesn't restart the easing
    c.Start(900, TurnKind::Curl, nullptr);
    utassert(c.CurlMs(900) == 571);
    utassert(c.CurlMs(1200) == 700);

    // a tap still under the current length follows its gap
    TurnClock d(700);
    d.Start(0, TurnKind::Curl, nullptr);
    d.Start(100, TurnKind::Curl, nullptr);
    d.Start(250, TurnKind::Curl, nullptr);
    utassert(d.CurlMs(250) == 600);
}

static void InAirTest() {
    TurnClock c(700);
    Landed landed;
    int first = c.Start(0, TurnKind::Curl, &landed);
    utassert(first != 0 && landed.n == 0 && c.InAir() == 1);
    utassert(Near(c.Progress(first), 0));
    c.Advance(350, &landed);
    utassert(landed.n == 0 && Near(c.Progress(first), 0.5f));
    c.Advance(700, &landed);
    utassert(landed.n == 1 && landed.ids[0] == first);
    utassert(c.InAir() == 0 && c.Progress(first) == -1);

    // Nothing is not a turn
    utassert(c.Start(800, TurnKind::Nothing, &landed) == 0);
    utassert(c.InAir() == 0);

    // a Bundle lasts 1.3 x as long
    TurnClock b(700);
    int bundle = b.Start(0, TurnKind::Bundle, &landed);
    b.Advance(700, &landed);
    utassert(landed.n == 0 && Near(b.Progress(bundle), 700.f / 910));
    b.Advance(910, &landed);
    utassert(landed.n == 1 && landed.ids[0] == bundle);

    // a turn sped up mid-air keeps its progress
    TurnClock s(700);
    int a = s.Start(0, TurnKind::Curl, &landed);
    s.Start(100, TurnKind::Curl, &landed);
    utassert(Near(s.Progress(a), 100.f / 700));
    s.Advance(200, &landed);
    utassert(Near(s.Progress(a), 100.f / 700 + 0.25f));

    // at most 5 in the air, Bundles counted; the 6th lands the oldest
    TurnClock m(700);
    int ids[6];
    for (int i = 0; i < 5; i++) {
        ids[i] = m.Start(i * 10, i == 2 ? TurnKind::Bundle : TurnKind::Curl, &landed);
        utassert(landed.n == 0);
    }
    utassert(m.InAir() == kMaxInAir);
    ids[5] = m.Start(50, TurnKind::Curl, &landed);
    utassert(landed.n == 1 && landed.ids[0] == ids[0]);
    utassert(m.InAir() == kMaxInAir);
    utassert(m.Progress(ids[0]) == -1 && m.Progress(ids[5]) >= 0);

    // the Curl started after the Bundle lands before it
    m.Advance(50 + 310, &landed);
    utassert(landed.n >= 1 && m.Progress(ids[2]) > 0);
    bool bundleLanded = false;
    for (int i = 0; i < landed.n; i++) {
        bundleLanded |= landed.ids[i] == ids[2];
    }
    utassert(!bundleLanded);

    m.LandAll(&landed);
    utassert(m.InAir() == 0 && landed.n >= 1);
    for (int i = 1; i < landed.n; i++) {
        utassert(landed.ids[i - 1] < landed.ids[i]);
    }
}

// a backward tap sends the newest page in the air back from where it is
static void SendBackTest() {
    Spread a{1, 1};
    Spread b{2, 2};
    Spread c{3, 3};
    Landed landed;

    TurnClock k(700);
    int id = k.Start(0, TurnKind::Curl, a, b, &landed);
    utassert(k.Reverse(350, c, b, &landed) == 0);
    utassert(k.Reverse(350, b, a, &landed) == id);
    utassert(k.InAir() == 1 && Near(k.Progress(id), 0.5f));
    utassert(k.IsBack(id));
    // a forward tap sends it on again
    utassert(k.Reverse(350, a, b, &landed) == id);
    utassert(!k.IsBack(id) && Near(k.Progress(id), 0.5f));

    // it counts as a tap: 100 ms after the last one, Curls last 4 x 100
    utassert(k.Reverse(450, b, a, &landed) == id);
    utassert(k.CurlMs(450) == 400);

    // landed: nothing to send back
    TurnClock l(700);
    int lid = l.Start(0, TurnKind::Curl, a, b, &landed);
    l.Advance(700, &landed);
    utassert(landed.n == 1 && landed.ids[0] == lid);
    utassert(l.Reverse(800, b, a, &landed) == 0);

    // two in the air: back taps send the newest, then the one before it
    TurnClock m(700);
    int first = m.Start(0, TurnKind::Curl, a, b, &landed);
    int second = m.Start(200, TurnKind::Curl, b, c, &landed);
    utassert(m.Reverse(250, b, a, &landed) == 0);
    utassert(m.Reverse(600, c, b, &landed) == second);
    utassert(m.Reverse(620, b, a, &landed) == first);
    utassert(m.IsBack(first) && m.IsBack(second) && m.InAir() == 2);

    // Bundles aren't sent back
    TurnClock n(700);
    n.Start(0, TurnKind::Bundle, a, c, &landed);
    utassert(n.Reverse(100, c, a, &landed) == 0);

    // the drawing thread's copy of the lengths matches the clock's
    Pace p = m.PaceNow();
    utassert(p.CurlMs(620) == m.CurlMs(620) && p.CurlMs(1500) == m.CurlMs(1500));
    utassert(p.LengthMs(TurnKind::Bundle, 620) == m.LengthMs(TurnKind::Bundle, 620));
}

static bool SameRoom(Room r, int added, int left, int right) {
    return r.added == added && r.left == left && r.right == right;
}

static void RoomTest() {
    // whole book at 96 dpi, scaled like WindowMargin
    utassert(RoomPx(40, Layout::Book) == 14);
    utassert(RoomPx(800, Layout::Single) == 46);
    utassert(RoomPx(2, Layout::Book) == 4);
    utassert(RoomPx(2, Layout::Single) == 4);
    utassert(RoomPx(3, Layout::Facing) == 4);
    // every page always on screen: no Page Edges, no room
    utassert(RoomPx(1, Layout::Book) == 0);
    utassert(RoomPx(1, Layout::Single) == 0);
    utassert(RoomPx(2, Layout::Facing) == 0);
    utassert(RoomPx(0, Layout::Facing) == 0);

    utassert(ScaleByDpi(46, 96) == 46);
    utassert(ScaleByDpi(46, 144) == 69);
    utassert(ScaleByDpi(14, 144) == 21);
    utassert(ScaleByDpi(14, 192) == 28);
    utassert(ScaleByDpi(5, 0) == 5);

    // added once, kept on re-layout, removed again
    Room r = ApplyRoom({}, 4, 2, 10);
    utassert(SameRoom(r, 10, 14, 12));
    r = ApplyRoom(r, 14, 12, 10);
    utassert(SameRoom(r, 10, 14, 12));
    r = ApplyRoom(r, 14, 12, 21);
    utassert(SameRoom(r, 21, 25, 23));
    r = ApplyRoom(r, 25, 23, 0);
    utassert(SameRoom(r, 0, 4, 2));
    r = ApplyRoom(r, 4, 2, 0);
    utassert(SameRoom(r, 0, 4, 2));

    // SetUiDpi reset the margin: it's the new base
    r = ApplyRoom({10, 14, 14}, 6, 6, 10);
    utassert(SameRoom(r, 10, 16, 16));
    // Presentation zeroes it and gets no room; leaving it, SetUiDpi restores
    r = ApplyRoom({10, 14, 14}, 0, 0, 0);
    utassert(SameRoom(r, 0, 0, 0));
    r = ApplyRoom(r, 4, 4, 10);
    utassert(SameRoom(r, 10, 14, 14));
    // a zero margin with no room yet
    r = ApplyRoom({}, 0, 0, 7);
    utassert(SameRoom(r, 7, 7, 7));
}

static bool SameSides(EdgesPx e, int left, int right) {
    return e.left == left && e.right == right;
}

static void RestingEdgesTest() {
    auto ltr = Reading::LeftToRight;
    auto rtl = Reading::RightToLeft;

    // Single Page: only the unread side, right (left in right-to-left)
    utassert(SameSides(RestingEdges(800, {1, 1}, Layout::Single, ltr, 69), 0, 69));
    utassert(SameSides(RestingEdges(800, {1, 1}, Layout::Single, rtl, 69), 69, 0));
    utassert(SameSides(RestingEdges(800, {400, 400}, Layout::Single, ltr, 46), 0, 23));
    utassert(SameSides(RestingEdges(800, {400, 400}, Layout::Single, rtl, 46), 23, 0));
    utassert(SameSides(RestingEdges(800, {800, 800}, Layout::Single, ltr, 46), 0, 0));

    // Facing / Book View: both sides, as SplitEdges
    utassert(SameSides(RestingEdges(800, {399, 400}, Layout::Facing, ltr, 46), 23, 23));
    utassert(SameSides(RestingEdges(800, {599, 600}, Layout::Book, rtl, 46), 12, 34));
    // lone cover: paper right of it; lone last page: left (mirrored in rtl)
    utassert(SameSides(RestingEdges(40, {1, 1}, Layout::Book, ltr, 21), 0, 21));
    utassert(SameSides(RestingEdges(40, {40, 40}, Layout::Book, ltr, 21), 21, 0));
    utassert(SameSides(RestingEdges(40, {40, 40}, Layout::Book, rtl, 21), 0, 21));

    // every page on screen, or no room: nothing
    utassert(SameSides(RestingEdges(2, {1, 2}, Layout::Facing, ltr, 4), 0, 0));
    utassert(SameSides(RestingEdges(40, {5, 6}, Layout::Facing, ltr, 0), 0, 0));

    // the split follows the scaled whole: sides sum to it
    EdgesPx e = SplitEdges(800, {399, 400}, ltr, 69);
    utassert(e.read + e.unread == 69 && e.read == 34);
}

static void EdgeLineTest() {
    // 96 dpi: lines at round(1.6 k) = 2, 3, 5, 6, 8, ...; outermost is the rim
    utassert(EdgeLineAt(0, 14, 96) == EdgeLine::None);
    utassert(EdgeLineAt(1, 14, 96) == EdgeLine::None);
    utassert(EdgeLineAt(2, 14, 96) == EdgeLine::Fine);
    utassert(EdgeLineAt(3, 14, 96) == EdgeLine::Fine);
    utassert(EdgeLineAt(4, 14, 96) == EdgeLine::None);
    utassert(EdgeLineAt(5, 14, 96) == EdgeLine::Fine);
    utassert(EdgeLineAt(13, 14, 96) == EdgeLine::Rim);
    utassert(EdgeLineAt(14, 14, 96) == EdgeLine::None);
    utassert(EdgeLineAt(-1, 14, 96) == EdgeLine::None);

    // 144 dpi: every 2.4 px = 2, 5, 7, 10, 12, ...
    utassert(EdgeLineAt(2, 21, 144) == EdgeLine::Fine);
    utassert(EdgeLineAt(3, 21, 144) == EdgeLine::None);
    utassert(EdgeLineAt(4, 21, 144) == EdgeLine::None);
    utassert(EdgeLineAt(5, 21, 144) == EdgeLine::Fine);
    utassert(EdgeLineAt(7, 21, 144) == EdgeLine::Fine);
    utassert(EdgeLineAt(20, 21, 144) == EdgeLine::Rim);
}

static bool Close(float a, float b) {
    return fabsf(a - b) < 1e-5f;
}

static void BundleSheetsTest() {
    // a Curl: one plain sheet
    Sheets c = SheetsOf(TurnKind::Curl, 1, 1, 500);
    utassert(c.n == 1 && c.px == 0 && c.lag == 0 && c.stiff == 0 && c.gap == 0);
    utassert(RadiusFactor(c) == 1 && LeadFactor(c) == 1);
    utassert(Close(SheetT(c, 0.3f, 0), 0.3f));

    // 38 pages in Facing: 18 px in 14 sheets, fan 20%, 40% stiffer
    Sheets b = SheetsOf(TurnKind::Bundle, 38, 19, 500);
    utassert(b.n == 14 && Near(b.px, 18) && Near(b.stiff, 0.4f));
    utassert(Close(b.gap, 18.f / 13));
    utassert(Close(b.lag, 18.f / 14 / 500 * 0.7f + 0.2f * 0.45f / 13));
    utassert(Close(RadiusFactor(b), 1 + 1.6f * 0.4f));
    utassert(Close(LeadFactor(b), 0.6f));
    // unknown page width: the fan alone
    utassert(Close(SheetsOf(TurnKind::Bundle, 38, 19, 0).lag, 0.2f * 0.45f / 13));

    // never more sheets than leaves turned: 4 pages in Facing are 2 leaves
    utassert(SheetsOf(TurnKind::Bundle, 4, 2, 500).n == 2);
    utassert(SheetsOf(TurnKind::Bundle, 4, 4, 500).n == 3);
    // thin Bundles are less stiff: full effect from 6 px
    utassert(Close(SheetsOf(TurnKind::Bundle, 3, 3, 500).stiff, 0.4f * 2.1f / 6));

    // the fan: the top sheet starts at once, the bottom one last; all land at 1
    float bottomStarts = b.lag * 13;
    utassert(SheetT(b, 0, 0) == 0 && SheetT(b, bottomStarts, 13) == 0);
    utassert(SheetT(b, 1, 0) == 1 && SheetT(b, 1, 13) == 1);
    utassert(SheetT(b, 0.5f, 0) > SheetT(b, 0.5f, 6) && SheetT(b, 0.5f, 6) > SheetT(b, 0.5f, 13));

    // lifted, the sheets stack a gap apart round one cylinder axis; flat at both ends
    Lift top = SheetLift(b, 0.5f, 0);
    Lift bottom = SheetLift(b, 0.5f, 13);
    utassert(Close(top.base, 13 * b.gap) && Close(top.radius, 0));
    utassert(Close(bottom.base, 0) && Close(bottom.radius, 13 * b.gap));
    utassert(SheetLift(b, 0, 0).base == 0 && SheetLift(b, 1, 0).base == 0);
    utassert(SheetLift(c, 0.5f, 0).base == 0 && SheetLift(c, 0.5f, 0).radius == 0);
}

static void BundleEdgesTest() {
    Sheets b = SheetsOf(TurnKind::Bundle, 38, 19, 500);
    float bottomStarts = b.lag * 13;
    // the start side waits until the bottom sheet lifts, then shrinks over the
    // first 12% of its lift (eased: sheet progress 0.157 is 6% of the way)
    utassert(EdgesDepart(b, bottomStarts) == 0);
    float midway = bottomStarts + 0.157f * (1 - bottomStarts);
    utassert(EdgesDepart(b, midway) > 0.3f && EdgesDepart(b, midway) < 0.7f);
    utassert(EdgesDepart(b, 0.5f) == 1);
    // a Curl's sheet starts at once
    Sheets c = SheetsOf(TurnKind::Curl, 1, 1, 500);
    utassert(EdgesDepart(c, 0.157f) > 0.3f && EdgesDepart(c, 0.157f) < 0.7f);

    // the end side grows over the last 18% of the turn
    utassert(EdgesArrive(0.82f) == 0 && EdgesArrive(1) == 1);
    utassert(EdgesArrive(0.91f) > 0.3f && EdgesArrive(0.91f) < 0.7f);

    // still to come, px: heading on, from 20 px to 2 px (start side) or 5 to 23 (end)
    utassert(Close(EdgesOffTarget(20, 2, 0, false), 18));
    utassert(Close(EdgesOffTarget(20, 2, 1, false), 0));
    utassert(Close(EdgesOffTarget(5, 23, 0.5f, false), -9));
    // heading back, the target is where it came from
    utassert(Close(EdgesOffTarget(20, 2, 0, true), 0));
    utassert(Close(EdgesOffTarget(20, 2, 1, true), -18));
}

static bool SameRange(PageRange r, int lo, int hi) {
    return r.lo == lo && r.hi == hi;
}

static void TurnedPagesTest() {
    utassert(SameRange(TurnedPages({2, 3}, {6, 7}), 3, 5));
    utassert(SameRange(TurnedPages({6, 7}, {2, 3}), 3, 5));
    utassert(SameRange(TurnedPages({3, 3}, {4, 4}), 3, 3));
    utassert(SameRange(TurnedPages({5, 5}, {3, 3}), 3, 4));
    utassert(SameRange(TurnedPages({1, 1}, {2, 3}), 1, 1));
    PageRange none = TurnedPages({}, {});
    utassert(none.lo > none.hi);

    utassert(Overlaps({3, 3}, {3, 5}) && Overlaps({3, 5}, {5, 9}));
    utassert(!Overlaps({3, 3}, {5, 5}) && !Overlaps({3, 3}, {4, 4}));
    utassert(!Overlaps(none, {0, 9}) && !Overlaps({0, 9}, none));
}

static void AcrossTest() {
    Spread a{1, 1};
    Spread b{2, 3};
    Spread c{4, 5};
    Spread z{40, 41};
    Landed landed;
    Landed across;
    TurnClock k(700);
    int ab = k.Start(0, TurnKind::Curl, a, b, Sheets(), &landed, &across);
    int bc = k.Start(100, TurnKind::Curl, b, c, Sheets(), &landed, &across);
    utassert(across.n == 0 && k.InAir() == 2);

    // sent back, then End: the jump crosses its page, which lands at once
    utassert(k.Reverse(200, c, b, &landed) == bc);
    Sheets s = SheetsOf(TurnKind::Bundle, 38, 19, 0);
    int bz = k.Start(250, TurnKind::Bundle, b, z, s, &landed, &across);
    utassert(across.n == 1 && across.ids[0] == bc);
    utassert(k.InAir() == 2 && k.Progress(ab) > 0 && k.Progress(bz) == 0);

    // turns beside each other don't cross
    TurnClock m(700);
    m.Start(0, TurnKind::Curl, b, c, Sheets(), &landed, &across);
    m.Start(50, TurnKind::Bundle, c, z, s, &landed, &across);
    utassert(across.n == 0 && m.InAir() == 2);
}

static void SplitTest() {
    Spread a{2, 3};
    Spread b{4, 5};
    Spread c{6, 7};
    Spread x{36, 37};
    Spread y{38, 39};
    Spread z{40, 41};
    Landed landed;
    Sheets s = SheetsOf(TurnKind::Bundle, 38, 19, 0);

    // a back tap splits off the Bundle's last sheet, which goes back from where it is
    TurnClock k(700);
    int bundle = k.Start(0, TurnKind::Bundle, a, z, s, &landed, nullptr);
    int id = 0;
    int one = k.Split(455, z, y, &landed, &id);
    utassert(one != 0 && id == bundle && k.InAir() == 2 && k.IsBack(one));
    utassert(Close(k.Progress(one), 1 - SheetT(s, 0.5f, 13)));
    // the next back tap splits off the next sheet
    int two = k.Split(500, y, x, &landed, &id);
    utassert(two != 0 && two != one && id == bundle && k.InAir() == 3);
    utassert(Close(k.Progress(two), 1 - SheetT(s, 500.f / 910, 12)));
    // a forward tap sends the newest split sheet on again
    utassert(k.Reverse(520, x, y, &landed) == two && !k.IsBack(two));

    // not a split: a tap from elsewhere, the same way, or back to its start
    TurnClock m(700);
    m.Start(0, TurnKind::Bundle, a, c, s, &landed, nullptr);
    utassert(m.Split(100, b, a, &landed, &id) == 0);
    utassert(m.Split(100, c, Spread{8, 9}, &landed, &id) == 0);
    TurnClock n(700);
    n.Start(0, TurnKind::Bundle, a, c, s, &landed, nullptr);
    utassert(n.Split(100, c, b, &landed, &id) != 0);
    utassert(n.Split(150, b, a, &landed, &id) == 0);
    // a Curl isn't split
    TurnClock p(700);
    p.Start(0, TurnKind::Curl, b, c, Sheets(), &landed, nullptr);
    utassert(p.Split(100, c, b, &landed, &id) == 0);
}

static void RenumberedTest() {
    Fingerprint p;
    p.dm = 1;
    p.displayMode = 2;
    p.layoutGeneration = 5;
    p.pageCount = 100;
    Fingerprint q = p;
    utassert(!OnlyRenumbered(q, p));
    q.layoutGeneration = 6;
    q.pageCount = 120;
    utassert(OnlyRenumbered(q, p));
    q.r2l = true;
    utassert(!OnlyRenumbered(q, p));
    q = p;
    q.pageCount = 120;
    q.dm = 3;
    utassert(!OnlyRenumbered(q, p));
}

void PageTurnLogic_UnitTests() {
    BundleSheetsTest();
    BundleEdgesTest();
    TurnedPagesTest();
    AcrossTest();
    SplitTest();
    RenumberedTest();
    SpreadOfTest();
    ClassifyTest();
    FingerprintTest();
    GateTest();
    NamesTest();
    SizesTest();
    RoomTest();
    RestingEdgesTest();
    EdgeLineTest();
    LengthTest();
    FastTapTest();
    EaseBackTest();
    InAirTest();
    SendBackTest();
}

#endif

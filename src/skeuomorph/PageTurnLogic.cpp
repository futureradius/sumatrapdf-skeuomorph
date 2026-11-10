/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "skeuomorph/PageTurnLogic.h"

namespace pageturn {

// float sums like 7 x (100 / 700) fall a hair short of 1
constexpr float kLandEps = 1e-4f;
// 0.70 x 10 pages must stay 7 px, not 6.9999
constexpr float kPxEps = 1e-3f;

bool Fingerprint::operator==(const Fingerprint& o) const {
    return dm == o.dm && displayMode == o.displayMode && facingStepOne == o.facingStepOne &&
           inPresentation == o.inPresentation && r2l == o.r2l && layoutGeneration == o.layoutGeneration &&
           pageCount == o.pageCount;
}

bool Fingerprint::operator!=(const Fingerprint& o) const {
    return !(*this == o);
}

// for the per-turn log: "nothing (<name>)"
Str SkipName(Skip why) {
    switch (why) {
        case Skip::None:
            return StrL("");
        case Skip::SameSpread:
            return StrL("same Spread");
        case Skip::SwitchOff:
            return StrL("switch off");
        case Skip::Continuous:
            return StrL("continuous view");
        case Skip::Minimized:
            return StrL("minimized");
        case Skip::Opening:
            return StrL("opening");
        case Skip::DisplayModeSwitch:
            return StrL("display mode switch");
        case Skip::StepOnePage:
            return StrL("step one page");
        case Skip::MangaToggle:
            return StrL("Manga Mode toggle");
        case Skip::EpubRenumbering:
            return StrL("EPUB renumbering");
        case Skip::Relayout:
            return StrL("relayout");
        case Skip::NoLayer:
            return StrL("no layer");
    }
    return StrL("?");
}

Str TurnKindName(TurnKind kind) {
    switch (kind) {
        case TurnKind::Nothing:
            return StrL("nothing");
        case TurnKind::Curl:
            return StrL("Curl");
        case TurnKind::Bundle:
            return StrL("Bundle");
    }
    return StrL("?");
}

// the Spread holding pageNo in a regular layout (DisplayModel's
// FirstPageInARowNo); Book View's lone cover is {1, 1}. Prefer
// dm->FirstPageInRow / LastPageInRow, which also know landscape spreads
Spread SpreadOf(int pageNo, int pageCount, Layout layout) {
    if (pageCount <= 0) {
        return {};
    }
    pageNo = ClampI(pageNo, 1, pageCount);
    if (layout == Layout::Single) {
        return {pageNo, pageNo};
    }
    if (layout == Layout::Facing) {
        int first = pageNo - ((pageNo - 1) % 2);
        return {first, std::min(first + 1, pageCount)};
    }
    if (pageNo == 1) {
        return {1, 1};
    }
    int first = pageNo - (pageNo % 2);
    return {first, std::min(first + 1, pageCount)};
}

// which fingerprint change explains a turn not to animate; None if unchanged
Skip FingerprintSkip(const Fingerprint& atHook, const Fingerprint& atPaint) {
    if (atHook == atPaint) {
        return Skip::None;
    }
    if (atPaint.dm == 0 || atHook.dm != atPaint.dm) {
        return Skip::Opening;
    }
    if (atHook.displayMode != atPaint.displayMode || atHook.inPresentation != atPaint.inPresentation) {
        return Skip::DisplayModeSwitch;
    }
    if (atHook.facingStepOne != atPaint.facingStepOne) {
        return Skip::StepOnePage;
    }
    if (atHook.r2l != atPaint.r2l) {
        return Skip::MangaToggle;
    }
    if (atHook.pageCount != atPaint.pageCount) {
        return Skip::EpubRenumbering;
    }
    return Skip::Relayout;
}

// spec 5.3 "don't animate"; the missing layer comes last so the log still says
// why a turn would be skipped while Curls can't play
Skip CheckGate(const TurnGate& gate) {
    if (!gate.switchOn) {
        return Skip::SwitchOff;
    }
    if (!gate.pagedView) {
        return Skip::Continuous;
    }
    if (gate.hidden) {
        return Skip::Minimized;
    }
    Skip fp = FingerprintSkip(gate.atHook, gate.atPaint);
    if (fp != Skip::None) {
        return fp;
    }
    if (gate.syncing) {
        return Skip::EpubRenumbering;
    }
    if (gate.stepsOnePage) {
        return Skip::StepOnePage;
    }
    if (!gate.layerLoaded) {
        return Skip::NoLayer;
    }
    return Skip::None;
}

static Turn NoTurn(Skip why) {
    Turn t;
    t.why = why;
    return t;
}

// Spreads tile the pages in order, so the next Spread starts right after the
// old one ends, whatever the layout (lone cover, lone last page, wide pages)
Turn ClassifyTurn(Spread from, Spread to, Reading reading) {
    if (from.first <= 0 || to.first <= 0) {
        return NoTurn(Skip::Opening);
    }
    if (from.first == to.first) {
        return NoTurn(Skip::SameSpread);
    }
    Turn t;
    t.forward = to.first > from.first;
    bool next = t.forward ? to.first == from.last + 1 : to.last == from.first - 1;
    t.kind = next ? TurnKind::Curl : TurnKind::Bundle;
    t.pages = abs(to.first - from.first);
    bool r2l = reading == Reading::RightToLeft;
    t.sweep = (t.forward != r2l) ? Sweep::ToLeft : Sweep::ToRight;
    return t;
}

Turn DecideTurn(Skip gate, Spread from, Spread to, Reading reading) {
    if (gate != Skip::None) {
        return NoTurn(gate);
    }
    return ClassifyTurn(from, to, reading);
}

// whole pixels, rounded down: 20 pages = 10.5 -> 10 px, as in the spec
int WholePx(float px) {
    if (px <= 0) {
        return 0;
    }
    return (int)floorf(px + kPxEps);
}

// whole-book thickness, "grows slowly"
float BookPx(int pageCount) {
    if (pageCount <= 0) {
        return 0;
    }
    float px = sqrtf(pageCount * kBookPxPerPage);
    return limitValue(px, (float)kBookMinPx, (float)kBookMaxPx);
}

// px written at 96 dpi to the window's dpi, like WindowMargin: the
// prototype's 46 px were judged in Edge at 150%, i.e. 69 screen px
int ScaleByDpi(int px, int dpi) {
    if (dpi <= 0) {
        dpi = kBaseDpi;
    }
    return (px * dpi + kBaseDpi / 2) / kBaseDpi;
}

EdgesPx SplitEdges(int pageCount, Spread at, Reading reading) {
    return SplitEdges(pageCount, at, reading, WholePx(BookPx(pageCount)));
}

// the whole book split by share of pages off the Spread: read before it,
// unread after it. Sides sum to the whole; a side with pages gets at least
// 1 px. Nothing when every page is on screen
EdgesPx SplitEdges(int pageCount, Spread at, Reading reading, int whole) {
    EdgesPx e;
    int nRead = std::max(at.first - 1, 0);
    int nUnread = std::max(pageCount - at.last, 0);
    int off = nRead + nUnread;
    if (at.first <= 0 || off <= 0 || whole <= 0) {
        return e;
    }
    int read = (int)lroundf((float)whole * nRead / off);
    int lo = nRead > 0 ? 1 : 0;
    int hi = nUnread > 0 ? whole - 1 : whole;
    e.read = ClampI(read, lo, hi);
    e.unread = whole - e.read;
    bool r2l = reading == Reading::RightToLeft;
    e.left = r2l ? e.unread : e.read;
    e.right = r2l ? e.read : e.unread;
    return e;
}

// what SumatraPDF paints at rest. Single Page: read pages just fade away,
// only the unread side shows (right, left in Right-to-Left Reading)
EdgesPx RestingEdges(int pageCount, Spread at, Layout layout, Reading reading, int wholePx) {
    EdgesPx e = SplitEdges(pageCount, at, reading, wholePx);
    if (layout != Layout::Single) {
        return e;
    }
    bool r2l = reading == Reading::RightToLeft;
    e.read = 0;
    e.left = r2l ? e.unread : 0;
    e.right = r2l ? 0 : e.unread;
    return e;
}

// room on each side of the Spread (96 dpi): the whole book, since on the last
// page all paper is on one side. None when every page is always on screen
// (1 page; Facing with 2), as there are never Page Edges to show
int RoomPx(int pageCount, Layout layout) {
    if (pageCount <= 1 || (layout == Layout::Facing && pageCount <= 2)) {
        return 0;
    }
    return WholePx(BookPx(pageCount));
}

// sets the room in the margin to px. If the margin isn't what was left there
// last time, SetUiDpi or Presentation reset it and it's the new base
Room ApplyRoom(Room last, int curLeft, int curRight, int px) {
    int baseLeft = curLeft;
    int baseRight = curRight;
    if (curLeft == last.left && curRight == last.right) {
        baseLeft -= last.added;
        baseRight -= last.added;
    }
    return {px, baseLeft + px, baseRight + px};
}

// col 0 touches the page. Fine lines every 1.6 px (scaled to dpi) at
// col = round(k x gap), as in the prototype; the outermost col is the rim
//   page |..||.||.|.||.||  <- rim
EdgeLine EdgeLineAt(int col, int thickPx, int dpi) {
    if (col < 0 || col >= thickPx) {
        return EdgeLine::None;
    }
    if (col == thickPx - 1) {
        return EdgeLine::Rim;
    }
    float gap = kEdgeLineGap * (dpi > 0 ? dpi : kBaseDpi) / kBaseDpi;
    for (int k = 1; k * gap < col + 1; k++) {
        if (lroundf(k * gap) == col) {
            return EdgeLine::Fine;
        }
    }
    return EdgeLine::None;
}

float BundlePx(int pages) {
    if (pages <= 0) {
        return 0;
    }
    return std::min(pages * kBundlePxPerPage, kBundleMaxPx);
}

// about 1 sheet per 1.1 px, rounded to nearest
int BundleSheets(float bundlePx) {
    int n = (int)lroundf(bundlePx / kPxPerSheet);
    return ClampI(n, kMinSheets, kMaxSheets);
}

static void AddLanded(Landed* landed, int id) {
    if (!landed || landed->n >= (int)dimof(landed->ids)) {
        return;
    }
    landed->ids[landed->n++] = id;
}

static void ResetLanded(Landed* landed) {
    if (landed) {
        landed->n = 0;
    }
}

TurnClock::TurnClock(int durationMs) : durationMs(durationMs) {}

void TurnClock::SetDuration(int ms) {
    durationMs = ms;
}

// normal length, or the fast-tapping one. Easing back: hold the fast length
// while another tap could still count as fast, then return linearly to normal
// over one normal Curl length
int TurnClock::CurlMs(i64 nowMs) const {
    if (fastMs == 0) {
        return durationMs;
    }
    int fast = std::min(fastMs, durationMs);
    i64 easing = nowMs - fastAtMs - fast;
    if (easing <= 0) {
        return fast;
    }
    if (easing >= durationMs) {
        return durationMs;
    }
    return fast + (int)((i64)(durationMs - fast) * easing / durationMs);
}

int TurnClock::LengthMs(TurnKind kind, i64 nowMs) const {
    if (kind == TurnKind::Curl) {
        return CurlMs(nowMs);
    }
    if (kind == TurnKind::Bundle) {
        return (int)lroundf(CurlMs(nowMs) * kBundleLenFactor);
    }
    return 0;
}

// a tap quicker than the current Curl sets every Curl to gap x 4, never below
// 310 ms nor above the normal length (so below 310 the normal length stays)
void TurnClock::Tap(i64 nowMs) {
    i64 gap = nowMs - lastTapMs;
    if (tapped && gap >= 0 && gap < CurlMs(nowMs)) {
        int fast = std::max(kFastMinMs, (int)gap * kFastGapFactor);
        fastMs = std::min(fast, durationMs);
        fastAtMs = nowMs;
    }
    tapped = true;
    lastTapMs = nowMs;
}

void TurnClock::LandFirst(Landed* landed) {
    if (nAir == 0) {
        return;
    }
    AddLanded(landed, air[0].id);
    for (int i = 1; i < nAir; i++) {
        air[i - 1] = air[i];
    }
    nAir--;
}

// moves every turn in the air on to nowMs at the current lengths, so a turn
// sped up mid-air keeps its progress; landed gets the ones that finished
void TurnClock::Advance(i64 nowMs, Landed* landed) {
    ResetLanded(landed);
    i64 dt = nowMs - advancedMs;
    if (dt <= 0) {
        return;
    }
    advancedMs = nowMs;
    int kept = 0;
    for (int i = 0; i < nAir; i++) {
        Flight f = air[i];
        f.progress += (float)dt / std::max(LengthMs(f.kind, nowMs), 1);
        if (f.progress >= 1 - kLandEps) {
            AddLanded(landed, f.id);
            continue;
        }
        air[kept++] = f;
    }
    nAir = kept;
}

// a new Curl or Bundle (both count as a tap); past 5 in the air the oldest
// lands at once. Returns the new turn's id, 0 for Nothing
int TurnClock::Start(i64 nowMs, TurnKind kind, Landed* landed) {
    Advance(nowMs, landed);
    if (kind == TurnKind::Nothing) {
        return 0;
    }
    Tap(nowMs);
    if (nAir == kMaxInAir) {
        LandFirst(landed);
    }
    Flight f;
    f.id = nextId++;
    f.kind = kind;
    air[nAir++] = f;
    return f.id;
}

void TurnClock::LandAll(Landed* landed) {
    ResetLanded(landed);
    while (nAir > 0) {
        LandFirst(landed);
    }
}

// 0..1, or -1 once landed
float TurnClock::Progress(int id) const {
    for (int i = 0; i < nAir; i++) {
        if (air[i].id == id) {
            return air[i].progress;
        }
    }
    return -1;
}

int TurnClock::InAir() const {
    return nAir;
}

} // namespace pageturn

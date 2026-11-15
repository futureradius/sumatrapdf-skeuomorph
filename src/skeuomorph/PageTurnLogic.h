/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Page Turn decisions: plain values in, no Windows, settings or DisplayModel
namespace pageturn {

constexpr int kFastMinMs = 310;
constexpr int kFastGapFactor = 4;
constexpr int kMaxInAir = 5;
constexpr float kBundleLenFactor = 1.3f;

constexpr float kBookPxPerPage = 0.12f * 46;
constexpr int kBookMinPx = 4;
constexpr int kBookMaxPx = 46;
constexpr float kBundlePxPerPage = 0.70f;
constexpr float kBundleMaxPx = 18;
constexpr float kPxPerSheet = 1.1f;
constexpr int kMinSheets = 2;
constexpr int kMaxSheets = 14;
// fine Page Edge lines every 1.6 px at 96 dpi, as in the prototype
constexpr float kEdgeLineGap = 1.6f;
constexpr int kBaseDpi = 96;

// Bundles (spec 3.5), as in the prototype
constexpr float kBundleFan = 0.2f;
constexpr float kBundleStiff = 0.4f;
constexpr float kStiffFullPx = 6;
constexpr float kStiffRadius = 1.6f;
// Page Edges: the start side shrinks over the first 12% of the bottom sheet's
// lift, the end side grows over the last 18% of the turn
constexpr float kDepartFrom = 0.01f;
constexpr float kDepartTo = 0.12f;
constexpr float kArriveFrom = 0.82f;

// pages on screen together; Single Page: first == last
struct Spread {
    int first = 0;
    int last = 0;
};

enum class Layout {
    Single,
    Facing,
    Book
};
enum class Reading {
    LeftToRight,
    RightToLeft
};
enum class TurnKind {
    Nothing,
    Curl,
    Bundle
};
// where the turning paper moves on screen
enum class Sweep {
    None,
    ToLeft,
    ToRight
};

// why a page change is not animated; None = animate
enum class Skip {
    None,
    SameSpread,
    SwitchOff,
    Continuous,
    Minimized,
    Opening,
    DisplayModeSwitch,
    StepOnePage,
    MangaToggle,
    EpubRenumbering,
    Relayout,
    NoLayer,
};

// per-window state that must not change between the last paint and a turn
struct Fingerprint {
    uintptr_t dm = 0; // 0: nothing painted yet, or the document closed
    int displayMode = 0;
    bool facingStepOne = false;
    bool inPresentation = false;
    bool r2l = false;
    int layoutGeneration = 0;
    int pageCount = 0;

    bool operator==(const Fingerprint& o) const;
    bool operator!=(const Fingerprint& o) const;
};

// everything the "don't animate" check needs, read at the start-of-turn hook
struct TurnGate {
    bool switchOn = false;
    bool pagedView = false;
    bool layerLoaded = false;
    bool syncing = false; // dm->syncingWithEngineLayout
    bool stepsOnePage = false;
    bool hidden = false; // iconic or hidden window
    Fingerprint atHook;
    Fingerprint atPaint;
};

struct Turn {
    TurnKind kind = TurnKind::Nothing;
    Skip why = Skip::None; // set when kind is Nothing
    bool forward = false;
    Sweep sweep = Sweep::None;
    int pages = 0; // pages turned over
};

Str SkipName(Skip why);
Str TurnKindName(TurnKind kind);

Spread SpreadOf(int pageNo, int pageCount, Layout layout);
Skip FingerprintSkip(const Fingerprint& atHook, const Fingerprint& atPaint);
bool OnlyRenumbered(const Fingerprint& atHook, const Fingerprint& atPaint);
Skip CheckGate(const TurnGate& gate);
Turn ClassifyTurn(Spread from, Spread to, Reading reading);
Turn DecideTurn(Skip gate, Spread from, Spread to, Reading reading);

struct EdgesPx {
    int read = 0;
    int unread = 0;
    int left = 0;
    int right = 0;
};

// what hook 4 left in windowMargin.left / .right
struct Room {
    int added = 0;
    int left = 0;
    int right = 0;
};

// one 1 px column of resting Page Edges
enum class EdgeLine {
    None,
    Fine,
    Rim
};

int WholePx(float px);
float BookPx(int pageCount);
int ScaleByDpi(int px, int dpi);
EdgesPx SplitEdges(int pageCount, Spread at, Reading reading);
EdgesPx SplitEdges(int pageCount, Spread at, Reading reading, int wholePx);
EdgesPx RestingEdges(int pageCount, Spread at, Layout layout, Reading reading, int wholePx);
int RoomPx(int pageCount, Layout layout);
Room ApplyRoom(Room last, int curLeft, int curRight, int px);
EdgeLine EdgeLineAt(int col, int thickPx, int dpi);
float BundlePx(int pages);
int BundleSheets(float bundlePx);

// the sheets one Page Turn draws: a Curl is one plain sheet
struct Sheets {
    int n = 1;
    float px = 0;    // the Bundle's thickness
    float lag = 0;   // each sheet starts this share of the turn after the one above
    float stiff = 0; // 0 .. kBundleStiff
    float gap = 0;   // px between neighbouring sheets while lifted
    int leaves = 1;  // leaves turned over: a back tap turns n / leaves sheets back
};

// how high a sheet lies over the page while lifted, and how much wider it
// wraps, px: the sheets of a Bundle share one cylinder axis
struct Lift {
    float base = 0;
    float radius = 0;
};

// pages turned over, by their front page: Facing {2,3} -> {6,7} is 3..5
struct PageRange {
    int lo = 0;
    int hi = -1; // lo > hi: none
};

float EaseInOut(float t);
float Smooth(float a, float b, float x);
Sheets SheetsOf(TurnKind kind, int pages, int leaves, float pageWidthPx);
float SheetT(const Sheets& s, float t, int fromTop);
Lift SheetLift(const Sheets& s, float p, int fromTop);
float RadiusFactor(const Sheets& s);
float LeadFactor(const Sheets& s);
float EdgesDepart(const Sheets& s, float t);
float EdgesArrive(float t);
float EdgesOffTarget(int fromPx, int toPx, float done, bool back);
int SheetsBack(const Sheets& s, int peeled);
PageRange TurnedPages(Spread from, Spread to);
bool Overlaps(PageRange a, PageRange b);

// ids of turns that land, oldest first
struct Landed {
    int ids[kMaxInAir + 1]{};
    int n = 0;
};

// Curl and Bundle lengths at a moment, with fast tapping and easing back. A
// plain value: the drawing thread keeps a copy to speed up Curls in the air
struct Pace {
    int durationMs = 0;
    int fastMs = 0; // 0: not fast tapping
    i64 fastAtMs = 0;

    int CurlMs(i64 nowMs) const;
    int LengthMs(TurnKind kind, i64 nowMs) const;
};

// Curl and Bundle lengths with fast tapping, and the turns in the air
class TurnClock {
  public:
    explicit TurnClock(int durationMs);
    void SetDuration(int durationMs);
    int Start(i64 nowMs, TurnKind kind, Landed* landed);
    int Start(i64 nowMs, TurnKind kind, Spread from, Spread to, Landed* landed);
    int Start(i64 nowMs, TurnKind kind, Spread from, Spread to, Sheets sheets, Landed* landed, Landed* across);
    int Reverse(i64 nowMs, Spread from, Spread to, Landed* landed);
    int Split(i64 nowMs, Spread from, Spread to, Landed* landed);
    int Peeled(int id) const;
    void Advance(i64 nowMs, Landed* landed);
    void LandAll(Landed* landed);
    int CurlMs(i64 nowMs) const;
    int LengthMs(TurnKind kind, i64 nowMs) const;
    Pace PaceNow() const;
    float Progress(int id) const;
    bool IsBack(int id) const;
    int InAir() const;

  private:
    struct Flight {
        int id = 0;
        TurnKind kind = TurnKind::Nothing;
        float progress = 0;
        Spread from; // where it heads from and to: swapped when sent back
        Spread to;
        bool back = false;
        Sheets sheets;
        Spread dest;       // where it first headed
        int peeled = 0;    // leaves turned back by taps the other way
        i64 holdUntil = 0; // turned-back sheets are home by then
    };
    void Tap(i64 nowMs);
    void LandFirst(Landed* landed);
    void LandAt(int i, Landed* landed);

    Flight air[kMaxInAir];
    int nAir = 0;
    int nextId = 1;
    Pace pace;
    bool tapped = false;
    i64 lastTapMs = 0;
    i64 advancedMs = 0;
};

} // namespace pageturn

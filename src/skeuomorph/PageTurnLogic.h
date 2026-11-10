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
Skip CheckGate(const TurnGate& gate);
Turn ClassifyTurn(Spread from, Spread to, Reading reading);
Turn DecideTurn(Skip gate, Spread from, Spread to, Reading reading);

struct EdgesPx {
    int read = 0;
    int unread = 0;
    int left = 0;
    int right = 0;
};

int WholePx(float px);
float BookPx(int pageCount);
EdgesPx SplitEdges(int pageCount, Spread at, Reading reading);
float BundlePx(int pages);
int BundleSheets(float bundlePx);

// ids of turns that land, oldest first
struct Landed {
    int ids[kMaxInAir + 1]{};
    int n = 0;
};

// Curl and Bundle lengths with fast tapping, and the turns in the air
class TurnClock {
  public:
    explicit TurnClock(int durationMs);
    void SetDuration(int durationMs);
    int Start(i64 nowMs, TurnKind kind, Landed* landed);
    void Advance(i64 nowMs, Landed* landed);
    void LandAll(Landed* landed);
    int CurlMs(i64 nowMs) const;
    int LengthMs(TurnKind kind, i64 nowMs) const;
    float Progress(int id) const;
    int InAir() const;

  private:
    struct Flight {
        int id = 0;
        TurnKind kind = TurnKind::Nothing;
        float progress = 0;
    };
    void Tap(i64 nowMs);
    void LandFirst(Landed* landed);

    Flight air[kMaxInAir];
    int nAir = 0;
    int nextId = 1;
    int durationMs = 0;
    bool tapped = false;
    i64 lastTapMs = 0;
    int fastMs = 0; // 0: not fast tapping
    i64 fastAtMs = 0;
    i64 advancedMs = 0;
};

} // namespace pageturn

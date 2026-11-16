/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Curls: one drawing thread per window owns Direct3D, the Curl layer and every
// Present. Up to 5 Curls are in the air at once. Everything here is called on
// the UI thread; calls only queue commands, except the two short waits of the
// no-flash hand-off (ADR 0005):
//
//   hook 3, gate passed   CurlCopyOld      old Spread -> GPU picture (waits ~1 ms)
//   FinishTurn, a Curl    CurlStart        a new Curl, or one in the air sent
//                                          back; waits until the frame showing
//                                          it is on screen (~10 ms), then
//                                          SumatraPDF repaints
//   hook 6, flushed       CurlWindowShowsNew  copies the landing page; the
//                                          uncovered parts go see-through one
//                                          refresh later, then the layer goes
//
// If the layer can't load, CurlIsLoaded stays false and Page Turns are
// instant jumps. Include PageTurnLogic.h first.
struct MainWindow;
struct DisplayModel;

// a Spread as on screen, canvas pixels. Single Page uses left only; a lone
// page in Facing or Book View sits on its side of the spine; a wide page
// alone across both columns is split at its middle
struct CurlSpread {
    Rect left;
    Rect right;
    float zoom = 0;    // screen px per page unit: pages of different sizes keep their real sizes
    float spineX = 0;  // Facing and Book View: the spine, canvas px
    int edgesLeft = 0; // Page Edges thickness beside the left page, screen px
    int edgesRight = 0;
    int pages[2] = {};  // page number on the left and right; 0: none
    bool sharp[2] = {}; // SumatraPDF had it drawn sharp when the screen was copied
};

enum class CurlShape {
    Single,
    Spread
};

struct CurlTurn {
    int clockId = 0;   // TurnClock's id
    bool back = false; // the Curl clockId is sent back (or on again): no new Curl
    pageturn::TurnKind kind = pageturn::TurnKind::Curl;
    pageturn::Sheets sheets;
    int firstPage = 0;          // the first page it turns over: orders sheets as in a book
    pageturn::Spread fromPages; // page numbers: what lies under the sheets, whose copy shows its landing
    pageturn::Spread toPages;
    int splitOf = 0;         // a tap the other way turns back leaves of Bundle splitOf: no new Curl
    int peeled = 0;          // its leaves turned back so far
    pageturn::Landed across; // turns in the air whose pages this one crosses: they land at once
    int dpi = 0;
    pageturn::Pace pace;
    CurlShape shape = CurlShape::Spread;
    pageturn::Sweep sweep = pageturn::Sweep::ToLeft;
    bool forward = true;
    int durationMs = 0;
    CurlSpread to;   // the new Spread, as laid out now
    Color paper = 0; // blank paper: the theme's page colour
    int edgesPx = 0; // room for Page Edges beside the Spread
};

// times of a Curl, ms on CurlNowMs()'s clock; 0 = didn't happen. A Curl sent
// back starts new times under a new id
struct CurlTimes {
    int id = 0;
    double start = 0;      // CurlStart
    double coverShown = 0; // first frame presented
    double covered = 0;    // first frame on screen
    double uiWaitMs = 0;   // CurlStart held SumatraPDF's repaint this long
    double showsNew = 0;   // SumatraPDF flushed the new Spread
    double seeThrough = 0;
    double landed = 0; // the Curl's own end (t = 1) presented
    double gone = 0;   // layer removed
    int frames = 0;
    int lateFrames = 0; // gaps over 1.5 refreshes
    bool timedOut = false;
    bool landedAtOnce = false;
};

double CurlNowMs();

void CurlEnsure(MainWindow* win);
bool CurlIsLoaded(MainWindow* win);
bool CurlInAir(MainWindow* win);

void CurlCopyOld(MainWindow* win, CurlSpread from);
void CurlStart(MainWindow* win, const CurlTurn& turn);
void CurlWindowShowsNew(MainWindow* win, CurlSpread now);
void CurlRough(MainWindow* win, DisplayModel* dm, const CurlSpread& shown);
bool CurlRoughPaint(MainWindow* win, HDC hdc, int pageNo, Rect bounds);

// why: a short reason for the debug log, e.g. "resize"; a string literal
void CurlLandAll(MainWindow* win, const char* why);
void CurlLandAllWindows(const char* why);
void CurlResize(MainWindow* win);
void CurlDestroy(MainWindow* win);

bool CurlLastTimes(MainWindow* win, CurlTimes* out);
bool CurlTimesOf(MainWindow* win, int id, CurlTimes* out);

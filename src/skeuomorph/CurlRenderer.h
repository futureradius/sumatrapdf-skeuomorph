/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Curls: one drawing thread per window owns Direct3D, the Curl layer and every
// Present. Everything here is called on the UI thread; calls only queue
// commands, except the two short waits of the no-flash hand-off (ADR 0005):
//
//   hook 3, gate passed   CurlCopyOld      old Spread -> GPU picture (waits ~1 ms)
//   FinishTurn, a Curl    CurlStart        waits until the first frame is on
//                                          screen (~10 ms), then SumatraPDF repaints
//   hook 6, flushed       CurlWindowShowsNew  copies the landing page; the
//                                          uncovered parts go see-through one
//                                          refresh later, then the layer goes
//
// If the layer can't load, CurlIsLoaded stays false and Page Turns are
// instant jumps. Include PageTurnLogic.h first.
struct MainWindow;

// a Spread as on screen, canvas pixels. Single Page uses left only; a lone
// page in Facing or Book View sits on its side of the spine; a wide page
// alone across both columns is split at its middle
struct CurlSpread {
    Rect left;
    Rect right;
    float zoom = 0;    // screen px per page unit: pages of different sizes keep their real sizes
    int edgesLeft = 0; // Page Edges thickness beside the left page, screen px
    int edgesRight = 0;
};

enum class CurlShape {
    Single,
    Spread
};

struct CurlTurn {
    CurlShape shape = CurlShape::Spread;
    pageturn::Sweep sweep = pageturn::Sweep::ToLeft;
    bool forward = true;
    int durationMs = 0;
    CurlSpread to;   // the new Spread, as laid out now
    Color paper = 0; // blank paper: the theme's page colour
    int edgesPx = 0; // room for Page Edges beside the Spread
};

// times of the last Curl, ms on CurlNowMs()'s clock; 0 = didn't happen
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

// why: a short reason for the debug log, e.g. "resize"; a string literal
void CurlLandAll(MainWindow* win, const char* why);
void CurlLandAllWindows(const char* why);
void CurlResize(MainWindow* win);
void CurlDestroy(MainWindow* win);

bool CurlLastTimes(MainWindow* win, CurlTimes* out);

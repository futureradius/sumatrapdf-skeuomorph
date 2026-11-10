/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// window changes mid-Curl (spec 3.11): subclasses the canvas and the frame
struct MainWindow;

void WatchWindowChanges(MainWindow* win);

/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Screen copies: the only code that reads win->buffer, SumatraPDF's back buffer.
// GDI copies it straight into a GPU picture on the drawing thread while the UI
// thread waits, so the buffer's DC is never used by both at once:
//
//   UI thread                                drawing thread
//   ScreenCopyAsk(desk, win, area, slot)
//     hands over the buffer's DC, wakes -->  ScreenCopyServe(desk, dev, slots)
//     waits (short timeout)          <----     BitBlt into slots[slot]
//   the DC is SumatraPDF's again
//
// No pointer into win->buffer is kept: it's recreated on resize.
// Include CurlD3D.h first.
struct MainWindow;
struct ScreenCopyDesk;

ScreenCopyDesk* NewScreenCopyDesk(HANDLE wake);
void DeleteScreenCopyDesk(ScreenCopyDesk* desk);

// UI thread
Size ScreenCopySize(MainWindow* win);
bool ScreenCopyAsk(ScreenCopyDesk* desk, MainWindow* win, Rect area, int slot);

// drawing thread
void ScreenCopyServe(ScreenCopyDesk* desk, ID3D11Device* dev, CurlPicture* slots, int nSlots);

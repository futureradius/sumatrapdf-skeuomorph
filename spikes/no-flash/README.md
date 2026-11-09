# No-flash spike (throwaway, never merged)

Answers [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19).

A test build of SumatraPDF using the hook points from
[Where does the new code hook in...](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/17). On every Page
Turn it copies the old screen, lets SumatraPDF jump as it does today, shows the Curl layer (Direct3D 11 on
DirectComposition, `topmost=FALSE`), plays one plain 700 ms Curl drawn on its own thread, and removes the layer once
SumatraPDF really shows the new Spread. A recorder (Windows Desktop Duplication) saves every frame Windows puts on the
screen, and `analyze.py` checks each turn.

## Files

| File | What |
| --- | --- |
| `src/skeuomorph/SpikeNoFlash.cpp` | the spike: layer, drawing thread, screen copies, recorder, automatic test run |
| `src/skeuomorph/SpikeCurl.hlsl`, `SpikeCurlShaders.h` | the Curl shader and its precompiled bytes (`compile-shaders.ps1`, uses the Windows SDK's `fxc`) |
| `DocController.h`, `DisplayModel.cpp`, `SumatraPDF.cpp`, `Canvas.cpp`, `premake5.files.lua` | 7 hook lines (hooks #1, #2, #3, #6, #15 of #17) |
| `make-test-pdf.py` | 200 coloured, numbered pages; 190-195 take ~0.3 s to draw, 196-199 several seconds |
| `run-one.ps1` | builds nothing; runs one automatic test (about a minute, the window stays on top) |
| `analyze.py` | judges every recorded turn: flash, old Spread back, gap at removal |
| `results/` | logs (`<run>.txt`), every frame's numbers (`<run>-frames.csv`), example filmstrips |

## Run it

```powershell
MSBuild.exe vs2022\SumatraPDF.sln /t:SumatraPDF /p:Configuration=Debug /p:Platform=x64 /m
.\spikes\no-flash\run-one.ps1 B "layer=ondemand,wait=1,flush=1,copy=lock"; python .\spikes\no-flash\analyze.py -v
```

Options: `layer=ondemand|attached|none` (layer only during Curls, always attached and empty, or no layer: plain
SumatraPDF), `wait=1` (SumatraPDF's repaint waits until the covering frame is on screen), `flush=N` (refreshes the
layer waits after SumatraPDF shows the new Spread before turning see-through and going away), `copy=ui|lock`,
`rec=0|2` (no recorder, or record only a band across the middle), `lag=50` (test of the test: layer 50 ms late). Third
argument `max` maximizes the window.

Each run: 10 next-page turns, one with a menu open mid-Curl, one with a dialog, one with a text box over the page, and
3-5 far jumps (two of them to pages slow enough that SumatraPDF holds the old Spread 500 ms and then shows "Rendering
page N...").

## Results

Intel Iris Xe, 3840×2400 at 60 Hz, window 1638×1000 unless marked 4K.

| Setup | Turns | Flash | Old Spread back | Gap at removal | UI waits (median / max) |
| --- | --- | --- | --- | --- | --- |
| On demand, no wait (A, A2, E) | 48 | **4** (A2: 1, E: 3) | 0 | 0 | 0 |
| On demand, wait (B, B2, B3, B4, CTRL2, CTRL3) | 102 | 0 | 0 | 0 | 9-12 / 21 ms |
| Attached, no wait (C, C2) | 32 | 0 | 0 | 0 | 0 |
| Attached, wait (D) | 16 | 0 | 0 | 0 | 15 / 27 ms |
| Test of the test: layer 50 ms late (CTRL) | 15 | **13** | 0 | 0 | 0 |
| 4K, on demand, wait (M2, band recorded) | 18 | 0 | (2, recorder overloaded) | 0 | 14 / 35 ms |
| 4K, on demand, no wait (MA, band recorded) | 18 | **1** | (1, recorder overloaded) | 0 | 0 |

- **A flash is a race.** Without waiting, SumatraPDF's repaint reaches the screen ~6 ms after the tap, the Curl layer's
  first frame ~5-10 ms after it. When a screen refresh falls between the two, the new Spread shows for one frame and is
  then covered (`E-turn01.png`). The control (`CTRL-turn01.png`) shows the recorder catches it.
- **Waiting closes it.** The posted task (which runs before SumatraPDF's repaint) waits until the layer's covering frame
  is on screen. The layer reports that by its next frame signal (the swap chain's frame-latency handle), no extra wait.
  Median wait 12 ms, at most 21 ms (35 ms at 4K). Nobody sees it: the Curl covers SumatraPDF's late repaint.
- **No old Spread coming back, no gap when the layer goes away**, in every setup. The layer turns see-through only one
  refresh after SumatraPDF shows the new Spread, and its last frame is pixel-for-pixel what SumatraPDF shows.
  Making it see-through from the start (CTRL2, CTRL3) caused no visible error either, because the back of the lifting
  page covers most of the uncovered area early in a corner peel; the one-refresh rule costs nothing, so keep it.
- **Slow pages** (`B3-turn16.png` vs plain SumatraPDF `NONE-turn16.png`): plain SumatraPDF holds the old Spread 500 ms
  and then shows "Rendering page N...". With the layer, the Curl starts on the tap, the back of the page is blank paper
  until SumatraPDF shows something, and it lands on whatever SumatraPDF shows ("Rendering page N..." here).
- **Attached vs on demand:** attached never flashed without the wait (0/32), but nothing rules the race out there
  either, and attached means a layer over every Paged View all the time. On demand + wait costs nothing more and
  showing the layer adds no measurable time (cover on screen 12-20 ms median in both). On demand can also free the
  swap chain between Curls: creating it took 2-5 ms (not tested per Curl).
- **Menus and dialogs:** a menu and a message box opened mid-Curl; the Curl kept playing on the drawing thread,
  0 late frames, landed on time.
- **Text box above the Curl:** with `topmost=FALSE`, a child edit box on the page stays above the Curl
  (`B4-editbox-midcurl.png`).
- **Frame timing:** drawing paced by the swap chain's frame-latency handle, `Present(1, 0)`: 0 late frames in ~9,000
  Curl frames at 1638×1000. At 4K without the recorder (M3): 17 late of 772 (about one per Curl, probably the 4K
  screen copies on the drawing thread). The first version blocked the drawing thread with `DwmFlush` to learn when a
  frame was shown; that caused one hitch per Curl and was replaced by the frame signal.

### Other numbers

| | 1638×1000 | 4K (3840×2170) |
| --- | --- | --- |
| Direct3D + layer setup on the drawing thread, at the first Paged View paint | 25-40 ms (device 22-32, layer 2-5, shaders 0.2) | 25 ms |
| UI thread cost of starting it | 0.04-0.1 ms | 0.1 ms |
| Shaders: precompiled bytes vs compiling at startup | 0.2-0.3 ms vs 12-20 ms (+ 1 ms loading `d3dcompiler_47.dll`) | |
| Screen copy, GDI straight into a GDI-compatible GPU image on the drawing thread, UI thread waits (`lock`) | UI 1.0-1.7 ms, drawing thread 2.7-3 ms | UI 3.7-5.5 ms, drawing thread 7.4-9.6 ms |
| Screen copy to memory on the UI thread, then upload (`ui`) | UI 2.7-3.3 ms, upload 1.8-2.3 ms | UI 13-19 ms, upload 7 ms |
| Copying only the Spread instead of the whole window | within noise | ~20% less |
| Swap chain memory while a layer exists | 12.5 MB | 64 MB |

Recorder limits: at 1638×1000 it missed 0-5 frames per turn, almost all mid-Curl (not where a flash happens). At 4K
it can't keep up (300+ missed frames per run even recording a band), so the 4K runs measure timing, not flashes.

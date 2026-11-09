# Rough Page spike (throwaway, never merged)

Answers [How fast do Rough Pages arrive, and how much memory do Curls need?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/20).

A debug build of SumatraPDF with the 4 hook lines from `spike/no-flash` (`BeforeSpreadChange` in `GoToPage`,
`SpikeAfterPaint` after the canvas paint). Curls are **not drawn**. Only their timing is modelled, using the
decisions in #8 and #15: 700 ms; fast taps shorten every Curl in the air to gap × 4, never below 310 ms; at most 5 in
the air, after which the oldest lands at once. From that, each page gets a *visible window*:

- the front of a lifting page: from when it shows (revealed under the Curl before it, or the tap) until the Curl
  passes the middle;
- the back of a lifting page: from the tap until the next Curl lands on top of it;
- pages of the Spread landed on: until SumatraPDF draws them sharp.

A page has a picture once SumatraPDF has shown its Spread fully drawn (**sharp**; the real build takes a screen copy
then) or once the worker has finished its **Rough Page**. Rough Pages follow the pacing rule from #18: one worker
thread at below-normal priority, a background copy of the document (`Clone()`), one page at a time, oldest need first,
half width and half height, aborted when no longer needed, thrown away once off screen.

## Files

| File | What |
| --- | --- |
| `src/skeuomorph/SpikeRoughPages.cpp` | the spike: Curl timing model, Rough Page worker, GPU memory test, automatic run |
| `make-test-pdfs.ps1` | writes `scan.pdf` (160 pages, one 2480×3508 colour JPEG each, 203 MB) and `text.pdf` (300 text pages, 1.2 MB) |
| `run-one.ps1` | runs one automatic test and prints its result |
| `results/` | `<run>.txt` (summary) and `<run>-pages.csv` (every page's timing) |

## Run it

```powershell
MSBuild.exe vs2022\SumatraPDF.sln /t:SumatraPDF /p:Configuration=Debug /p:Platform=x64 /m
.\spikes\rough-pages\run-one.ps1 S-tap-1920 scan "mode=tap,rough=1,store=0,size=1920x1200,gpu=1"
```

Options: `mode=tap|hold` (a tap every 77 ms, or this PC's key repeat), `rough=0|1`, `store=0` (empty the copy's
MuPDF cache after each Rough Page), `size=WxH|max`, `bursts=N`, `taps=N`, `gpu=1` (also allocate the worst-case GPU
pictures and measure GPU memory). Each run: 3 bursts of 24 taps, each from a fresh part of the book, in Facing view.

`run-one.ps1 <label> <scan|text> <options> rel` runs the release build (`out\rel64`, built with
`/p:Configuration=Release`). `scale=0.28` makes Rough Pages smaller than half size.

## Results

Laptop: Intel i7-1260P (16 logical processors), Iris Xe, 3840×2400 screen. "1920" = canvas 1958×1200, page 842×1190;
"4K" = maximized, canvas 3840×2170, page 1528×2160. Each row: 3 bursts × 24 turns = 141 flipped-past pages.
**Showed** = flipped-past pages that had a picture (sharp or Rough) at some point while on screen.
**Covered** = share of their time on screen with a picture instead of blank paper.

### Release build, plugged in (`AC-R-*`)

| PDF | Window | Input | Showed | Covered | Of which sharp | Rough Page draw (median) | Landed Spread sharp after last tap |
| --- | --- | --- | --- | --- | --- | --- | --- |
| scan | 1920 | tap 77 ms | 100% | 99% | all | not needed | 3 ms |
| scan | 1920 | hold key | 100% | 77% | 17 | 18 ms | 112-134 ms |
| scan | 4K | tap 77 ms | 100% | 84% | 9 | 29 ms | 352-427 ms (without Rough Pages 389-471) |
| scan | 4K | tap, no Rough Pages | 6% | 6% | 9 | - | 389-471 ms |
| scan | 4K | hold key | 59% | 27% | 15 | 29 ms | 310-497 ms |
| scan | 4K | hold key, `scale=0.28` | 100% | 68% | 15 | 19 ms | 387-479 ms |
| text | 1920 | hold key | 100% | 99% | all | not needed | 3 ms |
| text | 4K | tap 77 ms | 100% | 96% | 92 | 11 ms | 11-13 ms |
| text | 4K | hold key | 100% | 95% | 87 | 10 ms | 11-12 ms |

Debug build, plugged in, scan 4K tap (`AC-D-S-tap-max`): 62% showed, Rough Page 54 ms. MuPDF is compiled without
optimization in debug builds, so debug numbers are pessimistic.

### On battery (this PC much slower; a stand-in for a slower PC)

The release runs `R-*` ran on battery, right after a long compile (Windows reported "discharging"). Rough Pages then
took 2-3× longer (scan 1920: 56-57 ms instead of 18 ms). The earlier debug runs (`S-*`, `T-*`) didn't record the
power state; their Rough Pages (41 ms at 1920) sit between the two.

| Build | PDF | Window | Input | Showed | Covered | Rough Page draw |
| --- | --- | --- | --- | --- | --- | --- |
| release | scan | 1920 | tap | 55-61% | 22-29% | 56-57 ms |
| release | scan | 1920 | hold | 23% | 13% | 58 ms |
| release | scan | 4K | tap | 28% | 10% | 86 ms |
| release | scan | 4K | hold | 15% | 10% | 94 ms |
| release | text | 1920 / 4K | hold | 82% / 57% | 76% / 24% | 25 / 31 ms |
| any | scan | 1920 / 4K | tap, no Rough Pages | 6% / 4% | | |

### Background copy

- Opening it (`Clone()`): 4-6 ms plugged in (10-14 ms on battery), +1-4 MB.
- While flipping through the 203 MB scan, the copy's own MuPDF cache fills toward its 256 MB limit: process memory
  ended ~200 MB above the run without Rough Pages (`AC-R-S-tap-1920-store` 556 MB vs `AC-R-S-tap-1920-off` 354 MB).
- Emptying the copy's cache after each Rough Page (`store=0`, `fz_empty_store` on the copy) didn't slow Rough Pages
  (each page is drawn once anyway) and kept the extra at roughly 70-130 MB. Part of that is memory Windows hasn't taken
  back yet: closing the copy returned 60-100 MB.
- Text PDF: +4 MB, nothing measurable after that.

### GPU memory, worst case (5 Curls + a Bundle in the air)

Allocated on the default adapter (Iris Xe, which uses system RAM) and measured with `QueryVideoMemoryInfo`:

| | 1920 | 4K |
| --- | --- | --- |
| Layer: 2 swap-chain buffers + depth | 27 MB | 95 MB |
| Screen copy of the old Spread, 6× | 46 MB | 151 MB |
| Screen copy of the landing page, 6× | 23 MB | 76 MB |
| Rough Pages, 10 faces (half size) | 10 MB | 32 MB |
| **Planned / measured** | **105 / 95 MB** | **354 / 298 MB** |

Rough Pages in CPU memory before upload: at most 7 at once, under 10 MB.

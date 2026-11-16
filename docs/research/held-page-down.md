# Why holding Page Down keeps the old page until the key is released

Ticket [#50](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/50), map [#36](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/36). Code as of `9bab0cc6c` (branch `scrolling`).

## Answer

**SumatraPDF's own repaint never gets a turn while the key is held.** Windows hands a window its `WM_PAINT` only when no posted message and no keyboard input is waiting ([GetMessage remarks](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessage): sent, posted, input, sent, then `WM_PAINT`, then `WM_TIMER`). Each Page Turn keeps the UI thread busy for longer than the 33 ms between key repeats (41-49 ms just waiting for the Curl's cover at 4K, plus the screen copy). So the next repeat is always already waiting, and the paint is postponed until the key is released.

Because SumatraPDF never repaints:

- `win->buffer` keeps the picture from the first tap. **Every new Curl's "old Spread" copy is that same stale screen** (page 2 in the probe below), and it is marked sharp, so every sheet shows it.
- The drawing thread never hears "SumatraPDF shows the new Spread" (hook 6). The layer never goes see-through, the landing copies never come, and the page revealed under the newest sheet stays blank paper.
- On release the queue drains, `WM_PAINT` runs within about 150 ms, hook 6 fires, and everything updates at once.

Fast taps (about 8 a second) leave the queue empty between taps, so the paint runs. This is not the #42 landing race: there, the two clocks disagree about one Curl. Here, the clocks agree, but the screen copies are stale.

## Evidence

The probe was a throwaway Bun script, not checked in. It used `out/dbg64` (built 12:51) and `out/rel64` (13:47), with `numbered-800-pages.pdf`, on this PC (3840x2400 at 225%, Iris Xe). The held key came from a separate process, so sampling could not delay it: one `WM_KEYDOWN`, a 500 ms delay, then repeats with bit 30 set, busy-timed to 33.0 ms. Meanwhile the main process sampled a band of the canvas's **window DC**, about every 2 ms. The window DC is SumatraPDF's own GDI picture, without the Curl layer, so each change is a paint SumatraPDF flushed.

| Run | Window, build          | Repeats                   | SumatraPDF paints during a 2.5 s hold | First paint after release | Turns (Bundles) | Mean UI wait per Curl |
| --- | ---------------------- | ------------------------- | ------------------------------------- | ------------------------- | --------------- | --------------------- |
| A   | test layout*, dbg      | `sleep(33)`, about 34 ms  | 4                                     | +52 ms                    | 72 (4)          | 27.7 ms               |
| F   | test layout*, dbg      | 33.0 ms                   | 1                                     | +72 ms                    | 76 (2)          | 25.9 ms               |
| B   | maximized, dbg         | 33.0 ms                   | **0**                                 | +161 ms                   | 43 (31)         | 41.3 ms               |
| D   | maximized, **rel**     | 33.0 ms                   | **0**                                 | +146 ms                   | (no log)        |                       |
| D2  | maximized, rel, Facing | 33.0 ms                   | **0**                                 | +168 ms                   | (no log)        |                       |
| E   | maximized, dbg         | 70 ms                     | 5                                     | +93 ms                    | 38 (0)          | 44.1 ms               |
| C   | maximized, dbg         | 40 posted at once (flood) | 0                                     |                           | 2 (1)           |                       |

\* `tests/winapi.ts` "quarter" layout, 1890x1573 canvas px here.

Screenshots of the screen (layer included) at 1.5 s and 2.3 s into the hold in Single Page: the toolbar reached page 30+, but every sheet and the page under them still show the big "2" and the footer "page 2 of 800" from the copy made at the first tap.

Even at 70 ms between repeats (E), SumatraPDF painted only 5 times in 2.5 s. A maximized 4K Page Turn costs the UI thread roughly 60-70 ms.

## Cause (file:line)

1. **Each turn blocks the UI thread for more than one repeat:**
   - `CurlCopyOld` → `ScreenCopyAsk` waits for the drawing thread's copy (`ScreenCopy.cpp:59-98`). At 4K, `CopyWaitMs` (`CurlRenderer.cpp:2746`) makes it wait for the frame boundary.
   - `CurlStart` then waits for the cover, as ADR 0005 requires (`CurlRenderer.cpp:3043-3057`, `kCoverWaitMs` at `:51`).
   - Both run per key: `PageTurnDetect.cpp:394` and `:356`.
2. **The repaint is low priority:**
   - `GoToPage` → `RepaintDisplay` → `ScheduleRepaint` (`src/Canvas.cpp:4889`) posts a task.
   - That task only invalidates the canvas (`Canvas.cpp:4910-4918`). The paint itself is a `WM_PAINT`, which waits behind every queued key.
3. **The copy is trusted as sharp although it is stale:**
   - `SetSidePage` (`PageTurnDetect.cpp:134-137`) marks a page sharp when the render cache holds it. It does not check whether the screen in `win->buffer` shows that page.
   - `BestPicture` (`CurlRenderer.cpp:1257-1273`) then puts the stale copy on the sheet instead of the Rough Page.
   - `DetectBeforeSpread` already knows when the screen is stale: `wt->spreadAtPaint` is not `wt->from` (`PageTurnDetect.cpp:382`, set in `DetectAfterPaint`, `:417-418`). Nothing uses that.
4. **The revealed page waits for SumatraPDF:** `DrawRevealed` (`CurlRenderer.cpp:1609-1627`) draws blank paper for a page the render cache has, expecting SumatraPDF to paint it and the layer to go see-through. During a hold, neither happens.

## Why the tests don't catch it

- `tests/skeuomorph-fast.ts` and `tests/skeuomorph-rough.ts` judge only debug-log lines (turn kinds, lengths, in-air counts, landings). Starvation doesn't change those lines: every Curl starts, flies and lands on time.
- The face counter of `skeuomorph-rough` ("few blank") counts the stale copy as **sharp**, because of cause 3. So the check passes.
- The tests run in the quarter layout, where a turn costs about 26 ms. SumatraPDF still paints every 0.6 s or so there (runs A and F: starved most of the time, but not always). The human's maximized 4K window never paints.
- Posted keys are not real keys. A posted `WM_KEYDOWN` queues in order with `FinishTurn` (also posted, `src/base/UITask.cpp:201`). Two keys then often run before one `FinishTurn` and merge into a 2-page Bundle (run B: 31 Bundles). Real keys are input messages, which come after posted ones, so `FinishTurn` always runs first and each repeat is its own Curl, as spec 3.2 asks. That is why the human sees fast Curls, not Bundles. `WM_PAINT` comes after both, so the starvation is the same.

A test can see the bug with no app change: during a held key, sample the canvas window DC as the probe did, and require it to change at least every ~300 ms. Or add a debug log line at hook 6 (`DetectAfterPaint`, flushed) and count those lines during the hold.

## Fix options

None needs an original file except D.

| Option                                            | What                                                                                                                                                                                                                                                                                                                                 | Where                                                                                                                      | Cost        | Risk                                                                                                                                                                                                                                                                                                               |
| ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------- | ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **A. Repaint inside the turn**                    | At the end of `FinishTurn`, after `StartCurl` has waited for the cover, invalidate the canvas and call `UpdateWindow(win->hwndCanvas)`. SumatraPDF then paints the new Spread before the next key, and hook 6 fires.                                                                                                                 | `PageTurnDetect.cpp:356-357`                                                                                               | A few lines | Adds a full 4K paint plus the hook-6 landing copy to every key. Turns per second drop. If Windows queues every real repeat, the backlog grows and pages keep turning after release (check with a real keyboard). The original's own posted repaint runs once more at release (harmless). Keeps the ADR 0005 order. |
| **B. Let the layer carry the hold** (recommended) | When the screen is stale at hook 3 (`spreadAtPaint` is not `from`), mark the copied sides not sharp, so sheets show the Rough Page of the right page. In `DrawRevealed`, show the Rough Page until SumatraPDF really shows the page, even when it is in the render cache. SumatraPDF catches up on release, as now, under the layer. | `PageTurnDetect.cpp:134-137, 382-395`; `CurlRenderer.cpp:1609-1627`                                                        | Moderate    | No extra UI-thread time. Held pages show rough until release, which spec 3.10.2-3.10.3 allows. A Facing landing side drawn from a stale copy needs the same check (`DrawStaysRough`, `:1630`). Needs a probe-style test, because the log can't see it.                                                             |
| C. Make each turn cheaper than a repeat           | Skip the cover wait when a non-see-through Curl already covers the screen. Copy without waiting for the frame boundary.                                                                                                                                                                                                              | `CurlRenderer.cpp:2746-2757, 3043-3057`                                                                                    | Moderate    | At 4K a copy alone costs about a frame, so this can't reliably get under 33 ms. Weakens ADR 0005 (flash risk). Not enough alone.                                                                                                                                                                                   |
| D. Throttle repeats while a paint is pending      | Ignore a key-repeat `WM_KEYDOWN` (lParam bit 30) while the canvas still has an update region.                                                                                                                                                                                                                                        | Original `src/SumatraPDF.cpp:14251` (`WM_KEYDOWN`), about 3 lines and a new hook, or a frame subclass in `src/skeuomorph/` | Small       | Holding becomes as slow as painting, which contradicts spec 3.4 (holding works like fast tapping). Touches an original file (ADR 0003).                                                                                                                                                                            |

Recommendation: **B**, plus the probe-style check in `tests/skeuomorph-fast.ts`. A is the quick fallback if rough pages during a hold are not acceptable. If so, first measure the release overshoot with a real keyboard.

Open question for the human: after releasing a held key, do pages keep turning for a moment? That shows whether Windows queues every repeat here (it matters for A and D).

# Why a zoomed-in Curl shows blank where the page was off-screen

Ticket: [#37](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/37), map [#36](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/36). Code read at `826155744`. Found by reading the code; I did not reproduce it.

## Answer

The Curl turns the **whole page**, but it only has a **picture of what was on screen**. When the page is bigger than the window, the parts of the sheet that had no picture are drawn in the plain paper colour. Two things bring those parts into view:

1. **The fold.** As the sheet folds over the spine, its outer part swings across the window. In Facing at 200% that part was off-screen to the side.
2. **The glide.** Page Down leaves the old page scrolled to its bottom and lands the new one at its top. The two views differ, so the Curl treats the turn like a page-size change and pans the view from one to the other. The pan moves across page area that was never on screen.

Spec 3.2 already says "Zoomed in: the Curl plays on what's on screen". The code doesn't do that yet.

## Cause, with file:line

| Step                    | Where                                                                                      | What happens                                                                                                                                                                                                                         |
| ----------------------- | ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Spread geometry         | `src/skeuomorph/PageTurnDetect.cpp:136-140` (`SpreadOnScreen`)                             | takes `pi->pageOnScreen`, the **whole** page rect in canvas px. At 200% it reaches far past the canvas (negative `y`, `Right()` > window width). Used for the old Spread (`:312`), the new one (`:181`) and the landed one (`:334`). |
| Old picture             | `src/skeuomorph/CurlRenderer.cpp:1779` (`CurlCopyOld`)                                     | copies the canvas only, from `win->buffer`, which is window-sized.                                                                                                                                                                   |
| Landing picture         | `CurlRenderer.cpp:1846-1859` (`CurlWindowShowsNew`) and `src/skeuomorph/ScreenCopy.cpp:91` | the landing page's rect is cut down to the canvas before copying.                                                                                                                                                                    |
| Sheet                   | `CurlRenderer.cpp:544`, `:586` (`GeometryOf`), `:1023-1043` (`DrawSheet`)                  | the turning sheet is the whole old page rect. Its faces are mapped onto it (`frontSrc` / `backSrc` = the whole rects).                                                                                                               |
| Blank                   | `src/skeuomorph/Curl.hlsl:181-187` (`PictureAt`)                                           | a point outside the picture's area returns `paper.rgb`. This is the blank. Mode 5 (`:207-213`), used by the glide, goes through the same function.                                                                                   |
| Glide switched on       | `CurlRenderer.cpp:700-717` (`SidesOf`), `:734-748` (`GlideOf`)                             | the view's centre `cam.ay` comes from the whole page rects. Bottom of the old page vs top of the new one gives a different centre, so `sameView` is false and `gl.on` is true.                                                       |
| Glide pans over nothing | `CurlRenderer.cpp:1196-1255` (`RenderGlide`), `:1283`                                      | the camera moves by about (page height − window height) during the turn. Both pages and the sheet are drawn from canvas-only pictures, so everything that slides in is paper colour.                                                 |

Why there's no picture to use instead: SumatraPDF only renders tiles of the visible area. At zoom past Fit Page, a tile is at most screen-sized (`src/shared/RenderCache.cpp:201-207`, `:741-776`), and its cache isn't ours to read (ADR 0002). Rough Pages (spec 5.6, build step 8) aren't built yet. And even when they are, the wish list skips faces that "already have a sharp picture", which the on-screen pages do.

How Page Down gets there: `CmdScrollDownPage` scrolls a screen (`src/SumatraPDF.cpp:11586-11590`). At the page's bottom it calls `GoToNextPage`, which lands on the new page at `scrollY = 0` (`src/shared/DisplayModel.cpp:3199-3206`, `:3011-3012`).

## Fix options

All of them stay inside `src/skeuomorph/`, so none needs an original-file line (ADR 0003).

### C. Curl only what's on screen (clip the sheet to the window), recommended

Cut each page rect to the canvas in `SpreadOnScreen`, so the sheet and its pictures cover the same pixels. In Facing, the turning sheet is the visible part of the right page, with the window edge as its outer edge. A scroll-only change then gives equal views, so the glide stays off. Work out the spine from the whole rects before cutting, and zero the Page Edges on a side that was cut.

- **Cost:** about 15-30 lines in `PageTurnDetect.cpp` (plus a few in `GlideOf` if a zoom change still needs a guard). No shader change. No memory or render cost.
- **Risk:** low. This is what spec 3.2 says. Visible difference: the sheet's outer edge is the window edge, not the page's real edge, which is off-screen at rest anyway. Check: a lone page fully off-screen. `SpreadOnScreen` skips pages with `visibleRatio <= 0` (`:136`) and then guesses the spine from the window centre (`:168`), so a Facing view zoomed onto one page gets a wrong spine today. Handle it in the same change.
- **Original lines:** 0.

### B. Fill the hidden parts with Rough Pages

Keep the whole-page sheet. Where `PictureAt` finds no screen pixel, sample the page's Rough Page instead of paper. That needs a second texture and rect per face (two more shader inputs, shader recompiled), and a wish-list rule: at zoom past the window, prepare Rough Pages of the Spread at rest and of the landing page on tap.

- **Cost:** depends on build step 8 (Rough Pages), then about 60-120 lines: shader, constants, wish list, at-rest preparation. Memory is small (~1.4 MB per 600 px face).
- **Risk:** medium. Rough Pages are capped at ~600 px on the long side, while the page at 200% is ~3000 px. The filled parts are clearly blurrier, with a seam at the window edge. The glide still pans over them for the whole turn unless it's also switched off for scroll-only changes. On big scans the Rough Page may not be ready at the tap, so the blanks come back there.
- **Original lines:** 0.

### A. A sharp whole-page picture for the Curl

Render the old and new pages whole, at screen zoom, from the background copy.

- **Cost:** high. At 200% on 4K a page is about 4000×5600 px, ~90 MB per face, 2-4 faces in the air. That breaks the 300-350 MB budget (spec 4). Rendering takes 100 ms to seconds (the 71 MP scans), so it would have to run at rest after every landing. About 150+ lines, plus memory handling.
- **Risk:** high: memory, constant background work while zoomed in, and still blank on a fast tap if it isn't ready.
- **Original lines:** 0 with the background copy. Getting pixels from SumatraPDF's own render cache instead would touch `RenderCache.cpp` and `DisplayModel.cpp`, which ADR 0002 rejected.

### D. A smaller lift, or clipping the blank away

- **Smaller lift:** doesn't work for pages wider than the window. A turn must carry the whole sheet across the spine, so its off-screen outer part always crosses the window. It only shortens how long the blanks show.
- **See-through instead of paper** where there's no picture: shows holes in the paper. Worse.
- **Glide off for scroll-only changes alone** (about 5 lines in `GlideOf`, 0 original lines): removes the vertical pan, the biggest blank in the repro. In Facing the horizontal blanks from the fold stay. Good as a first step only.

## Recommendation

**C.** It matches the spec as written, costs no memory or render time, needs no original-file lines and doesn't wait for step 8. If the human later wants the real page edge to swing into view, B can be layered on after Rough Pages exist.

| Option               | Original-file lines      | New-code lines (estimate) | Fixes fold blanks | Fixes glide blanks            |
| -------------------- | ------------------------ | ------------------------- | ----------------- | ----------------------------- |
| C. clip to screen    | 0                        | 15-30                     | yes               | yes                           |
| B. Rough Pages fill  | 0                        | 60-120, after step 8      | blurry            | blurry, unless glide also off |
| A. whole-page render | 0 (more via RenderCache) | 150+                      | yes, when ready   | yes, when ready               |
| D. glide off only    | 0                        | ~5                        | no                | yes                           |

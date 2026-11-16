# Old Page Edges and cut-off fades when pages differ in size

Research for [#38](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/38) (map [#36](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/36)). Code references are to `scrolling` at `9bab0cc6c`.

## Short answer

Both symptoms come from the **glide** (`RenderGlide`, built in step 5 / #31). It draws every frame itself whenever the pages on a side change size. Both were ported faithfully from the prototype `prototypes/sizes-zoom-prototype.html` (branch `prototype/mixed-sizes`), so the prototype shows them too. Neither came in with steps 6 to 8.

1. **Old Page Edges stay visible.** A double-width page (7) and an A4 page (5) count as a "comic double spread", so the Page Edges on that side **crossfade** instead of **tucking under**. A crossfade draws the old strip, cut from the old screen copy, where page 7 used to lie. But page 7 is the sheet that is lifting and shrinking, and page 5 beneath it is half as wide. So for the first 35% of the turn the old strip floats in the background, beside nothing.
2. **The fade is cut off.** The leaving page fades only where it sticks out past the outline the new page will have **once it has landed**. Inside that outline it stays fully opaque. The incoming sheet reaches that outline only at the very end, so until then a band of the leaving page shows at full strength right next to its faded part. That leaves a hard vertical seam, and the incoming page covers the band instead of it fading.

Every fix below changes only `src/skeuomorph/`, so each needs **0 lines in original files** (ADR 0003).

## How it was checked

- Read `CurlRenderer.cpp`, `Curl.hlsl`, `PageEdges.cpp`, `PageTurnDetect.cpp`, `ScreenCopy.cpp` and the prototype's `edgeMode` / `drawFlat`.
- Ran the existing `out\dbg64\SumatraPDF.exe` (built from `79ad3e612`, same drawing code) with `-for-testing` and 4 s Curls in Facing / Fit Page. Screen captures were taken at 3-110% of the turn with a scratch script modelled on `tests/skeuomorph-curl-look.ts`. Turns probed:
  - `numbered-mixed-sizes.pdf`, page 7-8, Page Up (the repro). Log: `Curl backward ... [7-8 -> 5-6]`, `glide 1`.
  - the same file, page 5-6, Page Down (the mirror case);
  - `Garcia_Physical-sec.pdf`, page 21-22, Page Up.

What the repro frames show (page 7 = 1190×842 pt on the left, 8 = A4 on the right; lands on 5 = A4 left, 6 = 360×1080 pt right):

| Turn   | What is on screen                                                                                                                                                   |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 10%    | The old left Page Edges sit ~25 px left of page 7's sheet, with background between them.                                                                            |
| 20-30% | The strip drifts further away (70-160 px), fading. It stays visible until ~35%.                                                                                     |
| 70-80% | Page 8 is fading. A vertical seam runs through it (x≈1615 at 80%): on the left a band at full strength between the sheet and the seam, on the right the faded part. |
| 88-94% | A solid sliver of page 8's "8" is still visible beside the landing sheet, next to the almost-gone rest.                                                             |

## Symptom 1: old Page Edges stay visible

**Cause, step by step**

- `CurlRenderer.cpp:790-799` `EdgeModeOf`: a pair of pages counts as a "comic double spread" if one is twice as wide as the other (±4%, `kSpreadWide`, `:93`) at the same height (±3%, `kSpreadHigh`, `:94`). Page 7 against page 5 matches, so the left side gets `EdgeMode::Crossfade` (set in `GlideOf`, `:854`).
- `:1739-1742` `DrawSideEdges`, crossfade branch: the old strip is drawn beside `gl.oldPage[side]` (page 7's resting outline, carried along by the moving camera). It fades out over the first 35% on the side the sheet lifts from (`kUnderFade`, `:90`).
- `:1704-1715` `DrawEdgeStrip`: the strip is cut out of `f.oldPic`, the screen copy of the old Spread (`CurlCopyOld`, `:2981-2993`). That is why the ticket says the edges "are part of the screen copy". `ScreenCopy` itself is not at fault: it copies the whole canvas on purpose, and the glide otherwise samples only page rectangles (`Curl.hlsl:233-234`).
- The page those edges belonged to has meanwhile become the turning sheet. Its outline shrinks from page 7's size toward page 6's from the first frame (`Curl.hlsl:106-120`, `SheetNow` lerps gap and width by the turn progress), and its corner peels. The page underneath (5) is half as wide, so neither page has an outer edge where the old strip is drawn.
- A crossfade only works if the old and new page share their outer edge. For same-size pages they always do (`SameSize`, `:786-788`, pages line up on the spine). For a double-width page in one column they never do.
- **Mirror case** (Page Down 5-6 → 7-8): on the landing side the **new** strip fades in beside page 7's final outline over the last 35%, before page 7 has landed. Again it floats beside nothing (same lines).

**Why the exception exists.** In #31 the human kept crossfades for "same-size pages, and comic double spreads", taken from the prototype (`edgeMode`, `sizes-zoom-prototype.html:133-139`). In the real app, though, a comic that SumatraPDF treats as landscape-as-spread gets its wide page split in half, one half per side (`PageTurnDetect.cpp:176-186`, `IsWideAlone`). Each half is then the same size as a normal page, so `SameSize` already picks a crossfade. As far as the code shows, the "doubled" rule only fires when a wide page sits in **one** column, which is exactly the case where it looks wrong. (Check with the `comic-rtl` scenario when fixing.)

**Fix options**

| Option                                                              | What                                                                                                                                                                | Cost                                                         | Risk                                                                                                                                                                                                                                                       |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1A. Drop the double-spread exception** (recommended)              | `EdgeModeOf` returns `Crossfade` only for `SameSize`. Every real size change tucks: old edges go under the page in the first 15%, new ones slide out after landing. | ~5 lines removed in `CurlRenderer.cpp` (+ the two constants) | Low. Reverses one #31 decision, so it needs the human's ok; best decided together with [#40](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/40), which sets the tuck threshold anyway. Re-run `skeuomorph-curl-look` (`mixed`, `comic-rtl`). |
| 1B. Rephrase the rule as "crossfade while the outer edge stays put" | Compare the outer edge's position (world x) and the height instead of the size.                                                                                     | ~8 lines                                                     | Same result as 1A today, since pages line up on the spine. Only worth it if #40 adds other crossfade cases.                                                                                                                                                |
| 1C. Carry the old strip with the sheet                              | Recompute the sheet's flat outline on the CPU each frame and draw the fading strip beside it.                                                                       | 25-40 lines                                                  | Medium. Can't follow the curled corner, so it still detaches once the sheet lifts. Not recommended.                                                                                                                                                        |
| 1D. Crossfade faster (e.g. 10%)                                     | Change the window only.                                                                                                                                             | 1-2 lines                                                    | Hides the symptom less than it seems: still visible for a few frames. Not recommended.                                                                                                                                                                     |

## Symptom 2: the leaving page's fade is cut off

**Cause, step by step**

- `CurlRenderer.cpp:1807-1811` `RenderGlide`: the old page on the landing side (the "stays" page, page 8 here) is drawn with alpha `1 - SmoothStep(0.65, 1, q)` **outside `keepBack`** (`:1809`, `:1811`).
- `keepBack = ToScreen(cam, gl.back)`: the landing page's **final** outline (page 6 at rest), at the current camera. Inside it the shader draws fully opaque (`Curl.hlsl:235`: `Inside(c, keep) ? 1 : alpha`).
- The sheet only reaches that outline at the end of the turn: its outline morphs with the turn progress, and it folds over the spine progressively (`Curl.hlsl:106-120`, `Bend` `:132-157`).
- So from 65% to 100% the strip between the sheet's current edge and the final outline shows the leaving page **opaque**, right next to its **fading** part. That is the seam. Then the sheet slides over that strip, so those pixels never fade: they go from full strength to covered.
- **Mirror case**, same lines (`:1808`, `:1810`): the new page uncovered on the lifting side fades in only outside `keepFront`, the sheet's **starting** outline. As the sheet shrinks and peels, the area inside that outline is uncovered at full strength (or as blank paper before SumatraPDF shows the new Spread), next to the part still fading in. It shows less, because most of it stays under the sheet.
- The prototype does the same (`drawFlat(..., keep = pageRect(above))`, `sizes-zoom-prototype.html:831-832`). In #31 this rule was "kept from the prototype, not discussed".

**Fix options**

| Option                                        | What                                                                                                                                                                                                                                                                                            | Cost                                                                                                  | Risk                                                                                                                                                                                                                                                                            |
| --------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **2A. Retract instead of fade** (recommended) | Over the same window (last 35%), slide the leaving page's outline in from its own size to where it overlaps the landing outline, drawn opaque and clipped, not see-through. Mirror for the lifting side: the uncovered new page grows out from the sheet's starting outline over the first 35%. | ~20-30 lines in `RenderGlide` (a lerped dst/src rect into the existing `DrawImage`); no shader change | Low in code. It changes the look, so it needs the human's ok: a short prototype tweak or a slow-motion checklist. It matches what the human already chose for Page Edges in #31 ("thickness animates, not opacity") and their verdict that see-through crossfading looks cheap. |
| 2B. Fade the whole leaving page (no keep)     | Drop `keepBack` / `keepFront`.                                                                                                                                                                                                                                                                  | 2 lines                                                                                               | Low in code, but the background shows through where the new page is about to land: the see-through look the human disliked.                                                                                                                                                     |
| 2C. Keep follows the sheet's current outline  | Compute the lerped outline on the CPU and pass that as keep.                                                                                                                                                                                                                                    | ~15 lines                                                                                             | The seam stays; it just moves with the sheet. Not recommended.                                                                                                                                                                                                                  |
| 2D. Soften the keep edge                      | Feather the opaque/faded boundary over ~20-30 px in `Curl.hlsl` mode 5; re-run `compile-shaders.ps1`.                                                                                                                                                                                           | ~5 lines + regenerated `CurlShaders.h`                                                                | Blurs the seam but keeps the timing problem: the band is still covered instead of fading.                                                                                                                                                                                       |

## Recommendation

**1A + 2A**, specified and built together as one "mixed sizes, second pass" step:

- Drop the double-spread crossfade, so a real size change always tucks its Page Edges. Decide it inside #40, which already asks how big a size change must be before edges tuck.
- Animate the outline instead of the opacity of the page beneath where it sticks out.
- Both are small, live entirely in `CurlRenderer.cpp`, and touch **0 lines of original files**.

**Proof for the fix.** Add a `mixed-up` scenario to `tests/skeuomorph-curl-look.ts`: Facing, `-page 7`, Page Up. Add two checks:

- **No floating Page Edges:** at 20-30%, no edge-coloured columns lie in the background more than a few px away from any page or sheet.
- **No seam:** at 80-94%, inside the leaving page's old rectangle and outside the new one, no column is near-identical to "before" while its neighbour is faded.

Run it on today's build first to see it fail.

**Garcia.** On `Garcia_Physical-sec.pdf` (21-22 → 19-20) the glide was on, but the pages differ by well under 1%. The edges crossfade in place and the keep covers almost the whole page, so neither symptom showed there. Where scans differ by a few percent, the same two mechanisms give slivers a few px wide; #40's threshold decides whether those turns glide at all.

## Reproduce

```powershell
$d = "$env:TEMP\skeu-38"; mkdir $d -Force | Out-Null; Set-Content "$d\SumatraPDF-settings.txt" "SkeuomorphPageTurn = true`r`nPageTurnDuration = 4000`r`nDefaultDisplayMode = facing"; cd C:\code\sumatrapdf-skeuomorph; .\out\dbg64\SumatraPDF.exe -for-testing -appdata $d -page 7 C:\code\test-shelf\generated\numbered-mixed-sizes.pdf
```

Press Page Up once. Watch the far left during the first second: a thin strip of Page Edges is left behind in the grey. In the last second, watch page 8 on the right: part of it stays solid while the rest fades.

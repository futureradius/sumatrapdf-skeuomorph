# Skeuomorph Page Turn: spec

The single source for building the Skeuomorph Page Turn. Every decision was made on the map [Map: Skeuomorph Page Turn](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/3). Where this spec and an old ticket disagree, **this spec wins**. Each section links the ticket that holds the "why". The bigger choices also have decision records in [`docs/adr/`](adr/).

Words are used as defined in [GLOSSARY.md](../GLOSSARY.md): Paged View, Spread, Continuous View, Right-to-Left Reading, Page Turn, Curl (and "lands"), Bundle, Page Edges, Rough Page, Skeuomorph Page Turn.

---

## 1. Start here (every build session)

**What this is.** A personal SumatraPDF fork that makes turning pages look like turning real paper, so the reader keeps a physical sense of *where they are in the book*. Judge every choice by that.

**Read only this:**
1. this spec,
2. the **Built so far** list on the build parent issue, [Build: Skeuomorph Page Turn](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/26),
3. your own build issue (and the last "where I stopped" comment on it, if any).

You don't need to read the map or the old tickets. Open one only if you need the "why" behind a rule.

**The person driving this is not a programmer.**
- Explain plainly and say what a result means.
- Give exact copy-paste commands for Windows PowerShell 5.1: chain with `;`, never `&&`.

**Repo rules**
- Follow `agents.md`, except: **commit and push your own finished work without asking** (decided by the human on 2026-11-09; this overrides `agents.md`'s "never commit automatically").
- Work on the `scrolling` branch.
- Name test copies starting with `-for-testing`.
- This is a personal fork, never sent back to the original SumatraPDF. All new code goes in `src/skeuomorph/` (and `tests/skeuomorph-*.ts`). Original files change only at the hook points in [section 5.2](#52-hook-points-in-original-files), within each build step's line budget. Going over the budget means asking first.
- Never use private documents for tests, only the test shelf (below).

**GitHub.** This clone also has the original SumatraPDF as `upstream`, so `gh` can silently hit the wrong project. Before any `gh` command, check that `gh repo set-default --view` prints `futureradius/sumatrapdf-skeuomorph`. If it doesn't, run `gh repo set-default futureradius/sumatrapdf-skeuomorph`, or pass `--repo futureradius/sumatrapdf-skeuomorph`.

**Build**
- MSBuild lives in `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\`.
- Debug: `MSBuild.exe vs2022\SumatraPDF.sln /t:SumatraPDF /p:Configuration=Debug /p:Platform=x64 /m`. The program lands in `out\dbg64\SumatraPDF.exe`.
- Release (for speed checks): the same command with `/p:Configuration=Release`.
- Building the whole solution fails in the upstream `test_engines` project. Not our problem; build only the `SumatraPDF` target.
- New files in `src/skeuomorph/` reach the project files through `bin\premake5.exe vs2022`. That only adds them; no other changes.

**Code generator** (for settings and commands)
- Needs **bun**. The human consented to the one-time install on 2026-11-10: `powershell -c "irm bun.sh/install.ps1 | iex"`. bun stays a permanent requirement.
- Run the generator through `src\skeuomorph\run-gen-code.bat`, not plain `bun cmd/gen-code.ts`. Besides bun it needs clang-format (MSBuild's folder), `cl.exe` (`vcvars64.bat`) and vswhere (the VS Installer folder) on PATH, and the batch file sets all of them up. Found in build step 1.
- The generator's clang-format pass also reformats an unrelated array in `src/shared/EbookDoc.cpp`. Undo that with `git checkout src/shared/EbookDoc.cpp`. It also leaves a git-ignored `cmd/tmp/` folder.
- Never edit the generated `src/Settings.*` and `src/shared/Commands.*` by hand. After upstream merges, fix conflicts there by re-running the generator.

**Done for a build step** (before the next one starts):
1. The debug build succeeds.
2. All unit tests pass (`out\dbg64\SumatraPDF.exe -unit-tests`).
3. That step's `tests/skeuomorph-*.ts` scripts pass (`bun tests/skeuomorph-<name>.ts`).
4. The human said "ok" to the step's checklist: 5 to 10 lines of copy-paste PowerShell commands that open test-shelf documents, each with what to look for. Write the checklist at the end of the step, post it on the issue, and record the human's answer there.
5. Lines changed in original files stay within the step's budget.
6. Committed and pushed. Then close the issue with a short resolution comment and add one line to **Built so far** on the build parent: what works now, plus anything surprising.
7. Only for steps that change how Curls or Rough Pages are drawn: Release build, plugged in, a scripted burst of turns, no late frames at the usual window size. Battery runs are noted, not judged.

**More than one step per session.** When a step is done and less than about half of the session's context is used, tell the human, claim the next build issue and carry on. Otherwise stop and say so. If a session runs out in the middle of a step, it still leaves SumatraPDF building, commits and pushes, and posts a short "where I stopped" comment on the issue, so the next session continues instead of starting over.

**Test shelf.** `C:\code\test-shelf\` sits outside the repo and is never committed. The committed script from build step 1 fills it with:
- generated numbered books: every page shows its number large, in its own colour; 1, 2, 3, 40 and 800 pages, plus one with mixed page sizes;
- downloaded public-domain files: a right-to-left comic with double-page spreads, and an EPUB.

The human also puts public scans and text papers there. The scans are the worst case for speed checks; the biggest is "Garcia", 375 pages, up to 71 megapixels per page.

---

## 2. Ground rule

The effects are decoration, with these exceptions. All of them apply only in a Paged View with the switch on:

- **Room for Page Edges:** SumatraPDF keeps room beside the Spread, so pages are a little smaller. Presentation mode has no Page Edges and no room.
- **The cover beside the spine:** the lone Book View cover sits right of the spine instead of centred, and a lone last page sits left of it.
- **Rough Pages:** SumatraPDF prepares Rough Pages for pages on Curls in the air, so pages you flip past show up.

With the switch off, or in a Continuous View, the layout is exactly as in the original. Nothing else in SumatraPDF behaves differently. (Why: [Where do Page Edges go when the Spread fills the window?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/10), [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16), [What does a Curl show if a page is not ready yet?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/11); ADR [0004](adr/0004-room-for-page-edges-and-cover-beside-spine.md).)

---

## 3. What the reader sees

### 3.1 The switch and the speed

- **The switch:** View menu → **Skeuomorph Page Turn**, a checkmark item, on by default, remembered between sessions and saved straight away when clicked. No keyboard shortcut; the command palette finds it. Hidden in ng.
- **Settings file:** `SkeuomorphPageTurn = true` and `PageTurnDuration = 700`.
  - `PageTurnDuration` is the normal Curl length in milliseconds, so a bigger number is slower.
  - It shows in Settings → Advanced Settings by itself, and it's clamped to 100-5000 when read.
  - It is the only speed setting. The fast-tapping numbers are fixed in code.
- **Shared settings file:** ng keeps settings it doesn't know when it saves, so both values survive ng. Nothing to do.

([How are settings and View-menu switches added?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/7), [What happens on window changes mid-Curl, old Windows, and shared settings?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/23))

### 3.2 Which page changes are a Page Turn

**One action = one Page Turn.** Compare the Spread before the action with the Spread after it:
- one Spread apart → a Curl;
- more → a Bundle;
- the same Spread → nothing.

Steps inside one action (next page ×3, a link that jumps twice) never show on their own. Each key repeat and each wheel notch is its own action.

**These curl** (only in a Paged View with the switch on):
- next/previous by keys, wheel, clicks or toolbar;
- every jump: Go to page, table of contents (SumatraPDF's "Bookmarks"), Favorites, links, search results, Home/End, Back/Forward, thumbnails;
- Read Aloud following the voice, and reading auto-scroll: ordinary Curls, with the fast-tapping speed-up if they turn quickly;
- dragging the scrollbar thumb in Single Page (only with `ScrollbarInSinglePage` on): each step is an ordinary Page Turn, so it riffles;
- Presentation mode: Curls and Bundles, but no Page Edges.

**These never curl.** The new Spread just appears, and the Page Edges update at once:
- opening a document;
- switching display mode (Single/Facing/Book, entering or leaving Presentation);
- the `B` key;
- turning "step one page" on or off;
- EPUB chapters being renumbered while they're laid out;
- any turn while the window is minimized or hidden;
- toggling Manga Mode.

**"Step one page" on:** no Curls or Bundles at all; every change is an instant jump. Page Edges stay and update instantly.

**Zoomed in:** the Curl plays on what's on screen.

([Which page changes count as a Page Turn?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/15), [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16))

### 3.3 The Curl

**Look: corner peel.** The page wraps over a cylinder lying on it. The bottom outer corner lifts first. The fold line starts slanted and straightens as it sweeps to the spine. Near the end the cylinder shrinks, so the page lands flat. A backward turn is the same Curl, mirrored.

| Value | Chosen |
|---|---|
| Bend size | cylinder radius = 0.17 × page width; shrinks to 0 over the last 40% of the turn |
| Corner lead | fold slant = 35° × (1 − p)^1.5, where p = 0.5 − 0.5·cos(π·t) and t = time progress 0..1 |
| Shadow | 30%: soft, cast by lifted paper on the page below, only on paper, never on the background |
| Light and shade on the bend | 30% |
| 3D depth | 70%: perspective, camera distance ≈ 5.2 × window height, so lifted paper grows a little toward you |

The exact maths is in the prototype on the throwaway branch `prototype/curl-look` (`prototypes/curl-look-prototype.html`); port it from there.

**Single Page View:** the page turns over its left edge, its back is blank paper, and once over it fades away. Nothing stays beside the page.

**Facing and Book View:** the back of the turning page is the first page of the new Spread.

([What should a Curl look and feel like?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/8))

### 3.4 Timing and fast tapping

- **Every tap starts its own Curl,** so several pages can be in the air at different stages.
- **Normal Curl length:** `PageTurnDuration` (700 ms by default). Curls are timed by the clock: a slow PC shows fewer frames, never a longer Curl.
- **Fast tapping:** when taps come faster than one Curl lasts, every Curl in the air shortens to *gap between taps × 4*, but never below **310 ms**.
  - Holding a key works the same way, through key repeat.
  - After tapping stops, the length eases back to normal.
  - If `PageTurnDuration` is below 310, fast tapping just keeps it.
- **At most 5 in the air.** Past that, the oldest lands at once.
- **Bundles count** as a tap in the rhythm and as one of the 5. They last `PageTurnDuration × 1.3` and speed up in step with Curls.
- **A backward tap while a page is in the air** sends that page back from where it is.

([What should a Curl look and feel like?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/8), [Which page changes count as a Page Turn?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/15))

### 3.5 Bundles

| Value | Chosen |
|---|---|
| Thickness | 0.70 px per page turned over, at most 18 px (reached at about 26 pages). Deliberately thicker than real paper, so the sheets read at Curl speed. A Bundle is not as thick as the paper it takes off the Page Edges. |
| Sheets drawn | about 1 per 1.1 px of thickness, 2 to 14 |
| What the sheets show | the first sheet's front is the old page and the last sheet's back is the new page; the sheets between are blank paper. Pages inside a Bundle are never prepared. |
| Fan | 20%: each sheet starts a little after the one above, so they spread and land one after another |
| Stiffness | 40% stiffer than one page: radius × (1 + 1.6 × 0.4), corner lead 35° × 0.6; full effect from 6 px thickness up |
| Length | Curl length × 1.3 (910 ms by default) |

- **Lifting:** the Page Edges it leaves shrink while the Bundle's bottom sheet lifts off (the first 12% of that sheet's movement), not before.
- **Landing:** the Page Edges it lands on grow over the last 18% of the turn.
- **A backward tap while a Bundle is in the air** splits off its last sheet, which goes back from where it is.
- **A new jump across pages still in the air** lands them at once.
- **Different page sizes:** the sheets between step evenly from the top page's size to the bottom's.

Exact maths: prototype on the throwaway branch `prototype/bundles-edges` (`prototypes/bundles-edges-prototype.html`). ([What should Bundles and Page Edges look like?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/9))

### 3.6 Page Edges

**Look:** a strip of fine paper lines beside the outer edge of each page, the full page height, a little darker toward the outside. Flat, seen straight from above. Plain 1 px pixel-aligned lines.

**Thickness, "grows slowly":** the whole book's thickness is √(page count × 0.12 × 46) px, kept between 4 and 46 px. For example: 20 pages ≈ 10 px, 89 pages ≈ 22 px, 400 pages and more = 46 px. It's split between the two sides by their share of the pages: read pages on one side, unread on the other.

- **Facing and Book View:** Page Edges on both sides.
- **Single Page View:** only the unread Page Edges, on the right (left in Right-to-Left Reading). Read pages just fade away.
- **Each side follows the height of the page beside it.**
- **Presentation mode:** none.

**The room beside the Spread**
- **How wide:** on each side, this document's whole-book thickness, because on the last page all paper is on one side. Pages keep one size the whole time a document is read.
- **Single Page:** room on both sides too, so the page stays centred. The left room stays empty background.
- **It works like a wider window margin, at every zoom.** Fit Page and Fit Width shrink the pages to make room; at a fixed zoom such as 100%, pages keep their size and the room sits beside them.
- **Switching** the switch off, or to a Continuous View, removes the room at once, with one re-layout as on a zoom change. It comes back when switched on.
- **Zoomed in beyond the window:** Page Edges keep their true thickness, stay attached to the page and scroll with it. Whatever falls outside the window is cut off, never squeezed.

**While Curls or Bundles play,** the layer draws its own moving Page Edges with the same lines, so nothing jumps when the layer goes. At rest, Page Edges are drawn by SumatraPDF's own painting.

([What should Bundles and Page Edges look like?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/9), [Where do Page Edges go when the Spread fills the window?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/10))

### 3.7 Cover, last page, spine shadow

- **Book View cover:** with the switch on, the lone cover sits right of the spine (left in Right-to-Left Reading), in Presentation mode too. Opening it is a normal Curl.
- **Lone last page:** the cover's mirror. It sits left of the spine, the right side is empty background, and there's no spine shadow and no unread Page Edges. (Today it's centred in a Paged View; the same layout flag fixes both.)
- **Spine shadow:** only where there's paper on both sides of the spine. So there's none beside a lone cover or a lone last page. Facing's first Spread keeps it, since page 1 lies on the left. When turning back to the cover, the shadow stays until the turning page has swept over and landed, then fades.
- **The cover lands on a blank slate:** a side with no paper is empty background. The paper under a side appears as its first sheet lands and goes as its last sheet lifts, like opening a real book's cover. A Curl's shadow never falls on empty background.

([What should Bundles and Page Edges look like?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/9), [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16))

### 3.8 Other odd cases

- **Pages of different sizes:** a turning sheet changes shape smoothly from the front page's outline to the back page's.
- **Wide page alone across both columns** ("landscape as spread", comics and image folders): a picture printed across the spine. Curls fold it at the spine, half per side. Page Edges and spine shadow as usual.
- **EPUB page count changes:** Page Edges snap to the new size. A Bundle in the air keeps its starting size, and the Page Edges correct on landing.

([What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16))

### 3.9 Right-to-Left Reading

- **Trigger:** exactly SumatraPDF's Manga Mode (`dm->GetDisplayR2L()`), whether set by the document, the comic default or the reader.
- **Mirroring:** Curls, Bundles and Page Edges mirror, Single Page too: the page turns over its right edge, and unread Page Edges sit on the left.
- **Mirror Pages and rotation never change direction.** In a Paged View the Spread always stays side by side.
- **Toggling Manga Mode** is not a Page Turn: Curls in the air land, and the Page Edges swap sides instantly.

([Is right-to-left mirroring small enough to keep?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/12))

### 3.10 What a Curl shows when pages aren't ready

1. **A Curl starts the moment you tap.** It never waits for pages and never slows down.
2. **Each face shows, best first:** the sharp page, else its Rough Page, else blank paper in the theme's page colour. A better picture switches in instantly, even mid-air, with no fade.
3. **Flipped-past pages show up.** That's a core reason for the project. If preparing can't keep up (big scans, slow PCs), faces stay blank paper and fill in when ready.
4. **The old Spread never comes back** once you've tapped. The layer stays up until SumatraPDF really shows the new Spread or its message.
5. **After landing:** a Rough Page shows until the sharp page replaces it. SumatraPDF's own messages are unchanged: "Rendering page N..." if there isn't even a Rough Page half a second after the tap, and "Couldn't render page N" on failure. A page that fails while in the air stays blank paper. The Page Edges stay in place, and you can keep tapping.

([What does a Curl show if a page is not ready yet?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/11), [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19))

### 3.11 Window changes mid-Curl, and where Curls can't play

- **Everything in the air lands instantly** when the document area changes:
  - resize or maximize, DPI change, fullscreen or Presentation on/off;
  - tab switch, zoom, rotation, Manga Mode toggle;
  - closing the document, minimizing, the switch turned off.

  The layer disappears, SumatraPDF shows the final Spread, and the Page Edges snap. Nothing replays when you come back.
- **Keep playing** when the document area stays the same: moving the window, clicking into another program, a window or menu covering part of SumatraPDF. Curls also keep playing while a menu or dialog is open.
- **Switch off mid-Curl:** everything lands, the Page Edges and their room disappear, and the cover goes back to the centre (one layout jump).
- **Switch on:** no Curl. The Page Edges and their room just appear.
- **Windows 7/8.0, Remote Desktop, a broken graphics driver, or anywhere the Curl layer can't load:** Page Turns are instant jumps, but the Page Edges and the cover beside the spine still show.
  - No Curls means no Rough Pages and no background copy.
  - The menu item looks the same everywhere, with no "not supported" message.
- **Software graphics (WARP):** Curls still play, on time but choppy in big windows.

([What happens on window changes mid-Curl, old Windows, and shared settings?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/23), [Can a Curl layer run smoothly on this PC?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/14))

---

## 4. Numbers in one place

| What | Value |
|---|---|
| Normal Curl length | `PageTurnDuration`, default 700 ms, clamped 100-5000 |
| Fast tapping | gap × 4, never below 310 ms |
| In the air at most | 5 (Curls and Bundles together) |
| Curl bend / corner lead | radius 0.17 × page width, shrinking over the last 40%; 35° × (1 − p)^1.5 |
| Curl shadow / shading / depth | 30% / 30% / 70% (camera ≈ 5.2 × window height) |
| Bundle | 0.70 px per page, at most 18 px; 2-14 sheets (1 per 1.1 px); fan 20%; 40% stiffer; length × 1.3 |
| Bundle Page Edges | shrink over the first 12% of the bottom sheet's lift; grow over the last 18% of the turn |
| Page Edges | whole book √(pages × 0.12 × 46), clamped 4-46 px; room = that on each side |
| Rough Page size | half the page's on-screen size, long side at most ~600 px, same shape |
| Rough Page wish list | at most 10 faces (5 in the air × 2) |
| GPU memory budget | about 100 MB at 1920×1200, 300-350 MB at 4K (measured worst cases 95 / 298 MB) |

---

## 5. How it's built

### 5.1 Files

All new code goes in `src/skeuomorph/`. The suggested split:

| File | Job |
|---|---|
| `PageTurnHooks.h/.cpp` | the only header original files include; thin forwards |
| `PageTurnLogic` | pure decisions with no Windows calls: Curl/Bundle/nothing, fingerprint, timing, fast tapping, the at-most-5 rule, thicknesses, direction. Unit-tested. |
| `PageEdges` | the room beside the Spread, the cover flag, GDI drawing of resting Page Edges |
| `ScreenCopy` | the only code that reads `win->buffer` |
| `CurlLayer` | DirectComposition layer per window |
| `CurlRenderer` | the drawing thread, Direct3D 11, meshes, precompiled shader bytes |
| `RoughPages` | the background copy, the worker thread, the wish list, copied colour helpers |
| `FlashRecorder` | test-only tool, never in the everyday build |

Unit tests go in `tests/PageTurn_ut.cpp` (or next to the logic), and scripts in `tests/skeuomorph-*.ts`. Shader sources and the one-off `fxc` script also live in `src/skeuomorph/`.

### 5.2 Hook points in original files

On `scrolling` at `1d0b75378`, "insert after" the line. "Both" means built by classic and ng; in ng the line does nothing. Full table with reasons: [research/hook-points.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/hook-points/research/hook-points.md). ([Where does the new code hook in, and how few original lines change?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/17); ADR [0003](adr/0003-new-code-in-its-own-folder-behind-two-hooks.md))

| # | What | Where | Built by | Change | Lines | Step |
|---|---|---|---|---|---|---|
| 1 | Seam | `src/shared/DocController.h:78` | both | two empty `virtual` methods on `DocControllerCallback`: `BeforeSpreadChange(DisplayModel*, int)`, `BeforeRelayout(DisplayModel*)` | +2 | 2 |
| 2 | Seam, classic side | `src/SumatraPDF.cpp:966` (+ include) | classic | two `override` one-liners into the new code | +3 | 2 |
| 3 | Start a Page Turn, copy old screen | `src/shared/DisplayModel.cpp:2954`, just before `ChangeStartPage` | both | `cb->BeforeSpreadChange(this, pageNo);` | +1 | 2 |
| 4 | Room for Page Edges, cover flag | `src/shared/DisplayModel.cpp:2217`, start of `Relayout` | both | `cb->BeforeRelayout(this);` | +1 | 3 |
| 5 | Draw resting Page Edges | `src/Canvas.cpp:3098`, after the background, before pages | classic | `PageEdgesPaint(win, hdc, dm);` | +1 | 3 |
| 6 | What the screen really shows | `src/Canvas.cpp:3326`, after the flush (+ include) | classic | `PageTurnAfterPaint(win, <same flush condition>);` | +2 | 2 |
| 7 | Cover right of spine, lone last page left | `DocumentLayout.h:43`, `DocumentLayout.cpp:289` + change `:309`, `DisplayModel.h:431`, `DisplayModel.cpp:2306` | both | `pageBesideSpine` flag, default off | +6, 1 changed | 3 |
| 8 | Forget a closed document | `src/SumatraPDF.cpp:1079` (`CleanUp`) | classic | one call | +1 | 2 |
| 9 | Settings | `cmd/gen-settings.ts:2015`, `globalPrefsLayout` | classic generator | two `field(...)` | +14 | 1 |
| 10 | Command | `cmd/gen-commands.ts:384` | both | one entry at the end | +1 | 1 |
| 11 | View-menu entry | `src/shared/MenuDefs.cpp:354` | both | one entry | +4 | 1 |
| 12 | Hide in ng | `src/shared/CommandAvailability.cpp:1043` | both | `SUMATRA_NG` → hide, like `CmdMeasure` | +5 | 1 |
| 13 | Checkmark | `src/Menu.cpp:956` | classic | `MenuSetChecked` | +1 | 1 |
| 14 | Click handler | `src/SumatraPDF.cpp:12098` | classic | `case` into the new code, which also calls `ScheduleSaveSettings()` | +3 | 1 |
| 15 | Build the new folder | `premake5.files.lua:493` | classic | `files_in_dir("src/skeuomorph", ...)` | +1 | 1 |
| | **Total** | 13 files | | | **+46, 1 changed** | |

Allowed extra: unit-test registration in `src/tests/Sumatra_ut.cpp` (+2, debug only, step 2). Models to copy: the setting is like `ReadingAutoScrollSpeed`; the switch is like `HighlightFormFields` / `CmdToggleHighlightFormFields`, saved at once like `CmdToggleDisableLinks`. Read `gSettings` only in classic-only files. ([How are settings and View-menu switches added?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/7))

### 5.3 Detecting a Page Turn

Every Page Turn passes through `DisplayModel::GoToPage`. Hook 3 fires just before `ChangeStartPage` swaps the Spread, while `win->buffer` still holds the last painted frame.

- **One action:** on the first hook call of an action, post a task with `uitask::Post`. It runs after the current message (key, notch, command) has made all its `GoToPage` calls, and before SumatraPDF's repaint. There, compare the old and final Spread.
- **"Don't animate":** keep a per-window **fingerprint** (`dm`, `displayMode`, `facingStepOne`, `inPresentation`, `layoutGeneration`, `PageCount()`), refreshed at every paint (hook 6). A turn animates only if all of these hold:
  - the switch is on;
  - it's a Paged View;
  - the layer loaded;
  - the fingerprint at the hook equals the one from the last paint;
  - `!dm->syncingWithEngineLayout`;
  - `!dm->StepsOnePage()`;
  - the window is neither iconic nor hidden.
- **Hook 8** clears the fingerprint when a `DisplayModel` dies, so a new document at the same address never curls on opening.
- **Debug log:** one line per Page Turn (debug build only): what was decided (Curl, Bundle, nothing, and why), how long it ran, late frames. The `tests/skeuomorph-*.ts` scripts press keys and check these lines.

Code paths and non-turn callers: [research/page-turn-code-paths.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/page-turn-code-paths/research/page-turn-code-paths.md). ([Where do Page Turns happen in the code?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/4), [Which page changes count as a Page Turn?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/15))

### 5.4 Room for Page Edges and drawing them

- **Room (hook 4, start of `Relayout`):** add the whole-book thickness to `windowMargin.left/right` in a Paged View with the switch on and not in Presentation. Remember what was added: if `windowMargin` no longer matches, `SetUiDpi` or Presentation reset it, and the current value is the new base.
- **Cover flag:** set the `pageBesideSpine` flag in the same hook.
- **Re-layout:** switching the setting or view calls `dm->RelayoutKeepingView()`.
- **Drawing (hook 5):** resting Page Edges are drawn with GDI into SumatraPDF's back buffer, after the background and before the pages. The layer draws the same lines while Curls play.

([Where do Page Edges go when the Spread fills the window?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/10); ADR [0004](adr/0004-room-for-page-edges-and-cover-beside-spine.md))

### 5.5 The Curl layer

ADR [0001](adr/0001-curls-on-their-own-layer.md) covers the choice; [0005](adr/0005-repaint-waits-for-first-curl-frame.md) covers the flash rule.

**The layer**
- A DirectComposition visual on `hwndCanvas`, with `topmost = FALSE`, so form and annotation edit boxes stay on top.
- A composition swap chain, drawn with Direct3D 11 as a cylinder/cone mesh textured with page pictures, clipped to the document area.
- **Default graphics adapter** (not the discrete GPU).
- `dcomp.dll`, `d3d11.dll` and `d2d1.dll` are loaded by hand. `dcomp.h` needs the `NTDDI_VERSION` trick from `src/shared/PrintWin11.cpp:34-52`. If anything fails, Page Turns are instant jumps ([3.11](#311-window-changes-mid-curl-and-where-curls-cant-play)).

**The drawing thread**
- One drawing thread per window owns Direct3D and every `Present`, fed through a locked command queue.
- When idle it sleeps. While Curls play it waits on the swap chain's frame-latency handle, draws from the clock, then `Present(1, 0)`.
- No `SetTimer`, no compositor clock.

**Setup:** Direct3D and the layer are set up on the drawing thread at the first Paged View paint with the switch on (25-40 ms there, 0.1 ms on the UI thread). Shaders are **precompiled** with `fxc` by a one-off committed script, and the bytes are committed; re-run it only when a shader changes.

**Showing the layer**
- The layer is shown **on demand**, not kept attached.
- **The no-flash rule:** the posted task waits until the swap chain's next frame signal says the layer's first frame is on screen (9-12 ms, at most ~35 ms at 4K). Only then does SumatraPDF's repaint run.
- **Removing it:** the layer goes see-through, then away, one screen refresh after SumatraPDF really shows the new Spread or its message (hook 6, `flushed == true`). Until then, uncovered parts are blank paper. The layer's last frame matches SumatraPDF's pixel for pixel.

**Screen copies**
- GDI copies straight into a GDI-compatible GPU image, on the drawing thread, while the UI thread waits.
- Copy the old Spread and only the landing page of the new one. Never keep a pointer into `win->buffer` (it's recreated on resize).
- At 4K a copy costs about a frame. Spread copies between frames where possible.

**Memory:** free each picture when its Curl lands, within the budget in [section 4](#4-numbers-in-one-place). Sharing pictures between consecutive Curls is optional.

**Window changes:** subclass the canvas (`SetWindowSubclass`: `WM_SIZE`, `WM_NCDESTROY`, ...) to land Curls instantly and resize or destroy the layer ([3.11](#311-window-changes-mid-curl-and-where-curls-cant-play)).

**Background reading**
- [Which drawing technology can show a realistic Curl smoothly?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/6)
- [Can a Curl layer run smoothly on this PC?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/14)
- [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19)
- Working spike code to start from: the throwaway branches `spike/curl-layer` and `spike/no-flash` (the latter has the frame recorder, `analyze.py`, and a full hand-off).

### 5.6 Rough Pages

ADR [0002](adr/0002-rough-pages-from-a-background-copy.md) covers the choice.

**Where they come from**
- Never through SumatraPDF's render queue.
- One worker thread per window, at below-normal priority, draws them from a **background copy of the document** (`Clone()`, as `src/PageThumbnails.cpp` does).

**The wish list** (rebuilt on every tap and landing)
- **One at a time.** At most 10 faces: oldest Curl first, and within a Curl the face that shows first.
- **Skip** faces that already have a sharp picture or a Rough Page, and everything inside a Bundle.
- **Cancel by forgetting:** when a Curl lands or a face leaves the screen, drop it. If it's the one being drawn, abort it and throw the result away. Faces of the Spread landed on stay until SumatraPDF draws them sharp.
- **On 2 or fewer logical processors:** start one only while `RenderCache::IsBusyFor(dm)` is false.

**Size:** half the page's on-screen size, long side at most ~600 px, same shape. Not adaptive, not a setting.

**Colours:** dark mode, grayscale and brightness are applied by helpers **copied** from `src/shared/RenderCache.cpp:54-160` into `src/skeuomorph/`.

**The background copy**
- Made when a document shows in a Paged View with the switch on and the layer loaded.
- Closed when it leaves the screen (tab closed or switched away from, Continuous View, switch off). Made fresh after a save or reload.
- Its image cache is emptied after each Rough Page.
- Unsaved annotation edits may be missing on flipped-past pages; accepted.
- No brake for heavy files.
- If the copy can't be made, faces show blank paper.
- Check why some memory isn't returned when the copy closes (seen in testing); don't block on it.

**Background reading**
- [How can Rough Pages share the render queue without slowing the landing?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/18)
- [How fast do Rough Pages arrive, and how much memory do Curls need?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/20)
- [Is a background copy of each document acceptable for Rough Pages?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/22)
- [Should Rough Pages be smaller in big windows?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/25)
- Spike: `spike/rough-pages`.

### 5.7 Checks

**Unit tests** (debug, `-unit-tests`) cover every agreed number and rule in [section 4](#4-numbers-in-one-place) and [3.2](#32-which-page-changes-are-a-page-turn), plus right-to-left direction.

**Scripts:** `tests/skeuomorph-*.ts` press keys and check the per-turn log line.

**Flash recorder:** a test-only tool, ported from `spike/no-flash` in step 4. Used by steps that change what's on screen.

**Pixel comparison:** once, after step 3, against the version from before:
- every view, switch off;
- Continuous views also with the switch on;
- must match pixel for pixel.

The script stays in the repo for reruns.

**Not done:** running the ~700 existing scripts, building ng.

([How do we check it works and nothing else changed?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/21))

---

## 6. Out of scope

- **Continuous views:** pages don't jump there, so there's nothing to turn.
- **Grab and drag:** pulling a page corner by hand. Could be a later effort.
- **Contributing upstream.**
- **Sound and Page Flags:** saved as their own ideas.

---

## 7. Build steps

Issues are sub-issues of [Build: Skeuomorph Page Turn](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/26), each blocked by the one before. "Lines" is the step's share of the original-line budget.

| Step | Delivers | Spec sections | Lines |
|---|---|---|---|
| 1. [Switch, speed setting and test shelf](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/27) | bun installed; both settings; View-menu item (hidden in ng); empty `src/skeuomorph/` in the build; test-shelf script | 1, 3.1, 5.2 #9-15 | 29 |
| 2. [Page Turn detection, no drawing yet](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/28) | seam; start-of-turn hook; one action = one turn; don't-animate fingerprint; Curl/Bundle/nothing; fast-tap timing; at most 5; right-to-left direction; per-turn log; unit tests; first scripts | 3.2, 3.4, 3.9, 4, 5.2 #1-3, #6, #8, 5.3 | 9 (+2 tests) |
| 3. [Page Edges at rest and the cover beside the spine](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/29) | room beside the Spread; resting Page Edges (Single Page unread only, mirrored, none in Presentation); cover and lone last page beside the spine; switch on/off re-layout; pixel comparison | 2, 3.6, 3.7 (layout), 3.9, 5.2 #4, #5, #7, 5.4 | 8, 1 changed |
| 4. [One plain Curl with no flash](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/30) | Curl layer; drawing thread; precompiled shaders; screen copies; holding the repaint; a simple cylinder Curl, one page at a time; land instantly on window changes; instant jump where the layer can't load; flash recorder | 3.10 (1, 2, 4), 3.11, 5.5, 5.7 | 0 |
| 5. [The real Curl look](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/31) | corner peel, shadow, shading, 3D depth; backward turns; Single Page fade; right-to-left mirroring; different-size pages changing shape; wide pages folding at the spine | 3.3, 3.8, 3.9 | 0 |
| 6. [Several Curls at once and fast tapping](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/32) | up to 5 in the air; gap × 4 / 310 ms; backward tap sends a page back; freeing pictures; memory budget; Release speed check | 3.4, 4, 5.5 (memory) | 0 |
| 7. [Bundles](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/33) | sheets, fan, stiffness; Page Edges shrink and grow (drawn by the layer); splitting the last sheet; a new jump lands pages in the air; spine shadow; the cover's blank slate; EPUB page-count changes | 3.5, 3.6 (moving), 3.7, 3.8 | 0 |
| 8. [Rough Pages](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/34) | background copy; worker thread; wish list; ~600 px cap; copied colour helpers; image cache emptying; landing on a Rough Page; Release speed check on the test-shelf scans | 3.10, 5.6 | 0 |

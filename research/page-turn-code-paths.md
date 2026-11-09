# Where do Page Turns happen in the code?

Research for [#4](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/4) (map [#3](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/3)).
Source: this repo at commit `d66646860` (same code as branch `scrolling`). Classic Windows app only; `src/ng/` ignored.
Terms as in GLOSSARY.md on `scrolling` (Paged View, Spread, Page Turn, Curl, Bundle, Page Edges).

## In plain words

Every way of changing pages in a Paged View ends up in one function, `DisplayModel::GoToPage`.
At that moment the program still knows which Spread was showing and which one is coming, and the screen has not been redrawn yet.
So there is one good place to start a Curl or a Bundle.
The catch: the same function is also used for things that are not Page Turns (opening a file, changing the view, restoring a position), so the new code has to tell the two apart.
Right-to-left handling is small and lives in a few places; mirroring the Curl needs one yes/no value that already exists.

## 1. The funnel: `DisplayModel::GoToPage`

`void DisplayModel::GoToPage(int pageNo, int scrollY, bool addNavPt, int scrollX)` — `src/shared/DisplayModel.cpp:2932`.

What it does, in order:

1. Syncs with the engine's chapter layout (`:2933`), validates `pageNo` (`:2934-2938`).
2. Maybe records a Back/Forward history point (`:2942-2944`).
3. Facing / Book View: moves `pageNo` to the first page of its row, i.e. its Spread (`:2947-2949`, `RowStartForPage` at `:1182`).
4. **Paged View only: `ChangeStartPage(pageNo)` swaps the Spread** (`:2952-2955`). This is the only place in a Paged View where the shown Spread changes.
5. Sets the scroll position on the new Spread (`:2966-3016`).
6. Recomputes visible parts, asks for rendering, updates scrollbars, calls `cb->PageNoChanged(this, pageNo)` and `RepaintDisplay()` (`:3018-3025`).

`ChangeStartPage` (`src/shared/DisplayModel.cpp:2519-2547`):
- returns early when the Spread does not change (`:2525-2537`);
- otherwise sets `startPage`, marks only the new row's pages as shown (`isShown`) and relayouts (`:2539-2546`).
- It has exactly one caller: `GoToPage` (`:2955`).

Other writes to `startPage` are not Page Turns:
- `SetInitialViewSettings` at document open (`src/shared/DisplayModel.cpp:1524`);
- clamping after an EPUB restyle shrinks the page count (`BuildPagesInfo`, `:1570`).

In a Paged View `CurrentPageNo()` simply returns `startPage` (`src/shared/DisplayModel.cpp:1943-1948`), and scrolling inside a Spread (`ScrollYTo`/`ScrollXTo`/`ScrollYBy` without page change, `:3430-3536`) never changes it.

### Does the funnel know the old and new page?

Yes, though neither is passed as a parameter:
- **Old Spread**: `startPage` still holds it until `ChangeStartPage` runs (`:2539`); its pages are `FirstPageInRow(startPage)..LastPageInRow(startPage)` (`:1132`, `:1153`).
- **New Spread**: `pageNo` after `RowStartForPage` (`:2948`), pages `FirstPageInRow(pageNo)..LastPageInRow(pageNo)` (`:2521-2522`).
- **Direction** is `new > old`; **distance** (Bundle size) is the page difference.
- `addNavPt` is `true` for most jumps (Go to page, ToC, Home/End, thumbnails) and `false` for next/previous — a hint only, since search results and Back/Forward also pass `false`.

Facts that matter for drawing (details belong to [#5](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/5)):
- The repaint is **asynchronous**: `RepaintDisplay` (`:724-727`) calls `cb->Repaint()`, which is `ScheduleRepaint(win, 0)` (`src/SumatraPDF.cpp:950`), which posts a task (`src/Canvas.cpp:4882-4893`). When `GoToPage` returns, the window still shows the old Spread.
- After `ChangeStartPage` the old pages are no longer laid out (`isShown = false`, `:2541-2545`), so their on-screen positions are gone. A Curl needs the old Spread captured (pixels or positions) **before** `ChangeStartPage`.
- Several `GoToPage` calls inside one message only paint the last result (see "Coalescing" below).

### Existing UI-side notification

`ControllerCallbackHandler::PageNoChanged` (`src/SumatraPDF.cpp:1484-1556`) is called from `GoToPage` (`DisplayModel.cpp:3023`), from scrolling (`:3443`, `:3464`) and from `SetScrollState` (`:4098`). It compares the new page with `win->currPageNo` (`src/SumatraPDF.cpp:1500`) and stores it at `:1551`. It also fires in Continuous Views and for same-page calls, and gets called *after* the Spread is swapped, so it is a weaker hook than `GoToPage` itself.

## 2. Every path that turns a page

All of these end in `DisplayModel::GoToPage`. "Next/Prev" means `DisplayModel::GoToNextPage` (`src/shared/DisplayModel.cpp:3147-3206`) / `GoToPrevPage` (`:178-180`, `:3230-3310`), which compute the next Spread (incl. EPUB chapters, `:3159-3181`, `:3232-3256`) and call `GoToPage` (`:3180`, `:3204`, `:3255`, `:3304`, `:3308`).

### Next / previous

| Trigger | Where | Path |
|---|---|---|
| `N` / `P` keys, toolbar Next/Prev buttons, menu | `src/shared/Accelerators.cpp:54,57`; `src/shared/ToolbarCommon.cpp:426-427`; `src/shared/MenuDefs.cpp:406,410` | `CmdGoToNextPage`/`CmdGoToPrevPage` → `src/SumatraPDF.cpp:11531-11546` → Next/Prev, **repeated `n` times** for `CmdGoToNextPage n=3` (`:11535-11542`) |
| Page Down / Space / Enter / Ctrl+Down (and up equivalents) | `src/shared/Accelerators.cpp:43-52` | `CmdScrollDownPage`/`UpPage` → `src/SumatraPDF.cpp:11573-11590` / `11484-11501`: scroll first, Next/Prev if nothing scrolled |
| Shift+Up/Down (half page) | `src/shared/Accelerators.cpp:37-38` | `src/SumatraPDF.cpp:11466-11481`, `11556-11571` |
| Up / Down arrows | `src/shared/Accelerators.cpp:32-33` | `src/SumatraPDF.cpp:11503-11529`: Next/Prev when the Spread fits the window |
| Left / Right arrows | `src/shared/Accelerators.cpp:34-35` | `src/SumatraPDF.cpp:11593-11643` → `DisplayModel::GoToPageHorizontal` (`DisplayModel.cpp:715-722`, R2L-aware) |
| Canvas scrollbar steps (Single Page with `ScrollbarInSinglePage`) | `src/Canvas.cpp:414-455` | `ctrl->GoToPage(targetPage, true)` (`:450`), incl. thumb dragging (`SB_THUMBTRACK`, `:443-445`) |
| Mouse wheel (page fits, Fit Content, `MouseWheelTurnsPage`) | `src/Canvas.cpp:3801-3857` | `WheelTurnPage` (`:3633-3642`) → Next/Prev |
| Mouse wheel past the edge of a zoomed page (`ScrollEdgeTurnsPage`) | `src/Canvas.cpp:4000-4040` | Next/Prev (`:4028-4037`); also `ScrollYBy(…, true)` (`:3879`, `:3913`) → `DisplayModel.cpp:3502-3524` → Next/Prev |
| Wheel throttle for comics | `src/Canvas.cpp:3664-3687` | blocks a wheel turn until the current page is rendered and 250 ms passed |
| Click left/right fifth of the page (`ClickEdgeToTurnPage`) | `src/Canvas.cpp:2034-2059` | Next/Prev, sides swapped for R2L (`:2042-2047`) |
| Presentation mode clicks | `src/Canvas.cpp:2061-2069`, `2289-2296` | Next/Prev |
| Touch flick left/right | `src/Canvas.cpp:4228-4246` | `GoToPageHorizontal` (R2L-aware) |
| Reading auto-scroll | `src/shared/ReadingAutoScrollCommon.cpp:425-436` (arrow keys), `:501` (`ScrollContentBy(dy, true)` → `ScrollYBy`) | Next/Prev |
| `B` key (Facing ↔ Book View) | `src/shared/SumatraPDFCommon.cpp:3112-3140` | switches display mode, then Next/Prev |
| DjVu internal next/prev links | `src/shared/EngineDjvuDec.cpp:791-795` | `LinkHandler::GoToNextPage`/`GoToPrevPage` (`src/shared/MainWindowCommon.cpp:255-269`) |

### Jumps (candidates for a Bundle)

| Trigger | Where | Path |
|---|---|---|
| Home / End (Ctrl+Home/End) | `src/shared/Accelerators.cpp:60-63`; `src/SumatraPDF.cpp:11646-11671` | `GoToFirstPage` / `GoToLastPage` (`DisplayModel.cpp:3333-3352`, `3312-3331`) → `GoToPage(…, true)` |
| Go to page dialog (Ctrl+G) | `src/GoToPageDialog.cpp:113-130` | `GoToPage` / `GoToLocation` (`DisplayModel.cpp:3422-3428`) |
| Page number box in toolbar | `src/Toolbar.cpp:2905-2921` | `GoToPage` / `GoToLocation` |
| Table of contents click | `src/TableOfContents.cpp:127-151` | `ctrl->HandleLink` or `GoToPage(pageNo, true)` |
| Next/prev chapter | `src/shared/TableOfContentsCommon.cpp:478-495` | `GoToTocItem` → as ToC |
| Link inside the document | `src/Canvas.cpp:2011` | `ctrl->HandleLink` → engine (e.g. `src/shared/EngineMupdf.cpp:7945-7947`) → `LinkHandler::ScrollTo` (`src/shared/MainWindowCommon.cpp:271-278`, `344-372`) → `DisplayModel::ScrollTo` (`DisplayModel.cpp:4228-4330`) → `GoToPage` (`:4252`, `:4307`, `:4321`, `:4324`) |
| Named destinations (command line, DDE) | `src/shared/MainWindowCommon.cpp:397-440` | `ScrollTo` / `GoToPage` |
| JavaScript menu links | `src/MainWindow.cpp:584` | `GoToPage` |
| Search results (Find) | `src/shared/SearchAndDDECommon.cpp:1159-1182` | `GoToPage(…, false)` when the match is not shown (`:1168-1173`), then `ShowResultRectToScreen` (only scrolls, `DisplayModel.cpp:3923-3972`) |
| Forward search (LaTeX sync) | `src/SearchAndDDE.cpp:287-289` | `GoToPage(page, true)` |
| DDE `[GoToPage(...)]` | `src/SearchAndDDE.cpp:508-510` (doc); `src/shared/SearchAndDDECommon.cpp:815-832` | `GoToPage` / `SetScrollState` |
| Back / Forward (Alt+Left/Right, Backspace) | `src/shared/Accelerators.cpp:65-68`; `src/SumatraPDF.cpp:12340-12350` | `DisplayModel::Navigate` (`DisplayModel.cpp:4188-4207`) → `SetScrollState` (`:4035-4051`) → `GoToPage` |
| Favorites | `src/shared/FavoritesCommon.cpp:138-168` | `GoToLocation` or `SetScrollState` → `GoToPage` |
| Page thumbnails sidebar | `src/PageThumbnails.cpp:460-470` | `dm->GoToPage(pageNo, 0, true)` |
| Selecting an annotation on another page | `src/shared/AnnotEditToolbarCommon.cpp:930-941` | `GoToPage(pageNo, true)` |
| Read Aloud following the voice | `src/shared/ReadAloudSession.cpp:1090-1101` | `GoToPage(pageNo, false)` |
| Read Columns (Single Page) | `src/shared/DisplayModel.cpp:565-576` | `ShowColumn` → `GoToPage` |

Not Paged Views, so out of scope: CHM, Markdown and browser documents use their own `GoToPage` (`src/shared/ChmModel.cpp:75`, `src/shared/MarkdownModel.cpp:419`, `src/shared/DocController.cpp:165-272`), not `DisplayModel`.

## 3. `GoToPage` calls that are NOT Page Turns

The funnel also runs when nothing was turned. A hook has to ignore these:

| Caller | Where | Why it is not a Page Turn |
|---|---|---|
| Opening a document / session restore | `src/SumatraPDF.cpp:2295`, `14855`, `1871`, `14786` | first Spread; map says "opening shows no turn" |
| Switching display mode (Single/Facing/Book, presentation, fullscreen) | `DisplayModel::SetDisplayMode` `src/shared/DisplayModel.cpp:3028-3069`; `SetInPresentation` `:3071-3095` | re-pairs pages; Book→Facing even shifts the page (`:3043-3045`) |
| Toggling "facing: step one page" | `SetFacingStepOne` `src/shared/DisplayModel.cpp:311-321` | same start page, different pairing |
| Zoom / rotation | `SetZoomVirtual` `src/shared/DisplayModel.cpp:3661`; `RotateBy` `:3843-3854` | same page |
| Chapter layout finished (EPUB) | `SyncWithEngineLayout` `src/shared/DisplayModel.cpp:733-765` → `SetScrollState` (`:762`) | **flat page numbers shift** while the Location stays the same |
| Relayout keeping the view | `RelayoutKeepingView` `src/shared/DisplayModel.cpp:2358-2361` | same view |
| Toggles: manga mode, uniform width, trim margins, free pan | `src/shared/SumatraPDFCommon.cpp:1098-1107`; `src/SumatraPDF.cpp:8025-8054` | same page restored |
| Tab switch / window resize | `src/SumatraPDF.cpp:4496-4504` | same page restored |
| "Fully show the current page" | `GoToNextPage` `src/shared/DisplayModel.cpp:3183-3188` | same page |
| Columns whole-page toggle | `src/shared/DisplayModel.cpp:620-636` | same page |

Most of these keep the same Spread, so `ChangeStartPage` returns early and comparing old vs. new Spread filters them. Three do change the Spread or its page numbers without the reader turning: **display-mode switches**, **`SetFacingStepOne`**, and **EPUB chapter re-numbering**. A simple old/new check is not enough for those; they need either a "same display mode and same layout generation" condition or an explicit "don't animate" flag around them. The code already has a similar flag: `stableNavPoint.suppress` (`DisplayModel.cpp:4049`, `SearchAndDDECommon.cpp:1170-1172`), which stops Back/Forward history from recording restores.

## 4. Coalescing and fast tapping

- Repaint is posted, not immediate (section 1), so several `GoToPage` calls in one message show up as one jump on screen: `CmdGoToNextPage n=3` (`src/SumatraPDF.cpp:11536-11542`), the `B` key (mode switch + turn), and link `ScrollTo` (which can call `GoToPage` twice, `DisplayModel.cpp:4251-4252` and `4321-4324`).
- Holding a key gives repeated `WM_KEYDOWN` → one `GoToNextPage` per repeat. Nothing in the code detects "held" or "fast" tapping, except the comic wheel throttle (`src/Canvas.cpp:3664-3687`). Speed-up has to be measured by the new code (time between turns).
- Scrollbar thumb dragging in Single Page (`src/Canvas.cpp:443-450`) makes many jumps in a row.

### Spreads that don't behave like paper

- **Step one page** (`CmdToggleFacingStepOne`, `facingStepOne`, `src/shared/DisplayModel.h:431`; `StepsOnePage` `src/shared/DisplayModel.cpp:1127-1130`): in Facing / Book View next goes 1-2 → 2-3 → 3-4 (`:3197`). The right page becomes the left page; a real book cannot do that.
- **Landscape pages as their own Spread** (`ShouldTreatLandscapeAsSpread`, `src/shared/DisplayModel.h:300`; row tables `src/shared/DisplayModel.cpp:1141-1150`): a Spread can be one wide page between two-page Spreads.
- **EPUB chapters** are laid out lazily, so `PageCount()` and flat page numbers change while reading (`src/shared/DisplayModel.cpp:733-765`). Page Edges thickness and Bundle size would shift as chapters get laid out.

## 5. Right-to-left in Paged Views

Where the flag comes from:
- PDF `/ViewerPreferences /Direction /R2L` (`src/shared/EngineMupdf.cpp:4876-4879`, `5270-5272`), EPUB page-progression-direction (`src/shared/EngineEbook.cpp:1085-1087`); stored in `PageLayout::r2l` / `r2lDeclared` (`src/shared/EngineBase.h:71-75`).
- Copied into `DisplayModel::displayR2L` at open (`src/shared/DisplayModel.cpp:1548`), then overridden by the remembered per-file value or the `ComicBookUI.CbxMangaMode` default (`src/SumatraPDF.cpp:2114-2127`). Saved per file (`DisplayModel.cpp:971`, `src/Settings.cpp:898`).
- Toggled by the user with Manga Mode (`ToggleMangaMode`, `src/shared/SumatraPDFCommon.cpp:1098-1107`; menu check `src/Menu.cpp:870`).
- Read with `dm->GetDisplayR2L()` (`DisplayModel.cpp:342`).

What it changes:
1. **Layout**: in Facing / Book View the two pages of a Spread swap sides (`src/shared/DocumentLayout.cpp:316-318`). Single Page layout is unchanged.
2. **Direction of left/right input**: arrow keys (`src/SumatraPDF.cpp:11603-11636` via `GoToPageHorizontal`, `DisplayModel.cpp:715-722`), edge clicks (`src/Canvas.cpp:2042-2047`), touch flicks (`src/Canvas.cpp:4228-4242`).
3. Auto-rotation direction of wide comic pages (`DisplayModel.cpp:236`).

Logical next/previous (N/P, Page Down, Space, wheel, Home/End) do not care about direction; only the side of the screen does.

Not the same thing: **Mirror** (`CmdToggleMirror`, `DisplayModel.cpp:327-336`) flips the page *content* left-to-right when drawn (`src/Canvas.cpp:2909-2930`, `src/shared/RenderCache.cpp:1627-1632`, coordinate flip `DisplayModel.cpp:2666`, `2696`). It does not change reading direction.

### What mirroring Curls, Bundles and Page Edges would touch

- **Curl / Bundle**: forward in LTR lifts the right edge and turns it to the left; in R2L it lifts the left edge and turns it to the right. That is one sign flip in the new animation code, driven by `GetDisplayR2L()` plus the direction (old vs. new page) known in `GoToPage`. No original file needs to change for it.
- **Page Edges**: read pages sit on the left in LTR and on the right in R2L. Same flag, same sign flip, in new drawing code.
- **Single Page in R2L**: layout ignores R2L (`DocumentLayout.cpp:316` has `!single`), but the Curl should probably still go the R2L way, since the book reads that way. A small decision for [#12](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/12).
- **Interactions** to decide: Mirror on top of R2L, and rotated Paged Views (90°/270°) where "left/right" is on screen but the book's spine is top/bottom.

## 6. Answers to the ticket

- **Every path?** Yes: listed in section 2. All end in `DisplayModel::GoToPage` (`src/shared/DisplayModel.cpp:2932`), and the Spread itself only changes in `ChangeStartPage` (`:2519`), whose only caller is `GoToPage` (`:2955`).
- **One place for a Curl or Bundle?** Yes: inside `GoToPage`, just before `ChangeStartPage` (`:2952-2955`), with a one-line call into new code in a new file. Old Spread = `startPage`, new Spread = `pageNo`; direction and distance follow; the screen still shows the old Spread.
- **Catch:** that place also runs for non-turns (section 3). Display-mode switches, `SetFacingStepOne` and EPUB re-numbering need an extra condition or a "don't animate" flag; same-Spread calls filter themselves out.
- **Right-to-left:** one existing flag (`GetDisplayR2L()`) decides the direction. Mirroring Curls, Bundles and Page Edges is a sign flip in new code. Small.

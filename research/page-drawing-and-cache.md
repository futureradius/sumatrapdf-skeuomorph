# How SumatraPDF draws pages and keeps their images

Research for [#5](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/5) (map [#3](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/3)).
Classic Windows app only; `src/ng/` ignored. Line numbers are for commit `d66646860` (same source as `scrolling` at `1d0b75378`, which only adds `GLOSSARY.md`).
Terms follow [GLOSSARY.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/scrolling/GLOSSARY.md).

## In plain words

SumatraPDF never draws a page straight to the screen. Background threads turn each page into a picture, keep up to 256 of those pictures in memory, and the screen just copies the finished pictures. When you turn a page, it also quietly prepares the neighbouring Spreads, so a normal Page Turn usually finds the next pages ready. A far jump (Bundle) lands on pages that aren't ready, and while they render, SumatraPDF keeps showing the old Spread for up to half a second. After that it shows "Rendering page N...".

## 1. From document to screen

| Step | What happens | Where |
|---|---|---|
| Navigate | `GoToPage` → `ChangeStartPage` marks only the target Spread's pages as shown, re-lays out, then `RenderVisibleParts()` and `RepaintDisplay()` | `src/shared/DisplayModel.cpp:2932-3026`, `2519-2547` |
| Request | `RenderVisibleParts` requests every visible page, then a predictive chain, then the visible pages again so they go first | `DisplayModel.cpp:2788-2849` |
| Queue | `RenderCache::RequestRendering` → `Render` puts a `PageRenderRequest` in an 8-slot queue (oldest dropped when full) and wakes a thread | `src/shared/RenderCache.cpp:845-914`, `992-1081`; `RenderCache.h:11` |
| Render | Worker threads (spawned lazily, at least 8, up to 32) take the **newest** request first (LIFO), call `engine->RenderPage`, apply grayscale / brightness / dark-mode recolor, then `cache->Add` | `RenderCache.cpp:225-227`, `1105-1124`, `1388-1495`, `1497-1532` |
| Notify | `DisplayModel::RenderFinishedAsync` posts to the UI thread → `RenderFinished` → `RepaintDisplay()` → `ScheduleRepaint(win, 0)` → canvas invalidated | `DisplayModel.cpp:907-939`, `724-727`; `src/SumatraPDF.cpp:950`; `src/Canvas.cpp:4868-4893`, `4903-4911` |
| Paint | `WM_PAINT` → `OnPaintDocument` → `DrawDocument` into an off-screen buffer → `buffer->Flush(hdc)` | `Canvas.cpp:4513-4521`, `3308-3332` |

### Painting (`OnPaintDocument` / `DrawDocument`)

- `OnPaintDocument` draws the whole canvas into `win->buffer`, a per-window 32-bit DIB `DoubleBuffer` (`src/base/Win.cpp:1173-1229`, created in `src/MainWindow.cpp:336-337`), then BitBlts it to the screen (`Canvas.cpp:3320-3326`).
- `DrawDocument` (`Canvas.cpp:2941-3261`) paints the background around the pages (`3018-3098`). For each visible page it paints the frame and shadow in the placeholder colour (`3116-3126`), then the page bitmap via `PaintPageBitmap` → `gRenderCache->Paint` (`2914-2939`, `3142`). On top come annotations, selection, find matches, reading bar and similar overlays (`3198-3253`).
- `RenderCache::Paint` (`RenderCache.cpp:1670-1780`) walks the page's tiles. `PaintTile` (`1563-1655`) looks the tile up with `Find` and draws it with `BlitPixmapRegion`, a plain GDI BitBlt/StretchBlt (`src/base/Pixmap.cpp:509`). Painting from cache is therefore just a bitmap copy. It is cheap.
- `PaintPageBitmap` already shows how to paint a page into a **separate memory DC** (used for mirroring): `CreateCompatibleDC` + `gRenderCache->Paint(memDC, …)` (`Canvas.cpp:2922-2937`).

## 2. The cache of finished page images

- **What's stored:** `BitmapCacheEntry` = (DisplayModel, pageNo, rotation, zoom, tile, dark-mode epoch) + a `Pixmap` (`RenderCache.h:60-92`). On Windows a Pixmap is usually a GDI DIB section (`hbmp`), BGRA or a palette format that only GDI can read (`src/base/Pixmap.h:26-59`).
- **Tiles:** a page is split into 4^res tiles, each at most screen-sized (`RenderCache.h:39-55`, `maxTileSize` = screen size, `RenderCache.cpp:201-207`). At Fit Page or Fit Width, or when a page is smaller than the window, tiles are made larger, so a normal Paged View page is **one tile (res 0)**, sometimes 2×2 (res 1) on large screens (`RenderCache.cpp:741-775`). Plain requests and predictive requests are **skipped when res > 1**, i.e. when zoomed in far (`831-833`, `945-947`).
- **Size:** fixed 256 entries (`RenderCache.h:16`). When full, `FreeIfFull` evicts, in order: a page of the same document not "visible nearby", a tile at a stale zoom or rotation, an off-screen tile, then any page of another document (`RenderCache.cpp:378-440`).
- **"Visible nearby"** = the current Spread plus one Spread before and after (`DisplayModel.cpp:1634-1645`).
- **Memory trimming:** every cached bitmap is also registered with `CachedObjects` (`RenderCache.cpp:509-518`). After each add, `TryFreeCachedObjects` may free bitmaps of pages not visible nearby (`RenderCacheCanFree`, `RenderCache.cpp:456-467`). How much depends on `SaveMemory` (default 50) and the OS memory load: pressure = memLoad + 50 − 100, so trimming starts above 50 % system memory use (`src/shared/CachedObjects.cpp:85-110`, `172-215`; default in `cmd/gen-settings.ts:1954-1959`).
- **Reference counting:** `Find` bumps `refs`, and `DropCacheEntryIfNotUsed` won't free an entry with `refs > 1` (`RenderCache.cpp:298-317`, `370-376`). Exception: `ReduceTileSize` (called after an allocation failure) deletes **every** entry regardless of refs (`791-820`, `1596`). A held pointer can therefore dangle.

### Predictive rendering

After the visible pages, `RenderVisibleParts` chains: last visible + 1 and first visible − 1. In Facing and Book View it adds last + 2 and first − 2 (`DisplayModel.cpp:2815-2840`). They render one at a time and the chain stops once its origin page is no longer visible (`RenderCache.cpp:922-965`, `DisplayModel.cpp:935-938`). Zoom for a not-yet-shown page is computed for the current mode (`DisplayModel.cpp:2101-2135`), so a predicted page is cached at the zoom it will be shown at. **Both neighbouring Spreads are normally in the cache shortly after landing.** A debug command can switch this off (`SumatraPDF.cpp:12071-12074`).

## 3. Can a Curl get its three images from the cache?

| Image | Adjacent Page Turn | Far jump (Bundle) |
|---|---|---|
| Current Spread | Yes: visible, so cached. It is also exactly what's in `win->buffer` after the last flush | Same |
| Spread being turned to | Usually yes, from predictive rendering, if the reader paused long enough and zoom is at most res 1 | **No.** Requested only after the jump (`GoToPage` → `RenderVisibleParts`) |
| Back of the turning page | Facing / Book View: it's the first page of the next Spread, so it's predicted. Single Page: there is no real "back"; page N+1 is the only candidate (decision needed) | The Bundle's underside is the target Spread's first page, so it's not cached until rendered |

Points that matter for the design:

1. **The cache doesn't hand out "page pictures".** It holds tiles keyed by zoom and rotation. `Paint()` asserts the page is shown and visible (`RenderCache.cpp:1677`), and tile positions come from `pageOnScreen`, which is only set for the shown Spread. A page that isn't shown can't be painted through the normal path.
2. **A simpler source exists: whole frames.** `win->buffer` holds the last full frame: pages, background, overlays (`Canvas.cpp:3320`). A Curl could copy the frame **before** the jump, let SumatraPDF jump normally (state, layout and caches unchanged, which honours the map's ground rule), then copy the frame **after** the target Spread is drawn. In Facing and Book View the back of the turning leaf (or the Bundle's underside) is the target Spread's first page, so it is in the "after" frame. All three images then come from two frames, and the cache isn't touched. It also matches "zoomed in, the Curl plays on what's on screen."
3. **Ready signal:** `RenderCache::VisibleTargetTilesReady(dm)` says when every on-screen tile is at final resolution (`RenderCache.cpp:1247-1326`). `IsBusyFor(dm)` says whether a visible page is still rendering (`1223-1242`).
4. **Don't hold cache entries across frames:** eviction, trimming and `ReduceTileSize` can free them (see §2). Copy the pixels instead.
5. **Fast tapping:** each tap's `GoToPage` re-requests, aborts in-flight renders of pages no longer nearby (`RenderCache.cpp:853-858`), and the 8-slot LIFO queue drops the oldest requests (`1013-1022`). Spreads that are flown past quickly may never render. Several Curls in the air must cope with a missing image (placeholder or a lower-resolution stand-in).

## 4. How long does rendering take?

- The code treats > 300 ms as slow and logs it (`RenderCache.cpp:1438-1442`).
- Measured here with the repo's **debug** build (`SumatraPDF.exe -for-testing -console -bench <file>`, which renders each page at 100 % zoom on the UI thread, `src/shared/StressTesting.cpp:98-120`):
  - `tests/issue-6132.pdf` (5 pages): 5, 13, 33, 11, 36 ms per page.
  - `tests/issue-5871.pdf` (1 page): 28 ms.
  - Release builds are faster. Large scanned pages, 4K screens (more pixels) and slow machines are slower, possibly hundreds of ms. Not measured; worth a check in the "speed and load" ticket.
- Painting from the cache is only a BitBlt per tile. `OnPaintDocument` reports its own duration (`Canvas.cpp:3331`).

## 5. What shows while a page is still rendering

`PaintTile` returns a "render delay" instead of 0 when it has no bitmap (`RenderCache.cpp:1575-1608`). Then, in order:

1. **A stand-in if one exists:** the same tile at another zoom, stretched. Not for comics or images, and not in remote sessions (`1576-1586`). Lower-resolution tiles are painted first and higher ones on top (`1725-1768`). An out-of-date bitmap gets a small "reloading" icon (`Canvas.cpp:3178-3195`).
2. **Otherwise nothing new reaches the screen for up to 500 ms.** `gNoFlickerRender` is on (`Canvas.cpp:81-84`). If no page painted successfully, the buffer isn't flushed, so **the old Spread stays on screen** and a repaint is scheduled for the 500 ms mark (`Canvas.cpp:3149-3154`, `3324`, `kRenderDelayShowNotif` at `98`). The source comment says this "reduces the flickering … but creates impression of lag".
3. **After 500 ms:** a page-coloured placeholder with "Rendering page N..." (`3155-3165`). On failure: "Couldn't render page N" (`3128-3139`, `3167-3172`).
4. In Facing / Book View, if one page of the Spread is ready and the other isn't, the frame **is** flushed. The missing page shows as an empty placeholder (`3143-3148`).

Each finished render triggers another repaint (`DisplayModel.cpp:916-930`), so pages pop in as they finish.

## 6. What happens to the cache on a far jump

- Nothing is freed at the jump itself. `GoToPage` → `ChangeStartPage` only changes which pages are shown (`DisplayModel.cpp:2519-2547`).
- New requests for the target Spread **abort in-flight renders** of pages no longer visible nearby, and the old predictive chain stops because its origin page is gone (`RenderCache.cpp:853-858`, `927-930`).
- The old Spread's bitmaps stay in memory but immediately become **freeable**: by memory trimming after any later cache add (when system memory load > 50 %), or by eviction once 256 entries are reached (`RenderCache.cpp:378-398`, `456-467`). Jumping back soon is often instant. It isn't guaranteed.
- The target Spread renders from scratch (unless cached from earlier), then its neighbours are predicted.

## Things that suggest new decisions

- **Image source for Curls:** frame snapshots of `win->buffer` (before and after the jump) versus reading the RenderCache. Snapshots look simpler and leave the original code nearly untouched.
- **Jump first, animate over snapshots:** SumatraPDF's state jumps instantly and the Curl is pure decoration over it. This fits the ground rule. It needs a decision on what's shown before the target is rendered (wait up to N ms? curl onto a placeholder?).
- **Single Page back side:** there is no natural back page; choose page N+1, blank paper, or a faded mirror of page N.
- **Facing "step one page"** (`FacingStepOne`, `DisplayModel.cpp:1127-1130`, `3196`): 2-3 → 3-4 moves page 3 from right to left. That isn't a leaf turn, so it's an odd case for the map.
- **Landscape pages treated as their own Spread** (`ShouldTreatLandscapeAsSpread`, `DisplayModel.cpp:1143-1150`): another odd case.
- **Fast tapping and missing images:** intermediate Spreads may never render; the Curl needs a fallback.

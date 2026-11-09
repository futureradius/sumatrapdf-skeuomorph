# How Rough Pages can be prepared without slowing the landing

Research for [How can Rough Pages share the render queue without slowing the landing?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/18) (map: [Map: Skeuomorph Page Turn](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/3)).
Builds on [How does SumatraPDF draw pages and keep their images?](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/page-drawing-and-cache/research/page-drawing-and-cache.md) and the decision in [What does a Curl show if a page is not ready yet?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/11).
Classic Windows app only; `src/ng/` ignored. Line numbers are for `scrolling` at `1d0b75378` (same source as `d66646860`).
Terms follow [GLOSSARY.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/scrolling/GLOSSARY.md).

## In plain words

The shared render queue is a poor fit for Rough Pages, and the route chosen in the earlier ticket doesn't work the way it was assumed:

- A picture asked for through `RenderCache::Render(..., callback)` is **put into SumatraPDF's own cache, and the callback gets nothing**. The new code would never receive its Rough Page.
- A Rough Page that is being drawn is **cancelled by the next thing SumatraPDF asks to draw**, if its page is more than one Spread away from the current one. After a few fast taps, that is true of the older Curls, which are exactly the ones meant to go first.
- Every page of one PDF is drawn **one at a time**, whatever the number of threads, because of one engine-wide lock. A Rough Page of a big scan that is still being drawn **holds up the Spread you land on** until it finishes.

The clean way out is the one SumatraPDF already uses for its page-thumbnail palette: **a separate, background copy of the document** (`EngineBase::Clone()`), drawn by **one worker thread of our own**. It never touches SumatraPDF's queue, its 8 slots, its cache or its lock, so it can't push out or slow down the landing. All of this fits in new files.

## 1. How the shared queue orders, replaces and drops requests

| Behaviour | Where |
|---|---|
| 8 slots (`kMaxPageRequests`) shared by everything: visible pages, look-ahead, thumbnails, any callback request | `src/shared/RenderCache.h:11` |
| **Full queue: the oldest request is thrown out** (its callback is told "aborted"), whoever asked for it | `src/shared/RenderCache.cpp:1013-1023`, `983-990` |
| **Newest first (LIFO):** a free thread always takes the last request added | `RenderCache.cpp:1112-1114` |
| SumatraPDF re-asking for a queued tile **swaps it to the top**, moving whatever was on top (possibly a Rough Page) down toward the "dropped first" end | `RenderCache.cpp:884-893` |
| Asking for page P clears other queued requests for P whose tile size differs | `RenderCache.cpp:879-882`, `1338-1359` |
| The look-ahead (next/previous Spread) is a chain: one page at a time, the next asked for when the previous finishes; it stops if its starting page is no longer shown | `RenderCache.cpp:916-965`; `src/shared/DisplayModel.cpp:931-939`, `2815-2840` |

So callback requests **can jump ahead**: added last, they're taken first. And they **can push out** SumatraPDF's own requests, including a look-ahead page, whenever the 8 slots are full. The reverse also happens: SumatraPDF's newer requests push Rough Pages out.

### The callback route is not a side door

`Render(dm, pageNo, rotation, zoom, pageRect, callback)` (`RenderCache.cpp:967-979`) goes through the same code as SumatraPDF's own tiles, with two surprises:

1. **The picture goes to the cache, not to the callback.** `RenderCacheRunOne` calls `cache->Add(req, bmp)` and then sets `req.bmp = nullptr` *before* calling the callback (`RenderCache.cpp:1487-1492`). This has been so since `cb24e507e` ("move bitmap caching back to RenderCacheThread", April 2026). The start-screen thumbnail code that uses this route (`src/SumatraPDF.cpp:1014-1046`) therefore receives `nullptr` too. From reading the code, that feature looks broken; this wasn't checked by running it.
2. **The request's tile is never set.** For a callback request, only `pageRect` is filled in (`RenderCache.cpp:1035-1042`). The `tile` field keeps whatever the reused slot held before: usually `{0,0,0}`, the same key as SumatraPDF's own tile for a Paged View page. As a result:
   - `cache->Add` → `FreePage(dm, pageNo, &tile)` can throw out SumatraPDF's own tile of that page, and the Rough Page then sits in the cache under SumatraPDF's key (`RenderCache.cpp:482-519`, `580-603`).
   - When SumatraPDF asks for the same page while the Rough Page is queued, it treats the Rough Page as "same tile, other zoom" and **rewrites that request** to full size while keeping the Rough Page's callback (`RenderCache.cpp:894-904`). SumatraPDF's own "done, repaint" notice (`DisplayModel::RenderFinishedAsync`, `DisplayModel.cpp:907-914`) then never comes for that page. This is one way a shared Rough Page can **delay the landing**.

Using this route as planned would mean changing original files (set the tile to "none", skip `cache->Add` for callback requests, and exempt them from the cancelling in §2).

## 2. Can a callback request be cancelled or ignored?

- **There is no call that cancels one request.** The only cancels are per document: `AbortRendering(dm)` and `CancelRenderingBlocking(dm)` (`RenderCache.cpp:1328-1336`, `1150-1173`). Both would also cancel SumatraPDF's own work.
- **Ignoring works:** a callback can just throw its result away. It runs **on the render thread**, not the UI thread (`RenderCache.cpp:1491-1492`), so the new code has to post to the UI thread itself, as `RenderFinishedAsync` does (`DisplayModel.cpp:907-914`).
- **Tapping *does* cancel Rough Pages that are being drawn.** Every `RequestRendering` aborts any in-flight request of the same document whose page isn't within one Spread of the current one (`RenderCache.cpp:853-858`; "nearby" = `DisplayModel.cpp:1634-1645`). That covers every tap (`GoToPage` → `RenderVisibleParts`, `DisplayModel.cpp:2812`, `2847`), every look-ahead step and every repaint that finds a missing tile (`RenderCache.cpp:1588-1591`). The callback-only exemption at `RenderCache.cpp:1393` never applies to anyone, because every request has a callback, including SumatraPDF's own (`RenderCache.cpp:912-913`). Only *queued* requests escape this abort; requests already being drawn don't.
- Some paths drop queued requests **without calling the callback**: `RekeyForLayoutChange` (EPUB re-layout, `RenderCache.cpp:676-679`) and `ReduceTileSize` (after running out of memory, `RenderCache.cpp:813-817`). Any "waiting for" count kept by the new code has to survive a callback that never comes.

## 3. How many threads draw at once, and does a slow page block the others?

- **Threads:** at least 8, or one per logical processor, at most 32 (`RenderCache.cpp:49`, `224-227`; `RenderCache.h:133`). They're started lazily, one more each time a request arrives and no thread is idle (`RenderCache.cpp:1064-1078`).
- **But for one PDF, one page at a time.** The MuPDF engine (PDF, XPS and the other MuPDF formats) holds one engine-wide `renderLock` while loading a page, building its display list and drawing it (`src/shared/EngineMupdf.cpp:7644-7658`, `7689-7716`). The header says this is on purpose, to avoid crashes in shared image decoding (`src/shared/EngineMupdf.h:52-58`, `141-167`). Eight threads on one document mostly wait in line.
- **The line has no order.** `Mutex` is a Windows SRW lock (`src/base/Base.h:1151-1160`), and Microsoft documents that SRW locks are "neither fair nor FIFO" ([Slim Reader/Writer (SRW) Locks](https://learn.microsoft.com/en-us/windows/win32/sync/slim-reader-writer--srw--locks)). Once requests have moved from the queue onto waiting threads, the newest-first order no longer decides who goes next.
- **A slow page blocks the others in the same document.** A big scan holds `renderLock` for its whole render. Cancelling it only takes effect between display-list steps, and "a single big image decode makes that wait run into the hundreds of ms" (`RenderCache.cpp:1175-1179`). A Rough Page of a big scan that is still being drawn when you tap therefore holds up the landing Spread. Drawing it at half size helps only with the final drawing step. Opening the page, building its display list and decoding its images cost the same at any size.
- Different documents (tabs) have separate engines and locks, so they draw in parallel.

## 4. The route that avoids all of this

SumatraPDF already prepares page pictures outside the shared queue. The page-thumbnail palette uses **its own background job** with **a copy of the document** "loaded from the file, which doesn't compete with the view" (`src/PageThumbnails.cpp:45-58`, `186-207`, `700-715`):

- `EngineBase::Clone()` (`src/shared/EngineBase.h:505`; MuPDF: `EngineMupdf.cpp:3951-4000`, which reloads from the file, "streams large documents on demand", and falls back to in-memory data) gives a separate engine with **its own `renderLock`**. DjVu, EPUB, image and comic-book engines have `Clone()` too (`EngineDjvuDec.cpp:255`, `EngineEbook.cpp:1045`, `EngineImages.cpp:1357`, `2387`).
- `RenderPageThumbnail` (`PageThumbnails.cpp:123-142`) shows the call: page box, zoom and `RenderPageArgs` (with `loc` for reflowed EPUB). A Rough Page is the same call at half the view zoom. Passing `cookie_out` in `RenderPageArgs` gives an abort switch for the new code's own render (`EngineMupdf.cpp:7592-7597`).
- **What the new code must add itself:** dark mode, grayscale and brightness. `RenderCacheRunOne` sets them up before drawing and applies them after (`RenderCache.cpp:1419-1423`, `1447-1480`). The thumbnail code skips this. The helpers are `static` inside `RenderCache.cpp` (`54-160`), so they either get copied into the new file or made visible with a tiny change. `BuildViewDarkModeProfile` is already public (`src/shared/PdfDarkMode.h:38`).
- **Costs:** a second open of the file (once per document, in the background), its own memory, and decoding each page's images twice (once for the Rough Page, once for the sharp page), because the two engines don't share caches. The copy shows the file **as saved**: unsaved annotation edits don't appear on Rough Pages. The thumbnail code notes the same trade-off (`PageThumbnails.cpp:55-57`).

What is left shared is only the processor. SumatraPDF's own threads keep their queue, slots, lock and cache to themselves. One worker thread uses at most one core, and Windows lets a background thread run at lower priority (`SetThreadPriority`, not yet used anywhere in SumatraPDF).

## 5. Recommended pacing rule

**Rough Pages never use SumatraPDF's render queue.** They are drawn by one worker thread of our own, from a background copy of the document (`Clone()`), in new files only.

1. **One at a time.** At most one Rough Page is being drawn at any moment, per window. There is no queue of requests, only a *wish list* that is rebuilt on every tap and every Curl landing. It holds at most 10 faces (5 Curls in the air × 2 faces), **oldest Curl first**, and within a Curl the face that shows first.
2. **Skip what's already there.** Leave out faces that have a sharp picture (frame snapshot, or `RenderCache::Exists` at the view zoom) or a Rough Page already made. Never list pages inside a Bundle.
3. **Cancel by forgetting.** When a face's Curl lands or the face leaves the screen, take it off the list. If it's the one being drawn, trip its abort cookie and throw the result away. Faces of the Spread you land on stay on the list until SumatraPDF draws them sharp, as decided before.
4. **Never wait for a reply that may not come.** The worker owns its own work, so "pending" is simply "the one being drawn". There's no count of slots in someone else's queue to keep in step.
5. **Hold back only on very small machines.** On 2 or fewer logical processors, start a new Rough Page only while `RenderCache::IsBusyFor(dm)` is false (`RenderCache.cpp:1223-1242`), so the landing Spread gets the processor first. Otherwise start the next one straight away, at below-normal thread priority. Waiting for quiet on a normal PC would starve Rough Pages during fast tapping, which is exactly when they're needed.
6. **Fallback.** If the copy can't be made (`Clone()` returns `nullptr`) or isn't ready yet, faces show blank paper, as already decided for "can't keep up". Never fall back to the shared queue.

**Why:** the shared queue gives no safe way in. Its pictures land in the wrong place (§1), taps cancel exactly the older Curls' pages (§2), and its engine-wide lock makes any Rough Page in progress a direct delay for the landing (§3). A separate copy with one thread removes all three. The only thing it shares with the landing is processor time, which one below-normal thread barely dents on a multi-core PC. One-at-a-time with oldest-first keeps memory small (one Rough Page in progress, about 5 MB at 4K) and matches the decision that Curls never wait.

### If a second copy of the document is not wanted

The alternative is to share the queue, which needs three small changes to original files: give callback requests an empty tile, skip `cache->Add` for them and hand over the bitmap, and exempt them from the "not nearby" abort (`RenderCache.cpp:1035-1042`, `1487-1488`, `853-858`). The pacing rule then has to be: **at most 1 Rough Page queued or being drawn, and only while `IsBusyFor(dm)` is false**. Even so, a tap that comes in while that one Rough Page is being drawn waits for it, up to a whole big-scan render, because of `renderLock`.

## Questions this raises for the plan

- **Accept the second copy?** It costs one more background file open per document, memory, and double image decoding. Rough Pages show the file as saved, without unsaved annotation edits.
- **When to make and free the copy:** at the first Page Turn with the switch on (lazily), or when the document opens in a Paged View? Free it when the tab closes, the switch is turned off, or the view goes Continuous?
- **Dark mode / grayscale / brightness for Rough Pages:** copy the `static` helpers from `RenderCache.cpp` into the new file, or make them visible (a few-line change to an original file)?
- **The start-screen thumbnail route looks broken in the current code** (§1). It's an upstream matter and outside this project, but no new code should copy that route.

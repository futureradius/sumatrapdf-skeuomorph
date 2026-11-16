# Where do the ~1.8 MB per open-flip-close cycle go?

Issue [#43](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/43), map #36, spec 5.6 (memory note).
Measured 2026-11-16 on `scrolling` at 9bab0cc6c, `out/rel64/SumatraPDF.exe` (built from 79ad3e612), 20-core PC,
quarter-screen test window, Facing view, fit page.

## Answer

- **Where:** in the C heap (the Windows segment heap that `malloc` / mupdf allocate from, declared in
  `src/shared/SumatraPDF.exe.manifest:28`). The memory stays inside heap regions the process already reserved
  (16, 32 and 64 MB). It is **not** GDI bitmaps, handles, new standalone allocations, Direct3D or the Curl layer:
  those stay flat.
- **When:** only when a background copy that has **drawn Rough Pages is closed**. Making and closing a copy
  without drawing leaves nothing. Drawing with the copy left open leaves nothing.
- **How much:** it grows with how much the copy drew and aborted before closing. On the scan, holding Page Down
  for 3 s leaves ~1.7 MB. Eight spaced-out turns leave ~0.1 MB. The numbered book leaves ~0.35 MB.
- **Growth:** linear. There was no plateau in 30 cycles: 1.68 MB per cycle from cycle 8 to cycle 30.
- **Original SumatraPDF** (the same exe with the switch off) fills its caches in the first ~6 cycles, then stays
  flat: +0.7 MB over 23 cycles.
- **Cause:** not pinned down to a line. Either a small per-page leak when the copy is torn down, or heap
  fragmentation left by freeing the copy's ~250 MB. The engine's per-page cleanup reads correct (see below).
  Telling the two apart needs allocation tracing, which this PC lacks (no cdb/UMDH).
- **Worth fixing:** not now. A copy closes only on a tab switch away, Continuous View or the switch going off.
  Even 50 such closes after fast flipping through a scan cost ~85 MB, and that is returned at exit. Keep spec
  5.6's "don't block on it". Revisit if long sessions show it.

## Method

`tests/ad-hoc-skeuomorph-memory.ts` loop: switch on (copy made), hold Page Down 3 s, switch off (copy closed),
Home. A scratch variant also recorded the process's committed private memory per allocation (`VirtualQueryEx`),
plus handle, GDI and USER object counts. It added a `slow` mode: 8 single Page Downs, 1.2 s apart. Values are
private bytes after the copy closed. One process per run, run one after another. Documents:
`Albertii - De re ædificatoria.pdf` (776 MB, 388 pages, one JPEG each) and `generated/numbered-800-pages.pdf`.

| Run                | Copy          | Drawing        | Steady growth per cycle                       | Rough Pages done / aborted per cycle |
| ------------------ | ------------- | -------------- | --------------------------------------------- | ------------------------------------ |
| scan, switch off   | none          | –              | 0.03 MB (cycles 7-30)                         | –                                    |
| scan, noflip       | made + closed | none           | 0 (cycles 7-15)                               | 0 / 0                                |
| scan, notoggle     | stays open    | yes            | 0 (cycles 3-14)                               | ~8 / ~7                              |
| scan, cycled       | made + closed | hold 3 s       | **1.68 MB** (cycles 8-30)                     | ~9 / ~79                             |
| scan, cycled, slow | made + closed | 8 spaced turns | 0.09 MB (cycles 10-20)                        | ~3.5 / ~4.5                          |
| book, switch off   | none          | –              | 0.05 MB (cycles 10-20)                        | –                                    |
| book, cycled       | made + closed | hold 3 s       | 0.69 MB total, 0.37 MB in heap (cycles 10-20) | ~87 / 0                              |

Observations:

- **Two phases on the scan, with the switch on or off.** For the first ~6-7 cycles, memory rises by 5-25 MB per
  cycle as new standalone ~200-460 KB allocations, with GDI objects climbing (184 → ~384). That is SumatraPDF's own
  render cache filling with page bitmaps: each is a DIB section (`AllocPixmapDIB`, `src/base/Pixmap.cpp:107`), up
  to `kMaxBitmapsCached = 256` (`src/shared/RenderCache.h:16`), trimmed by memory load only above ~50%
  (`BytesWeWantFreed`, `src/shared/CachedObjects.cpp:172`). Together with the main engine's mupdf store
  (`FZ_STORE_DEFAULT`, `src/shared/EngineMupdf.cpp:3733`), this plateaus. With the switch off it then stays flat.
- **With the copy cycled, a second phase follows:** a steady ~1.7 MB per cycle. GDI objects (384), handles
  (~511) and the number of allocations (426) stay flat. The growth is inside existing 16/32/64 MB heap regions.
  Diff between cycles 15 and 30: +26.2 MB in existing allocations, +0.6 MB new, −0.4 MB freed.
- **The copy's own memory does come back.** It peaks at ~230-250 MB over the closed value (e.g. 906 → 680 MB at
  cycle 30), as the 9bab0cc6c note says.
- **Notoggle jumped +38 MB at cycle 15.** That is the open copy reaching new pages: it keeps each page's display
  list until it closes (`FzPageInfo::displayList`, `src/shared/EngineMupdf.h:59`). It is not growth per cycle.

## What the residue is tied to

- The worker frees everything it owns. Rough Pages are freed on a new look (`NewLook`,
  `src/skeuomorph/RoughPages.cpp:369`). The abort cookie is deleted after each draw (`:333`). The copy is
  `Release()`d on the worker when the copy changes (`:280-283`). The copy's store is emptied after each Rough Page
  (`:306-308`).
- `EngineMupdf::~EngineMupdf` (`src/shared/EngineMupdf.cpp:3802`) frees every page through `FreePageInfo`
  (`:3762`): links, images, annotations, display list, page. It also drops the per-thread contexts the worker
  cloned (`ReleaseAllPerThreadContexts`, `:3707`).
- Aborted renders drop their draw device. `UnhookAbortedDevices` (`:7577`) only clears `close_device`, and
  `fz_draw_drop_device` still frees the stacked pixmaps (`ext/mupdf/source/fitz/draw-device.c:3168`).
- What the numbers rule out: a cost for creating or destroying a copy (noflip is flat), and a cost per render
  while the copy lives (notoggle is flat). What is left scales with the pages the copy touched before it closed:
  roughly 15-20 KB per page drawn or aborted on the scan, ~4 KB on the book. Both explanations fit that: a
  per-page leak at teardown, or fragmentation from freeing a large, page-proportional heap.

## If someone picks it up

1. Install Debugging Tools for Windows (UMDH + gflags `+ust`). Snapshot the heap after cycle 10 and after cycle
   30 of the cycled scan run, then diff. Stacks that grow name the leak. No growing stacks means fragmentation.
2. Cheaper and in-tree: give the copy's `fz_new_context` (`src/shared/EngineMupdf.cpp:3733`) a counting
   `fz_alloc_context` and log the bytes still out after `fz_drop_context`. Non-zero means a mupdf-side leak.
3. If it is fragmentation, keeping one copy per document across tab switches (instead of closing it) would stop
   the churn. That trades RAM held while the tab is hidden against the residue.

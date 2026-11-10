# Rough Pages are drawn from a background copy of the document

When you flip fast, SumatraPDF never draws the pages you fly past, but showing them is a core reason for this project. So the new code prepares **Rough Pages** itself: one worker thread of its own, one page at a time, drawn from a **second copy of the document opened in the background** (`Clone()`, as SumatraPDF's page-thumbnail palette already does). It never uses SumatraPDF's own page queue.

## Considered options

- **SumatraPDF's own render queue (`RenderCache::Render` with a callback)** was the first plan. Research showed it can't work cleanly:
  - the finished picture lands in SumatraPDF's cache, not with us;
  - it collides with SumatraPDF's own picture of that page;
  - tapping cancels the older Curls' pages, the ones meant to go first;
  - the queue's 8 slots would be fought over;
  - one engine-wide lock means a Rough Page in progress delays the page you land on.
- **Three small changes to `RenderCache.cpp` plus a strict "one at a time" rule:** fewer extra resources, but it touches original files, and a tap could still wait for one Rough Page.

## Consequences

- **Costs:** one extra background file open per document on screen (4-28 ms), some memory (up to ~230 MB on huge scans, with the copy's image cache emptied after each page), and each page's images decoded twice.
- **Unsaved annotation edits** can be missing on a flipped-past page for a few hundred milliseconds. The landed page is always the real one.
- **Colours:** the dark-mode, grayscale and brightness helpers are copied into `src/skeuomorph/`, so they can drift if upstream changes them.
- **The payoff:** on big scans, flipped-past pages go from about 6% shown to 60-100%, and landing is never slowed (except sometimes on huge scans, accepted).

Source: [How can Rough Pages share the render queue without slowing the landing?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/18), [How fast do Rough Pages arrive, and how much memory do Curls need?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/20), [Is a background copy of each document acceptable for Rough Pages?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/22).

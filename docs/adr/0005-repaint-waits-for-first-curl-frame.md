# SumatraPDF's repaint waits about 10 ms for the Curl's first frame

When you tap, SumatraPDF jumps to the new Spread and repaints it at once. The Curl layer also needs about one screen refresh to appear. If a refresh falls between the two, the new Spread flashes for one frame before the Curl covers it (4 of 48 turns in testing). So, at each Page Turn, the new code's task (which already runs before SumatraPDF's repaint) **waits until the layer reports that its first frame is on screen**, then lets SumatraPDF repaint. The wait is usually 9-12 ms (up to ~35 ms at 4K), and nobody sees it, because the Curl is covering the screen by then. With the wait: 0 flashes in 102 turns.

## Considered options

- **No wait:** a visible flash now and then, depending on timing luck.
- **Keeping the layer attached and empty the whole time a Paged View is open:** it never flashed in testing, but nothing rules the race out. It also puts a layer over every Paged View all the time, and it costs memory when idle.
- **Blocking with `DwmFlush`:** caused one hitch per Curl.

## Consequences

- **A deliberate delay on SumatraPDF's UI thread at every animated Page Turn.** Someone "optimising" it away brings the flash back; this record exists to stop that.
- **"On screen" means the swap chain's next frame signal,** which the drawing thread waits on anyway.
- **The layer goes see-through, then away, one screen refresh after SumatraPDF really shows the new Spread** (or its "Rendering page N..." message), so removing it can't be seen.

Source: [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19).

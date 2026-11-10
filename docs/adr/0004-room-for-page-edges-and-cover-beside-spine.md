# Pages get a little smaller for Page Edges, and the cover sits beside the spine

The plan started with a strict rule: the effects are pure decoration and never change SumatraPDF's layout. After trying the prototypes, the human chose two deliberate exceptions. Both apply only in a Paged View with the switch on.

1. **Room for Page Edges.** On each side of the Spread, SumatraPDF keeps room as wide as this document's whole-book thickness (4-46 px). It's added as a wider window margin, so Fit Page makes pages a little smaller. A book needs paper beside it to feel like a book, and pages keep one size the whole time you read. Presentation mode keeps the full screen and gets no Page Edges.
2. **The cover sits right of the spine** (left in Right-to-Left Reading), like a closed book's front, instead of centred. A lone last page sits left of it. This makes opening the cover a real turn onto a blank slate.

With the switch off, or in a Continuous View, the layout is exactly the original.

## Considered options

- **Drawing Page Edges in whatever background is left, without making room:** they'd vanish or squeeze whenever the page fills the window, and the book would seem to change thickness.
- **Thinner Page Edges when space is short:** same problem.
- **Keeping the cover centred:** the cover would then have to jump sideways when it's opened.

## Consequences

- **Turning the switch on or off re-lays out the document once,** like a zoom change.
- **Zoomed in past the window,** Page Edges are cut off at the window edge, never squeezed.
- **Two kinds of layout hooks:** one in `Relayout` and a small `pageBesideSpine` flag in `DocumentLayout`.
- **One pixel-by-pixel check** against the original, with the switch off, proves nothing else moved.

Source: [Where do Page Edges go when the Spread fills the window?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/10), [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16).

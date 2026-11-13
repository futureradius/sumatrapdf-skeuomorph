# Mixed Page Sizes Transition Prototype

This prototype tests four different ways to handle page size changes during page turns in skeuomorphic book readers.

## The Problem

When turning from a page of one size to a page of a different size (e.g., portrait to landscape, or A4 to A5), the surroundings (page edges, background) need to transition smoothly. If not handled well, this can look jarring or "cheap".

## Four Transition Options

### A. Settle crossfade
The old screen stays as it was during the turn, then crossfades to the new screen in the last part of the turn (around the last 30%). This is what's currently built in SumatraPDF.

**Pros:** Simple, predictable
**Cons:** Can look like a "cut" at the end, especially with very different sizes

### B. Morph
The page outline and its surroundings (page edges) smoothly change shape from the old size to the new one during the turn. The turning page itself morphs.

**Pros:** Most natural, feels like a real book where pages can be different sizes
**Cons:** More complex to implement, might look strange with very different aspect ratios

### C. Album
Every page lies on a sheet the size of the book's largest page. Smaller pages sit centered on blank paper. The book never changes shape.

**Pros:** Consistent look, no size changes to animate
**Cons:** Changes SumatraPDF's normal appearance, smaller pages have blank margins

### D. Two beats
The turn plays with the old outline and page edges unchanged (first beat, ~85% of duration), then a quick slide of about 150ms moves everything to the new page's position and size (second beat).

**Pros:** Clear separation, quick at the end
**Cons:** Two distinct motions might feel less smooth

## How to Use

1. Open `mixed-sizes-prototype.html` in a modern browser (Edge, Chrome)
2. Use these controls:
   - **→ / PageDown / Space** or click right half: turn page forward
   - **← / PageUp** or click left half: turn page backward
   - **Hold keys**: turn pages faster
   - **[ and ]**: cycle through transition options (A, B, C, D)
   - **Transition selector**: choose specific transition
   - **View selector**: switch between Single Page, Facing, Book View
   - **Page sizes selector**: test with different page size configurations
   - **Slow motion**: check to see animation 5× slower
   - **Freeze animation**: check and use slider to inspect specific moments
   - **Duration**: adjust the animation speed

3. Pay attention to:
   - How the page edges look during the transition
   - Whether anything "pops" or jumps
   - How it feels with different page size combinations
   - Which transition feels most like a real book

## Test Documents

### Mixed
A variety of page sizes: portrait, landscape, A5, tall strip, double-page spread. This is the main test case.

### Same Size
All pages are the same size. This should look smooth with all transitions and serves as a baseline.

### Facing Mixed
Mixed sizes in facing view, which adds complexity with the spine and two pages visible.

## What to Look For

- **Visual continuity**: Does the animation feel continuous, or are there jumps?
- **Page edges**: Do they transition smoothly or pop?
- **Background**: Does the desk background remain stable?
- **Realism**: Does it feel like turning a real book with mixed page sizes?
- **Performance**: Does the animation run smoothly?

## Recommendations

After testing all four options, consider:

1. Which feels most natural for a book-like experience?
2. Which is most visually polished?
3. Which would be most practical to implement in SumatraPDF?
4. Are there any transitions that feel worse with certain page size combinations?

## Notes

- This is a WebGL-based prototype, so it requires a browser with WebGL support.
- The animation is simplified compared to the actual SumatraPDF implementation.
- The focus is on the transition of page surroundings, not the page curl itself.
- For the best test, try it on a machine with good graphics performance.

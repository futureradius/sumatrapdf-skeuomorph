# Skeuomorph Page Turn

A personal SumatraPDF fork that makes turning pages look like turning real paper, so the reader keeps a physical sense of where they are in the book.

## Views

**Paged View**:
Any view that shows pages one step at a time: Single Page, Facing or Book View.
_Avoid_: page mode, non-continuous mode

**Spread**:
What a Paged View shows at once: one page in Single Page, two side-by-side pages in Facing and Book View.
_Avoid_: page pair, double page

**Continuous View**:
Any view where all pages scroll as one long strip. Has no Page Turn.
_Avoid_: scroll mode

**Right-to-Left Reading**:
Reading a book whose spine is on the right, so pages are turned from left to right, as in manga. Set by the document or by SumatraPDF's Manga Mode.
_Avoid_: R2L, RTL document, mirrored, manga mode (that is the switch, not the reading)

## Page turning

**Page Turn**:
Moving from one Spread to another in a Paged View, whether to the next one or a jump many pages away. One action is one Page Turn, however many steps it takes inside. Re-pairing the pages (switching view mode, "step one page") is not a Page Turn.
_Avoid_: page flip, scroll, page change

**Curl**:
The realistic paper animation shown during a Page Turn: the page bends over, showing a shadow and its back. Several Curls can be in progress at once, each at its own stage. A Curl **lands** when its page lies flat on the other side; landing instantly means skipping straight to that end. Bundles land the same way.
_Avoid_: flip animation, slide

**Bundle**:
A stack of pages that Curls over together during a Page Turn that skips pages. Its thickness shows how many pages are skipped.
_Avoid_: page stack, multi-page flip

**Page Edges**:
The drawn stacks of paper edges beside a Spread: read pages on one side, unread pages on the other, each as thick as its share of the book.
_Avoid_: book thickness, page stack

**Rough Page**:
A quick, half-sharp version of a page, prepared so a page the reader flips past still shows on its Curl. Replaced by the sharp page once that is ready.
_Avoid_: thumbnail, preview, low-res

**Skeuomorph Page Turn**:
The user setting that turns Curls, Bundles and Page Edges on or off together. When off, a Page Turn is an instant jump, as in original SumatraPDF.
_Avoid_: book mode, animation setting

## Related ideas

**Page Flag**:
A coloured marker the reader sticks on any page to jump back to it quickly, like a sticky tab in a paper book. Not a Bookmark: in SumatraPDF, Bookmarks are the document's table of contents.
_Avoid_: bookmark, tab, sticky note

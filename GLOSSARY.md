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

## Page turning

**Page Turn**:
Moving from one Spread to another in a Paged View, whether to the next one or a jump many pages away.
_Avoid_: page flip, scroll, page change

**Curl**:
The realistic paper animation shown during a Page Turn: the page bends over, showing a shadow and its back. Several Curls can be in progress at once, each at its own stage.
_Avoid_: flip animation, slide

**Bundle**:
A stack of pages that Curls over together during a Page Turn that skips pages. Its thickness shows how many pages are skipped.
_Avoid_: page stack, multi-page flip

**Page Edges**:
The drawn stacks of paper edges beside a Spread: read pages on one side, unread pages on the other, each as thick as its share of the book.
_Avoid_: book thickness, page stack

**Skeuomorph Page Turn**:
The user setting that turns Curls, Bundles and Page Edges on or off together. When off, a Page Turn is an instant jump, as in original SumatraPDF.
_Avoid_: book mode, animation setting

## Related ideas

**Page Flag**:
A coloured marker the reader sticks on any page to jump back to it quickly, like a sticky tab in a paper book. Not a Bookmark: in SumatraPDF, Bookmarks are the document's table of contents.
_Avoid_: bookmark, tab, sticky note

# Skeuomorph hooks in original files

Everything else lives in `src/skeuomorph/` and `tests/skeuomorph-*.ts`. Only
the lines below touch SumatraPDF's own files. Most carry a
`// skeuomorph: hook <name>` tag, so a search for `skeuomorph:` finds them.

Each code block below is the exact text that must be in the file named after
its language. `bun tests/skeuomorph-hooks.ts` checks every block against the
code, and that no tagged line is missing from this list.

## Merging upstream

1. Merge. On a conflict in a hook, keep upstream's code and put the hook back
   next to the same code (the "where" line), not at the old line number.
2. `src\skeuomorph\run-gen-code.bat`, then `git checkout src/shared/EbookDoc.cpp`:
   regenerates `src/Settings.*` and `src/shared/Commands.*`. Never fix
   conflicts in those by hand.
3. `bin\premake5.exe vs2022`: puts `src/skeuomorph` back in the project files.
4. `bun tests/skeuomorph-hooks.ts`: every hook still there.
5. Build, unit tests, then the `tests/skeuomorph-*.ts` scripts.

## Hooks

### seam: two callbacks a document view calls

Where: `src/shared/DocController.h`, end of `struct DocControllerCallback`.
Empty by default, so ng is unchanged.

```cpp src/shared/DocController.h
    virtual void BeforeSpreadChange(DisplayModel*, int) {} // skeuomorph: hook seam
    virtual void BeforeRelayout(DisplayModel*) {} // skeuomorph: hook seam
```

### seam-classic: the classic app forwards them

Where: `src/SumatraPDF.cpp`, the include list and `struct ControllerCallbackHandler`.

```cpp src/SumatraPDF.cpp
#include "skeuomorph/PageTurnHooks.h" // skeuomorph: hook seam-classic
    void BeforeSpreadChange(DisplayModel* dm, int pageNo) override { PageTurnBeforeSpread(win, dm, pageNo); }
    void BeforeRelayout(DisplayModel* dm) override { PageTurnBeforeRelayout(win, dm); }
```

### spread-change: a Page Turn starts

Where: `src/shared/DisplayModel.cpp`, `GoToPage`, just before `ChangeStartPage(pageNo);`.
The screen still shows the old Spread: it's copied here.

```cpp src/shared/DisplayModel.cpp
        cb->BeforeSpreadChange(this, pageNo); // skeuomorph: hook spread-change
```

### relayout: room for Page Edges, the cover flag

Where: `src/shared/DisplayModel.cpp`, `Relayout`, before the layout is computed.

```cpp src/shared/DisplayModel.cpp
    cb->BeforeRelayout(this); // skeuomorph: hook relayout
```

### beside-spine: the cover right of the spine, a lone last page left of it

Where: a flag in `DisplayModel` and `DocumentLayoutParams`, passed on in
`Relayout`, used in `RelayoutRows` (`src/shared/DocumentLayout.cpp`). Off by
default. The last line is upstream's, changed: `&& !params.pageBesideSpine` added.

```cpp src/shared/DisplayModel.h
    bool pageBesideSpine = false; // Skeuomorph Page Turn: lone page beside the spine
```

```cpp src/shared/DocumentLayout.h
    bool pageBesideSpine = false; // Skeuomorph Page Turn: lone page beside the spine
```

```cpp src/shared/DisplayModel.cpp
        params.pageBesideSpine = pageBesideSpine; // skeuomorph: hook beside-spine
```

```cpp src/shared/DocumentLayout.cpp
    if (params.pageBesideSpine && (columnMaxWidth[0] == 0) != (columnMaxWidth[1] == 0)) {
        columnMaxWidth[0] = columnMaxWidth[1] = std::max(columnMaxWidth[0], columnMaxWidth[1]);
            if (single || row.isSpread || (cover && !IsContinuous(params.displayMode) && !params.pageBesideSpine)) {
```

### edges-paint: Page Edges at rest

Where: `src/Canvas.cpp`, `DrawDocument`, after the background, before the pages.

```cpp src/Canvas.cpp
    PageEdgesPaint(win, hdc, dm); // skeuomorph: hook edges-paint
```

### after-paint: what the screen really shows

Where: `src/Canvas.cpp`, the include list, and `OnPaintDocument` after the
back buffer is flushed (same condition as the flush).

```cpp src/Canvas.cpp
#include "skeuomorph/PageTurnHooks.h" // skeuomorph: hook after-paint
        PageTurnAfterPaint(win, !gNoFlickerRender || shouldPaint || showFocus); // skeuomorph: hook after-paint
```

### rough-paint: a page still rendering shows its Rough Page

Where: `src/Canvas.cpp`, `DrawDocument`, right after `PaintPageBitmap`,
before the `if (renderDelay != 0)` block that writes "Rendering page N...".
Added in build step 8, approved by the human on 2026-11-16.

```cpp src/Canvas.cpp
        if (PageTurnRoughPaint(win, hdc, pageNo, bounds, renderDelay)) { // skeuomorph: hook rough-paint
            shouldPaint = true;
            continue;
```

### forget: a closed document

Where: `src/SumatraPDF.cpp`, `ControllerCallbackHandler::CleanUp`, after
`gRenderCache->FreeForDisplayModel(dm);`.

```cpp src/SumatraPDF.cpp
    PageTurnForget(win, dm); // skeuomorph: hook forget
```

### scroll-stop: a mode switch stops smooth scrolling

Where: `src/SumatraPDF.cpp`, `SwitchToDisplayMode`, before
`win->ctrl->SetDisplayMode(...)`. Fixes an upstream bug that left a blank view
(build step 3, approved over budget).

```cpp src/SumatraPDF.cpp
    win->scrollAnimActive = false; // smooth-scroll timer must not chase the old view's target
```

### settings: `SkeuomorphPageTurn` and `PageTurnDuration`

Where: `cmd/gen-settings.ts`, `globalPrefs` (after `ReadingAutoScrollMinutes`'s
field) and `globalPrefsLayout`. Then run the generator.

```ts cmd/gen-settings.ts
  field("SkeuomorphPageTurn", Bool, true, "if true, page turns in Paged View look like turning paper").ver("3.7"),
    "PageTurnDuration",
    "length of a normal Skeuomorph Page Turn in milliseconds; bigger is slower. 100 to 5000",
  "SkeuomorphPageTurn", // skeuomorph: hook settings
  "PageTurnDuration",
```

### command: the switch's command

Where: `cmd/gen-commands.ts`, the very end of `commandsRaw`. Then run the generator.

```ts cmd/gen-commands.ts
    "CmdToggleSkeuomorphPageTurn", "Skeuomorph Page Turn", // skeuomorph: hook command
```

### menu, checkmark, click, hide-ng: View → Skeuomorph Page Turn

Where: `menuDefView` in `src/shared/MenuDefs.cpp` (after Highlight Form
Fields); `MenuUpdateStateForWindow` in `src/Menu.cpp`; `FrameOnCommand` in
`src/SumatraPDF.cpp` (before `CmdToggleDisableLinks`);
`GetCommandVisibility` in `src/shared/CommandAvailability.cpp` (hidden in ng).

```cpp src/shared/MenuDefs.cpp
        TrN("Skeuomorph Page Turn"),
        CmdToggleSkeuomorphPageTurn, // skeuomorph: hook menu
```

```cpp src/Menu.cpp
    MenuSetChecked(win->menu, CmdToggleSkeuomorphPageTurn, gSettings->skeuomorphPageTurn); // skeuomorph: hook checkmark
```

```cpp src/SumatraPDF.cpp
        case CmdToggleSkeuomorphPageTurn: // skeuomorph: hook click
            ToggleSkeuomorphPageTurn();
```

```cpp src/shared/CommandAvailability.cpp
    if (cmdId == CmdToggleSkeuomorphPageTurn) { // skeuomorph: hook hide-ng
```

### build: the new folder in the project

Where: `premake5.files.lua`, `sumatrapdf_files()`. Then run premake.

```lua premake5.files.lua
  files_in_dir("src/skeuomorph", { "*.cpp", "*.h" }) -- skeuomorph: hook build
```

### unit-tests: debug builds run the Page Turn unit tests

Where: `src/tests/Sumatra_ut.cpp`, the declarations and `RunAppUnitTests`.

```cpp src/tests/Sumatra_ut.cpp
void PageTurnLogic_UnitTests(); // skeuomorph: hook unit-tests
    PageTurnLogic_UnitTests(); // skeuomorph: hook unit-tests
```

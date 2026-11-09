# Where does the new code hook in, and how few original lines change?

Research for [#17](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/17) (map [#3](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/3)).
Source: this repo at commit `1d0b75378` (branch `scrolling`). Terms as in [GLOSSARY.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/scrolling/GLOSSARY.md) (Paged View, Spread, Page Turn, Curl, Bundle, Page Edges, Rough Page).
Builds on [page-turn-code-paths.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/page-turn-code-paths/research/page-turn-code-paths.md), [page-drawing-and-cache.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/page-drawing-and-cache/research/page-drawing-and-cache.md), [settings-and-menu.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/settings-and-menu/research/settings-and-menu.md), [curl-drawing-technology.md](https://github.com/futureradius/sumatrapdf-skeuomorph/blob/research/curl-drawing-technology/research/curl-drawing-technology.md) and the spike in [`spike/curl-layer`](https://github.com/futureradius/sumatrapdf-skeuomorph/tree/spike/curl-layer/spikes/curl-layer).

## In plain words

All new code can live in one new folder, `src/skeuomorph/`. SumatraPDF's own files get about **46 added lines and 1 changed line**, spread over 13 files. Most of those are one-line calls ("tell the new code that X is about to happen"). The rest is the usual boilerplate every setting, command and menu item needs.

The trick that keeps ng untouched: SumatraPDF already has a "callback" object that the shared page code uses to talk to the app (`DocControllerCallback`). Classic SumatraPDF and ng each fill it in their own way. We add two new entries to it that **do nothing by default**. Classic SumatraPDF fills them in and calls the new code; ng never does, so for ng nothing changes and none of ng's own files are edited.

Most things the plan needs don't need a hook at all. The new code can work them out from what it sees:
- **Don't-animate cases** (display-mode switch, `B` key, "step one page", EPUB renumbering, Presentation switch, hidden window): the new code compares a small "fingerprint" of the view (display mode, step-one-page, Presentation, layout generation, page count) at each turn with the one it saw at the last paint. If it changed, the turn is just a jump.
- **Resize, DPI change, fullscreen, closing:** the new code listens to the document window's own messages (window "subclassing", a pattern the repo already uses).
- **Frame timing:** a background drawing thread owned by the new code, woken by the Curl layer itself once per screen refresh. SumatraPDF's message loop stays as it is.

## 1. How shared code reaches classic-only code

Which program builds what:
- `src/shared/*` and `src/base/*` are built by **both** classic and ng (`cmd/helper/ng-shared.ts`, e.g. `DisplayModel.cpp` at `:273`, `DocController.h` at `:276`, `MenuDefs.cpp` at `:169`, `DocumentLayout.cpp` at `:280`).
- `src/Canvas.cpp`, `src/SumatraPDF.cpp`, `src/Menu.cpp`, `src/MainWindow.cpp` are **classic only**. ng has its own `src/ng/SumatraPDF.cpp`, `src/ng/Menu.cpp`, `src/ng/MainWindow.cpp`, and no `Canvas.cpp` (it uses `src/ng/gui/DocCanvas.cpp`, `cmd/helper/ng-targets.ts:1627`).
- Every ng compile defines `SUMATRA_NG` (`cmd/helper/ng-compile.ts:275`, `:321`).

The seam: `struct DocControllerCallback` (`src/shared/DocController.h:42-79`), held by every `DisplayModel` as `cb` (`DocController.h:85`). Its implementers are classic `ControllerCallbackHandler` (`src/SumatraPDF.cpp:943-967`, one per `MainWindow`, field `win`), ng's `ControllerCallbackHandler` (`src/ng/MainWindow.cpp:76`) and four test stubs (`src/ng/tools/test_engines.cpp:240`, `src/ng/tests/RenderScale_ut.cpp:26`, `src/shared/tests/PagePosition_ut.cpp:12`, `src/shared/tests/RenderCache_ut.cpp:62`). All its methods today are pure (`= 0`).

Add two **non-pure** methods with empty bodies after `PagesRenumbered` (`DocController.h:78`):

```cpp
virtual void BeforeSpreadChange(DisplayModel*, int /*newStartPage*/) {}
virtual void BeforeRelayout(DisplayModel*) {}
```

ng and the stubs don't override them, so they compile unchanged and do nothing. Classic overrides both in `ControllerCallbackHandler` (after `src/SumatraPDF.cpp:966`) with one-line forwards into the new files, passing `win`.

Rejected alternatives:
- The repo's `FooCommon.h` "implemented by each app" pattern (`agents.md`, ng section) would force an implementation into ng.
- A global function pointer in a shared file works too, but adds a global and a startup line; the callback already carries `win`.
- `gOnDocControllerDestroyed` (`DocController.h:82`) is a single slot already taken (`src/SumatraPDF.cpp:1655`).

## 2. The hook table

Line numbers are "insert after this line" unless they say "change". "Both" = built by classic and ng; in ng the line is a no-op.

| # | What | Where | Built by | Smallest change | Lines |
|---|---|---|---|---|---|
| 1 | Seam | `src/shared/DocController.h:78` | both | two empty virtuals (above) | +2 |
| 2 | Seam, classic side | `src/SumatraPDF.cpp:966` (+ include after `:160`) | classic | two `override` one-liners calling the new code, `#include "skeuomorph/PageTurnHooks.h"` | +3 |
| 3 | **Start a Page Turn**, copy old screen | `src/shared/DisplayModel.cpp:2954` (just before `ChangeStartPage(pageNo);` at `:2955`) | both | `cb->BeforeSpreadChange(this, pageNo);` | +1 |
| 4 | **Room for Page Edges** (and cover flag, #7) | `src/shared/DisplayModel.cpp:2217` (start of `Relayout`, after the `pagesInfo` check) | both | `cb->BeforeRelayout(this);` | +1 |
| 5 | **Draw Page Edges** in SumatraPDF's own painting | `src/Canvas.cpp:3098` (after the background, before the page loop at `:3100`) | classic | `PageEdgesPaint(win, hdc, dm);` | +1 |
| 6 | **Know what the screen really shows** | `src/Canvas.cpp:3326` (after the flush decision, inside the `else`) (+ include after `:79`) | classic | `PageTurnAfterPaint(win, !gNoFlickerRender \|\| shouldPaint \|\| showFocus);` | +2 |
| 7 | **Cover right of the spine** (and lone last page left of it) | `src/shared/DocumentLayout.h:43`, `DocumentLayout.cpp:289` and change `:309`, `DisplayModel.h:431`, `DisplayModel.cpp:2306` | both | a `bool` layout flag, default off (section 6) | +6, 1 changed |
| 8 | Forget a closed document | `src/SumatraPDF.cpp:1079` (in `ControllerCallbackHandler::CleanUp`) | classic | `PageTurnForget(win, dm);` | +1 |
| 9 | Settings: on/off and speed | `cmd/gen-settings.ts:2015` (next to `ReadingAutoScrollSpeed`) and `globalPrefsLayout` (`:2513`) | classic generator only | two `field(...)` entries + two names | +14 |
| 10 | Command | `cmd/gen-commands.ts:384` (end of list) | both (ng re-exports, `cmd/ng-gen-commands.ts`) | one entry | +1 |
| 11 | View-menu entry | `src/shared/MenuDefs.cpp:354` (after Highlight Form Fields) | both | one `{ TrN(...), Cmd... },` entry | +4 |
| 12 | Hide it in ng | `src/shared/CommandAvailability.cpp:1043` | both | `#if defined(SUMATRA_NG)` → `Hide`, as `CmdMeasure` does at `:1034-1042` | +5 |
| 13 | Checkmark | `src/Menu.cpp:956` | classic | one `MenuSetChecked(...)` | +1 |
| 14 | Click handler | `src/SumatraPDF.cpp:12098` | classic | `case Cmd...: PageTurnToggle(win, cmd); break;` | +3 |
| 15 | Build the new folder | `premake5.files.lua:493` (in `sumatrapdf_files()`) | classic | `files_in_dir("src/skeuomorph", { "*.cpp", "*.h" })` | +1 |
| | **Total** | 13 files | | | **+46, 1 changed** |

Optional extras: unit-test registration in `src/tests/Sumatra_ut.cpp:92` and `:621` (+2, debug builds only, classic only); on/off state in the command palette, `src/CommandPalette.cpp:1382` (+4); `[Cmd... on/off]` arguments, hand-written table in `src/shared/Commands.cpp:442-453` (+1).

Regenerated, never hand-edited: `src/Settings.h/.cpp` (classic only; ng has its own `src/ng/Settings.*` from the independent `cmd/ng-gen-settings.ts`), `src/shared/Commands.h/.cpp` (both) and the four `vs2022/SumatraPDF*.vcxproj(.filters)` files. Tried here: `bin\premake5.exe vs2022` on an unchanged tree produced no diff; with the line in #15 and two dummy files it added only those two files to the four project files (23 lines in all). The trial was reverted.

**ng's own files touched: 0.** Shared files touched: `DocController.h`, `DisplayModel.cpp/.h`, `DocumentLayout.cpp/.h`, `MenuDefs.cpp`, `CommandAvailability.cpp`. In ng each change is either an empty default, a flag nobody sets, or hidden by `SUMATRA_NG`.

## 3. Starting a Page Turn and grouping one action

**Hook #3** runs in `DisplayModel::GoToPage` (`src/shared/DisplayModel.cpp:2932`) inside the Paged View branch (`:2952`), right before `ChangeStartPage` swaps the Spread (`:2955`; its only caller). At that moment:
- old Spread start = `dm->startPage` (`DisplayModel.h:350`), new Spread start = `pageNo` (already moved to its row start at `:2947-2949`);
- `win->buffer` still holds the last painted frame (written only by `OnPaintDocument`, `src/Canvas.cpp:3320`).

The hook fires on every Paged View `GoToPage`, also when the Spread stays the same (zoom, tab switch, restore). That's useful: the new code learns about tab switches and same-Spread calls there too, and filters them.

**Copy the old screen** only on the first candidate turn of an action, and only if the last paint was flushed (`gNoFlickerRender`, `src/Canvas.cpp:84`, `:3324`). If the last paint wasn't flushed, `win->buffer` holds a frame that never reached the screen. That happens while a Curl is already in the air (the layer covers the screen then, so the new code uses its own picture of the Spread it last landed on). Copy pixels; never keep a pointer into `win->buffer`, because `MainWindow::UpdateCanvasSize` deletes and recreates it on every canvas size change (`src/MainWindow.cpp:336-337`).

**One action = one Page Turn** ([Which page changes count as a Page Turn?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/15)): on the first hook call of an action, the new code posts its own task with `uitask::Post` (`src/base/UITask.cpp:190`, a `PostMessageW` at `:201`). Windows hands out posted messages before keyboard and mouse input and before `WM_PAINT`. So that task runs right after the current message (one key press, one wheel notch, one command) has finished all its `GoToPage` calls, and before the next key repeat. It then compares old and final Spread: Curl, Bundle or nothing.

This is slightly sharper than "all `GoToPage` calls before the posted repaint runs". SumatraPDF's repaint (`ScheduleRepaint`, `src/Canvas.cpp:4882-4893`) only invalidates the window, and the actual `WM_PAINT` waits until no input is queued. So when the PC is slow, two queued key repeats could fall into one paint. With the posted task they stay two Page Turns, which is what the decision says ("each key-repeat or wheel notch is its own turn"). It also lets the Curl start before SumatraPDF paints.

The posted task runs **before** SumatraPDF's own repaint task (hook #3 comes before `RepaintDisplay` at `DisplayModel.cpp:3024`). So the new code gets a moment to put the Curl layer up before SumatraPDF draws the new Spread. Whether that is enough for no flash is [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19). If it isn't, the task can also wait there briefly until the layer's covering frame is presented. That needs no extra original lines.

## 4. The "don't animate" cases, with no extra lines

Every non-turn changes the view's pairing state **before** it calls `GoToPage`:

| Case | What changes first | Where |
|---|---|---|
| `SetDisplayMode` | `displayMode` set at `:3046`, then `GoToPage` at `:3068` | `src/shared/DisplayModel.cpp:3028` |
| `SetInPresentation` | `inPresentation` at `:3076`, then `SetDisplayMode` / `SetZoomVirtual` | `DisplayModel.cpp:3071` |
| `SetFacingStepOne` | `facingStepOne` at `:315`, then `GoToPage(startPage, 0)` at `:320` | `DisplayModel.cpp:311` |
| `B` key | `SwitchToDisplayMode` at `:3133`, then next/prev page in the same action | `OnFrameKeyB`, `src/shared/SumatraPDFCommon.cpp:3113` |
| EPUB renumbering | `syncingWithEngineLayout = true` (`:740`), `layoutGeneration` bumps, then `SetScrollState` → `GoToPage` | `SyncWithEngineLayout`, `DisplayModel.cpp:733-768` |
| Opening a document | new `DisplayModel`, first `GoToPage` before its first paint | `SetInitialViewSettings`, `DisplayModel.cpp:1524` |

So the new code keeps, per window, a **fingerprint** taken at every paint (hook #6) and at every turn hook: `dm`, `displayMode`, `facingStepOne`, `inPresentation`, `layoutGeneration`, `PageCount()` (all public fields of `struct DisplayModel`, `DisplayModel.h:331-449`). A turn animates only if:
- the switch is on, the view is a Paged View, and Direct3D loaded;
- the fingerprint at the hook equals the one from the last paint (catches every row above; the paint-time refresh also catches a trip through Continuous View, where hook #3 never fires);
- `dm->syncingWithEngineLayout` is false (`DisplayModel.h:332`);
- `dm->StepsOnePage()` is false (`DisplayModel.cpp:1127-1130`): "step one page" on means no Curls and no Bundles at all ([What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16));
- the window is not minimized or hidden (`IsIconic` / `IsWindowVisible` on `win->hwndFrame`).

**Hook #8** clears the fingerprint when a `DisplayModel` dies (`ControllerCallbackHandler::CleanUp`, `src/SumatraPDF.cpp:1077`, called from `~DisplayModel` at `DisplayModel.cpp:1353`). Without it a new document allocated at the freed address could match the old fingerprint and curl on opening.

## 5. Room for Page Edges, and drawing them

`windowMargin` (`DisplayModel.h:362`) is read in about 20 places: fit zoom (`DisplayModel.cpp:1750`, `:1898`, `DocumentLayout.cpp:109-110`), layout (`:2300`), scrolling (`:2151-2188`, `:2981-3009`, `:4279-4314`). So the room must be in `windowMargin` itself. It is written in only two places: `SetUiDpi` (`:1324`) and Presentation's zeroing (`:3081`).

[Where do Page Edges go when the Spread fills the window?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/10) suggested hooking `SetUiDpi`. That alone misses display-mode switches, which never call `SetUiDpi` (`SetDisplayMode`, `:3028-3069`). **`Relayout` is better** (hook #4, `:2209`): every zoom, rotation, mode switch, Spread change, resize and settings reload goes through it (comment at `:2203-2208`; reload path `src/shared/AppSettings.cpp:426-432`), and it runs before fit zoom is computed (`ViewportReadyForRelayout` at `:2226`, `CalcZoomReal`).

The new code adds `extra` to `windowMargin.left/right`, with `extra` = this document's whole-book thickness in a Paged View with the switch on and not in Presentation, else 0. It remembers what it wrote. If `windowMargin` no longer equals that, `SetUiDpi` or Presentation reset it, and the current value is the new base. Switching the setting or the view calls `dm->RelayoutKeepingView()` (`:2358`) from the new code.

**Drawing (hook #5):** after the background is painted and the clip restored (`src/Canvas.cpp:3096-3098`), before pages (`:3100`). Page shadows are compiled out (`// #define DRAW_PAGE_SHADOWS`, `src/shared/DisplayModel.h:5`), so nothing SumatraPDF draws overlaps the margin.

## 6. Cover right of the spine (asked for during this ticket)

Decided in [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16): with the switch on, the lone Book View cover sits right of the spine (left in right-to-left), Presentation too.

Today, in `LayoutFacing` (`src/shared/DocumentLayout.cpp`):
- column widths come only from **shown** rows (`:259-279`; `isShown` is only the current Spread in a Paged View, `:250`). The cover goes in column 1 (`:273`), so column 0 is 0 wide;
- the cover is centred: `cover && !IsContinuous(...)` at `:309-310` (the `cover` flag at `:305`);
- the canvas (`:296`) is then narrower than the window and gets centred by `CenterCanvasX` (`:208-216`).

Turning off the centring alone isn't enough: with column 0 at 0 width, the cover would still land almost in the middle. The smallest change:

1. `DocumentLayout.h:43`: `bool pageBesideSpine = false;` in `DocumentLayoutParams` (+1).
2. `DocumentLayout.cpp:289`: if the flag is set and exactly one column is 0 wide, give it the other column's width (+3). This only triggers for a lone-page row: a normal two-page Spread has both columns, a "landscape as spread" row has neither (`:270-271`).
3. `DocumentLayout.cpp:309`: change to `cover && !IsContinuous(params.displayMode) && !params.pageBesideSpine` (1 changed). The cover then takes the right-column branch (`:313-314`), and right-to-left mirrors it (`:316-318`).
4. `DisplayModel.h:431`: `bool pageBesideSpine = false;` (+1), set by the new code in hook #4.
5. `DisplayModel.cpp:2306`: `params.pageBesideSpine = pageBesideSpine;` (+1).

Size doesn't change: fit zoom in Facing/Book View already fits two columns (`DocumentLayout.cpp:97-103`), so the cover keeps today's size. Switch off: flag is false, layout is byte-for-byte the original. ng never sets it.

**Finding about the lone last page:** [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16) says a lone last page "already sits left of the spine" (`:311-312`). That is true in Continuous View, where all rows add to the column widths. In a Paged View only the last row is shown, so column 1 is 0 wide and `CenterCanvasX` centres the page (off by half the page gap). Step 2 above also fixes that, with no extra lines. Found by reading the code, not by running it.

Zoomed in beyond the window, `GoToPage` scrolls page 1 so its left edge sits at the margin (`DisplayModel.cpp:2987-2989`, "don't display the blank space beside the first page"). With the cover beside the spine, that scrolls the empty left half away. Probably fine; worth a look in testing.

## 7. When SumatraPDF really shows the new Spread

**Hook #6** gets `flushed` = the exact flush condition of `OnPaintDocument` (`src/Canvas.cpp:3324`), repeated as an argument, so the original line stays as it is. `flushed == true` means the screen now shows SumatraPDF's own picture of the current Spread:
- all pages drawn, or one page of a Facing Spread drawn and the other a placeholder (`:3143-3148`);
- "Rendering page N..." after 500 ms (`:3153-3165`, `kRenderDelayShowNotif` at `:98`);
- "Couldn't render page N" (`:3128-3139`, `:3167-3172`);
- no page visible (`:3257-3259`).

`flushed == false` means the old screen is still up (no-flicker hold). Not called in Presentation black/white screens (`:3315-3318`).

Together with `gRenderCache->VisibleTargetTilesReady(dm)` (`src/shared/RenderCache.h:228`) and `IsBusyFor(dm)` (`:222`), that is all the new code needs to keep the layer up until SumatraPDF really shows the new Spread or its message, as [What does a Curl show if a page is not ready yet?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/11) requires. The same paint hook also refreshes the fingerprint (section 4) and starts the Direct3D setup (section 9).

## 8. The Curl layer's life: create, resize, destroy

No hook lines. The new code **subclasses the document window** (`win->hwndCanvas`) with `SetWindowSubclass` the first time hook #6 sees a window. The repo already does this (`src/Menu.cpp:1813`, `src/AIChatPanel.cpp:1014`, `src/gui/win/BrowserDocView.cpp:206`).

| Event | How the new code sees it |
|---|---|
| Resize, maximize, fullscreen, Presentation | `WM_SIZE` on the canvas (SumatraPDF's own handler: `src/Canvas.cpp:5381-5387` → `UpdateCanvasSize`, `src/MainWindow.cpp:324-345`) |
| DPI change | frame gets `WM_DPICHANGED` (`src/SumatraPDF.cpp:14140`), which resizes the canvas → `WM_SIZE`; margins follow through hook #4 |
| Tab switch | hook #3 fires with a different `dm` (`LoadModelIntoTab` restores the view, `src/SumatraPDF.cpp:4494-4510`), and hook #6 sees `win->AsFixed()` change |
| Document closed | hook #8 |
| Window closed | `WM_NCDESTROY` on the canvas (a child of the frame, created at `src/SumatraPDF.cpp:2853`) |
| Switch turned off | the new command handler (hook #14) |

What happens to Curls in the air at these moments is a plan decision (see "Open questions").

Facts the layer code needs:
- The canvas has non-client scrollbars (`WS_HSCROLL | WS_VSCROLL`, `src/SumatraPDF.cpp:2847-2850`). Size the swap chain to the client area (`HwndClientRect`, as `MainWindow.cpp:328` does) and clip the visual to it, so Curls never draw over scrollbars.
- Create the target with `topmost = FALSE`: the visual tree then sits between the window's own drawing and its child windows, so form-field and annotation edit boxes stay on top ([CreateTargetForHwnd](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd), "four layers"). The spike used `TRUE` (`spikes/curl-layer/curl-layer-spike.cpp:463` on `spike/curl-layer`). The window "must belong to the calling process", nothing about threads (same page).
- Headers: the build targets Windows 7 (`WINVER=0x0601`, `NTDDI_VERSION=0x06010000`, `premake5.lua:102-110`), and `dcomp.h` only declares anything for `NTDDI_VERSION >= NTDDI_WIN8` (Windows SDK 10.0.26100 `um/dcomp.h:19`). Copy the trick in `src/shared/PrintWin11.cpp:34-52` (`#pragma push_macro("NTDDI_VERSION")`, redefine, include, pop). Load `dcomp.dll`, `d3d11.dll`, `dxgi.dll` with `LoadLibrary`/`GetProcAddress`, as `PrintWin11.cpp:351-360` and `src/gui/GfxDirect2D.cpp:55-83` do. Then no link settings change.

## 9. Direct3D setup ahead of the first turn

[Can a Curl layer run smoothly on this PC?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/14) measured ~55 ms (device 34, layer 4, shaders 15) and said to do it before the first tap. Hook #6 gives the moment: the first paint of a Paged View with the switch on. Hand the work to the new render thread (section 10), so the screen isn't blocked for 55 ms. `ID3D11Device` is thread-safe ([Introduction to Multithreading in Direct3D 11](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)), and DirectComposition "is thread-safe. An application can call any method from any thread at any time" ([DirectComposition architecture](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)). A tap that comes before setup has finished is an instant jump. Use the default adapter (`nullptr`), as decided.

## 10. Timing frames to the screen refresh

### What SumatraPDF has

- The main loop is a plain `GetMessage` loop (`src/SumatraPDF.cpp:15274-15294`). Dialogs run their own (`RunModalWindow`, `src/gui/win/WindowBase.cpp:2098-2125`), and so do menus and window dragging, inside Windows.
- Its only animation, smooth scrolling, is `SetTimer(..., 1, ...)` plus `timeBeginPeriod(1)` (`src/Canvas.cpp:330-333`). `SetTimer` rounds anything below 10 ms up to `USER_TIMER_MINIMUM` ([SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer)), so its ticks drift against the 16.7 ms refresh.

### Documented options for a DirectComposition layer

| Option | What Microsoft says | Windows | Fit here |
|---|---|---|---|
| **Waitable swap chain** | Create with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`; `GetFrameLatencyWaitableObject` "returns a waitable handle that signals when the DXGI adapter has finished presenting a new frame"; "for every frame it renders, the app should wait on this handle before starting any rendering", including the first; close it with `CloseHandle` ([GetFrameLatencyWaitableObject](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)). Default latency is 1, "ensures that DXGI does not queue more than one frame at a time ... only render after each VSync" ([Reduce latency with DXGI 1.3 swap chains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)) | 8.1+ | **Recommended.** The spike used exactly this (`SetMaximumFrameLatency(1)`, wait, draw, `Present(1, 0)`) and got 59.4-60 fps with ≤0.7 % late frames on the Iris Xe |
| `Present(1, 0)` alone | flip model: "Synchronize presentation for at least n vertical blanks" ([Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)) | 8+ | blocks inside `Present` instead of before drawing, so each frame is drawn on older data; fallback only |
| **Compositor clock** | `DCompositionWaitForCompositorClock(count, handles, timeout)` "halts a thread until the next signal from the compositor clock"; returns at once with `STATUS_GRAPHICS_PRESENT_OCCLUDED` when the display is off ([function](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-dcompositionwaitforcompositorclock)). It is meant "to replace usage of IDXGIOutput::WaitForVBlank", can also wake on the app's own events, and fixes that "the DXGI swapchain API always uses the cadence of the primary monitor". Apps "are expected to use this API to control rendering threads, separate from general-purpose UI threads" ([Compositor clock](https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock)) | Windows 11 (build 22000+) | optional upgrade, loaded by `GetProcAddress` (the SDK only declares it for `NTDDI_WIN10_CO`, `dcomp.h:163-236`). The function page says it returns an NTSTATUS, while the overview's example checks `WAIT_OBJECT_0 + 1`; test before relying on either |
| Frame statistics | `IDCompositionDevice::GetFrameStatistics` gives timing "to synchronize the rasterization of bitmaps with independent animations"; `nextEstimatedFrameTime` estimates the next compositor frame ([GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-getframestatistics); [Compositor clock](https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock)) | 8+ | use it to place each Curl at the moment its frame will show, not when it was drawn |
| `MsgWaitForMultipleObjectsEx` in the UI loop | waits on handles **and** window messages; returns `WAIT_OBJECT_0 + nCount` for new input; needs `MWMO_INPUTAVAILABLE` to not miss input already seen ([MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex)) | XP+ | would mean replacing `GetMessage` at `src/SumatraPDF.cpp:15277`, and Curls would still freeze inside menus, dialogs and window dragging. Rejected |

### Recommendation: one render thread owned by the new code

- **Thread:** one for the whole app, one D3D11 device; one composition swap chain + DirectComposition target per `MainWindow`.
- **It owns** the immediate context and **all DXGI calls** (`Present`, `ResizeBuffers`, surface `GetDC`). Microsoft: "only one thread at a time should use the immediate context. However, your application should also use that same thread for ... DXGI operations, especially when the application makes calls to the IDXGISwapChain::Present method" ([Multithreading and DXGI](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)). The deadlock warnings in the [DXGI overview](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi) ("Multithread considerations") concern full-screen swap chains tied to a window; a composition swap chain has no window.
- **Loop:** idle = `WaitForSingleObject(wakeEvent, INFINITE)`, so no CPU and no wakeups when nothing moves. With Curls in the air: wait on `{frameLatencyWaitable, wakeEvent}`, compute each Curl from the clock (`QueryPerformanceCounter`, or `nextEstimatedFrameTime`), draw, `Present(1, 0)`. Curls are clock-timed, as decided, so a slow frame shows fewer frames, not a longer Curl.
- **UI thread** (the hooks) never waits on the GPU. It puts commands (start Curl with its pictures, resize, show/hide, tear down) in a locked queue and sets `wakeEvent`. DirectComposition `Commit` stays on the UI thread only, because a `Commit` "on one thread will submit the batch of all commands issued by both threads" ([DirectComposition architecture](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)).
- **Screen copies:** the UI thread copies the needed part of `win->buffer` into its own memory at hook #3/#6 time (it is overwritten by the next paint); the render thread uploads it. That's the "two steps" path from [Can a Curl layer run smoothly on this PC?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/14) (10.4 ms for a whole 4K window, less for just the Curl's area). The faster 6.6 ms path (GDI straight into a GDI-compatible GPU image) needs `IDXGISurface1::GetDC`, a DXGI call, so it belongs on the render thread or behind a lock shared with it. Worth measuring in [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19).
- **Device lost** (`DXGI_ERROR_DEVICE_REMOVED` from `Present`, e.g. driver update, [Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)): drop all layers, turns become instant jumps, set up again on the next paint.
- **Message loop changes: none.**

## 11. Settings and the View-menu switch without changing ng

Pieces and models as in [How are settings and View-menu switches added?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/7) (`HighlightFormFields` / `CmdToggleDisableLinks`). For ng specifically:
- Settings go only into `cmd/gen-settings.ts`. `cmd/ng-gen-settings.ts` is a separate file and doesn't import it (`cmd/ng-gen-settings.ts:4-6`), so ng's `src/ng/Settings.h` doesn't change. `agents.md` says to add new settings to both; this fork deliberately doesn't. So `gSettings->...` for the new fields may only be read in classic-only files (the new folder, `src/Menu.cpp`, `src/SumatraPDF.cpp`).
- The command is shared (`cmd/ng-gen-commands.ts` re-exports `gen-commands`). ng builds its menus from the same `menuDefMenubar` → `menuDefView` (`src/ng/Menu.cpp:765`, `src/shared/MenuDefs.cpp:886-893`), so the new menu entry **would show up in ng** unless hidden. Hook #12 hides it from ng's menus and command palette with the existing `SUMATRA_NG` pattern (`src/shared/CommandAvailability.cpp:846-853`, `:1034-1042`).
- The click handler (hook #14) only forwards: `ShouldToggle`, flipping the setting, `ScheduleSaveSettings()`, removing layers and `RelayoutKeepingView()` for every window all live in the new file.
- The speed value is read live at each Page Turn, so editing it in Advanced Settings needs no hook. A settings reload already relayouts (`AppSettings.cpp:426-432`), so hook #4 picks up an on/off change from there too.
- Not checked: ng's Windows build reads and writes the same `SumatraPDF-settings.txt` (`src/shared/AppSettings.cpp:256-262`). If it drops unknown keys when saving, running ng would reset the two new settings. ng is only for testing, so this only matters if someone runs it.

## 12. Recommended layout of the new files

Everything in `src/skeuomorph/` (unique name, so upstream won't collide; one `premake5.files.lua` line picks up every file). Follow `agents.md`: `base/Base.h` first, own header last, no `#pragma once`, comments in `.cpp`.

```
src/skeuomorph/
  PageTurnHooks.h/.cpp  the only header original files include; one function per hook
                        (BeforeSpreadChange, BeforeRelayout, AfterPaint, PageEdgesPaint,
                        Forget, Toggle); checks switch + Paged View, owns per-window state,
                        subclasses the canvas
  PageTurnLogic.h/.cpp  pure decisions, no Win32/DirectX: fingerprint filter, action →
                        Curl / Bundle / nothing, fast-tapping rhythm (gap × 4, ≥ 310 ms,
                        ≤ 5 in the air), Bundle thickness, Page Edges sizes, room per side,
                        right-to-left direction
  PageEdges.h/.cpp      margin room and cover flag (from BeforeRelayout), GDI drawing of
                        resting Page Edges
  ScreenCopy.h/.cpp     the only code that reads win->buffer; copies regions to memory
  RoughPages.h/.cpp     Rough Page requests through RenderCache::Render(..., callback),
                        pacing from "How can Rough Pages share the render queue..."
  CurlLayer.h/.cpp      per window: DirectComposition target + composition swap chain,
                        show/hide/resize/destroy; LoadLibrary of dcomp/d3d11/dxgi;
                        the NTDDI_VERSION header trick
  CurlRenderer.h/.cpp   the render thread: device, frame loop, command queue, picture
                        upload, Curl and Bundle meshes and shading
  CurlShaders.h         compiled shader bytes (if precompiled, see open questions)
  tests/PageTurn_ut.cpp unit tests for PageTurnLogic, registered in src/tests/Sumatra_ut.cpp
```

Layering: original files → `PageTurnHooks` → (`PageTurnLogic`, `PageEdges`, `ScreenCopy`, `RoughPages`, `CurlLayer`) → `CurlRenderer`. Only `CurlLayer`/`CurlRenderer` include DirectX headers. Only `PageTurnHooks` knows `MainWindow`. That makes `PageTurnLogic` testable with the debug `-unit-tests` run, which [How do we check it works and nothing else changed?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/21) needs.

## Open questions this raises for the plan

1. **Curls in the air when the window changes** (resize, maximize, DPI, fullscreen, tab switch, switch off): the screen copies no longer fit. Suggestion: end all Curls at once (land instantly, Page Edges correct), like the don't-animate cases.
2. **Windows version floor:** the waitable swap chain needs Windows 8.1, DirectComposition Windows 8. Below that, Page Turns are instant jumps. Do the resting Page Edges (plain GDI, work everywhere) still show there? Suggestion: yes, they don't depend on the layer.
3. **Compositor clock on Windows 11:** use it from the start or only if multi-monitor setups with different refresh rates stutter? Suggestion: start with the waitable swap chain (proven in the spike).
4. **Shaders:** compile at startup with `d3dcompiler_47.dll` (15 ms, part of Windows 10+) or commit precompiled bytes (needs a one-off generator script, no build change)?
5. **Render thread:** the plan should accept a second thread with a locked command queue. The alternative (drawing on the UI thread) can't follow the refresh without changing the message loop, and freezes in menus and dialogs.
6. **Lone last page in Paged Views is centred today**, not left of the spine as [What happens at the cover, the last page and other odd cases?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/16) assumed (section 6). The cover fix covers it at no extra cost; the decision should say it applies to the last page too.

## Sources

Code: this repo at `1d0b75378`, file:line as cited. Spike: `spike/curl-layer` branch.

Microsoft:
- [IDCompositionDevice::CreateTargetForHwnd](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd)
- [DirectComposition architecture and components](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)
- [Compositor clock](https://learn.microsoft.com/en-us/windows/win32/directcomp/compositor-clock/compositor-clock)
- [DCompositionWaitForCompositorClock](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-dcompositionwaitforcompositorclock)
- [IDCompositionDevice::GetFrameStatistics](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-getframestatistics)
- [IDXGISwapChain2::GetFrameLatencyWaitableObject](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject)
- [Reduce latency with DXGI 1.3 swap chains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)
- [IDXGISwapChain::Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
- [DXGI overview](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/d3d10-graphics-programming-guide-dxgi)
- [Introduction to Multithreading in Direct3D 11](https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-render-multi-thread-intro)
- [MsgWaitForMultipleObjectsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex)
- [SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer)

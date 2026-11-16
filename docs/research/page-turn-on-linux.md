# What would it take to bring the Page Turn to Linux?

Ticket [#51](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/51), map [#36](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/36). Read from `scrolling` at `9bab0cc6c`. Code and docs only: ng was not built or run.

## Answer

The Page Turn's decisions are portable. Its drawing is not, and ng on Linux has nothing that can draw it smoothly today.

- **Portable now:** `PageTurnLogic` and its tests (~1,500 lines). The bend math in `Curl.hlsl` translates to any language.
- **Portable after small shims:** detection, Page Edges, Rough Pages and the hooks. They use `MainWindow` fields, `HDC`, `HWND` and a few Win32 thread calls.
- **Windows-only:** the Curl layer, the Curl renderer, screen copies, window subclassing and the flash recorder. That is about two thirds of the code.
- **GPUI** (ng's toolkit) on Linux is a 2D painter. It draws with Cairo on the CPU into one buffer per window and copies that buffer to an X11 window. It has no shaders, no GPU textures, no layers and no way to draw a picture bent, rotated or skewed.

**Recommendation:** stay a Windows-only hooked fork for now. Do one cheap thing to keep the door open: split the Curl renderer into "what to draw" and "how Direct3D draws it" the next time it changes anyway (Grab and drag, or the #45 state work). If Linux becomes real, the human should first spend a week reading in ng on Linux. Then build a throwaway prototype of route A (a CPU-drawn Curl, below) to measure frame times before committing to anything.

## 1. Which parts are portable

Sizes are line counts on `9bab0cc6c`.

| File                         | Lines | Status                   | What ties it to Windows                                                                                                                                                                                                                                                           |
| ---------------------------- | ----: | ------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `PageTurnLogic.h/.cpp`       |   963 | portable as is           | Nothing. Its header says "plain values in, no Windows" (`PageTurnLogic.h:4`). It includes only `base/Base.h`.                                                                                                                                                                     |
| `PageTurnLogic_ut.cpp`       |   803 | portable as is           | Nothing.                                                                                                                                                                                                                                                                          |
| `Curl.hlsl` (bend math)      |   276 | portable once translated | HLSL. `Bend`, `Project` and the pixel shader are plain math over a 48x64 grid (`CurlRenderer.cpp:98-99`). They port to GLSL, Metal or C++.                                                                                                                                        |
| `PageTurnDetect.cpp`         |   486 | small shims              | `IsIconic` and `IsWindowVisible` on `win->hwndFrame` (`:235-237`), and classic's `MainWindow`. `uitask::Post` is already shared with ng (`cmd/helper/ng-shared.ts:79`).                                                                                                           |
| `PageEdges.cpp`              |   315 | small shims              | Layout rules are portable. Drawing is GDI `FillRect` into an `HDC` (`:231-241`, `:295`).                                                                                                                                                                                          |
| `RoughPages.cpp`             |   564 | small shims              | The MuPDF background copy is portable. It uses Win32 for its event (`HANDLE wake`), `InterlockedIncrement`, `SetThreadPriority` and `GetProcessAffinityMask` (`:64`, `:251`, `:400`, `:413`), and it paints into an `HDC` (`:552`).                                               |
| `PageTurnHooks.cpp`          |    74 | small shims              | Forwards from classic's hooks. It takes an `HDC` (`:49`, `:63`).                                                                                                                                                                                                                  |
| `CurlRenderer.cpp`           | 3,224 | Windows-only             | Direct3D 11, the swap chain's frame-latency wait and `Present`. Roughly half is choreography: which sheet, which picture, geometry, glide (`GeometryOf`, `GlideOf`, `FacePicture`, `DrawSheets`...). That half is portable in principle, but every function takes the D3D `Gpu&`. |
| `CurlLayer.cpp`, `CurlD3D.h` |   239 | Windows-only             | DirectComposition visual on `hwndCanvas` (`CurlD3D.h:12-15`).                                                                                                                                                                                                                     |
| `ScreenCopy.cpp`             |   177 | Windows-only             | GDI copies from classic's `win->buffer`. Ng has no such buffer, and GPUI's `CanvasReadPixels` returns `false` on Linux, macOS and Wasm (`ext/gpui/gpui.h:36027-36035`).                                                                                                           |
| `WindowChanges.cpp`          |   119 | Windows-only             | `SetWindowSubclass` on the canvas.                                                                                                                                                                                                                                                |
| `FlashRecorder.cpp`          |   975 | Windows-only, test only  | DXGI capture. Not needed on Linux.                                                                                                                                                                                                                                                |

The seam is already compiled into ng. `src/shared/DocController.h:79-80` has the two empty methods, and ng's `ControllerCallbackHandler` (`src/ng/MainWindow.cpp:76`) doesn't override them. Hooks 3, 4 and 7 in the shared `DisplayModel` / `DocumentLayout` therefore already run in ng and do nothing. The switch is hidden in ng at `src/shared/CommandAvailability.cpp:1043-1047`.

## 2. How ng draws pages, and what GPUI offers

**ng's page drawing.** `DrawDocument` (`src/ng/gui/DocCanvas.cpp:821`) paints each visible tile from the shared render cache. The tile's `Pixmap` becomes a GPUI `RenderImage` once, through `RenderImageFromBgra` (`:182-228`), and is cached on the entry. `RenderImageDraw` then places it, axis-aligned, under a rectangular clip (`:249-292`). Animation runs from GPUI's render loop: smooth scrolling ticks with real elapsed time in `DocCanvasSmoothScrollTick` (`DocCanvas.cpp:1144`, called from `src/ng/gui/AppShell.cpp:3285`). `WindowRequestAnimationFrame` (`gpui.h:9606`) asks for the next frame. Ng has no back buffer like classic's `win->buffer`, so "the old screen" would have to be rebuilt from cached tiles or from an offscreen paint (`PaintTargetBeginOffscreen` / `PaintTargetEndOffscreen`, `gpui.h:35971-35973`). Ng doesn't use those yet.

**GPUI's paint API** (`ext/gpui/gpui.h:35965-36103`, vendored dist `45f6b70`, 2026-11-08) offers:

- rectangles, rounded rectangles, lines and ellipses (`CanvasFillRect`, `CanvasLine`, `CanvasEllipseGradient`...);
- paths filled or stroked with a solid colour or a two-stop linear gradient (`PathFill`, `PathFillGradient`, `PathStrokeGradient`);
- rectangular clips only (`CanvasPushClip`);
- images drawn into an axis-aligned rectangle, optionally grayscale (`RenderImageDraw`);
- offscreen targets that read back as BGRA.

It has **no** image transform (rotate, skew or affine), no clip to a path, no blend modes, no triangle meshes, no custom shaders and no GPU texture handles. The only transforms are element-level `Rotate` / `Opacity` / `MotionTransform` for UI widgets (`gpui.h:6085`, `:10885`). Those don't apply to drawing calls.

**Backends** (`gpui.h:36130-36150`, `ImageFrame` at `:36198-36215`):

| Platform | Backend                                                                                                                                                                                                                                                     |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | Direct2D on a DXGI swap chain (`gpui.cpp:237543-237576`)                                                                                                                                                                                                    |
| Linux    | Cairo, CPU only, X11 (Xlib) only. Every frame draws the whole window into a `cairo_image_surface`, then copies it to the X window and calls `XFlush`, with no vsync (`gpui.cpp:229462-229484`, `:239455-239500`). Wayland desktops run it through XWayland. |
| macOS    | CoreGraphics (`CGContextDrawImage`)                                                                                                                                                                                                                         |
| Wasm     | its own JS bridge                                                                                                                                                                                                                                           |

So a bent, shaded page can't be drawn with GPUI as it is. The nearest it can do is a flat fold: a rectangular piece of the page under a rectangular clip, with gradient-filled paths for shading and shadow. That is a sliding wipe, not a Curl, because the fold can't slant and the paper can't bend.

GPUI is upstream's own framework. AGENTS.md says framework changes belong in `../gpui-kit-cpp`, and `gpui.cpp` / `gpui.h` here are published amalgams that must not be edited. Any GPUI change is therefore an upstream change, or a second fork.

## 3. Routes

Sizes are rough new or changed lines. Every route also needs the **shared groundwork**:

- Split `CurlRenderer` into a scene that lists what to draw and a backend that draws it.
- Replace the Win32 primitives in Rough Pages and detection with `base/` equivalents or `_win` / `_posix` siblings.
- Give the hooks a small "window" interface, so they don't reach into classic's `MainWindow`.

That groundwork is about 1,000-1,500 lines moved and a few hundred changed. It is useful on Windows too: the scene becomes unit-testable, which #45 also wanted.

**ng wiring**, needed by routes A-C:

- Override the two seam methods in ng's `ControllerCallbackHandler`.
- Add an after-paint hook and a Page Edges hook in `DocCanvas.cpp`.
- Add the setting to `cmd/ng-gen-settings.ts`.
- Un-hide the command.
- Land the turn on resize, with no subclassing: GPUI delivers resize events.
- Rebuild the "old screen" from cached tiles or an offscreen paint.

That is roughly 300-500 lines, and **10-20 hook lines in ng's own files**. ADR 0003 promised zero.

### A. A Curl drawn on the CPU, shown as a GPUI image

Translate the bend math to C++. Each frame, draw the bent sheet, shading and shadow into a BGRA buffer the size of the moving area. Split the rows across worker threads, then hand the buffer to GPUI with `RenderImageFromBgra` and draw it over the pages.

- **Size:** groundwork + ng wiring + a 1,000-1,500-line software renderer.
- **Needs no GPUI change.** Runs on Linux, macOS, ng-on-Windows and Wasm (Wasm single-threaded, so slower).
- **Looks:** the same Curl. It is the same math and can match the Direct3D output pixel for pixel in principle.
- **Risk: smoothness, unmeasured.** The bend costs per pixel, and on Linux GPUI then re-composites the whole window on the CPU every frame with no vsync. A 1080p window is plausible with several cores. 4K is doubtful. Tearing without vsync is likely.
- **This is the route a one-week prototype should measure first.**

### B. Add one drawing primitive to GPUI, draw the Curl as many small flat pieces

Add "draw an image through an affine transform, clipped to a triangle or path" to GPUI. All four backends have a native equivalent: Direct2D transforms with geometry masks, `cairo_set_matrix` with a path clip, `CGContextConcatCTM` with a clip, and Canvas2D `setTransform` with `clip`. Draw the sheet as a coarse grid of triangles, each with its own transform, and draw shading as gradient paths.

- **Size:** groundwork + ng wiring + ~800 lines of Curl drawing + a GPUI change of ~300-600 lines across four backends in `gpui-kit-cpp`.
- **Looks:** close. Each triangle is flat, so perspective is approximate, and shading follows the fold rather than each pixel.
- **Risk:** it depends on upstream accepting the GPUI change. Otherwise we fork GPUI too, and must re-vendor its amalgam on every update. On Linux it is still all CPU (Cairo), so it is no faster than A. It is faster on macOS and Windows, whose backends use the GPU.

### C. Our own GPU layer per platform, as on Windows

On Linux, an X11 child window over the canvas with OpenGL or Vulkan, and a GLSL port of `Curl.hlsl`. On macOS, a `CAMetalLayer`. Each mirrors `CurlLayer` + `CurlRenderer`.

- **Size:** groundwork + ng wiring + ~2,500-3,500 lines per platform.
- **Looks:** the same as Windows, at full frame rate.
- **Risk: high.**
  - Ng hands out a native window handle (`src/ng/SumatraPDF.cpp:7133`). A child window would cover GPUI-drawn edit boxes and popups in the canvas, which `topmost = FALSE` solves on Windows.
  - X11 has no equivalent of the swap-chain signal behind the no-flash rule (ADR 0005), so flashes are likely.
  - It doesn't help Wasm.
- **Two native graphics stacks to maintain**, in an app whose toolkit exists to avoid them (AGENTS.md: "Do not add native toolkit UI").

### D. Other cross-platform options

- **Run classic SumatraPDF under Wine on Linux.** Classic itself works there, but the Curl needs DirectComposition and Direct3D 11 (spec 5.5). Wine's DirectComposition support is, as far as I know, incomplete. If the layer fails to load, spec 3.11 makes page turns instant jumps, so the result is classic without the Curl. Not verified: a 30-minute test on a Linux box would settle it. This repo retired its own Wine builds (AGENTS.md), so expect no help there.
- **A separate cross-platform viewer** (another app, or a web viewer). Out of scope: everything else the human relies on in SumatraPDF would be lost.

### Summary

| Route             | New / changed lines                | Linux smoothness | Depends on upstream | Risk    |
| ----------------- | ---------------------------------- | ---------------- | ------------------- | ------- |
| Groundwork (all)  | ~1,000-1,500 moved + a few hundred | n/a              | no                  | low     |
| A. CPU Curl       | +1,300-2,000                       | unknown, measure | no                  | medium  |
| B. GPUI primitive | +1,100-1,300, plus GPUI ~300-600   | CPU-bound like A | yes (GPUI)          | medium  |
| C. Own GPU layer  | +2,800-4,000 per platform          | good             | no                  | high    |
| D. Wine           | 0                                  | probably no Curl | no                  | test it |

For scale, the Windows feature is ~8,500 lines in `src/skeuomorph/` without the generated shader bytes.

## 4. Would the human be happy reading in ng on Linux?

From code and docs only, ng is close to classic for everyday reading:

- **Same core:** it shares the engines, `DisplayModel`, layout, settings format and commands (`cmd/helper/ng-shared.ts` lists 485 shared paths).
- **Same reading features**, in its own files: tabs, sidebar with Bookmarks / Thumbnails / Favorites / Layers (`src/ng/gui/Sidebar.cpp:118`), find bar, command palette, smooth scrolling, presentation, annotations, read aloud (`ReadAloud_linux.cpp`), forms, signing on Linux with OpenSSL (`CommandAvailability.cpp:461`).
- **Of classic's `.cpp` files, only these have no ng or shared counterpart:** `MeasurementToolbar`, `PageInfoBar`, `SignatureUI`, `LayersUI`, `SumatraPDFWebDialog`, registry and preview handlers, and test files. Several (Layers, thumbnails) are rebuilt inside ng's own `Sidebar`.

Known gaps:

- **Hidden commands:** the measure tool (`CommandAvailability.cpp:1034`), replace or edit image externally (`:846`), the caption toggle off Windows (`:445`) and the Page Turn.
- **GPUI gaps noted in code:**
  - the clipboard is text only (`src/ng/ImageSaveCropResize.h:77`);
  - annotation icon cursors are Windows-only (`src/ng/AnnotPlacement.cpp:73`);
  - fullscreen is handled outside GPUI (`src/ng/gui/AppShell.h:120`).
- **Linux drawing is CPU-only on X11.** Smooth scrolling on a large or 4K screen may feel less fluid than classic's Windows GPU path. Not measured.
- **Young and fast-moving.** `src/ng` entered this repo on 2026-10-05 and has ~1,000 commits since, while `ext/gpui` was re-vendored 18 times. Releases are dated preview `.deb`s per Ubuntu LTS (AGENTS.md, `build-ng-linux.ts`).

Verdict: likely good enough for reading, but young. Only real use settles it, so try it before planning any port.

## 5. Effect on "stay hooked fork / integrate deeper / offer upstream"

- **Linux means ng, and ng doubles the merge surface.** Today's hooks touch classic files plus a few shared ones. A Linux port adds hooks in ng's own files, which upstream changes daily. Staying a hooked fork costs more with every app we hook.
- **Route B turns into an upstream conversation either way.** The GPUI primitive lives in upstream's framework, so a Linux Curl via B _is_ offering something upstream, just at the toolkit level.
- **Upstream acceptance (#46) said "Windows-only, because ng can't draw the Curl yet."** The groundwork split, plus route A or B, would answer that objection, and it is the shape upstream would want if it ever takes the feature.
- **So the Linux goal pushes toward "integrate deeper / offer upstream".** It doesn't force that choice now. While the human reads on Windows, the hooked fork remains the cheapest option, and the groundwork split is the only step whose value doesn't depend on the answer.

## Recommendation

1. **Now:** nothing Linux-specific. When `CurlRenderer` is next reworked (Grab and drag, #45's "one list of pages in the air"), split it into a portable scene and a Direct3D backend. Keep `PageTurnLogic` free of Windows, as it already is.
2. **When Linux becomes real:** the human reads in ng on Linux for a week. If ng is acceptable, build a throwaway prototype of route A on Linux and measure frame time at 1080p and 4K.
3. **Pick by the measurement.** Route A if smooth. Route B if A is too slow on macOS or Windows-ng but fine on Linux, and only after asking upstream about the GPUI primitive. Route C only if neither is good enough and the human accepts maintaining native graphics code per platform.
4. **Test Wine once** (route D): it may give classic on Linux without the Curl for zero cost.

## Sources

- `src/skeuomorph/*` (line counts and Win32 uses above), `src/skeuomorph/Curl.hlsl:1-40`, `CurlRenderer.cpp:98-99`
- `ext/gpui/gpui.h:9606`, `:35965-36103`, `:36130-36300`; `ext/gpui/gpui.cpp:229462-229484`, `:237543-237576`, `:239455-239500`; `ext/gpui/VERSION.txt`
- `src/ng/gui/DocCanvas.cpp:182-292`, `:821`, `:1144`; `src/ng/gui/AppShell.cpp:3285`; `src/ng/MainWindow.cpp:76`; `src/ng/MainWindow.h`
- `src/shared/DocController.h:79-80`; `src/shared/CommandAvailability.cpp:445`, `:461`, `:846`, `:1034-1047`
- `cmd/helper/ng-shared.ts`; `src/ng/README.md`; AGENTS.md "SumatraPDF ng" and "GPUI integration"
- `docs/skeuomorph-page-turn-spec.md` 3.1, 5.1-5.6; `docs/adr/0003-new-code-in-its-own-folder-behind-two-hooks.md`
- Closed tickets [#45](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/45) and [#46](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/46) (resolution comments)
- Not verified: Wine's DirectComposition support, and every frame-rate estimate. Nothing was built or run.

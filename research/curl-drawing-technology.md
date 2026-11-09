# Which drawing technology can show a realistic Curl smoothly?

Research for [#6](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/6), part of the map [#3](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/3). Terms (Curl, Bundle, Page Edges, Spread, Paged View) as in `GLOSSARY.md` on the `scrolling` branch.

Code references are to commit `d66646860` (the base of `scrolling`). `src/ng/` is ignored.

## Short answer

GDI can't do it. It copies and stretches rectangles, but it can't bend a picture, and on Windows it draws mostly on the processor, not the graphics card. A realistic Curl at 60 fps on a 4K screen needs the graphics card. The least invasive way: leave SumatraPDF's drawing exactly as it is, and put a see-through **animation layer** on top of the document area only while a Curl is playing. Windows offers this layer through **DirectComposition**. The Curls are drawn into it with **Direct3D 11** as a bent mesh (cylinder or cone mapping), with **Direct2D** for flat extras.

## 1. How the classic app paints today

- **The canvas is plain GDI with a back buffer.** `OnPaintDocument` (`src/Canvas.cpp:3308-3332`) calls `BeginPaint`, draws the whole document into `win->buffer->GetDC()` via `DrawDocument` (`src/Canvas.cpp:2941`), then `win->buffer->Flush(hdc)` copies it to the window.
- **The back buffer is one 32-bit DIB the size of the canvas.** `DoubleBuffer` (`src/base/Win.h:239-253`, `src/base/Win.cpp:1173-1236`) is a memory DC plus a `CreateMemoryBitmap` DIB. `Flush` is one `BitBlt` (`src/base/Win.cpp:1226`). It is recreated on every resize (`src/MainWindow.cpp:336-337`). The comment at `src/base/Win.cpp:1178` says it is 32-bit "so Direct2D can BindDC this memory DC".
- **Pages are cached bitmaps, blitted with GDI.** `RenderCache::PaintTile` ends in `BlitPixmapRegion` (`src/shared/RenderCache.cpp:1637`), which is `BitBlt` or `StretchBlt` (`src/base/Pixmap.cpp:509-545`). A `Pixmap` is BGRA8, often backed by a GDI DIB section (`src/base/Pixmap.h:38-58`). So a rendered page is already a ready-made picture that could be handed to the graphics card as a texture.
- **Page frame and shadow are GDI** (`PaintPageFrameAndShadow`, `src/Canvas.cpp:2321`, `2356`). A few marks use GDI+ (`src/Canvas.cpp:2759-2865`).
- **The canvas window** is a `WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS` child of the frame (`src/SumatraPDF.cpp:2844-2854`). It has its own child windows: form-field and annotation edit boxes (`src/FormFields.cpp:219`, `src/AnnotEditToolbar.cpp:1992`). The canvas DC is always left-to-right (`src/Canvas.cpp:3312-3313`).
- **Existing animation** (smooth scrolling) is a `WM_TIMER` loop: `SetTimer(..., 1, ...)` plus `timeBeginPeriod(1)` to reduce jitter (`src/Canvas.cpp:294-335`, handler at `src/Canvas.cpp:5079`). Each tick repaints the canvas through GDI. Note that `SetTimer` clamps anything under 10 ms to `USER_TIMER_MINIMUM` (10 ms) ([SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer)), so that loop is not tied to the screen refresh.

## 2. Does it already use Direct2D or Direct3D?

Yes, but not for the document.

- **Direct2D for UI controls, in software mode.** `GfxDirect2D` (`src/gui/GfxDirect2D.cpp`) draws toolbars, tabs and similar "virtual controls" through an `ID2D1DCRenderTarget` bound to an HDC (`src/gui/Gfx.h:4-9`). `d2d1.dll` and `dwrite.dll` are loaded by hand, falling back to GDI+ if missing (`src/gui/GfxDirect2D.cpp:11-13`, `55-83`). The render target is deliberately `D2D1_RENDER_TARGET_TYPE_SOFTWARE`. The comment at `src/gui/GfxDirect2D.cpp:101-104` says a hardware DC render target "copies its whole surface through GDI interop on every BeginDraw and EndDraw, which was 90% of a toolbar or tab bar paint", and its D3D device cost "~100 ms of driver loading" on first paint. The first `BindDC` also throws a first-chance C++ exception inside d3d11 (`src/gui/GfxDirect2D.cpp:86-88`, `src/ReadAloud.cpp:460`).
- **Direct3D 11 + Direct2D 1.1 for print preview only.** `PrintGraphics::Initialize` (`src/shared/PrintWin11.cpp:337-390`) loads `d2d1.dll` and `d3d11.dll` by hand, creates a hardware `ID3D11Device` with `D3D11_CREATE_DEVICE_BGRA_SUPPORT`, falls back to WARP, and creates an `ID2D1Device`. This is a working in-repo template for the device setup a Curl layer needs.
- **No DirectComposition, no DXGI swap chain on screen.** No `dcomp`, `IDXGISwapChain` or `CreateSwapChainFor*` in `src/` outside `src/ng/`. The libraries linked by default are GDI-era ones (`premake5.lua:275-279`). D3D/D2D are always loaded at runtime, never linked.
- **Overlay windows exist, CPU-composited.** The overlay scrollbar is a `WS_EX_LAYERED | WS_EX_TRANSPARENT` popup updated with `UpdateLayeredWindow` (`src/OverlayScrollbar.cpp:697-704`, `306`). The frame-rate window is also layered (`src/gui/win/FrameRateWnd.cpp:63`).
- **Build target is Windows 7 headers** (`WINVER=0x0601`, `premake5.lua:102-110`, used by `winver_defines`, `premake5.lua:136-138`). DirectComposition is Windows 8+ ([CreateTargetForHwnd requirements](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd)), so it must be loaded by hand from `dcomp.dll`, as d2d1/d3d11 already are.

## 3. Can GDI do a realistic Curl?

No, for three reasons.

1. **GDI can't bend a picture.** Its most flexible image copy, `PlgBlt`, maps a rectangle to a *parallelogram* ([PlgBlt](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-plgblt)). A curled page is curved, and seen in perspective it isn't even a parallelogram. The only GDI route is to compute every output pixel on the processor and blit the result.
2. **GDI draws mostly on the processor.** Microsoft: "GDI performs a large part of its rendering to aperture memory using the CPU", while "Direct2D translates its APIs calls into Direct3D primitives ... rendered on the GPU"; GDI keeps bitmaps in system memory, Direct2D in video memory ([Comparing Direct2D and GDI Hardware Acceleration](https://learn.microsoft.com/en-us/windows/win32/direct2d/comparing-direct2d-and-gdi)). Of GDI, only operations like BitBlt, AlphaBlend and StretchBlt are accelerated (same page).
3. **GDI has no frame model for animation.** "In the Direct2D model, an application renders to a back-buffer, and the result is displayed when the application is done drawing. This allows Direct2D to handle animation scenarios much more fluidly than GDI can." (same page). GDI also handles transparency inconsistently: "most GDI APIs simply ignore the alpha channel" (same page). Shadows and see-through paper need alpha.

**Rough load (my estimate, not from a source):** a 4K canvas is 3840 × 2160 ≈ 8.3 million pixels, 33 MB per frame at 4 bytes each. At 60 fps that is about 2 GB/s just to write the frame, before any bending, filtering, shading or several Curls overlapping, and the result then still goes through a full-canvas GDI `BitBlt`. A graphics card does this as a matter of course; a processor on a slow machine won't.

## 4. Direct2D, Direct3D, or both?

- **Direct2D through the existing DC render target: no.** It "renders Direct2D content to an internal bitmap, and then renders the bitmap to the DC with GDI" ([Direct2D and GDI Interoperability](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-gdi-interoperation-overview)). In hardware mode that is a GPU→CPU copy per frame. The repo already measured that copy as 90% of a toolbar paint (`src/gui/GfxDirect2D.cpp:101-104`); at full-canvas 4K size every frame it would be worse. In software mode it is back to the processor.
- **Direct2D 1.1 on its own surface: possible, limited.** `ID2D1DeviceContext::DrawBitmap` takes an optional 4×4 perspective matrix ([DrawBitmap](https://learn.microsoft.com/en-us/windows/win32/api/d2d1_1/nf-d2d1_1-id2d1devicecontext-drawbitmap(id2d1bitmap_constd2d1_rect_f_float_d2d1_interpolation_mode_constd2d1_rect_f_constd2d1_matrix_4x4_f))). A cylinder curl can be approximated as many thin flat strips, each drawn with its own perspective matrix (each strip of a cylinder is flat, so this is exact per strip). A cone curl (from a corner) has no flat strips, so it can't be drawn this way. Direct2D custom effects can take a custom vertex shader and vertex buffer ([CreateVertexBuffer](https://learn.microsoft.com/en-us/windows/win32/api/d2d1effectauthor/nf-d2d1effectauthor-id2d1effectcontext-createvertexbuffer)), but at that point it is Direct3D with extra wrapping.
- **Direct3D 11 mesh: the proven path.** Microsoft's own realistic page curl (Project "Austin", Windows 8) deforms a page **vertex buffer** around a cone, a cylinder, or a blend of both, every frame, and renders it with DirectX at a 60 fps target ([Brumer, MSDN Magazine, Aug 2013](https://learn.microsoft.com/en-us/archive/msdn-magazine/2013/august/directx-real-time-realistic-page-curling-with-directx-c-and-xaml)). The page picture is a texture on that mesh; the back of the page is just the other side of the same mesh; shading and shadow are cheap per-pixel work on the graphics card.
- **Both together** is normal: create one `ID3D11Device` with BGRA support and an `ID2D1Device` on it, as `src/shared/PrintWin11.cpp:337-390` already does. Direct3D draws the bent pages; Direct2D can draw flat extras (soft shadow gradients, Page Edges) into the same surface.

## 5. How page-curl effects work

- **Cylinder mapping.** Lay the page flat; roll it around an imaginary cylinder lying on the page. A flat point at distance *x* from the cylinder's touch line goes to the cylinder point whose *arc* length is *x*: angle β = x / r. Move the origin down by *r*, rotate by β, move back up ([Brumer](https://learn.microsoft.com/en-us/archive/msdn-magazine/2013/august/directx-real-time-realistic-page-curling-with-directx-c-and-xaml), "The Geometry of Page Curling", Figure 5). Then rotate the whole sheet about the spine by an angle α to turn it over. Paper doesn't stretch, which is why this looks right.
- **Cone mapping.** Hong, Card and Chen, "Turning Pages of 3D Electronic Books", IEEE 3DUI 2006, pp. 159-165 ([DBLP listing](https://dblp.org/db/conf/3dui/3dui2006)), wrap the page around a cone instead, so a page lifted at a corner curls more at that corner. Brumer blends cone and cylinder by where the page is grabbed: top corner → cone, middle → cylinder, bottom → inverted cone (same article, `curlPage`, Figure 6).
- **Mesh size and cost.** Brumer deforms the mesh on the processor (≈9 `sin`, 8 `cos`, 1 `asin`, 1 `sqrt` per vertex per frame) and needed SSE2 auto-vectorisation to stay well inside the 15 ms frame budget (same article, "Performance"). Doing the same maths in a vertex shader moves it to the graphics card entirely.
- **Smoothness.** Keep "current" and "target" curl parameters and move the current ones toward the target by a capped step each frame, so a fast input never jumps (same article, "Smooth Animation", Figure 8). Relevant to "tapping fast speeds Curls up".
- **Shading.** Light falls off along the bend (brightness from the surface normal), a shadow is cast on the page underneath near the fold, and the back of the page is the next page's image, usually a little darker. All are per-pixel and cheap on the graphics card.

## 6. Least invasive way to draw on top of the canvas

**Recommended: a DirectComposition layer on `hwndCanvas`, visible only while Curls are playing.**

- `IDCompositionDevice::CreateTargetForHwnd(hwnd, topmost, ...)` attaches a picture layer to an existing window. Microsoft: "the content is always composed on top of whatever is drawn directly to that window through the device context". With `topmost = FALSE` it sits **above the GDI canvas but below the canvas's child windows** (the four layers listed under Remarks in [CreateTargetForHwnd](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondevice-createtargetforhwnd)). Edit boxes for forms and annotations stay on top. It works on child windows and needs no new window.
- The layer's content is a swap chain from `IDXGIFactory2::CreateSwapChainForComposition`: flip model only, `DXGI_SCALING_STRETCH`, and **software reference drivers are not supported** ([CreateSwapChainForComposition](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition)). With premultiplied alpha, everything outside the Curl stays see-through, so the GDI canvas shows through.
- Windows' compositor runs in `dwm.exe`, composes "one frame for each vertical blank", and writes "directly to the screen back buffer" ([DirectComposition architecture](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)). No pixel goes back through GDI.
- **What SumatraPDF's own drawing needs to change: nothing in `DrawDocument`.** The canvas paints the *destination* Spread as it does today. The layer covers it with the parts that haven't turned yet (the old Spread's still-flat half and the Curls in the air). When the last Curl lands, the layer is cleared and the canvas underneath is already correct. This keeps the ground rule: layout, zoom and Fit Page are untouched, and with the switch off the layer is never created.
- **Where the pictures come from.** Either from the render cache (`Pixmap`, BGRA8, `src/base/Pixmap.h:38-58`), or simpler: a snapshot of the canvas back buffer (`DoubleBuffer`, a 32-bit DIB, `src/base/Win.cpp:1178-1182`) taken just before the turn. The snapshot matches "zoomed in, the Curl plays on what's on screen", and uploading one 4K snapshot per turn (≈33 MB) is a one-off, not per frame.
- **Fallback.** `dcomp.dll`, `d3d11.dll` and `d2d1.dll` are loaded by hand (as `src/gui/GfxDirect2D.cpp:55-83` and `src/shared/PrintWin11.cpp:351-360` already do). If any step fails, a Page Turn stays an instant jump, as in the original app.
- **Microsoft's note.** The DirectComposition docs recommend Windows.UI.Composition for Windows 10 apps ([DirectComposition architecture](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components), top note). That needs WinRT plumbing the repo doesn't have. DirectComposition is the plain-Win32 option, matching how the repo already loads DirectX.

**Alternatives considered:**

| Option | Smooth at 4K / 60 fps? | Invasive? | Verdict |
|---|---|---|---|
| GDI, processor-drawn into the existing back buffer | No: per-pixel bending on the processor, then a full-canvas `BitBlt` per frame | Changes the canvas paint path | Reject |
| Direct2D DC render target into the back buffer (existing `GfxDirect2D` style) | No: GPU→GDI copy per frame (`src/gui/GfxDirect2D.cpp:101-104`) | Changes the canvas paint path | Reject |
| Layered popup window with `UpdateLayeredWindow` (like the overlay scrollbar) | No: a processor-made picture of the whole canvas per frame | New window; must track canvas moves | Reject |
| Child window with its own D3D swap chain (`CreateSwapChainForHwnd`) | Yes | Opaque: it must redraw the canvas content under the Curl itself; adds a window that steals mouse input | Possible, worse |
| **DirectComposition layer on `hwndCanvas` + D3D11 (+ D2D)** | **Yes** | **No window, no canvas changes, see-through** | **Recommend** |

## 7. Bundles and Page Edges

- A **Bundle** is the same bent mesh made thick: two page surfaces (front and back), a few pixels apart, with a thin edge strip between them. Same layer, same technique.
- **Page Edges** are mostly still. They could be drawn by GDI in the canvas paint (cheap, but touches `DrawDocument` and needs space in the margin), or by the composition layer (no canvas change, but then the layer stays alive while a document is open, not just during a Curl). Which one is a decision for the "how it plugs in" ticket.

## Open points for later tickets

- **Timing.** The present-to-screen loop should follow the screen refresh (e.g. `Present` with sync interval 1, or the compositor's frame timing), not the 10 ms-clamped `WM_TIMER` used for smooth scrolling (`src/Canvas.cpp:327-333`).
- **Back of the page in Single Page view** (next page, blank, or mirrored?) is a behaviour decision, not a drawing one.
- **Several Curls at once:** each Curl needs its own front and back pictures; with snapshots, each new tap needs a picture of the page that will be revealed, which may not be rendered yet. How the render cache delivers these is part of the "how it plugs in" ticket.
- **Not verified here:** whether composition swap chains work with the WARP software device (Microsoft only names the reference driver as unsupported), and real frame times on this machine. A small prototype would settle both.

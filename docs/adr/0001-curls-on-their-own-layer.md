# Curls are drawn on their own see-through layer, not by SumatraPDF

SumatraPDF draws pages with GDI, which can copy and stretch rectangles but can't bend a picture. It also works mostly on the processor, too slow for a realistic Curl at 60 fps on a 4K screen. So Curls and Bundles are drawn with **Direct3D 11** (a page mesh bent around a cylinder or cone) on a see-through **DirectComposition layer** laid over the document area. The layer is shown only while Curls play, and it's drawn on its **own thread** that is woken once per screen refresh. SumatraPDF's own drawing stays exactly as it is. If any of this can't load (Windows 7/8.0, Remote Desktop, a broken driver), a Page Turn is simply an instant jump.

## Considered options

- **GDI on the processor, or Direct2D drawn into GDI:** too slow at 4K, and GDI can't bend a picture.
- **Layered pop-up windows:** too slow at 4K.
- **A separate Direct3D child window:** it's not see-through and it steals mouse clicks.
- **Rewriting SumatraPDF's page drawing in Direct3D:** a huge change to original files, which would make every upstream update painful.
- **Drawing on SumatraPDF's own message loop with a timer:** Windows timers can't tick faster than about every 10 ms, and drawing would freeze while a menu or dialog is open.

## Consequences

- A second thread and hand-loaded DirectX DLLs. Shaders are precompiled and committed, so there's no runtime compiler.
- Tested on this PC (Iris Xe): 60 fps even at 4K with 5 Curls and a Bundle. With software graphics only (WARP), Curls still play on time, but choppy in big windows.
- Use the default graphics adapter, not the faster discrete one: that one needs 1.2 s to wake and gave more late frames.
- The layer sits below the window's own child windows, so form and annotation edit boxes stay on top.

Source: [Which drawing technology can show a realistic Curl smoothly?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/6), [Can a Curl layer run smoothly on this PC?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/14), [Can the Curl layer cover a jump with no flash?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/19).

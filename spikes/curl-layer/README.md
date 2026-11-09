# Curl layer spike (throwaway, never merged)

Answers [Can a Curl layer run smoothly on this PC?](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/14).

`curl-layer-spike.exe` opens a plain GDI window (a checkerboard standing in for SumatraPDF), puts a see-through
DirectComposition layer over it, and draws bent, textured pages on that layer with Direct3D 11. It runs four scenes for
5 seconds each and records frame times:

| Scene | What is drawn each frame |
| --- | --- |
| see-through, 1 Curl | one Curl, nothing else (the checkerboard must show around it) |
| 1 Curl | window-size copies of the old and new Spread, plus one Curl with its shadow |
| 5 Curls | the same with 5 Curls in the air (the most allowed) |
| 5 Curls + Bundle | the same plus an 18-sheet Bundle |

It also measures setup time, copying a window-size GDI buffer into a GPU image (a Curl's picture), and how long
showing/hiding the layer takes.

## Run it

```powershell
.\build.bat; .\run-spike.ps1
```

Takes about 2 minutes; windows open and close by themselves. Results land in `results\`.

## Results on the author's laptop

Intel Iris Xe (drives the 3840×2400, 60 Hz screen) + NVIDIA MX550, i7-1260P. "Late" = a frame shown more than 1.5 screen
refreshes after the previous one.

| Run | 1 Curl | 5 Curls | 5 Curls + Bundle | Busiest frame's GPU time (p95) |
| --- | --- | --- | --- | --- |
| Iris Xe, maximized 3840×2241 | 59.4 fps, 0.7% late | 59.7 fps, 0.3% late | 59.7 fps, 0.7% late | 13.1 ms of 16.7 |
| Iris Xe, 1920×1200 | 60.0 fps, 0% late | 59.8 fps, 0.3% late | 60.0 fps, 0% late | 9.4 ms |
| MX550, maximized | 59.6 fps, 0.7% late | 58.1 fps, 3.2% late | 58.9 fps, 1.7% late | 5.5 ms |
| WARP (software), maximized | 21.7 fps | 9.3 fps | 6.9 fps | 159 ms |
| WARP, 1920×1200 | 60.0 fps | 28.3 fps | 16.4 fps | 57 ms |
| WARP, 1280×800 | 60.0 fps | 58.4 fps, 2.7% late | 27.3 fps | 34 ms |

Other numbers (Iris Xe, maximized): setup ~55 ms (device 34, layer 4, shaders 15); copying one window-size GDI buffer
to the GPU 10.4 ms in two steps, 6.6 ms straight into a GDI-compatible GPU image; showing or hiding the layer takes
effect after one screen refresh (16.7 ms); 401 MB GPU memory for 2 window-size and 10 page-size images. The MX550 needs
1.2 s to wake up for its first device.

Raw output and screenshots are in `results\`.

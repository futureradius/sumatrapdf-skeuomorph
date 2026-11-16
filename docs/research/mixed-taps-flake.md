# Why `skeuomorph-mixed-taps` fails sometimes

Ticket [#42](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/42), map [#36](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/36). Code as of `9bab0cc6c` (branch `scrolling`).

## Answer

**The app has the bug, not the test.** A race between two clocks:

- the UI thread's `TurnClock` decides whether a tap sends a page back or starts a new Curl;
- the drawing thread keeps its own copy of every Curl's position, and only updates it when it draws a frame.

Sometimes the UI clock says a Curl has just landed, a few ms before the next tap in the other direction. The UI then starts a new Curl. But the drawing thread last moved that Curl up to a frame earlier, so it still sees it in the air, heading the other way. It then lands **every Curl in the air at once** (`"next Curl"`). The reader sees pages drop without animation, which is the symptom #33 was about. The test catches this correctly.

It is not about focus: keys are posted to the window, and the failure shows up in the log as a timing disagreement. A busy PC makes frames later, so the gap is wider and the race more likely.

## Reproduction

`bun tests/skeuomorph-mixed-taps.ts` against `out/dbg64/SumatraPDF.exe` (built 12:51 on 2026-11-16, between `d803bca76` and `79ad3e612`; the commits after it only touch Rough Pages). Results:

- 15 runs: 1 failure (run 13: Single Page, seed 2, `at once: 1x next Curl`). The other 14 passed.
- `.work/test-times.txt` in the main checkout: 3 failures in 11 earlier runs.
- The tap order is fixed per seed, but the timing jitters. Seed 2 in Single Page has a reversal that lands within a few ms of the next tap, so it fails most often. Facing goes through the same code.

The failing log, in UI clock ids (`sends #39 back`) and drawing-thread ids (`#64`...`#67`; a Curl gets a new drawing-thread id each time it is sent back):

```
skeuomorph: Curl backward, 1 page, sweep right, 310 ms, 3 in air, 0 landed [379-379 -> 378-378]    <- UI #39 starts
skeuomorph: Curl forward, 1 page, sweep left, 580 ms, 2 in air, 1 landed [378-378 -> 379-379], sends #39 back
skeuomorph-curl: #64 sent back after 144 ms, 8 frames, ...
skeuomorph: Curl backward, 1 page, sweep right, 336 ms, 1 in air, 1 landed [379-379 -> 378-378], sends #39 back
skeuomorph-curl: #65 sent back after 84 ms, 4 frames, ...
skeuomorph: Curl forward, 1 page, sweep left, 310 ms, 1 in air, 0 landed [378-378 -> 379-379], sends #39 back
skeuomorph-curl: #66 sent back after 48 ms, 2 frames, ...
skeuomorph: Curl backward, 1 page, sweep right, 588 ms, 1 in air, 0 landed [379-379 -> 378-378]    <- not "sends #39 back"
skeuomorph-curl: #67 landed at once (next Curl) after 146 ms, 7 frames, ...                        <- the failure
```

Replaying the UI clock (`TurnClock::Advance`, `Reverse`) with these gaps and lengths:

| Step                | Position of #39 (0 = on 379, 1 = on 378) |
| ------------------- | ---------------------------------------- |
| start, 310 ms       | 0                                        |
| +144 ms, flip       | 0.465                                    |
| +84 ms at 580, flip | 0.320                                    |
| +48 ms at 336, flip | 0.463                                    |
| +146 ms at 310      | -0.008: home, so it lands                |

The UI clock landed #39 2.6 ms before the last tap, so `Reverse` found nothing and a new Curl started. The drawing thread drew #67 7 times in 146 ms (about 20 ms per frame), so its last position was up to about 0.06 from home. It was not at rest.

## Cause (file:line, `src/skeuomorph/`)

1. **`OnStart` judges the Curls on their position from the last frame.** `CurlRenderer.cpp:2477` (`AtRest(last)`) and the `FliesWith` check above it run on the `Flight::t` that `Advance` last set. `Advance` runs only in `DrawFrame` (`CurlRenderer.cpp:2661`). `OnStart` (`:2436`) never brings the Curls forward to the tap time, so a Curl the UI clock has landed is still "in the air, heading the other way". The fallback `LandAll(r, g, st, LandHow::AtOnce, "next Curl")` at `CurlRenderer.cpp:2481` then drops everything in the air.
2. **Flips add error.** `SendBack` (`:2361`) and `Peel` (`:2398`) flip direction without moving the Curl to the moment of the flip. The next `Advance` then runs the whole interval since the last frame in the new direction, and `OnStart` sets `st.pace` (`:2437`) before that interval is counted, so it uses the new length. Each flip shifts the drawing thread from the UI clock by up to about 2 frames, in either direction.
3. **Two sources of truth.** The UI clock already knows which turns it landed (`landed` in `FinishTurn`, `PageTurnDetect.cpp:332-353`). The drawing thread is not told, and guesses again from its own clock.

A side bug that only affects the log: `TurnClock::Split` (`PageTurnLogic.cpp:628`) calls `Advance`, which starts with `ResetLanded(landed)` (`PageTurnLogic.cpp:536`). This clears what `Reverse` landed, so the per-turn line under-reports `landed` (the failing line says `0 landed` although #39 had just landed).

## Recommended fix

In order of importance, all in the app:

1. **Make the UI clock's decision the only one.** Pass the ids the UI clock landed on this tap to the drawing thread, the way `across` is passed (`CurlTurn` → `TurnData`). In `OnStart`, finish those Curls with `LandHow::Done` (they are at rest, so nothing visibly jumps) before the `FliesWith` loop. Then `"next Curl"` only fires when the two threads really disagree.
2. **Bring the drawing thread up to the tap first.** At the top of `OnStart`, call `Advance(st, turn.t0)` with the _old_ pace, then set `st.pace = turn.pace`. Do this before `SendBack` and `Peel` flip a Curl too. This removes the skew from stale frames and from flips.
3. **Keep `landed` across `Reverse` → `Split` → `Start`** in `FinishTurn`, by passing a separate `Landed` to `Split`, or by having `Split` skip the reset when `dt == 0`. The per-turn log then shows what really landed.

Test side, optional: `judge` (`tests/skeuomorph-mixed-taps.ts:150`) does not check that every tap became a turn, although its header says so. Adding `played.length === TAPS` would close that gap. It did not cause this failure.

To check the fix without waiting for luck, add a debug-only delay before `OnStart` reads the Curls, like `FlashLayerLagMs` for the flash test. Then `seed 2` fails every run before the fix and passes after it.

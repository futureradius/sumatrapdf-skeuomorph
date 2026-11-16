# Would one explicit state machine for a Page Turn help?

Research for [#45](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/45), map [#36](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/36).
Planning only, nothing was refactored. Line numbers are on `scrolling` at `9bab0cc6c`.
Words as in [GLOSSARY.md](../../GLOSSARY.md).

## Short answer

**Partly.** One machine for everything would be wrong: a Page Turn's life is spread over two
threads, and Rough Pages and Page Edges are not stages of a turn. Each is a cache or a layout that
outlives many turns. Two smaller changes are worth making:

1. **One list of what is in the air, not two.** Today the UI thread (`TurnClock`) and the drawing
   thread (`Stage`) each keep their own list of Curls in the air, with their own landing rules. The
   drawing thread lands Curls that `TurnClock` still counts. This is the one state problem the code
   has now.
2. **One phase per Curl instead of six flags.** A Curl's life on the drawing thread is `t`, `back`,
   `showsNew`, `seeThrough`, `restShown` and `refreshesLeft`. A single `Phase` with a pure
   `Step(phase, event)` in `PageTurnLogic` can be unit-tested, and Grab and drag needs that.

The three known bugs (#37, #38, #39) are **not** state bugs. They come from which picture a face
shows and how the geometry is laid out. A state machine wouldn't fix them, and they shouldn't
wait for one.

## Where a Page Turn's state lives today

| What                                     | Where                                                                            | Thread              | Holds                                                                                                                                       |
| ---------------------------------------- | -------------------------------------------------------------------------------- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| One action under way                     | `WindowTurns`, `PageTurnDetect.cpp:40-55`                                        | UI                  | `pending`, `gate`, `from`, `pendingDm`, `atPaint` fingerprint, `renumbered`, `roomPx`                                                       |
| Curls in the air, as the logic sees them | `TurnClock`, `PageTurnLogic.h:218-261` (code `PageTurnLogic.cpp:457-700`)        | UI                  | `air[5]`: `progress`, `back`, `peeled`, `holdUntil`, `dest`; fast-tap `Pace`                                                                |
| The newest turn, renderer side           | `Renderer`, `CurlRenderer.cpp:212-243`                                           | UI                  | `haveFrom`, `shape`, `sweep`, `forward`, `glide`, `split`, `landingCopy` (one slot, newest turn only)                                       |
| Hand-over counters                       | `Renderer`, `CurlRenderer.cpp:225-229`                                           | both, mutex         | `started`, `coveredId`, `goneId`; `InAir() = goneId < started` (`:2930`)                                                                    |
| Curls in the air, as drawn               | `Stage` + `Flight`, `CurlRenderer.cpp:905-947`                                   | drawing             | per Curl `t`, `back`, `dir[]`/`ownT[]` (peeled sheets), `showsNew`, `seeThrough`, `restShown`, `refreshesLeft`; `needCover`, `pendingCover` |
| Screen copies in transit                 | `ScreenCopyDesk`, `ScreenCopy.cpp:23-35`; inbox slots `CurlRenderer.cpp:112-116` | both                | ticket `asked`/`served`; `kSlotNextOld`, `kSlotLanding`                                                                                     |
| Layer shown                              | `CurlLayer.cpp:25`                                                               | drawing             | `shown`                                                                                                                                     |
| Rough Pages                              | `RoughPages`, `RoughPages.cpp:61-92`                                             | UI, drawing, worker | look + generation, wishes, the page being drawn, finished pictures                                                                          |
| Page Edges room, cover flag              | `gRooms`, `gModes`, `PageEdges.cpp:40-53`                                        | UI                  | layout, per document                                                                                                                        |
| Window size                              | `CanvasWatch`, `WindowChanges.cpp:147-150`                                       | UI                  | last canvas size                                                                                                                            |

So "the state of a Page Turn" is in at least **five** places on two threads. Hook entry points:
`docs/skeuomorph-hooks.md` (spread-change, relayout, after-paint, rough-paint, forget) and the canvas/frame
subclass in `WindowChanges.cpp:163-209`.

## The life of one Page Turn

```
 UI thread                                              drawing thread
 (PageTurnDetect: WindowTurns, TurnClock)               (CurlRenderer: Stage, Flight)
 ─────────────────────────────────────────              ─────────────────────────────────
 IDLE  pending = false
   │ key / wheel / link -> GoToPage -> hook spread-change
   ▼                                                    PageTurnDetect.cpp:360
 PENDING  gate checked, from, pendingDm, Post(FinishTurn)
   │ gate passed: CurlCopyOld ── ticket ──────────────▶ ServeCopy: win->buffer -> inbox[NextOld]
   │   UI waits up to 200 ms                            (whole canvas only: CurlRenderer.cpp:2989)
   │ more GoToPage calls in this message: ignored
   ▼ FinishTurn, after the message, before the repaint   PageTurnDetect.cpp:294
 DECIDE  DecideTurn
   ├─ Nothing ─▶ land everything (or keep flying: EPUB renumbered) ─▶ IDLE
   └─ Curl / Bundle:
        TurnClock.Reverse ─ or ─ .Split ─ or ─ .Start    (UI's own list: lands across, 6th)
        CurlStart ── Cmd Start ───────────────────────▶ OnStart  CurlRenderer.cpp:2436
   ▼                                                      sent back? SendBack   (:2361)
 WAIT FOR COVER  up to 100 ms (ADR 0005)                  Bundle peel? Peel     (:2398)
   │                                                      else new Flight; lands others:
   │                                                        across, other way, glide, 6th
   │ ◀───── coveredId ────────────────────────────────── needCover -> Present -> NoteCovered
   ▼
 IDLE  SumatraPDF repaints the new Spread under the layer
   │ hook after-paint, flushed: CurlWindowShowsNew
   │   ── ticket (landing page) ──────────────────────▶ inbox[Landing]
   │   ── Cmd ShowsNew ───────────────────────────────▶ OnShowsNew: newest Flight.showsNew
   ▼
 next tap: again from PENDING, while earlier Curls still fly
```

## The life of one Curl on the drawing thread

```
              OnStart (new Flight, t = 0)
                     │
                     ▼
   ┌────────▶ TURNING  t: 0 -> EndT, driven by the clock (Advance :2569)
   │ tap on      │  │
   │             │  └─ tap back ─▶ SENT BACK  back = true, t runs down, new id (:2361)
   └─────────────┼──────────────── tap on again (back = false)
                 │     Bundle + tap back ─▶ PEELED  per sheet dir[k], ownT[k] (:2398)
                 ▼  t >= EndT and sheets home     (sent back: t <= 0)    AtRest :1846
             AT REST ── a frame shows it ─▶ restShown
                 │  needs SumatraPDF's new picture:
                 │  showsNew -> refreshesLeft 1 -> 0 -> seeThrough   CountToSeeThrough :2620
                 ▼
              GONE  Finish (:2315): pictures to spares; the last one hides the layer (LandAll :2331)

   at any time:
     LANDED AT ONCE  across ("jump across"), other way ("next Curl"), a glide flies alone,
                     6th ("sixth Curl"), resize, Cmd LandAll                  (:2451-2485)
     TIMED OUT       on screen > max(4 s, length + 1 s)                      IsStuck :2558
```

The phase is never stored. `AtRest()`, `Rested()` and `DropRested()` (`:1846`, `:2585`, `:2592`)
recompute it from `t`, `back`, `dir[]`, `ownT[]`, `restShown` and `seeThrough` every frame.

## Interruptions

```
 resize ─────────── WindowChanges CanvasProc WM_SIZE  ──┐
 hidden ─────────── WM_SHOWWINDOW                     ──┤
 other doc / view ─ WM_PAINT -> DetectBeforePaint     ──┤
 minimize, DPI ──── FrameProc                         ──┼─▶ DetectLandAll
 zoom, rotation ─── hook relayout -> DetectBeforeRelayout┤      ├─ TurnClock.LandAll   (UI list)
 closed document ── hook forget -> DetectForget       ──┘      └─ CurlLandAll: Cmd LandAll, waits ≤50 ms for goneId
 switch off ─────── ToggleSkeuomorphPageTurn -> DetectLandAllWindows + EdgesRelayoutAll
```

Every interruption does the same thing, "everything lands", and both lists are cleared together.
That path is sound.

## Rough Pages arriving

Rough Pages don't move a turn from one stage to the next. Each frame, the drawing thread asks for
what its faces lack (`RoughOf`, `CurlRenderer.cpp:1233`, and `RoughWish`, `RoughPages.cpp:491`). A
finished picture is uploaded at the next frame (`RoughFrameStart`, `:1166`) and swaps in. After
landing, hook rough-paint shows it in SumatraPDF's own paint. This works like a cache lookup,
which is the right shape. It should stay outside any turn state machine.

## Where the known bugs and risks sit

| Issue                                                              | Where                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          | A state problem?                                                                                                                                                                                                                                                                                          |
| ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **#37** zoomed-in Curl shows blank off-screen parts                | `CurlCopyOld` copies only the canvas (`CurlRenderer.cpp:2989`), but faces are laid out on the whole page (`pageOnScreen`, `PageTurnDetect.cpp:141-194`). `BestPicture` (`:1257`) takes the copy whenever the page was `sharp`, so the Rough Page fallback is never asked for the off-screen part.                                                                                                                                                                                                                                                                                                                                                                              | No. It's about which picture a face uses.                                                                                                                                                                                                                                                                 |
| **#38** old Page Edges and fades cut off with different page sizes | The old screen copy has the Page Edges baked in, and they are kept on purpose until landing (`WithEdges`, `:581`). The glide draws both sets (`DrawSideEdges` `:1721`, `RenderGlide` `:1767`). The fade on the leaving page competes with the new screen's picture (`NewPicture`, `landPic` from `OnShowsNew` `:2523`) during the hand-over to SumatraPDF.                                                                                                                                                                                                                                                                                                                     | Mostly no. The hand-over timing (`showsNew` -> `seeThrough`) is involved, but the fix is in what gets drawn.                                                                                                                                                                                              |
| **#39** Single Page forward vs backward                            | `SingleGeometry` (`:648`): forward is the old page turning to blank paper and fading out (`Fade::Out`). Backward is blank paper turning onto the new page, hinged on its edge (`Fade::In`). Also `DrawSingleAround` (`:1644`).                                                                                                                                                                                                                                                                                                                                                                                                                                                 | No, it's geometry. The "loose sheet both ways" variant is close to the existing SENT BACK state, though: a Page Down Curl played backward is what `SendBack` already draws. A Page Up from rest could start a Flight in that state at `t = EndT`, if the sheet's front (the previous page) has a picture. |
| **Two lists of Curls in the air** (risk, maybe #42)                | `TurnClock.air` lands only the 6th and crossed turns (`PageTurnLogic.cpp:566-592`). `OnStart` also lands every turn heading the other way and everything before a glide (`CurlRenderer.cpp:2468-2482`, `FliesWith` `:2352`). Nothing reports these landings back to `TurnClock`. So `TurnClock` can count ghosts: the "N in air" log line, fast tapping, the at-most-5 rule and `KeepsFlying` see Curls that no longer fly. A `Reverse()` that matches a Curl the renderer already dropped arrives as `back = true` with no Flight. `OnStart` (`:2438`) then makes a fresh Curl with `landingCopy = false` (`:3010`), so its landing page stays blank paper until see-through. | **Yes.** This is the one real state problem. Worth checking first in #42 (`skeuomorph-mixed-taps` fails sometimes).                                                                                                                                                                                       |
| One-slot UI state                                                  | `Renderer.shape/sweep/forward/glide/split/landingCopy` describe only the newest turn (`:236-243`). `CurlWindowShowsNew` uses them to decide what to copy (`:3073`).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Yes, but small. It's correct as long as only the newest turn needs a landing copy.                                                                                                                                                                                                                        |

## What one explicit state machine would cost and gain

**Shape:** a pure `CurlPhase { Turning, Held, SentBack, AtRest, HandingOver, Gone }`, with
`Step(phase, event, now) -> phase` and events `Tap`, `TapBack`, `Frame`, `ShowsNew`, `LandNow`,
`Grab`, `Move`, `Release`. It lives in `PageTurnLogic`, next to `TurnClock`. The landing rules
(`FliesWith`, glide flies alone, crossing, 6th) move there too, so both threads use one decision.

|                              | Gain                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          | Cost                                                                                                                                                                                                           |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Testability**              | `PageTurnLogic_ut.cpp` already tests `TurnClock` (`InAirTest`, `SendBackTest`, `SplitTest`, `AcrossTest`). None of the drawing thread's lifecycle (`OnStart`, `DropRested`, `CountToSeeThrough`, about 550 lines at `CurlRenderer.cpp:2278-2830`) can be unit-tested today. It is checked only by the slow GUI scripts. Moving the transitions into pure code would make tap sequences like "Down, Down, Up, Up, Up" unit tests.                                                                                                                                                                                                                              | Splitting decisions from drawing in `CurlRenderer.cpp` means touching the no-flash hand-over (ADR 0005), which took several spikes to get right. The flash recorder and `tests/skeuomorph-*.ts` must be rerun. |
| **Bug prevention**           | Removes the two-lists drift. Names the hand-over, so "the layer goes when the last Curl is AT REST and SumatraPDF shows its Spread" is one rule, not three functions.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | Doesn't fix #37, #38 or #39.                                                                                                                                                                                   |
| **Grab and drag**            | A drag needs a Curl whose progress follows the pointer instead of the clock. It must be able to go forward and back many times, then on release either land or go back. That is a `Held` phase plus the existing SENT BACK. With six flags it would be a seventh flag interacting with `back`, `holdUntil`, `dir[]` and `TurnClock`'s own `progress`, which would be wrong while the hand holds the page. The cheapest drag commits `GoToPage` at grab and treats "release before halfway" as a back tap, which `Reverse` already handles. The mouse can be caught in the existing canvas subclass (`WindowChanges.cpp:163`), with no new original-file hook. | The `Held` phase also needs the cover hand-over to work without a clock-timed first frame. That design work is the same with or without a state machine.                                                       |
| **Deeper integration (#47)** | If SumatraPDF ever owned the Page Turn, an explicit lifecycle object is what it would own. Hooks become events into it, and the fingerprint guessing in `PageTurnDetect` could become real events from `DisplayModel`.                                                                                                                                                                                                                                                                                                                                                                                                                                        | None now: the change is all in `src/skeuomorph/`, with 0 original lines and no change to ADR 0003's budget.                                                                                                    |

**Rough size:** one build session for each step. Step 1 (one list, landing rules shared) changes
about 150 lines in `PageTurnLogic` and `OnStart`. Step 2 (the `CurlPhase` enum and `Step`) is
about 300 lines in `CurlRenderer.cpp`, plus unit tests.

## Recommendation: partial

1. **Do now, before Grab and drag:** one source of truth for what is in the air. Move the landing
   rules into `PageTurnLogic` and have `TurnClock` apply them, or have the renderer report every
   landing back. Add unit tests for mixed-direction tap sequences. Check whether this explains #42.
2. **Do with Grab and drag:** replace the per-Curl flags with `CurlPhase` + `Step`, and add
   `Held`.
3. **Don't:** fold Rough Pages, Page Edges, screen-copy tickets or the per-action detection
   (`pending`/`FinishTurn`, small and clear) into the machine.
4. **Fix #37, #38, #39 on their own.** They don't depend on this.

## Sources

- Code on `scrolling` at `9bab0cc6c`: `src/skeuomorph/PageTurnDetect.cpp`, `PageTurnLogic.h/.cpp`,
  `PageTurnLogic_ut.cpp`, `CurlRenderer.h/.cpp`, `ScreenCopy.h/.cpp`, `RoughPages.h/.cpp`,
  `CurlLayer.h/.cpp`, `PageEdges.h/.cpp`, `WindowChanges.cpp`, `PageTurnHooks.cpp`.
- `docs/skeuomorph-hooks.md`, `docs/skeuomorph-page-turn-spec.md` (3.3, 3.4, 3.10, 3.11, 5.3, 5.5,
  5.6), ADRs 0001, 0002, 0003, 0005.
- Issues [#37](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/37),
  [#38](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/38),
  [#39](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/39),
  [#42](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/42),
  [#47](https://github.com/futureradius/sumatrapdf-skeuomorph/issues/47).
- `tests/skeuomorph-mixed-taps.ts` (what counts as a failure: "landed at once" other than "sixth
  Curl").

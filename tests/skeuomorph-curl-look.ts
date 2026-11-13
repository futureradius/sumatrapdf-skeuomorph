// Skeuomorph Page Turn, build step 5: the real Curl look
// (docs/skeuomorph-page-turn-spec.md, sections 3.3, 3.6, 3.8, 3.9).
//
// Slow Curls (3 s) in every Paged View, forward and back, right-to-left, mixed
// page sizes and a comic's double-page spreads. For each turn:
//   - the Curl plays to its end ("landed", not "landed at once");
//   - the frame at 75% differs from the screen before and after the turn: the
//     layer shows something in between;
//   - outside the new Spread and its Page Edges, the frame at 95% is almost the
//     screen after: nothing pops when the sheet lands (a wide page's sides
//     vanishing at the end, say). The landing page itself may still be
//     settling, and when the page size changes its Page Edges slide out after
//     landing;
//   - pages of different sizes (the view glides): once the Page Edges have
//     slid out (113%), the whole frame is almost the screen after: nothing
//     pops when the layer goes;
//   - Single Page: the old page's Page Edges and the new page's never show
//     both at full strength (pages of different sizes put them elsewhere);
// and stepping through mixed page sizes at Fit Width, a turn that shows or hides
// a scrollbar keeps its Curl (it used to land as a "resize").
//
// Screen captures of every turn land in tests/tmp/skeuomorph-curl-look/ to look at:
// <scenario>-<n>-before.png, -10.png ... -95.png, -after.png.
// The window is put on top for the run: the captures read the real screen.
// Uses the test shelf (C:\code\test-shelf), read only.
//
// Build the debug exe first, then: bun tests/skeuomorph-curl-look.ts [scenario ...]

import { dlopen, FFIType } from "bun:ffi";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { cmdId, prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { findCanvas, killAndWait, launchControlled } from "./win-automation.ts";
import {
  captureScreenRect,
  clientToScreen,
  getClientRect,
  getWindowLong,
  postMessage,
  sendMessage,
  setForegroundWindow,
  setProcessDpiAware,
  sleep,
  VK_NEXT,
  WM_COMMAND,
  WM_KEYDOWN,
  WM_KEYUP,
} from "./winapi.ts";
import type { Rect } from "./winapi.ts";

const SHELF = "C:\\code\\test-shelf";
const BOOK = join(SHELF, "generated", "numbered-040-pages.pdf");
const MIXED = join(SHELF, "generated", "numbered-mixed-sizes.pdf");
const COMIC = join(SHELF, "downloads", "bakemono-shiuchi-hyobanki-rtl.cbz");

const VK_PRIOR = 0x21;
const HWND_TOPMOST = -1;
const HWND_NOTOPMOST = -2;
const SWP_NOMOVE = 0x2;
const SWP_NOSIZE = 0x1;

const CURL_MS = 3000;
const SHOTS = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.75, 0.85, 0.95, 1.13];
const SETTLE_MS = 600;
const LAND_WAIT_MS = CURL_MS + 4000;
// the 75% frame differs from before and after in at least this share of pixels (%)
const CHECK_AT = 0.75;
const MIN_CHANGE = 1;
const CHANGED = 32;
// outside the new Spread, the 95% frame differs from after in at most this
// share of the canvas's pixels (%)
const POP_AT = 0.95;
const POP_MAX = 0.5;
// a glide's layer stays while the new Page Edges slide out (15% of the Curl)
const SLID_OUT_AT = 1.13;
// Page Edges at full strength: this share of their changing pixels match a screen
const FULL = 0.9;
const MIN_EDGE_PX = 100;

const GWL_STYLE = -16;
const WS_SCROLLBARS = 0x00300000; // WS_VSCROLL | WS_HSCROLL

const TURN_PREFIX = "skeuomorph: ";
const CURL_PREFIX = "skeuomorph-curl: ";
const RECTS_PREFIX = "skeuomorph-rects: ";

const OUT = tmpPath("skeuomorph-curl-look");

function settings(view: string, zoom: string): string {
  return [
    "Theme = Light",
    "CheckForUpdates = false",
    "RememberOpenedFiles = false",
    "RememberStatePerDocument = false",
    "RestoreSession = false",
    "ShowToc = false",
    "ShowStartPage = false",
    "SmoothScroll = false",
    `DefaultDisplayMode = ${view}`,
    `DefaultZoom = ${zoom}`,
    "SkeuomorphPageTurn = true",
    `PageTurnDuration = ${CURL_MS}`,
    "",
  ].join("\r\n");
}

const user32 = dlopen("user32.dll", {
  SetWindowPos: {
    args: [FFIType.u64, FFIType.i64, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.u32],
    returns: FFIType.i32,
  },
});

function setTopmost(hwnd: number, on: boolean): void {
  const after = on ? HWND_TOPMOST : HWND_NOTOPMOST;
  user32.symbols.SetWindowPos(hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

// share of pixels (%) whose colour changed clearly; outside `skip` only
function change(a: Uint8Array | null, b: Uint8Array | null, w = 0, skip: Box | null = null): number {
  if (!a || !b || a.length !== b.length) {
    return 0;
  }
  let changed = 0;
  for (let i = 0; i < a.length; i += 4) {
    if (skip) {
      const x = (i / 4) % w;
      const y = Math.floor(i / 4 / w);
      if (x >= skip.x && x < skip.x + skip.dx && y >= skip.y && y < skip.y + skip.dy) {
        continue;
      }
    }
    const d = Math.max(Math.abs(a[i] - b[i]), Math.abs(a[i + 1] - b[i + 1]), Math.abs(a[i + 2] - b[i + 2]));
    if (d > CHANGED) {
      changed++;
    }
  }
  return (100 * changed) / (a.length / 4);
}

// canvas px
type Box = { x: number; y: number; dx: number; dy: number };

// "#3 from 10,20,300,400 to 5,20,310,400 edges 12 glide 1"
function parseRects(line: string): { from: Box; to: Box; edges: number; glide: boolean } | null {
  const m = /from (-?\d+),(-?\d+),(\d+),(\d+) to (-?\d+),(-?\d+),(\d+),(\d+) edges (\d+) glide (\d)/.exec(line);
  if (!m) {
    return null;
  }
  const n = m.slice(1).map(Number);
  const box = (i: number) => ({ x: n[i], y: n[i + 1], dx: n[i + 2], dy: n[i + 3] });
  return { from: box(0), to: box(4), edges: n[8], glide: n[9] === 1 };
}

// the room for Page Edges on both sides of a page
function edgeRoom(page: Box, edges: number): Box[] {
  return [
    { x: page.x - edges, y: page.y, dx: edges, dy: page.dy },
    { x: page.x + page.dx, y: page.y, dx: edges, dy: page.dy },
  ];
}

function differ(a: Uint8Array, b: Uint8Array, i: number): boolean {
  return Math.max(Math.abs(a[i] - b[i]), Math.abs(a[i + 1] - b[i + 1]), Math.abs(a[i + 2] - b[i + 2])) > CHANGED;
}

// in boxes, the pixels that differ between screens `shows` and `other`: the
// share of them where frame matches `shows`, and how many there are
function shownShare(
  frame: Uint8Array,
  shows: Uint8Array,
  other: Uint8Array,
  boxes: Box[],
  w: number,
): { share: number; px: number } {
  const h = frame.length / 4 / w;
  let px = 0;
  let match = 0;
  for (const b of boxes) {
    for (let y = Math.max(0, b.y); y < Math.min(h, b.y + b.dy); y++) {
      for (let x = Math.max(0, b.x); x < Math.min(w, b.x + b.dx); x++) {
        const i = (y * w + x) * 4;
        if (!differ(shows, other, i)) {
          continue;
        }
        px++;
        if (!differ(frame, shows, i)) {
          match++;
        }
      }
    }
  }
  return { share: px > 0 ? match / px : 0, px };
}

type Scenario = {
  name: string;
  file: string;
  view: string;
  zoom?: string;
  page: number;
  manga?: boolean;
  keys: number[];
  // step through without captures; a key may scroll instead of turning
  quick?: boolean;
};

const SCENARIOS: Scenario[] = [
  { name: "facing", file: BOOK, view: "facing", page: 3, keys: [VK_NEXT, VK_PRIOR] },
  { name: "book-cover", file: BOOK, view: "book view", page: 1, keys: [VK_NEXT, VK_PRIOR] },
  { name: "single", file: BOOK, view: "single page", page: 5, keys: [VK_NEXT, VK_PRIOR] },
  { name: "mixed", file: MIXED, view: "facing", page: 1, keys: [VK_NEXT, VK_NEXT, VK_NEXT, VK_PRIOR] },
  // a bigger page's parts beyond the old one keep the old screen until the turn brings them
  {
    name: "mixed-single",
    file: MIXED,
    view: "single page",
    page: 1,
    keys: [VK_NEXT, VK_NEXT, VK_NEXT, VK_PRIOR, VK_PRIOR, VK_PRIOR],
  },
  // pages 5 alone, 6 a spread, 7-8: into and out of a wide page, right to left
  { name: "comic-rtl", file: COMIC, view: "facing", page: 5, manga: true, keys: [VK_NEXT, VK_NEXT, VK_PRIOR] },
  { name: "single-rtl", file: COMIC, view: "single page", page: 3, manga: true, keys: [VK_NEXT, VK_PRIOR] },
  {
    name: "mixed-fit-width",
    file: MIXED,
    view: "facing",
    zoom: "fit width",
    page: 1,
    keys: Array(6).fill(VK_NEXT),
    quick: true,
  },
  {
    name: "mixed-single-fit-width",
    file: MIXED,
    view: "single page",
    zoom: "fit width",
    page: 1,
    keys: Array(14).fill(VK_NEXT),
    quick: true,
  },
];

class Run {
  seen = 0;
  constructor(
    public frame: number,
    public canvas: number,
    public logPath: string,
  ) {}

  lines(prefix: string): string[] {
    const text = existsSync(this.logPath) ? readFileSync(this.logPath, "utf8") : "";
    return text
      .split(/\r?\n/)
      .filter((l) => l.startsWith(prefix))
      .map((l) => l.slice(prefix.length));
  }

  curls(): number {
    return this.lines(TURN_PREFIX).filter((t) => t.startsWith("Curl")).length;
  }

  canvasRect(): Rect {
    const c = getClientRect(this.canvas);
    const tl = clientToScreen(this.canvas, 0, 0);
    return { left: tl.x, top: tl.y, right: tl.x + c.right - c.left, bottom: tl.y + c.bottom - c.top };
  }

  key(vk: number): void {
    postMessage(this.frame, WM_KEYDOWN, vk, 0);
    postMessage(this.frame, WM_KEYUP, vk, 0);
  }

  async waitLanded(): Promise<void> {
    const deadline = Date.now() + LAND_WAIT_MS;
    while (Date.now() < deadline) {
      if (this.lines(CURL_PREFIX).length >= this.curls()) {
        return;
      }
      await sleep(50);
    }
  }
}

// Single Page: no frame shows the old page's Page Edges and the new page's both
// at full strength
function checkEdges(
  tag: string,
  rectsLine: string,
  before: Uint8Array | null,
  after: Uint8Array | null,
  mid: (Uint8Array | null)[],
  area: Rect,
  fails: string[],
): void {
  const rects = parseRects(rectsLine);
  if (!rects) {
    fails.push(`${tag}: no page rects in the log: ${rectsLine}`);
    return;
  }
  const w = area.right - area.left;
  if (!before || !after || before.length !== after.length) {
    return;
  }
  const oldRoom = edgeRoom(rects.from, rects.edges);
  const newRoom = edgeRoom(rects.to, rects.edges);
  for (let i = 0; i < mid.length; i++) {
    const frame = mid[i];
    if (!frame || frame.length !== before.length) {
      continue;
    }
    const o = shownShare(frame, before, after, oldRoom, w);
    const n = shownShare(frame, after, before, newRoom, w);
    if (o.px >= MIN_EDGE_PX && n.px >= MIN_EDGE_PX && o.share >= FULL && n.share >= FULL) {
      const at = Math.round(SHOTS[i] * 100);
      fails.push(
        `${tag} at ${at}%: old and new Page Edges both at full strength (${(100 * o.share).toFixed(0)}% / ${(100 * n.share).toFixed(0)}%)`,
      );
    }
  }
}

async function runScenario(s: Scenario, fails: string[]): Promise<void> {
  const appdata = tmpPath(`skeuomorph-curl-look-${s.name}-appdata`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), settings(s.view, s.zoom ?? "fit page"));
  const logPath = tmpPath(`skeuomorph-curl-look-${s.name}-log.txt`);
  writeFileSync(logPath, "");

  const args = ["-appdata", appdata, "-log-to-file", logPath, "-page", String(s.page), s.file];
  const { proc, client, frame } = await launchControlled(args);
  try {
    setTopmost(frame, true);
    setForegroundWindow(frame);
    if (s.manga) {
      sendMessage(frame, WM_COMMAND, cmdId("CmdToggleMangaMode"), 0);
    }
    await client.waitForRenderIdle();
    await sleep(SETTLE_MS);
    const r = new Run(frame, findCanvas(frame), logPath);

    let bars = getWindowLong(r.canvas, GWL_STYLE) & WS_SCROLLBARS;
    let barChanges = 0;
    for (let n = 0; n < s.keys.length; n++) {
      const tag = `${s.name}-${n + 1}`;
      const area = r.canvasRect();
      const before = s.quick ? null : captureScreenRect(area, join(OUT, `${tag}-before.png`));
      const curlsBefore = r.curls();
      const t0 = Date.now();
      r.key(s.keys[n]);
      const mid: (Uint8Array | null)[] = [];
      for (const at of s.quick ? [] : SHOTS) {
        const wait = t0 + at * CURL_MS - Date.now();
        if (wait > 0) {
          await sleep(wait);
        }
        mid.push(captureScreenRect(area, join(OUT, `${tag}-${Math.round(at * 100)}.png`)));
      }
      await sleep(200);
      await r.waitLanded();
      await client.waitForRenderIdle();
      await sleep(SETTLE_MS);
      const after = s.quick ? null : captureScreenRect(r.canvasRect(), join(OUT, `${tag}-after.png`));

      const barsNow = getWindowLong(r.canvas, GWL_STYLE) & WS_SCROLLBARS;
      if (barsNow !== bars) {
        barChanges++;
        bars = barsNow;
      }
      if (s.quick) {
        continue;
      }
      if (r.curls() !== curlsBefore + 1) {
        fails.push(`${tag}: expected one Curl, log has ${r.curls() - curlsBefore}`);
        continue;
      }
      const landed = r.lines(CURL_PREFIX).at(-1) ?? "";
      console.log(`  ${tag}: ${landed}`);
      if (!/ landed after /.test(landed)) {
        fails.push(`${tag}: the Curl didn't play to its end: ${landed}`);
      }
      // other frames are for looking: only a corner has lifted at 30%, and in
      // Single Page backward the page is still fading in beside its edge
      const w = area.right - area.left;
      const rects = parseRects(r.lines(RECTS_PREFIX).at(-1) ?? "");
      if (!rects) {
        fails.push(`${tag}: no Spread rects in the log`);
        continue;
      }
      for (let i = 0; i < mid.length; i++) {
        if (SHOTS[i] === POP_AT) {
          const t = rects.to;
          const withEdges = { x: t.x - rects.edges, y: t.y, dx: t.dx + 2 * rects.edges, dy: t.dy };
          const pop = change(mid[i], after, w, withEdges);
          console.log(`    at 95%: ${pop.toFixed(2)}% differs from after`);
          if (pop > POP_MAX) {
            fails.push(`${tag}: pops at the end: ${pop.toFixed(2)}% of the 95% frame differs from after`);
          }
        }
        if (SHOTS[i] === SLID_OUT_AT && rects.glide) {
          const pop = change(mid[i], after, w);
          console.log(`    glide, at 113%: ${pop.toFixed(2)}% differs from after`);
          if (pop > POP_MAX) {
            fails.push(`${tag}: pops when the layer goes: ${pop.toFixed(2)}% of the 113% frame differs from after`);
          }
        }
        if (SHOTS[i] !== CHECK_AT) {
          continue;
        }
        const fromBefore = change(mid[i], before);
        const fromAfter = change(mid[i], after);
        if (fromBefore < MIN_CHANGE || fromAfter < MIN_CHANGE) {
          const at = Math.round(SHOTS[i] * 100);
          fails.push(
            `${tag} at ${at}%: no Curl on screen (changed ${fromBefore.toFixed(2)}% / ${fromAfter.toFixed(2)}%)`,
          );
        }
      }
      if (s.view === "single page") {
        checkEdges(tag, r.lines(RECTS_PREFIX).at(-1) ?? "", before, after, mid, area, fails);
      }
    }

    // a scrollbar shown or hidden by a turn must not land its Curl
    const atOnce = r.lines(CURL_PREFIX).filter((l) => / at once /.test(l));
    if (s.quick) {
      console.log(`  ${r.curls()} Curls, ${barChanges} scrollbar changes, ${atOnce.length} landed at once`);
      if (barChanges === 0) {
        fails.push(`${s.name}: no turn showed or hid a scrollbar, so the check proves nothing`);
      }
    }
    for (const l of atOnce) {
      fails.push(`${s.name}: ${l}`);
    }
  } finally {
    setTopmost(frame, false);
    await killAndWait(proc);
  }
}

export async function testit(): Promise<void> {
  for (const f of [BOOK, MIXED, COMIC]) {
    if (!existsSync(f)) {
      throw new Error(`test shelf missing: ${f} (run bun tests/skeuomorph-test-shelf.ts)`);
    }
  }
  // screen px, not scaled ones: the captures cut the canvas out of the screen
  setProcessDpiAware();
  rmSync(OUT, { recursive: true, force: true });
  mkdirSync(OUT, { recursive: true });

  const fails: string[] = [];
  // bun tests/skeuomorph-curl-look.ts [name ...]: only those scenarios
  const only = process.argv.slice(2).filter((a) => !a.startsWith("-"));
  for (const s of SCENARIOS.filter((x) => only.length === 0 || only.includes(x.name))) {
    console.log(`${s.name}:`);
    await runScenario(s, fails);
  }
  console.log(`\ncaptures in ${OUT}`);
  if (fails.length > 0) {
    throw new Error(`${fails.length} failed:\n  ${fails.join("\n  ")}`);
  }
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-curl-look", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

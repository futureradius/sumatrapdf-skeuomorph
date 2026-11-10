// Skeuomorph Page Turn, build step 4: window changes mid-Curl
// (docs/skeuomorph-page-turn-spec.md, section 3.11).
//
// For each case: Page Down starts a Curl, ~300 ms later the window changes,
// then the Curl's debug line says how it ended:
//   skeuomorph-curl: #3 landed after 3021 ms, 181 frames, ...             kept playing
//   skeuomorph-curl: #4 landed at once (resize) after 318 ms, 18 frames, ...   landed at once
// The document area changing (resize, maximize, minimize, DPI, zoom, rotation,
// Manga Mode, switch off, fullscreen, Presentation, tab switch, closing the
// document) lands it at once. Moving the window, another program in front, a
// context menu or a dialog keep it playing. After each landing, the change is
// undone and the next Page Down must play a whole Curl (the layer was fitted
// to the new canvas size). Last, the window closes mid-Curl and the app must exit.
//
// Uses numbered books from the test shelf (C:\code\test-shelf), read only.
// Settings live in a throwaway -appdata folder under .work.
//
// Build the debug exe first, then: bun tests/skeuomorph-window-changes.ts

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { cmdId, prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import {
  CANVAS_CLASS,
  FRAME_CLASS,
  killAndWait,
  launchControlled,
  launchSumatra,
  openContextMenu,
  pressKey,
  sendCommandSync,
} from "./win-automation.ts";
import {
  clientToScreen,
  enumWindows,
  findChildWindow,
  getClassName,
  getClientRect,
  getForegroundWindow,
  getWindowPid,
  getWindowRect,
  isWindowVisible,
  postMessage,
  setProcessDpiAware,
  setWindowPos,
  SWP_NOACTIVATE,
  SWP_NOZORDER,
  showWindow,
  sleep,
  SW_MAXIMIZE,
  SW_RESTORE,
  SW_SHOWMINNOACTIVE,
  VK_ESCAPE,
  VK_NEXT,
  waitForTopWindow,
  WM_CLOSE,
  WM_COMMAND,
  WM_KEYDOWN,
  WM_KEYUP,
  sendCopyDataW,
} from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const SHELF = "C:\\code\\test-shelf\\generated";
const BOOK40 = `${SHELF}\\numbered-040-pages.pdf`;
const BOOK3 = `${SHELF}\\numbered-003-pages.pdf`;

// long Curls leave time to change the window mid-Curl
const DURATION_MS = 3000;
// the change comes this long after Page Down
const CHANGE_AT_MS = 300;
// landed at once: well before the Curl's own end
const AT_ONCE_MS = 1000;
// kept playing: about the whole length the turn line gave (fast tapping
// shortens a Curl that follows a quick Bundle)
const WHOLE_SLACK_MS = 100;
// a whole Curl draws about one frame per refresh; below 20 per second is broken
const MIN_FPS = 20;
const FLIGHT_WAIT_MS = DURATION_MS + 3000;
const SETTLE_MS = 300;
const EXIT_WAIT_MS = 8000;
const ABOUT_CLASS = "SUMATRA_PDF_ABOUT";
const DDE_COPYDATA_ID = 0x44646557;

const TURN_PREFIX = "skeuomorph: ";
const CURL_PREFIX = "skeuomorph-curl: ";

const SETTINGS = [
  "Theme = Light",
  "CheckForUpdates = false",
  "RememberOpenedFiles = false",
  "RememberStatePerDocument = false",
  "RestoreSession = false",
  "ShowToc = false",
  "ShowStartPage = false",
  "SmoothScroll = false",
  "DefaultDisplayMode = facing",
  "DefaultZoom = fit page",
  "SkeuomorphPageTurn = true",
  `PageTurnDuration = ${DURATION_MS}`,
  "",
].join("\r\n");

type Row = { name: string; expect: string; got: string; ok: boolean };
const rows: Row[] = [];

// "#4 landed at once (resize) after 318 ms, 18 frames, 0 late, ..."
type Flight = { id: number; how: string; why: string; afterMs: number; frames: number; line: string };

function parseFlight(line: string): Flight | null {
  const m = /^#(\d+) (landed at once|landed|TIMEOUT)(?: \(([^)]*)\))? after (-?\d+) ms, (\d+) frames/.exec(line);
  if (!m) {
    return null;
  }
  return { id: +m[1], how: m[2], why: m[3] ?? "", afterMs: +m[4], frames: +m[5], line };
}

class Session {
  private seenTurns = 0;
  private seenCurls = 0;

  constructor(
    private client: ControlClient,
    readonly frame: number,
    readonly pid: number,
    private logPath: string,
  ) {}

  private lines(prefix: string): string[] {
    const text = existsSync(this.logPath) ? readFileSync(this.logPath, "utf8") : "";
    return text
      .split(/\r?\n/)
      .filter((l) => l.startsWith(prefix))
      .map((l) => l.slice(prefix.length));
  }

  // per-turn lines since the last call
  newTurns(): string[] {
    const all = this.lines(TURN_PREFIX);
    const fresh = all.slice(this.seenTurns);
    this.seenTurns = all.length;
    return fresh;
  }

  newFlights(): Flight[] {
    const all = this.lines(CURL_PREFIX);
    const fresh = all.slice(this.seenCurls);
    this.seenCurls = all.length;
    return fresh.map(parseFlight).filter((f): f is Flight => f !== null);
  }

  // forget what was logged so far
  mark(): void {
    this.newTurns();
    this.newFlights();
  }

  async settle(): Promise<void> {
    await this.client.waitForRenderIdle();
    await sleep(SETTLE_MS);
  }

  cmd(name: string): void {
    sendCommandSync(this.frame, cmdId(name));
  }

  pageDown(): void {
    postMessage(this.frame, WM_KEYDOWN, VK_NEXT, 0);
    postMessage(this.frame, WM_KEYUP, VK_NEXT, 0);
  }

  // the next Curl's line, or null
  async waitFlight(timeoutMs = FLIGHT_WAIT_MS): Promise<Flight | null> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const f = this.newFlights();
      if (f.length > 0) {
        return f[0];
      }
      await sleep(50);
    }
    return null;
  }

  // Page Down, wait until the Curl plays; its length from the turn line
  // ("Curl forward, 2 pages, sweep left, 3000 ms, ..."), or what went wrong
  async startCurl(): Promise<{ err: string; lenMs: number }> {
    this.mark();
    this.pageDown();
    await sleep(CHANGE_AT_MS);
    const turns = this.newTurns();
    const curl = turns.find((t) => t.startsWith("Curl"));
    if (!curl) {
      return { err: `no Curl started: ${turns.join(" | ") || "(no line)"}`, lenMs: 0 };
    }
    const lenMs = +(/, (\d+) ms,/.exec(curl)?.[1] ?? 0);
    const early = this.newFlights();
    if (early.length > 0) {
      return { err: `landed before the change: ${early[0].line}`, lenMs };
    }
    return { err: "", lenMs };
  }
}

enum Rhythm {
  Any,
  // the tap rhythm is slow again: the next Curl isn't a shortened fast tap
  Slow,
}

// back to the first Spread; from the second that's a Curl, wait it out
async function toFirstPage(s: Session, rhythm = Rhythm.Any): Promise<void> {
  s.mark();
  s.cmd("CmdGoToFirstPage");
  await s.settle();
  const turns = s.newTurns();
  if (turns.some((t) => t.startsWith("Curl"))) {
    await s.waitFlight();
  } else if (rhythm === Rhythm.Slow && turns.some((t) => t.startsWith("Bundle"))) {
    await sleep(DURATION_MS);
  }
  s.mark();
}

function flightText(f: Flight | null): string {
  return f ? f.line : "(no skeuomorph-curl line)";
}

function landedAtOnce(f: Flight | null): boolean {
  return f !== null && f.how === "landed at once" && f.afterMs < AT_ONCE_MS;
}

function playedWhole(f: Flight | null, lenMs: number): boolean {
  const minFrames = (lenMs * MIN_FPS) / 1000;
  return f !== null && f.how === "landed" && f.afterMs >= lenMs - WHOLE_SLACK_MS && f.frames >= minFrames;
}

// Page Down plays a whole Curl
async function checkWholeCurl(s: Session, name: string): Promise<void> {
  await s.settle();
  const { err, lenMs } = await s.startCurl();
  const f = err ? null : await s.waitFlight();
  const ok = !err && playedWhole(f, lenMs);
  const expect = `whole Curl (${lenMs} ms), >= ${MIN_FPS} fps`;
  rows.push({ name, expect, got: err || flightText(f), ok });
  await s.settle();
}

type Case = {
  name: string;
  change: (s: Session) => Promise<string | void>;
  undo?: (s: Session) => Promise<void>;
};

// a Curl, the change mid-Curl, then it must land at once; undo, then a whole Curl again
async function landsAtOnce(s: Session, c: Case): Promise<void> {
  await toFirstPage(s);
  const { err } = await s.startCurl();
  let note = "";
  let f: Flight | null = null;
  if (!err) {
    note = (await c.change(s)) || "";
    f = await s.waitFlight();
  }
  const got = (err || flightText(f)) + (note ? ` [${note}]` : "");
  rows.push({ name: c.name, expect: `landed at once (< ${AT_ONCE_MS} ms)`, got, ok: !err && landedAtOnce(f) });
  if (c.undo) {
    await c.undo(s);
  }
  await checkWholeCurl(s, `  then Page Down`);
}

// a Curl, the change mid-Curl, then it must play to the end
async function keepsPlaying(s: Session, c: Case): Promise<void> {
  await toFirstPage(s, Rhythm.Slow);
  const { err, lenMs } = await s.startCurl();
  let note = "";
  let f: Flight | null = null;
  if (!err) {
    note = (await c.change(s)) || "";
    f = await s.waitFlight();
  }
  const got = (err || flightText(f)) + (note ? ` [${note}]` : "");
  const expect = `kept playing (${lenMs} ms)`;
  rows.push({ name: c.name, expect, got, ok: !err && playedWhole(f, lenMs) });
  if (c.undo) {
    await c.undo(s);
  }
  await s.settle();
}

const SWP_NOSIZE = 0x0001;

type Box = { x: number; y: number; w: number; h: number };

function frameBox(frame: number): Box {
  const r = getWindowRect(frame);
  return { x: r.left, y: r.top, w: r.right - r.left, h: r.bottom - r.top };
}

function placeFrame(frame: number, b: Box): void {
  setWindowPos(frame, b.x, b.y, b.w, b.h);
}

// moves without touching the size
function moveFrame(frame: number, x: number, y: number): void {
  setWindowPos(frame, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// a shown popup menu of process `pid`
async function waitForVisibleMenu(pid: number, timeoutMs = 3000): Promise<number> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    let menu = 0;
    enumWindows((h) => {
      if (getWindowPid(h) === pid && getClassName(h) === "#32768" && isWindowVisible(h)) {
        menu = h;
        return false;
      }
      return true;
    });
    if (menu) {
      return menu;
    }
    await sleep(50);
  }
  return 0;
}

async function waitForExit(proc: Bun.Subprocess, timeoutMs: number): Promise<boolean> {
  const exited = proc.exited.then(() => true);
  const timeout = sleep(timeoutMs).then(() => false);
  return Promise.race([exited, timeout]);
}

function printTable(): void {
  const w0 = Math.max(...rows.map((r) => r.name.length));
  const w1 = Math.max(...rows.map((r) => r.expect.length));
  for (const r of rows) {
    const mark = r.ok ? "PASS" : "FAIL";
    console.log(`${mark}  ${r.name.padEnd(w0)}  ${r.expect.padEnd(w1)}  got: ${r.got}`);
  }
}

export async function testit(): Promise<void> {
  for (const doc of [BOOK40, BOOK3]) {
    if (!existsSync(doc)) {
      throw new Error(`test shelf missing: ${doc} (run bun tests/skeuomorph-test-shelf.ts)`);
    }
  }
  const appdata = tmpPath("skeuomorph-window-changes-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);
  const logPath = tmpPath("skeuomorph-window-changes-log.txt");
  writeFileSync(logPath, "");

  // window sizes in real pixels, not scaled for a DPI-unaware test
  setProcessDpiAware();
  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", "facing", "-zoom", "fit page", BOOK40];
  const { proc, client, frame } = await launchControlled(args);
  let other: Bun.Subprocess | null = null;
  let focused = false;
  try {
    const s = new Session(client, frame, proc.pid!, logPath);
    await s.settle();
    const home = frameBox(frame);
    const canvas = findChildWindow(frame, CANVAS_CLASS);

    await checkWholeCurl(s, "Page Down, nothing changes");

    // --- keep playing: the document area stays the same
    await keepsPlaying(s, {
      name: "move the window",
      change: async () => moveFrame(frame, home.x - 40, home.y + 30),
      undo: async () => moveFrame(frame, home.x, home.y),
    });

    // another program starts mid-Curl: it takes the focus and covers half of this window
    await keepsPlaying(s, {
      name: "another program in front",
      change: async () => {
        const cover = `${home.w}x${home.h - 120}@${home.x - (home.w >> 1)}x${home.y + 60}`;
        other = launchSumatra(["-window-pos", cover]);
        const otherFrame = await waitForTopWindow(other.pid!, FRAME_CLASS);
        for (let i = 0; i < 20 && getForegroundWindow() !== otherFrame; i++) {
          await sleep(50);
        }
        focused = otherFrame !== 0 && getForegroundWindow() === otherFrame;
        return focused ? "it has the focus" : "focus did not move";
      },
      undo: async () => {
        if (other) {
          await killAndWait(other);
          other = null;
        }
      },
    });
    rows[rows.length - 1].ok &&= focused;

    await keepsPlaying(s, {
      name: "context menu open and closed",
      change: async (ss) => {
        const rc = getClientRect(canvas);
        const pt = clientToScreen(canvas, (rc.right - rc.left) >> 1, (rc.bottom - rc.top) >> 1);
        openContextMenu(canvas, pt.x, pt.y);
        const menu = await waitForVisibleMenu(ss.pid);
        if (!menu) {
          return "menu did not open";
        }
        await sleep(500);
        await pressKey(menu, VK_ESCAPE);
        return "menu was open";
      },
    });

    await keepsPlaying(s, {
      name: "About dialog open and closed",
      change: async (ss) => {
        // posted: don't wait on the dialog
        postMessage(frame, WM_COMMAND, cmdId("CmdHelpAbout"), 0);
        const about = await waitForTopWindow(ss.pid, ABOUT_CLASS, 2000).catch(() => 0);
        if (!about) {
          return "About did not open";
        }
        await sleep(500);
        postMessage(about, WM_CLOSE, 0, 0);
        return "About was open";
      },
    });

    // --- land at once: the document area changes
    await landsAtOnce(s, {
      name: "resize",
      change: async () => placeFrame(frame, { ...home, w: home.w - 120, h: home.h - 80 }),
      undo: async () => placeFrame(frame, home),
    });
    await landsAtOnce(s, {
      name: "maximize",
      change: async () => {
        showWindow(frame, SW_MAXIMIZE);
      },
      undo: async () => {
        showWindow(frame, SW_RESTORE);
        placeFrame(frame, home);
      },
    });
    await landsAtOnce(s, {
      name: "minimize",
      change: async () => {
        showWindow(frame, SW_SHOWMINNOACTIVE);
      },
      undo: async () => {
        showWindow(frame, SW_RESTORE);
        await sleep(300);
      },
    });
    // debug builds pretend a monitor with another scaling: 100% -> 125% -> 150% -> 75% -> 100%
    await landsAtOnce(s, {
      name: "DPI change",
      change: async (ss) => ss.cmd("CmdDebugToggleDpiOverride"),
      undo: async (ss) => {
        for (let i = 0; i < 3; i++) {
          ss.cmd("CmdDebugToggleDpiOverride");
          await ss.settle();
        }
        placeFrame(frame, home);
      },
    });
    await landsAtOnce(s, {
      name: "zoom in",
      change: async (ss) => ss.cmd("CmdZoomIn"),
      undo: async (ss) => ss.cmd("CmdZoomFitPage"),
    });
    await landsAtOnce(s, {
      name: "rotate",
      change: async (ss) => ss.cmd("CmdRotateRight"),
      undo: async (ss) => ss.cmd("CmdRotateLeft"),
    });
    await landsAtOnce(s, {
      name: "Manga Mode toggle",
      change: async (ss) => ss.cmd("CmdToggleMangaMode"),
      undo: async (ss) => ss.cmd("CmdToggleMangaMode"),
    });
    await landsAtOnce(s, {
      name: "switch off",
      change: async (ss) => ss.cmd("CmdToggleSkeuomorphPageTurn"),
      undo: async (ss) => ss.cmd("CmdToggleSkeuomorphPageTurn"),
    });
    await landsAtOnce(s, {
      name: "fullscreen",
      change: async (ss) => ss.cmd("CmdToggleFullscreen"),
      undo: async (ss) => ss.cmd("CmdToggleFullscreen"),
    });
    await landsAtOnce(s, {
      name: "Presentation",
      change: async (ss) => ss.cmd("CmdTogglePresentationMode"),
      undo: async (ss) => ss.cmd("CmdTogglePresentationMode"),
    });

    // a second document in a tab; back to the first
    if (!sendCopyDataW(frame, DDE_COPYDATA_ID, `[Open("${BOOK3}", 0, 1, 0)]`)) {
      throw new Error("could not open the second document");
    }
    await s.settle();
    s.cmd("CmdPrevTab");
    await s.settle();
    await landsAtOnce(s, {
      name: "tab switch",
      change: async (ss) => ss.cmd("CmdNextTab"),
      undo: async (ss) => {
        ss.cmd("CmdPrevTab");
        await ss.settle();
        // coming back: nothing replays
        const turns = ss.newTurns().filter((t) => !t.startsWith("nothing"));
        const flights = ss.newFlights();
        const quiet = turns.length === 0 && flights.length === 0;
        const got = [...turns, ...flights.map((f) => f.line)].join(" | ") || "(no Curl)";
        rows.push({ name: "  back to the tab", expect: "nothing replays", got, ok: quiet });
      },
    });

    // a Curl in the second tab, then close that document
    s.cmd("CmdNextTab");
    await s.settle();
    {
      const { err } = await s.startCurl();
      let f: Flight | null = null;
      if (!err) {
        s.cmd("CmdCloseCurrentDocument");
        f = await s.waitFlight();
      }
      const got = err || flightText(f);
      rows.push({
        name: "close the document",
        expect: `landed at once (< ${AT_ONCE_MS} ms)`,
        got,
        ok: !err && landedAtOnce(f),
      });
      await toFirstPage(s);
      await checkWholeCurl(s, "  then Page Down");
    }

    // a Markdown tab: its view is a child window over the canvas, so the
    // canvas gets no WM_PAINT; the Curl under it must land all the same
    const md = tmpPath("skeuomorph-window-changes.md");
    writeFileSync(md, "# Markdown tab\n\nA document that isn't a Paged View.\n");
    if (!sendCopyDataW(frame, DDE_COPYDATA_ID, `[Open("${md}", 0, 1, 0)]`)) {
      throw new Error("could not open the Markdown document");
    }
    await s.settle();
    s.cmd("CmdPrevTab");
    await s.settle();
    await landsAtOnce(s, {
      name: "Markdown tab switch",
      change: async (ss) => ss.cmd("CmdNextTab"),
      undo: async (ss) => ss.cmd("CmdPrevTab"),
    });

    // close the window mid-Curl: the app exits, no hang
    await toFirstPage(s);
    const { err } = await s.startCurl();
    postMessage(frame, WM_CLOSE, 0, 0);
    const exited = await waitForExit(proc, EXIT_WAIT_MS);
    rows.push({
      name: "close the window mid-Curl",
      expect: `exits within ${EXIT_WAIT_MS} ms`,
      got: err || (exited ? `exited, code ${proc.exitCode}` : "still running"),
      ok: !err && exited,
    });

    const log = readFileSync(logPath, "utf8");
    const stuck = log.split(/\r?\n/).filter((l) => /TIMEOUT|didn't stop|Present failed/.test(l));
    rows.push({
      name: "log",
      expect: "no TIMEOUT, no stuck thread",
      got: stuck.join(" | ") || "clean",
      ok: stuck.length === 0,
    });
  } finally {
    if (other) {
      await killAndWait(other);
    }
    await killAndWait(proc);
  }

  printTable();
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-window-changes: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-window-changes: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-window-changes", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

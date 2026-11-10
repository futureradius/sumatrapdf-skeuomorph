// Skeuomorph Page Turn, build step 4: no flash (docs/skeuomorph-page-turn-spec.md,
// sections 3.10 and 5.7, ADR 0005).
//
// The debug build's flash recorder (src/skeuomorph/FlashRecorder.cpp, on with
// SKEUOMORPH_FLASH) records every frame Windows shows and logs one line per Curl:
//   skeuomorph-flash: #3 ok, Curl #3, 46 frames, 0 missed, ...
//   skeuomorph-flash: #4 FLASH (flicker 61% for 3 frames at 17 ms), Curl #4, ...
//
// 1. A burst of turns (forward, back, fast taps): every Curl checked, none flashed.
// 2. Test of the test: SumatraPDF repaints before the Curl covers the screen and
//    the layer is 50 ms late (SKEUOMORPH_FLASH=nowait,lag=50). The recorder must
//    flag flashes, or run 1 proves nothing.
//
// The window is put on top for the run: the recorder sees the real screen.
// Uses numbered books from the test shelf (C:\code\test-shelf), read only.
// Filmstrips of flashed turns and every frame's numbers land in tests/tmp/skeuomorph-flash-*.
//
// Build the debug exe first, then: bun tests/skeuomorph-flash.ts

import { dlopen, FFIType } from "bun:ffi";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import { postMessage, setForegroundWindow, sleep, VK_NEXT, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const BOOK = "C:\\code\\test-shelf\\generated\\numbered-040-pages.pdf";

const VK_PRIOR = 0x21;
const HWND_TOPMOST = -1;
const HWND_NOTOPMOST = -2;
const SWP_NOMOVE = 0x2;
const SWP_NOSIZE = 0x1;

const FAST_TAP_MS = 120;
const SETTLE_MS = 500;
const FLIGHT_WAIT_MS = 6000;
// the recorder judges a Curl ~400 ms after its layer went
const VERDICT_WAIT_MS = 8000;
// test of the test: the spike flagged 13 of 15 turns; demand at least half
const MIN_CONTROL_SHARE = 0.5;

const TURN_PREFIX = "skeuomorph: ";
const CURL_PREFIX = "skeuomorph-curl: ";
const FLASH_PREFIX = "skeuomorph-flash: ";

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
  "",
].join("\r\n");

// SetWindowPos with a z-order; tests/winapi.ts' wrapper always keeps the z-order
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

type Verdict = { n: number; ok: boolean; checked: boolean; line: string };

// "#3 ok, Curl #3, ..." / "#4 FLASH (...), ..." / "#5 not checked (...), ..."
function parseVerdict(line: string): Verdict | null {
  const m = /^#(\d+) (ok|FLASH|not checked)/.exec(line);
  if (!m) {
    return null;
  }
  return { n: +m[1], ok: m[2] === "ok", checked: m[2] !== "not checked", line };
}

class Run {
  private seen = new Map<string, number>();

  constructor(
    private client: ControlClient,
    readonly frame: number,
    private logPath: string,
  ) {}

  lines(prefix: string): string[] {
    const text = existsSync(this.logPath) ? readFileSync(this.logPath, "utf8") : "";
    return text
      .split(/\r?\n/)
      .filter((l) => l.startsWith(prefix))
      .map((l) => l.slice(prefix.length));
  }

  fresh(prefix: string): string[] {
    const all = this.lines(prefix);
    const out = all.slice(this.seen.get(prefix) ?? 0);
    this.seen.set(prefix, all.length);
    return out;
  }

  async settle(): Promise<void> {
    await this.client.waitForRenderIdle();
    await sleep(SETTLE_MS);
  }

  key(vk: number): void {
    postMessage(this.frame, WM_KEYDOWN, vk, 0);
    postMessage(this.frame, WM_KEYUP, vk, 0);
  }

  // waits until every Curl started so far has landed
  async waitLanded(): Promise<void> {
    const deadline = Date.now() + FLIGHT_WAIT_MS;
    while (Date.now() < deadline) {
      const curls = this.lines(TURN_PREFIX).filter((t) => t.startsWith("Curl")).length;
      const landed = this.lines(CURL_PREFIX).length;
      if (landed >= curls) {
        return;
      }
      await sleep(50);
    }
  }

  // one turn, then wait for it to land and settle (a clean start and end on screen)
  async turn(vk: number): Promise<void> {
    this.key(vk);
    await sleep(100);
    await this.waitLanded();
    await this.settle();
  }

  // two taps, the second while the first Curl is in the air
  async fastTaps(vk: number): Promise<void> {
    this.key(vk);
    await sleep(FAST_TAP_MS);
    this.key(vk);
    await sleep(100);
    await this.waitLanded();
    await this.settle();
  }

  curls(): number {
    return this.lines(TURN_PREFIX).filter((t) => t.startsWith("Curl")).length;
  }

  // the recorder's verdicts, once it judged every Curl
  async verdicts(): Promise<Verdict[]> {
    const want = this.curls();
    const deadline = Date.now() + VERDICT_WAIT_MS;
    let v: Verdict[] = [];
    while (Date.now() < deadline) {
      v = this.lines(FLASH_PREFIX)
        .map(parseVerdict)
        .filter((x): x is Verdict => x !== null);
      if (v.length >= want) {
        break;
      }
      await sleep(100);
    }
    return v;
  }
}

type Burst = (r: Run) => Promise<void>;

// forward, back, fast taps forward and back
const fullBurst: Burst = async (r) => {
  for (let i = 0; i < 5; i++) {
    await r.turn(VK_NEXT);
  }
  for (let i = 0; i < 3; i++) {
    await r.turn(VK_PRIOR);
  }
  for (let i = 0; i < 2; i++) {
    await r.fastTaps(VK_NEXT);
  }
  await r.fastTaps(VK_PRIOR);
};

const shortBurst: Burst = async (r) => {
  for (let i = 0; i < 6; i++) {
    await r.turn(VK_NEXT);
  }
};

type Result = { curls: number; verdicts: Verdict[]; log: string };

async function runBurst(name: string, flashEnv: string, burst: Burst): Promise<Result> {
  const appdata = tmpPath(`skeuomorph-flash-${name}-appdata`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);
  const outDir = tmpPath(`skeuomorph-flash-${name}`);
  rmSync(outDir, { recursive: true, force: true });
  const logPath = tmpPath(`skeuomorph-flash-${name}-log.txt`);
  writeFileSync(logPath, "");

  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", "facing", "-zoom", "fit page", BOOK];
  const env = { SKEUOMORPH_FLASH: `${flashEnv}out=${outDir}` };
  const { proc, client, frame } = await launchControlled(args, { env });
  try {
    setTopmost(frame, true);
    setForegroundWindow(frame);
    const r = new Run(client, frame, logPath);
    await r.settle();
    await burst(r);
    const verdicts = await r.verdicts();
    return { curls: r.curls(), verdicts, log: logPath };
  } finally {
    setTopmost(frame, false);
    await killAndWait(proc);
  }
}

function show(name: string, r: Result): void {
  console.log(`\n${name}: ${r.curls} Curls, ${r.verdicts.length} verdicts (log ${r.log})`);
  for (const v of r.verdicts) {
    console.log(`  ${v.line}`);
  }
}

export async function testit(): Promise<void> {
  if (!existsSync(BOOK)) {
    throw new Error(`test shelf missing: ${BOOK} (run bun tests/skeuomorph-test-shelf.ts)`);
  }
  const fails: string[] = [];

  const clean = await runBurst("burst", "", fullBurst);
  show("burst of turns", clean);
  const flashed = clean.verdicts.filter((v) => v.checked && !v.ok);
  const unchecked = clean.verdicts.filter((v) => !v.checked);
  if (clean.curls === 0) {
    fails.push("burst: no Curls played");
  }
  if (clean.verdicts.length < clean.curls) {
    fails.push(`burst: only ${clean.verdicts.length} of ${clean.curls} Curls judged`);
  }
  if (unchecked.length > 0) {
    fails.push(`burst: ${unchecked.length} Curls not checked (recorder saw no frames)`);
  }
  if (flashed.length > 0) {
    fails.push(`burst: ${flashed.length} Curls flashed (filmstrips in tests/tmp/skeuomorph-flash-burst)`);
  }

  const control = await runBurst("control", "nowait,lag=50,", shortBurst);
  show("test of the test (no cover wait, layer 50 ms late)", control);
  const caught = control.verdicts.filter((v) => v.checked && !v.ok).length;
  const need = Math.ceil(control.curls * MIN_CONTROL_SHARE);
  if (control.curls === 0 || caught < need) {
    fails.push(`test of the test: flagged ${caught} of ${control.curls} Curls, need at least ${need}`);
  }

  console.log("");
  if (fails.length > 0) {
    throw new Error(`skeuomorph-flash failed:\n  ${fails.join("\n  ")}`);
  }
  console.log(
    `skeuomorph-flash: passed (burst ${clean.curls} Curls, 0 flashes; test of the test flagged ${caught} of ${control.curls})`,
  );
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-flash", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

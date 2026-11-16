// Skeuomorph Page Turn, build step 8: Rough Pages
// (docs/skeuomorph-page-turn-spec.md, sections 3.10 and 5.6).
//
// Reads the debug log:
//   skeuomorph-rough: background copy made in 12 ms
//   skeuomorph-rough: page 17, done, 9 ms
//   skeuomorph-faces: #12 sharp 30 rough 12 blank 3
// "faces" counts face-frames of one Curl: a face drawn in one frame shows a
// sharp screen copy, a Rough Page, or blank (paper or an unsharp copy).
//
// 1. Opening a document makes the background copy.
// 2. Holding Page Down: pages flipped past show up, sharp or rough; few blank.
// 3. A Bundle (End): its outer sheets get Rough Pages of pages in between.
// 4. A scan from the test shelf, held Page Down: printed, not judged (speed
//    depends on the PC; the Release speed check is separate).
//
// Uses books from the test shelf (C:\code\test-shelf), read only. Settings
// live in a throwaway -appdata folder.
//
// Build the debug exe first, then: bun tests/skeuomorph-rough.ts

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import { postMessage, sleep, VK_NEXT, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const BOOK = "C:\\code\\test-shelf\\generated\\numbered-800-pages.pdf";
const SCAN = "C:\\code\\test-shelf\\Albertii - De re ædificatoria.pdf";

const VK_END = 0x23;
const KEY_REPEAT = 1 << 30;
const KEY_REPEAT_MS = 33;
const HOLD_MS = 1500;
const SETTLE_MS = 400;
const LAND_WAIT_MS = 4000;
const COPY_WAIT_MS = 5000;
// held Page Down on simple pages: at most this share of face-frames blank
const MAX_BLANK_SHARE = 0.25;

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
  "PageTurnDuration = 700",
  "",
].join("\r\n");

type Row = { name: string; expect: string; got: string; ok: boolean };
const rows: Row[] = [];

function check(name: string, expect: string, got: string, ok: boolean): void {
  rows.push({ name, expect, got, ok });
}

type Faces = { sharp: number; rough: number; blank: number };

function parseFaces(line: string): Faces | null {
  const m = /^skeuomorph-faces: #\d+ sharp (\d+) rough (\d+) blank (\d+)/.exec(line);
  return m ? { sharp: +m[1], rough: +m[2], blank: +m[3] } : null;
}

function parseRough(line: string): { page: number; what: string; ms: number } | null {
  const m = /^skeuomorph-rough: page (\d+), (\w+), (\d+) ms/.exec(line);
  return m ? { page: +m[1], what: m[2], ms: +m[3] } : null;
}

class Session {
  private seen = 0;
  constructor(
    private client: ControlClient,
    private frame: bigint,
    private logPath: string,
  ) {}

  // log lines since the last call
  fresh(): string[] {
    const lines = readFileSync(this.logPath, "utf8").split(/\r?\n/);
    const out = lines.slice(this.seen, Math.max(this.seen, lines.length - 1));
    this.seen = Math.max(this.seen, lines.length - 1);
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

  async hold(vk: number, ms: number): Promise<void> {
    postMessage(this.frame, WM_KEYDOWN, vk, 0);
    const end = Date.now() + ms;
    while (Date.now() < end) {
      await sleep(KEY_REPEAT_MS);
      postMessage(this.frame, WM_KEYDOWN, vk, KEY_REPEAT);
    }
    postMessage(this.frame, WM_KEYUP, vk, 0);
  }

  async waitFor(re: RegExp, ms: number): Promise<string[]> {
    const got: string[] = [];
    const deadline = Date.now() + ms;
    while (Date.now() < deadline) {
      got.push(...this.fresh());
      if (got.some((l) => re.test(l))) {
        break;
      }
      await sleep(100);
    }
    return got;
  }

  // the lines logged until every Curl has landed
  async collect(): Promise<string[]> {
    await sleep(LAND_WAIT_MS);
    await this.settle();
    return this.fresh();
  }
}

function tally(lines: string[]): Faces & { curls: number } {
  const t = { sharp: 0, rough: 0, blank: 0, curls: 0 };
  for (const l of lines) {
    const f = parseFaces(l);
    if (f) {
      t.sharp += f.sharp;
      t.rough += f.rough;
      t.blank += f.blank;
      t.curls++;
    }
  }
  return t;
}

function describe(t: Faces & { curls: number }, lines: string[]): string {
  const done = lines.map(parseRough).filter((r) => r?.what === "done");
  const ms = done.map((r) => r!.ms).sort((a, b) => a - b);
  const median = ms.length ? ms[Math.floor(ms.length / 2)] : 0;
  return `${t.curls} Curls: sharp ${t.sharp}, rough ${t.rough}, blank ${t.blank}; ${done.length} Rough Pages, median ${median} ms`;
}

async function open(path: string, logName: string) {
  const appdata = tmpPath(`skeuomorph-rough-appdata-${logName}`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);
  const logPath = tmpPath(`skeuomorph-rough-log-${logName}.txt`);
  writeFileSync(logPath, "");
  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", "facing", "-zoom", "fit page", path];
  const launched = await launchControlled(args);
  await launched.client.setNotificationsEnabled(false);
  return { ...launched, s: new Session(launched.client, launched.frame, logPath) };
}

async function numberedBook(): Promise<void> {
  const { proc, s } = await open(BOOK, "book");
  try {
    const opened = await s.waitFor(/skeuomorph-rough: background copy/, COPY_WAIT_MS);
    const copy = opened.find((l) => l.includes("background copy")) ?? "none";
    check("open a document", "background copy made", copy.replace("skeuomorph-rough: ", ""), copy.includes("made"));
    await s.settle();
    s.fresh();

    await s.hold(VK_NEXT, HOLD_MS);
    let lines = await s.collect();
    let t = tally(lines);
    const total = t.sharp + t.rough + t.blank;
    const blankShare = total ? t.blank / total : 1;
    check(
      "hold Page Down",
      `Rough Pages shown, blank <= ${MAX_BLANK_SHARE * 100}%`,
      `${describe(t, lines)}, blank ${(blankShare * 100).toFixed(0)}%`,
      t.curls > 0 && t.rough > 0 && blankShare <= MAX_BLANK_SHARE,
    );

    // a Bundle to the end: its outer sheets show pages from far in between
    s.key(VK_END);
    lines = await s.collect();
    const between = lines.map(parseRough).filter((r) => r?.what === "done" && r.page > 50 && r.page < 790);
    check(
      "Bundle to the end (End)",
      "Rough Pages of sheets in between",
      `pages ${between.map((r) => r!.page).join(", ") || "-"}`,
      between.length > 0,
    );
  } finally {
    await killAndWait(proc);
  }
}

async function scan(): Promise<void> {
  if (!existsSync(SCAN)) {
    console.log(`skeuomorph-rough: scan skipped, ${SCAN} is missing`);
    return;
  }
  const { proc, s } = await open(SCAN, "scan");
  try {
    await s.waitFor(/skeuomorph-rough: background copy/, COPY_WAIT_MS);
    await s.settle();
    s.fresh();
    await s.hold(VK_NEXT, HOLD_MS);
    const lines = await s.collect();
    check("scan, hold Page Down (not judged)", "printed", describe(tally(lines), lines), true);
  } finally {
    await killAndWait(proc);
  }
}

function printTable(): void {
  for (const r of rows) {
    console.log(`${r.ok ? "ok  " : "FAIL"} ${r.name}\n     expect: ${r.expect}\n     got:    ${r.got}`);
  }
}

export async function testit(): Promise<void> {
  if (!existsSync(BOOK)) {
    console.log(`skeuomorph-rough: skipped, ${BOOK} is missing (run bun tests/skeuomorph-test-shelf.ts)`);
    return;
  }
  rows.length = 0;
  await numberedBook();
  await scan();

  printTable();
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-rough: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-rough: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-rough", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

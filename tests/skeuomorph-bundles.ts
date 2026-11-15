// Skeuomorph Page Turn, build step 7: Bundles
// (docs/skeuomorph-page-turn-spec.md, sections 3.5 and 3.7).
//
// Reads the debug log's per-turn lines and per-Curl lines:
//   skeuomorph: Bundle 18 px forward, 798 pages, sweep left, 910 ms, 1 in air, 0 landed [1-2 -> 799-800]
//   skeuomorph: Curl backward, 2 pages, sweep right, 700 ms, 2 in air, 0 landed [799-800 -> 797-798], peels #3
//   skeuomorph-curl: #3 landed after 912 ms, 55 frames, 0 late, ..., 14 sheets, 1 back
//
// 1. Home and End curl a Bundle of 14 sheets that plays its whole length
//    (PageTurnDuration x 1.3), not an instant jump.
// 2. A small jump draws fewer sheets: 2 pages, 2 sheets.
// 3. A tap the other way while a Bundle is in the air turns its last sheet
//    back: still one turn, landing in its own time, on the Spread before.
//    Enough such taps turn the whole Bundle back (5 Page Downs, Home, 6 Page Downs).
// 4. A jump across a page still in the air lands that page at once.
// 5. Bundles count in the tapping rhythm and the 5 in the air: after quick
//    taps, End is shorter than 910 ms and shares the air.
// 6. Book View: opening and closing the cover with a Bundle.
// What the sheets and moving Page Edges look like is for eyes: see the checklist
// on the build issue.
//
// Uses numbered books from the test shelf (C:\code\test-shelf), read only.
// Settings live in a throwaway -appdata folder under tests/tmp.
//
// Build the debug exe first, then: bun tests/skeuomorph-bundles.ts

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import { postMessage, sleep, VK_NEXT, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const SHELF = "C:\\code\\test-shelf\\generated";
const BOOK800 = join(SHELF, "numbered-800-pages.pdf");
const BOOK40 = join(SHELF, "numbered-040-pages.pdf");
const BOOK3 = join(SHELF, "numbered-003-pages.pdf");

const VK_PRIOR = 0x21;
const VK_END = 0x23;
const VK_HOME = 0x24;
const NORMAL_MS = 700;
const BUNDLE_MS = 910;
const MAX_SHEETS = 14;
// a Bundle plays its length; landing at once would be far shorter
const PLAYS_MIN_MS = BUNDLE_MS - 60;
const PLAYS_MAX_MS = BUNDLE_MS + 700;
const SETTLE_MS = 400;
const LAND_WAIT_MS = 6000;

const TURN_PREFIX = "skeuomorph: ";
const CURL_PREFIX = "skeuomorph-curl: ";

function settingsFor(view: string): string {
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
    "DefaultZoom = fit page",
    "SkeuomorphPageTurn = true",
    `PageTurnDuration = ${NORMAL_MS}`,
    "",
  ].join("\r\n");
}

type Row = { name: string; expect: string; got: string; ok: boolean };
const rows: Row[] = [];

function check(name: string, expect: string, got: string, ok: boolean): void {
  rows.push({ name, expect, got, ok });
}

// "Bundle 18 px forward, 798 pages, sweep left, 910 ms, 1 in air, 0 landed [1-2 -> 799-800]"
type Turn = { kind: string; lenMs: number; inAir: number; from: string; to: string; peels: boolean; line: string };

function parseTurn(line: string): Turn | null {
  const m = /^(\S+).*, (\d+) ms, (\d+) in air, \d+ landed \[([\d-]+) -> ([\d-]+)\]/.exec(line);
  if (!m) {
    return null;
  }
  return { kind: m[1], lenMs: +m[2], inAir: +m[3], from: m[4], to: m[5], peels: line.includes(", peels #"), line };
}

// "#3 landed after 912 ms, 55 frames, 0 late, ..., 14 sheets, 1 back"
type Flight = { id: number; how: string; why: string; afterMs: number; sheets: number; back: number; line: string };

function parseFlight(line: string): Flight | null {
  const m = /^#(\d+) (landed at once|landed|TIMEOUT|sent back)(?: \(([^)]*)\))? after (-?\d+) ms/.exec(line);
  if (!m) {
    return null;
  }
  const s = /, (\d+) sheets(?:, (\d+) back)?$/.exec(line);
  const sheets = s ? +s[1] : 1;
  const back = s?.[2] ? +s[2] : 0;
  return { id: +m[1], how: m[2], why: m[3] ?? "", afterMs: +m[4], sheets, back, line };
}

class Session {
  private seen = new Map<string, number>();

  constructor(
    private client: ControlClient,
    readonly frame: number,
    private logPath: string,
  ) {}

  private lines(prefix: string): string[] {
    const text = existsSync(this.logPath) ? readFileSync(this.logPath, "utf8") : "";
    return text
      .split(/\r?\n/)
      .filter((l) => l.startsWith(prefix))
      .map((l) => l.slice(prefix.length));
  }

  private fresh(prefix: string): string[] {
    const all = this.lines(prefix);
    const out = all.slice(this.seen.get(prefix) ?? 0);
    this.seen.set(prefix, all.length);
    return out;
  }

  mark(): void {
    this.fresh(TURN_PREFIX);
    this.fresh(CURL_PREFIX);
  }

  async settle(): Promise<void> {
    await this.client.waitForRenderIdle();
    await sleep(SETTLE_MS);
  }

  key(vk: number): void {
    postMessage(this.frame, WM_KEYDOWN, vk, 0);
    postMessage(this.frame, WM_KEYUP, vk, 0);
  }

  // the turns and flights since mark(), once every one has left the air
  async collect(): Promise<{ turns: Turn[]; flights: Flight[] }> {
    const turns: Turn[] = [];
    const flights: Flight[] = [];
    const deadline = Date.now() + LAND_WAIT_MS;
    while (Date.now() < deadline) {
      turns.push(
        ...this.fresh(TURN_PREFIX)
          .map(parseTurn)
          .filter((t): t is Turn => t !== null),
      );
      flights.push(
        ...this.fresh(CURL_PREFIX)
          .map(parseFlight)
          .filter((f): f is Flight => f !== null),
      );
      const ended = flights.filter((f) => f.how !== "sent back").length;
      const starts = turns.filter((t) => !t.line.includes("sends #") && !t.peels).length;
      if (turns.length > 0 && ended >= starts) {
        break;
      }
      await sleep(100);
    }
    await this.settle();
    flights.sort((a, b) => a.id - b.id);
    return { turns, flights };
  }
}

function describe(turns: Turn[], flights: Flight[]): string {
  const t = turns.map((x) => `${x.kind} ${x.lenMs} ms ${x.from}->${x.to}${x.peels ? " peels" : ""}`).join("; ");
  const f = flights
    .map((x) => {
      const sheets = x.sheets > 1 ? `, ${x.sheets} sheets${x.back ? `, ${x.back} back` : ""}` : "";
      return `#${x.id} ${x.how}${x.why ? ` (${x.why})` : ""} ${x.afterMs} ms${sheets}`;
    })
    .join("; ");
  return `${t} | ${f}`;
}

function plays(f: Flight | undefined): boolean {
  return !!f && f.how === "landed" && f.afterMs >= PLAYS_MIN_MS && f.afterMs <= PLAYS_MAX_MS;
}

async function homeEnd(s: Session): Promise<void> {
  for (const [name, vk] of [
    ["End", VK_END],
    ["Home", VK_HOME],
  ] as const) {
    s.mark();
    s.key(vk);
    const { turns, flights } = await s.collect();
    const ok =
      turns.length === 1 &&
      turns[0].kind === "Bundle" &&
      turns[0].lenMs === BUNDLE_MS &&
      flights.length === 1 &&
      flights[0].sheets === MAX_SHEETS &&
      plays(flights[0]);
    check(`Facing: ${name}`, `Bundle of ${MAX_SHEETS} sheets plays ~${BUNDLE_MS} ms`, describe(turns, flights), ok);
  }
}

async function splitLastSheet(s: Session): Promise<void> {
  s.mark();
  s.key(VK_END);
  await sleep(350);
  s.key(VK_PRIOR);
  const { turns, flights } = await s.collect();
  const bundle = flights[0];
  const ok =
    turns.length === 2 &&
    turns[1].peels &&
    turns[1].inAir === 1 &&
    turns[1].to === "797-798" &&
    flights.length === 1 &&
    bundle.how === "landed" &&
    bundle.sheets === MAX_SHEETS &&
    bundle.back === 1;
  check("End, Page Up mid-Bundle", "last sheet turns back, lands on 797-798", describe(turns, flights), ok);
  s.key(VK_HOME);
  await s.collect();
}

// the human's case: reading on right after Home turns the whole Bundle back
async function readOnAfterHome(s: Session): Promise<void> {
  s.mark();
  for (let i = 0; i < 5; i++) {
    s.key(VK_NEXT);
    await sleep(150);
  }
  s.key(VK_HOME);
  await sleep(150);
  for (let i = 0; i < 6; i++) {
    s.key(VK_NEXT);
    await sleep(150);
  }
  const { turns, flights } = await s.collect();
  const home = flights.find((f) => f.sheets > 1);
  const ok =
    turns[turns.length - 1]?.to === "13-14" &&
    turns.filter((t) => t.peels).length === 5 &&
    !!home &&
    home.how === "landed" &&
    home.back === home.sheets;
  check("5 Page Downs, Home, 6 Page Downs", "whole Bundle turns back, lands on 13-14", describe(turns, flights), ok);
  s.key(VK_HOME);
  await s.collect();
}

async function jumpAcross(s: Session): Promise<void> {
  s.mark();
  s.key(VK_NEXT);
  await sleep(200);
  s.key(VK_PRIOR);
  await sleep(60);
  s.key(VK_END);
  const { turns, flights } = await s.collect();
  const across = flights.filter((f) => f.why === "jump across");
  const last = flights[flights.length - 1];
  // quick taps: the Bundle is shorter than 910 ms, but plays to its end
  const ok = turns.length === 3 && across.length === 1 && last?.sheets === MAX_SHEETS && last.how === "landed";
  check("Page Down, Page Up, End", "the page in the air lands at once", describe(turns, flights), ok);
  s.key(VK_HOME);
  await s.collect();
}

async function rhythm(s: Session): Promise<void> {
  s.mark();
  s.key(VK_NEXT);
  await sleep(120);
  s.key(VK_NEXT);
  await sleep(120);
  s.key(VK_END);
  const { turns, flights } = await s.collect();
  const end = turns[turns.length - 1];
  const ok =
    turns.length === 3 &&
    end?.kind === "Bundle" &&
    end.lenMs < BUNDLE_MS &&
    end.inAir >= 2 &&
    flights.every((f) => f.how === "landed");
  check("2 quick Page Downs, End", "faster Bundle shares the air", describe(turns, flights), ok);
  s.key(VK_HOME);
  await s.collect();
}

async function smallJump(s: Session): Promise<void> {
  s.mark();
  s.key(VK_END);
  const { turns, flights } = await s.collect();
  const ok =
    turns.length === 1 &&
    turns[0].kind === "Bundle" &&
    flights.length === 1 &&
    flights[0].sheets === 2 &&
    plays(flights[0]);
  check("Single, 3 pages: End", "Bundle of 2 sheets plays", describe(turns, flights), ok);
}

async function cover(s: Session): Promise<void> {
  for (const [name, vk, to] of [
    ["End from the cover", VK_END, "40-40"],
    ["Home to the cover", VK_HOME, "1-1"],
  ] as const) {
    s.mark();
    s.key(vk);
    const { turns, flights } = await s.collect();
    const ok = turns.length === 1 && turns[0].kind === "Bundle" && turns[0].to === to && plays(flights[0]);
    check(`Book View: ${name}`, `Bundle plays, lands on ${to}`, describe(turns, flights), ok);
  }
}

async function withBook(name: string, book: string, view: string, run: (s: Session) => Promise<void>): Promise<void> {
  const appdata = tmpPath(`skeuomorph-bundles-${name}-appdata`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), settingsFor(view));
  const logPath = tmpPath(`skeuomorph-bundles-${name}-log.txt`);
  writeFileSync(logPath, "");

  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", view, "-zoom", "fit page", book];
  const { proc, client, frame } = await launchControlled(args);
  try {
    await client.setNotificationsEnabled(false);
    const s = new Session(client, frame, logPath);
    await s.settle();
    await run(s);
  } finally {
    await killAndWait(proc);
  }
}

function printTable(): void {
  const w = Math.max(...rows.map((r) => r.name.length));
  const e = Math.max(...rows.map((r) => r.expect.length));
  for (const r of rows) {
    console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name.padEnd(w)}  ${r.expect.padEnd(e)}  got: ${r.got}`);
  }
}

export async function testit(): Promise<void> {
  for (const book of [BOOK800, BOOK40, BOOK3]) {
    if (!existsSync(book)) {
      console.log(`skeuomorph-bundles: skipped, ${book} is missing (run bun tests/skeuomorph-test-shelf.ts)`);
      return;
    }
  }
  rows.length = 0;
  await withBook("facing", BOOK800, "facing", async (s) => {
    await homeEnd(s);
    await splitLastSheet(s);
    await readOnAfterHome(s);
    await jumpAcross(s);
    await rhythm(s);
  });
  await withBook("book", BOOK40, "book view", cover);
  await withBook("single", BOOK3, "single page", smallJump);

  printTable();
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-bundles: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-bundles: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-bundles", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

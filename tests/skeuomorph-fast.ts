// Skeuomorph Page Turn, build step 6: several Curls at once and fast tapping
// (docs/skeuomorph-page-turn-spec.md, section 3.4).
//
// Reads the debug log's per-turn lines and per-Curl lines:
//   skeuomorph: Curl forward, 2 pages, sweep left, 480 ms, 3 in air, 0 landed [5-6 -> 7-8]
//   skeuomorph: Curl backward, 2 pages, sweep right, 700 ms, 1 in air, 0 landed [3-4 -> 1-2], sends #7 back
//   skeuomorph-curl: #9 landed after 482 ms, 29 frames, 0 late, ...
//   skeuomorph-curl: #10 landed at once (sixth Curl) after 250 ms, ...
//   skeuomorph-curl: #11 sent back after 250 ms, ...
//
// 1. Fast taps: every tap is its own Curl, several in the air, each shorter
//    than a normal one but never below 310 ms.
// 2. Very fast taps and 3. holding a key: Curls are 310 ms, never more than 5
//    in the air.
// 4. A backward tap while a page is in the air sends it back; with two in the
//    air, two backward taps send both back.
// 5. After tapping stops, the length eases back to normal.
// Every Curl ends in its own time or at once as the sixth; none times out.
// Late frames are printed, not judged: the Release speed check judges them.
//
// Uses numbered books from the test shelf (C:\code\test-shelf), read only.
// Settings live in a throwaway -appdata folder under .work.
//
// Build the debug exe first, then: bun tests/skeuomorph-fast.ts

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import { postMessage, sleep, VK_NEXT, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const BOOK = "C:\\code\\test-shelf\\generated\\numbered-800-pages.pdf";

const VK_PRIOR = 0x21;
const NORMAL_MS = 700;
const FAST_MIN_MS = 310;
const MAX_IN_AIR = 5;
// WM_KEYDOWN lParam for a key repeat: the key was down before
const KEY_REPEAT = 1 << 30;
const KEY_REPEAT_MS = 33;
const HOLD_MS = 1200;
const SETTLE_MS = 400;
const LAND_WAIT_MS = 6000;
// the fast length holds while another tap could count, then eases back over a normal length
const EASE_BACK_MS = 2 * NORMAL_MS + 200;

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
  `PageTurnDuration = ${NORMAL_MS}`,
  "",
].join("\r\n");

type Row = { name: string; expect: string; got: string; ok: boolean };
const rows: Row[] = [];

function check(name: string, expect: string, got: string, ok: boolean): void {
  rows.push({ name, expect, got, ok });
}

// "Curl forward, 2 pages, sweep left, 480 ms, 3 in air, 0 landed [5-6 -> 7-8], sends #7 back"
type Turn = { kind: string; lenMs: number; inAir: number; from: string; to: string; sendsBack: boolean };

function parseTurn(line: string): Turn | null {
  const m = /^(\S+).*, (\d+) ms, (\d+) in air, \d+ landed \[([\d-]+) -> ([\d-]+)\](, sends #\d+ back)?/.exec(line);
  if (!m) {
    return null;
  }
  return { kind: m[1], lenMs: +m[2], inAir: +m[3], from: m[4], to: m[5], sendsBack: !!m[6] };
}

// "#9 landed after 482 ms, 29 frames, 0 late, ..."
type Flight = { how: string; why: string; afterMs: number; late: number; line: string };

function parseFlight(line: string): Flight | null {
  const m =
    /^#\d+ (landed at once|landed|TIMEOUT|sent back)(?: \(([^)]*)\))? after (-?\d+) ms, \d+ frames, (\d+) late/.exec(
      line,
    );
  if (!m) {
    return null;
  }
  return { how: m[1], why: m[2] ?? "", afterMs: +m[3], late: +m[4], line };
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

  // forget what was logged so far
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

  async taps(vk: number, n: number, gapMs: number): Promise<void> {
    for (let i = 0; i < n; i++) {
      this.key(vk);
      await sleep(gapMs);
    }
  }

  // key repeat while held, as Windows sends it
  async hold(vk: number, ms: number): Promise<number> {
    postMessage(this.frame, WM_KEYDOWN, vk, 0);
    let n = 1;
    const end = Date.now() + ms;
    while (Date.now() < end) {
      await sleep(KEY_REPEAT_MS);
      postMessage(this.frame, WM_KEYDOWN, vk, KEY_REPEAT);
      n++;
    }
    postMessage(this.frame, WM_KEYUP, vk, 0);
    return n;
  }

  // the turns and Curls since mark(), once every Curl has ended
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
      // a send-back adds a "sent back" line, then its Curl ends once more
      const curls = turns.filter((t) => t.kind === "Curl").length;
      if (curls > 0 && flights.length >= curls) {
        break;
      }
      await sleep(100);
    }
    await this.settle();
    return { turns, flights };
  }
}

function summary(turns: Turn[], flights: Flight[]): string {
  const lens = [...new Set(turns.map((t) => t.lenMs))].join("/");
  const air = Math.max(0, ...turns.map((t) => t.inAir));
  const late = flights.reduce((s, f) => s + f.late, 0);
  const atOnce = flights.filter((f) => f.how === "landed at once").map((f) => f.why);
  const timeouts = flights.filter((f) => f.how === "TIMEOUT").length;
  return `${turns.length} turns, ${lens} ms, max ${air} in air, ${late} late, at once: ${atOnce.join(",") || "-"}, ${timeouts} timeouts`;
}

// every Curl ends in its own time, or at once as the sixth
function endsOk(flights: Flight[]): boolean {
  return flights.every((f) => f.how === "landed" || f.how === "sent back" || f.why === "sixth Curl");
}

async function fastTaps(s: Session): Promise<void> {
  s.mark();
  const gap = 120;
  await s.taps(VK_NEXT, 8, gap);
  const { turns, flights } = await s.collect();
  const later = turns.slice(1);
  const ok =
    turns.length === 8 &&
    turns.every((t) => t.kind === "Curl") &&
    turns[0].lenMs === NORMAL_MS &&
    later.every((t) => t.lenMs >= FAST_MIN_MS && t.lenMs < NORMAL_MS) &&
    Math.max(...turns.map((t) => t.inAir)) >= 3 &&
    endsOk(flights);
  check(`fast taps (${gap} ms apart)`, "8 Curls, 700 then 310-699 ms, >= 3 in air", summary(turns, flights), ok);
}

async function veryFastTaps(s: Session): Promise<void> {
  s.mark();
  const gap = 50;
  await s.taps(VK_NEXT, 12, gap);
  const { turns, flights } = await s.collect();
  const curls = turns.filter((t) => t.kind === "Curl");
  const ok =
    curls.length >= 10 &&
    curls.slice(2).every((t) => t.lenMs === FAST_MIN_MS) &&
    turns.every((t) => t.inAir <= MAX_IN_AIR) &&
    Math.max(...turns.map((t) => t.inAir)) === MAX_IN_AIR &&
    endsOk(flights);
  check(`very fast taps (${gap} ms apart)`, "310 ms Curls, at most 5 in air", summary(turns, flights), ok);
}

async function holdKey(s: Session): Promise<void> {
  s.mark();
  const n = await s.hold(VK_NEXT, HOLD_MS);
  const { turns, flights } = await s.collect();
  const curls = turns.filter((t) => t.kind === "Curl");
  const ok =
    curls.length >= n / 2 &&
    curls.slice(2).every((t) => t.lenMs === FAST_MIN_MS) &&
    turns.every((t) => t.inAir <= MAX_IN_AIR) &&
    endsOk(flights);
  check(`hold Page Down (${n} key repeats)`, "310 ms Curls, at most 5 in air", summary(turns, flights), ok);
}

async function sendBack(s: Session): Promise<void> {
  s.mark();
  s.key(VK_NEXT);
  await sleep(250);
  s.key(VK_PRIOR);
  const { turns, flights } = await s.collect();
  const ok =
    turns.length === 2 &&
    turns[1].sendsBack &&
    turns[1].inAir === 1 &&
    turns[1].to === turns[0].from &&
    flights.length === 2 &&
    flights[0].how === "sent back" &&
    flights[1].how === "landed" &&
    flights[1].afterMs < NORMAL_MS;
  check(
    "Page Down, Page Up mid-Curl",
    "sent back, lands where it started, 1 in air",
    `${turns.map((t) => (t.sendsBack ? "sends back" : t.kind)).join(", ")}; ${flights.map((f) => `${f.how} after ${f.afterMs} ms`).join(", ")}`,
    ok,
  );
}

async function sendBothBack(s: Session): Promise<void> {
  s.mark();
  s.key(VK_NEXT);
  await sleep(250);
  s.key(VK_NEXT);
  await sleep(150);
  s.key(VK_PRIOR);
  await sleep(60);
  s.key(VK_PRIOR);
  const { turns, flights } = await s.collect();
  const back = turns.filter((t) => t.sendsBack);
  const ok =
    turns.length === 4 &&
    back.length === 2 &&
    turns[3].to === turns[0].from &&
    flights.filter((f) => f.how === "sent back").length === 2 &&
    flights.filter((f) => f.how === "landed").length === 2;
  check(
    "two in the air, two Page Ups",
    "both sent back, ends where it started",
    `${turns.map((t) => (t.sendsBack ? "sends back" : t.kind)).join(", ")}; ${summary(turns, flights)}`,
    ok,
  );
}

// "#12 landed after 40 ms" -> {id, line}
function flightIds(lines: Flight[]): { id: number; f: Flight }[] {
  return lines.map((f) => ({ id: +(/^#(\d+)/.exec(f.line)?.[1] ?? 0), f })).sort((a, b) => a.id - b.id);
}

// Page Up, Page Down (sent back, it comes home), Page Up again: once home, the
// page leaves the air when the next Curl starts. Left lying on its side, it
// hid the next page lifting there (seen as a page sliding out of the binding)
async function upDownUp(s: Session): Promise<void> {
  const gaps = [
    [30, 40],
    [30, 50],
    [30, 60],
    [40, 60],
    [40, 80],
  ];
  const stayed: string[] = [];
  let tried = 0;
  for (const [down, up] of gaps) {
    s.key(VK_NEXT);
    await s.collect();
    s.mark();
    s.key(VK_PRIOR);
    await sleep(down);
    s.key(VK_NEXT);
    await sleep(up);
    s.key(VK_PRIOR);
    const { flights } = await s.collect();
    const byId = flightIds(flights);
    // sent back, came home (its own line), then a new Curl
    if (byId.length !== 3 || byId[0].f.how !== "sent back" || byId[1].f.how === "sent back") {
      continue;
    }
    tried++;
    if (byId[1].f.afterMs > 200) {
      stayed.push(`#${byId[1].id} home after ${byId[1].f.afterMs} ms`);
    }
  }
  check(
    "Page Up, Down, Up quickly",
    "a page back home leaves the air",
    `${tried} of ${gaps.length} came home first; ${stayed.join(", ") || "none stayed"}`,
    tried > 0 && stayed.length === 0,
  );
}

// Page Down, Page Up (sent back), Page Up again: a Curl the other way lands
// the one in the air. The new one must still play and go, not leave blank
// pages until the safety timeout (or a mouse move)
async function downUpUp(s: Session): Promise<void> {
  s.mark();
  s.key(VK_NEXT);
  await sleep(200);
  s.key(VK_PRIOR);
  await sleep(50);
  s.key(VK_PRIOR);
  const { turns, flights } = await s.collect();
  const last = flightIds(flights).pop()?.f;
  const ok = turns.length === 3 && !!last && last.how === "landed" && last.afterMs < 2 * NORMAL_MS;
  check(
    "Page Down, Up, Up quickly",
    "the last Curl lands in its time",
    flights.map((f) => `${f.how}${f.why ? ` (${f.why})` : ""} after ${f.afterMs} ms`).join(", "),
    ok,
  );
}

async function easeBack(s: Session): Promise<void> {
  s.mark();
  await s.taps(VK_NEXT, 4, 100);
  await sleep(EASE_BACK_MS);
  s.key(VK_NEXT);
  const { turns } = await s.collect();
  const last = turns[turns.length - 1];
  check(
    "tapping stops, then one tap",
    "700 ms again",
    last ? `${last.lenMs} ms` : "(no line)",
    last?.lenMs === NORMAL_MS,
  );
}

function printTable(): void {
  const w = Math.max(...rows.map((r) => r.name.length));
  const e = Math.max(...rows.map((r) => r.expect.length));
  for (const r of rows) {
    console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name.padEnd(w)}  ${r.expect.padEnd(e)}  got: ${r.got}`);
  }
}

export async function testit(): Promise<void> {
  if (!existsSync(BOOK)) {
    console.log(`skeuomorph-fast: skipped, ${BOOK} is missing (run bun tests/skeuomorph-test-shelf.ts)`);
    return;
  }
  rows.length = 0;
  const appdata = tmpPath("skeuomorph-fast-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);
  const logPath = tmpPath("skeuomorph-fast-log.txt");
  writeFileSync(logPath, "");

  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", "facing", "-zoom", "fit page", BOOK];
  const { proc, client, frame } = await launchControlled(args);
  try {
    await client.setNotificationsEnabled(false);
    const s = new Session(client, frame, logPath);
    await s.settle();
    await fastTaps(s);
    await veryFastTaps(s);
    await holdKey(s);
    await sendBack(s);
    await sendBothBack(s);
    await downUpUp(s);
    await upDownUp(s);
    await easeBack(s);
  } finally {
    await killAndWait(proc);
  }

  printTable();
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-fast: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-fast: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-fast", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

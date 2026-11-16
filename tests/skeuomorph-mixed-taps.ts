// Skeuomorph Page Turn, build step 7: random mixed Page Down / Page Up taps
// (docs/skeuomorph-page-turn-spec.md, section 3.4).
//
// Taps Page Down / Page Up in a seeded random order, 40-150 ms apart, in
// Facing and Single Page, from the middle of an 800 page book. Every tap must
// play: no Curl lands at once (except the oldest of 6 in the air, by spec),
// none times out, every tap is a turn, and every turn gets a Curl.
//
// Reads the debug log:
//   skeuomorph: Curl backward, 1 page, sweep right, 310 ms, 3 in air, 0 landed [400-400 -> 399-399]
//   skeuomorph: nothing (<why>) [..]
//   skeuomorph-curl: #12 landed at once (next Curl) after 120 ms, ...
//   skeuomorph-curl: no Curl (no copy of the old Spread)
//
// Uses numbered books from the test shelf (C:\code\test-shelf), read only.
// Build the debug exe first, then: bun tests/skeuomorph-mixed-taps.ts

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import { postMessage, sleep, VK_NEXT, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const BOOK = "C:\\code\\test-shelf\\generated\\numbered-800-pages.pdf";
const START_PAGE = 400;

const VK_PRIOR = 0x21;
const TAPS = 40;
const MIN_GAP_MS = 40;
const MAX_GAP_MS = 150;
// chance the next tap goes the same way: runs make Curls pile up both ways
const KEEP_WAY = 0.6;
const SEEDS = [1, 2, 3];
const QUIET_MS = 1500;
const LAND_WAIT_MS = 8000;
const ALLOWED_AT_ONCE = "sixth Curl";

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
    "PageTurnDuration = 700",
    "",
  ].join("\r\n");
}

// mulberry32: the same taps every run
function rng(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

type Row = { name: string; ok: boolean; got: string };
const rows: Row[] = [];

class Log {
  private seen = 0;
  constructor(private path: string) {}

  private all(): string[] {
    const text = existsSync(this.path) ? readFileSync(this.path, "utf8") : "";
    return text.split(/\r?\n/);
  }

  mark(): void {
    this.seen = this.all().length;
  }

  // the lines since mark(), once nothing more is logged for a while
  async fresh(client: ControlClient): Promise<string[]> {
    const deadline = Date.now() + LAND_WAIT_MS;
    let n = -1;
    let quietSince = Date.now();
    while (Date.now() < deadline) {
      const now = this.all().length;
      if (now !== n) {
        n = now;
        quietSince = Date.now();
      } else if (Date.now() - quietSince >= QUIET_MS) {
        break;
      }
      await sleep(100);
    }
    await client.waitForRenderIdle();
    const out = this.all().slice(this.seen);
    this.seen += out.length;
    return out;
  }
}

function key(frame: number, vk: number): void {
  postMessage(frame, WM_KEYDOWN, vk, 0);
  postMessage(frame, WM_KEYUP, vk, 0);
}

async function tapRandomly(frame: number, seed: number): Promise<string> {
  const rand = rng(seed);
  let down = true;
  let keys = "";
  for (let i = 0; i < TAPS; i++) {
    if (rand() >= KEEP_WAY) {
      down = !down;
    }
    key(frame, down ? VK_NEXT : VK_PRIOR);
    keys += down ? "D" : "U";
    await sleep(MIN_GAP_MS + rand() * (MAX_GAP_MS - MIN_GAP_MS));
  }
  return keys;
}

function count(items: string[]): string {
  const m = new Map<string, number>();
  for (const s of items) {
    m.set(s, (m.get(s) ?? 0) + 1);
  }
  return [...m].map(([k, v]) => `${v}x ${k}`).join(", ") || "-";
}

function judge(name: string, keys: string, lines: string[]): void {
  const turns = lines.filter((l) => l.startsWith(TURN_PREFIX)).map((l) => l.slice(TURN_PREFIX.length));
  const curls = lines.filter((l) => l.startsWith(CURL_PREFIX)).map((l) => l.slice(CURL_PREFIX.length));

  const played = turns.filter((t) => /^(Curl|Bundle)/.test(t));
  const nothing = turns.filter((t) => t.startsWith("nothing")).map((t) => /^nothing \(([^)]*)\)/.exec(t)?.[1] ?? t);
  const atOnce = curls.map((c) => /landed at once \(([^)]*)\)/.exec(c)?.[1]).filter((w): w is string => !!w);
  const dropped = atOnce.filter((w) => w !== ALLOWED_AT_ONCE);
  const timeouts = curls.filter((c) => /TIMEOUT/.test(c)).length;
  const noCurl = curls.filter((c) => c.startsWith("no Curl")).length;
  const bundles = played.filter((t) => t.startsWith("Bundle")).length;

  const ok = dropped.length === 0 && nothing.length === 0 && timeouts === 0 && noCurl === 0;
  const got =
    `${TAPS} taps -> ${played.length} turns (${bundles} Bundles); at once: ${count(atOnce)}; ` +
    `nothing: ${count(nothing)}; ${timeouts} timeouts; ${noCurl} without a Curl`;
  rows.push({ name, ok, got });
  if (!ok) {
    console.log(`--- ${name}: ${keys}`);
    for (const l of lines.filter((l) => l.startsWith(TURN_PREFIX) || l.startsWith(CURL_PREFIX))) {
      console.log(`  ${l}`);
    }
  }
}

async function runView(view: string, label: string): Promise<void> {
  const appdata = tmpPath(`skeuomorph-mixed-${label}-appdata`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), settingsFor(view));
  const logPath = tmpPath(`skeuomorph-mixed-${label}-log.txt`);
  writeFileSync(logPath, "");

  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", view, "-page", `${START_PAGE}`, BOOK];
  const { proc, client, frame } = await launchControlled(args);
  try {
    await client.setNotificationsEnabled(false);
    const log = new Log(logPath);
    await log.fresh(client);
    for (const seed of SEEDS) {
      log.mark();
      const keys = await tapRandomly(frame, seed);
      judge(`${label}, seed ${seed}`, keys, await log.fresh(client));
    }
  } finally {
    await killAndWait(proc);
  }
}

export async function testit(): Promise<void> {
  if (!existsSync(BOOK)) {
    console.log(`skeuomorph-mixed-taps: skipped, ${BOOK} is missing (run bun tests/skeuomorph-test-shelf.ts)`);
    return;
  }
  rows.length = 0;
  await runView("facing", "Facing");
  await runView("single page", "Single Page");

  const w = Math.max(...rows.map((r) => r.name.length));
  for (const r of rows) {
    console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name.padEnd(w)}  ${r.got}`);
  }
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-mixed-taps: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-mixed-taps: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-mixed-taps", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

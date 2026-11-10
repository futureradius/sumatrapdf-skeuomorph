// Skeuomorph Page Turn, build step 2: Page Turn detection.
// Presses keys and sends commands, then checks the per-turn debug log line
// ("skeuomorph: Curl ...", "skeuomorph: Bundle 18 px ...",
// "skeuomorph: nothing (<why>) ..."). Nothing is drawn yet.
//
// Uses the 40-page numbered book from the test shelf (C:\code\test-shelf),
// read only. Settings live in a throwaway -appdata folder under .work.
//
// Build the debug exe first, then: bun tests/skeuomorph-detect.ts

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { cmdId, prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled, sendCommandSync } from "./win-automation.ts";
import { postMessage, sleep, VK_END, VK_HOME, VK_NEXT, WM_CHAR, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";
import type { ControlClient } from "./control.ts";

const SHELF = "C:\\code\\test-shelf\\generated";
const BOOK40 = `${SHELF}\\numbered-040-pages.pdf`;
const LOG_PREFIX = "skeuomorph: ";
// a flushed paint after render-idle refreshes the fingerprint
const SETTLE_MS = 300;

type Row = { name: string; expect: string; got: string; ok: boolean };

class Session {
  private seen = 0;

  constructor(
    private client: ControlClient,
    private frame: number,
    private logPath: string,
  ) {}

  // skeuomorph lines logged since the last call
  newLines(): string[] {
    const text = existsSync(this.logPath) ? readFileSync(this.logPath, "utf8") : "";
    const lines = text.split(/\r?\n/).filter((l) => l.startsWith(LOG_PREFIX));
    const fresh = lines.slice(this.seen);
    this.seen = lines.length;
    return fresh.map((l) => l.slice(LOG_PREFIX.length));
  }

  async settle(): Promise<void> {
    await this.client.waitForRenderIdle();
    await sleep(SETTLE_MS);
  }

  // one key press = one action
  async key(vk: number): Promise<string[]> {
    this.newLines();
    postMessage(this.frame, WM_KEYDOWN, vk, 0);
    postMessage(this.frame, WM_KEYUP, vk, 0);
    await this.settle();
    return this.newLines();
  }

  // 'b' goes through WM_CHAR, not an accelerator
  async char(c: string): Promise<string[]> {
    this.newLines();
    postMessage(this.frame, WM_CHAR, c.charCodeAt(0), 0);
    await this.settle();
    return this.newLines();
  }

  async cmd(name: string): Promise<string[]> {
    this.newLines();
    sendCommandSync(this.frame, cmdId(name));
    await this.settle();
    return this.newLines();
  }
}

const rows: Row[] = [];

// exactly one line, starting with `start` and containing every `has`
function expectOne(name: string, lines: string[], start: string, has: string[] = []): void {
  const expect = [start, ...has].join(" ... ");
  const ok = lines.length === 1 && lines[0].startsWith(start) && has.every((h) => lines[0].includes(h));
  rows.push({ name, expect, got: lines.join(" | ") || "(no line)", ok });
}

function expectNoTurn(name: string, lines: string[]): void {
  const ok = lines.every((l) => l.startsWith("nothing"));
  rows.push({ name, expect: "no Curl / Bundle", got: lines.join(" | ") || "(no line)", ok });
}

function expectSilent(name: string, lines: string[]): void {
  rows.push({ name, expect: "(no line)", got: lines.join(" | ") || "(no line)", ok: lines.length === 0 });
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
  if (!existsSync(BOOK40)) {
    throw new Error(`test shelf missing: ${BOOK40}`);
  }
  const appdata = tmpPath("skeuomorph-detect-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  const logPath = tmpPath("skeuomorph-detect-log.txt");
  writeFileSync(logPath, "");

  // opens on page 5 in Single Page: the jump while opening must not curl
  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", "single page", "-zoom", "fit page"];
  const { proc, client, frame } = await launchControlled([...args, "-page", "5", BOOK40]);
  try {
    const s = new Session(client, frame, logPath);
    await s.settle();
    const opening = s.newLines();
    expectNoTurn("opening on page 5", opening);
    expectOne("opening logs why", opening.slice(-1), "nothing (");

    expectOne("Single: Page Down", await s.key(VK_NEXT), "Curl forward", ["sweep left", "[5-5 -> 6-6]"]);
    expectOne("Single: End", await s.key(VK_END), "Bundle 18 px forward", ["34 pages", "sweep left"]);
    expectOne("Single: Home", await s.key(VK_HOME), "Bundle 18 px backward", ["39 pages", "sweep right"]);

    expectOne("Single -> Facing", await s.cmd("CmdFacingView"), "nothing (display mode switch)");
    expectOne("Facing: Page Down", await s.key(VK_NEXT), "Curl forward", ["2 pages", "[1-2 -> 3-4]"]);
    // Book View, then next page: two GoToPage calls, one action
    expectOne("Facing: B key", await s.char("b"), "nothing (display mode switch)", ["[3-4 -> 4-5]"]);
    await s.cmd("CmdFacingView");

    await s.cmd("CmdToggleFacingStepOne");
    expectOne("step one page: Page Down", await s.key(VK_NEXT), "nothing (step one page)");
    await s.cmd("CmdToggleFacingStepOne");

    await s.cmd("CmdGoToFirstPage");
    await s.cmd("CmdToggleMangaMode");
    expectOne("Manga Mode: Page Down", await s.key(VK_NEXT), "Curl forward", ["sweep right", "[1-2 -> 3-4]"]);
    await s.cmd("CmdToggleMangaMode");

    await s.cmd("CmdToggleSkeuomorphPageTurn");
    expectOne("switch off: Page Down", await s.key(VK_NEXT), "nothing (switch off)");
    await s.cmd("CmdToggleSkeuomorphPageTurn");

    await s.cmd("CmdToggleContinuousView");
    expectSilent("Continuous: End", await s.key(VK_END));
    expectSilent("Continuous: Home", await s.key(VK_HOME));
    await s.cmd("CmdToggleContinuousView");
    expectOne("back to Facing: End", await s.key(VK_END), "Bundle 18 px forward", ["sweep left"]);

    await client.quit();
    await proc.exited;
  } finally {
    await killAndWait(proc);
  }

  printTable();
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-detect: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-detect: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-detect", testit);
  } catch (e) {
    console.error(`
${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

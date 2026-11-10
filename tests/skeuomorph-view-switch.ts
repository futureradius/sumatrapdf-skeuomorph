// Skeuomorph Page Turn, build step 3: switching between Paged Views puts a
// lone page where that view puts it, at once (docs/skeuomorph-page-turn-spec.md,
// sections 3.6 and 3.7). With the switch on, Book View's lone cover sits right
// of the spine and a lone last page left of it; Single Page centres them.
//
// For each case: open a test-shelf book in one view on a page, switch to
// another view by command, and compare where the shown pages sit (layout
// position and on-screen rect) with a fresh launch straight into that view on
// the same page. They must match exactly.
//
// Uses the numbered books from the test shelf (C:\code\test-shelf), read only.
// Settings live in throwaway -appdata folders.
//
// Build the debug exe first, then: bun tests/skeuomorph-view-switch.ts [-exe <path>]

import { existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, type ControlClient } from "./control.ts";
import { cmdId, prepareTestEnvironment, runTest, tmpPath } from "./util.ts";
import { killAndWait, launchControlled, sendCommandSync } from "./win-automation.ts";
import { sleep } from "./winapi.ts";

const SHELF = "C:\\code\\test-shelf\\generated";
const book = (n: string) => `${SHELF}\\numbered-${n}.pdf`;

type Case = { doc: string; from: string; page: number; cmd: string; to: string };
const CASES: Case[] = [
  // the cover: same pages shown before and after, so nothing else re-lays out
  { doc: "040-pages", from: "book view", page: 1, cmd: "CmdSinglePageView", to: "single page" },
  { doc: "040-pages", from: "single page", page: 1, cmd: "CmdBookView", to: "book view" },
  { doc: "040-pages", from: "book view", page: 1, cmd: "CmdFacingView", to: "facing" },
  { doc: "040-pages", from: "facing", page: 1, cmd: "CmdBookView", to: "book view" },
  // a lone last page: Book View of 2 pages, Facing of 3
  { doc: "002-pages", from: "single page", page: 2, cmd: "CmdBookView", to: "book view" },
  { doc: "002-pages", from: "book view", page: 2, cmd: "CmdSinglePageView", to: "single page" },
  { doc: "003-pages", from: "single page", page: 3, cmd: "CmdFacingView", to: "facing" },
  { doc: "003-pages", from: "facing", page: 3, cmd: "CmdSinglePageView", to: "single page" },
  // a one-page book
  { doc: "001-page", from: "single page", page: 1, cmd: "CmdBookView", to: "book view" },
  { doc: "001-page", from: "book view", page: 1, cmd: "CmdSinglePageView", to: "single page" },
];

const SETTINGS = [
  "Theme = Light",
  "CheckForUpdates = false",
  "RememberOpenedFiles = false",
  "RememberStatePerDocument = false",
  "RestoreSession = false",
  "ShowToc = false",
  "ShowStartPage = false",
  "SmoothScroll = false",
  "SkeuomorphPageTurn = true",
  "",
].join("\r\n");

let launchNo = 0;

// "n=1 pos=x,y,dx,dy screen=x,y,dx,dy" for every shown page (TestLayout lists pages 1-8)
async function shownPages(client: ControlClient): Promise<string> {
  const res = await client.request(ControlCommand.TestLayout, ["get"]);
  const raw = String(res[1] ?? "");
  const pages = raw
    .split("\n")
    .filter((l) => l.startsWith("page n=") && l.includes(" shown=1 "))
    .map((l) => l.replace(/^page /, "").replace(" shown=1", "").trim());
  return pages.join(" ; ") || "(no page shown)";
}

async function layoutAfter(doc: string, view: string, page: number, cmd?: string): Promise<string> {
  const appdata = tmpPath(`skeuomorph-view-switch-${++launchNo}`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);
  const args = ["-appdata", appdata, "-view", view, "-zoom", "fit page", "-page", String(page), doc];
  const { proc, client, frame } = await launchControlled(args);
  try {
    await client.waitForRenderIdle();
    if (cmd) {
      sendCommandSync(frame, cmdId(cmd));
      await client.waitForRenderIdle();
      await sleep(100);
    }
    const got = await shownPages(client);
    await client.quit();
    await proc.exited;
    return got;
  } finally {
    await killAndWait(proc);
  }
}

export async function testit(): Promise<void> {
  for (const c of CASES) {
    if (!existsSync(book(c.doc))) {
      throw new Error(`test shelf missing: ${book(c.doc)} (run bun tests/skeuomorph-test-shelf.ts)`);
    }
  }
  const rows: { name: string; ok: boolean; want: string; got: string }[] = [];
  for (const c of CASES) {
    const name = `${c.doc} p${c.page}: ${c.from} -> ${c.to}`;
    const want = await layoutAfter(book(c.doc), c.to, c.page);
    const got = await layoutAfter(book(c.doc), c.from, c.page, c.cmd);
    rows.push({ name, ok: got === want, want, got });
  }

  const w = Math.max(...rows.map((r) => r.name.length));
  for (const r of rows) {
    console.log(`${r.ok ? "PASS" : "FAIL"}  ${r.name.padEnd(w)}  ${r.got}`);
    if (!r.ok) {
      console.log(`      ${"".padEnd(w)}  want: ${r.want}`);
    }
  }
  const failed = rows.filter((r) => !r.ok).length;
  if (failed > 0) {
    throw new Error(`skeuomorph-view-switch: ${failed} of ${rows.length} failed`);
  }
  console.log(`skeuomorph-view-switch: all ${rows.length} passed`);
}

// no build here: cmd/build.ts wants VS 2026, this fork builds with MSBuild
// (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-view-switch", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

// Skeuomorph Page Turn, build step 3: pixel comparison against the build from
// before (docs/skeuomorph-page-turn-spec.md, section 5.7 "Pixel comparison").
//
// Opens the same test-shelf documents in two builds, in the same window, the
// same views and on the same pages, screenshots the document area (the canvas
// window's client area) and compares the two pictures pixel for pixel:
// - every view with the switch (SkeuomorphPageTurn) off, including Presentation
//   and Manga Mode;
// - the Continuous views with the switch on, also after a trip through a Paged
//   View and after turning the switch on while in a Continuous View.
// All of them must match exactly.
//
// Run (PowerShell):
//   bun tests/skeuomorph-pixels.ts [beforeDir] [afterDir] [options]
//     beforeDir          default out\dbg64-before-step3
//     afterDir           default out\dbg64
//     --only <regex>     run only the groups whose name matches
//     --no-presentation  skip the Presentation groups (they cover the whole screen)
//     --out <dir>        where PNGs go (default: a new folder in %TEMP%)
//     --save-all         also save before / after PNGs of the cases that match
//     --after-margin "T R B L"
//                        self-test only: give the "after" side this PDF window
//                        margin (default "2 4 2 4"); "8 4 8 4" shrinks the
//                        pages a little, to prove mismatches are caught
//
// The builds are never run in place: SumatraPDF.exe, libsumatrapdf.dll and
// their .pdb files are copied to a temp folder first (an exe running inside
// out\dbg64 would lock it against the next build). Each launch gets a fresh
// throwaway -appdata folder with a settings file written here, -for-testing
// (new instance, no session restore, settings never saved) and -dbg-control.
//
// Waiting for rendering: the -dbg-control WaitRenderIdle command (the visible
// pages are rendered at full resolution), then screenshots every 120 ms until
// three in a row are identical.
//
// Commands and page jumps go through the app's DDE-over-WM_COPYDATA interface
// by name ("[CmdFacingView]", "[GotoPage(...)]"), so each build resolves the
// command names with its own command table.

import { copyFileSync, existsSync, mkdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { deflateSync } from "node:zlib";
import { ControlCommand, withControlledSumatra, type ControlClient } from "./control.ts";
import { pngChunk, ROOT } from "./util.ts";
import { findCanvas, waitForFrame } from "./win-automation.ts";
import { captureWindowPixels, getWorkArea, killAndWait, sendCopyDataW, setProcessDpiAware, sleep } from "./winapi.ts";

const SHELF = "C:\\code\\test-shelf";
const DOCS: { key: string; path: string }[] = [
  { key: "1-page", path: `${SHELF}\\generated\\numbered-001-page.pdf` },
  { key: "3-pages", path: `${SHELF}\\generated\\numbered-003-pages.pdf` },
  { key: "40-pages", path: `${SHELF}\\generated\\numbered-040-pages.pdf` },
  { key: "mixed-sizes", path: `${SHELF}\\generated\\numbered-mixed-sizes.pdf` },
  { key: "comic-rtl", path: `${SHELF}\\downloads\\bakemono-shiuchi-hyobanki-rtl.cbz` },
];
const BOOK40 = DOCS[2]!.path;

// WM_COPYDATA id SumatraPDF takes DDE command strings on ("DdeW")
const COPYDATA_DDE = 0x44646557;
const STABLE_INTERVAL_MS = 120;
const STABLE_NEEDED = 3; // identical captures in a row
const STABLE_TIMEOUT_MS = 20_000;
// window size in physical pixels, the same for both builds and every run
// (smaller when the screen is)
const WINDOW_DX = 1280;
const WINDOW_DY = 1000;

// ---------- the matrix ----------

// a step runs its commands, then visits the first, middle and last page
type Step = { label: string; cmds: string[] };
type Group = {
  name: string;
  doc: string;
  switchOn: boolean;
  launchView: string;
  presentation?: boolean;
  steps: Step[];
};

const shot = (label: string, ...cmds: string[]): Step => ({ label, cmds });

function buildGroups(withPresentation: boolean): Group[] {
  const groups: Group[] = [];
  for (const d of DOCS) {
    groups.push({
      name: `off ${d.key}`,
      doc: d.path,
      switchOn: false,
      launchView: "single page",
      steps: [
        shot("Single Page", "CmdSinglePageView"),
        shot("Facing", "CmdFacingView"),
        shot("Book View", "CmdBookView"),
        // keeps Book View, adds Continuous
        shot("Continuous Book View", "CmdToggleContinuousView"),
        shot("Continuous Facing", "CmdFacingView"),
        shot("Continuous", "CmdSinglePageView"),
      ],
    });
    if (withPresentation) {
      groups.push({
        name: `off presentation ${d.key}`,
        doc: d.path,
        switchOn: false,
        launchView: "single page",
        presentation: true,
        steps: [
          shot("Presentation", "CmdSinglePageView"),
          shot("Presentation Facing", "CmdFacingView"),
          shot("Presentation Book View", "CmdBookView"),
        ],
      });
    }
    groups.push({
      name: `on ${d.key}`,
      doc: d.path,
      switchOn: true,
      launchView: "continuous",
      steps: [
        shot("Continuous"),
        shot("Continuous Facing", "CmdFacingView"),
        shot("Continuous Book View", "CmdBookView"),
        // to Paged Book View (where step 3 adds room) and back
        shot("Continuous Book View, after Paged and back", "CmdToggleContinuousView", "CmdToggleContinuousView"),
        shot(
          "Continuous, after Paged and back",
          "CmdSinglePageView",
          "CmdToggleContinuousView",
          "CmdToggleContinuousView",
        ),
      ],
    });
  }
  // Manga Mode flips the 40-page book to right-to-left and the comic to the
  // other direction from the one it states
  for (const d of [DOCS[2]!, DOCS[4]!]) {
    groups.push({
      name: `off manga ${d.key}`,
      doc: d.path,
      switchOn: false,
      launchView: "facing",
      steps: [
        shot("Facing, Manga Mode toggled", "CmdToggleMangaMode"),
        shot("Book View, Manga Mode toggled", "CmdBookView"),
        shot("Continuous Book View, Manga Mode toggled", "CmdToggleContinuousView"),
      ],
    });
    groups.push({
      name: `on manga ${d.key}`,
      doc: d.path,
      switchOn: true,
      launchView: "continuous facing",
      steps: [
        shot("Continuous Facing, Manga Mode toggled", "CmdToggleMangaMode"),
        shot("Continuous Book View, Manga Mode toggled", "CmdBookView"),
      ],
    });
  }
  groups.push({
    name: "on from paged 40-pages",
    doc: BOOK40,
    switchOn: true,
    launchView: "facing",
    steps: [shot("Facing -> Continuous Facing", "CmdToggleContinuousView")],
  });
  groups.push({
    name: "switch turned on 40-pages",
    doc: BOOK40,
    switchOn: false,
    launchView: "continuous facing",
    steps: [shot("Continuous Facing, switch turned on", "CmdToggleSkeuomorphPageTurn")],
  });
  return groups;
}

// ---------- settings and exe copies ----------

function settingsText(switchOn: boolean, margin: string): string {
  const fixedPageUI = margin ? ["FixedPageUI [", `\tWindowMargin = ${margin}`, "]"] : [];
  return [
    "Theme = Light",
    "CheckForUpdates = false",
    "RememberOpenedFiles = false",
    "RememberStatePerDocument = false",
    "RestoreSession = false",
    "ReuseInstance = false",
    "ShowToc = false",
    "ShowStartPage = false",
    "ShowFileNavigateHint = false",
    "ShowAnnotationNotification = false",
    "SmoothScroll = false",
    "Scrollbars = windows",
    "DefaultDisplayMode = single page",
    "DefaultZoom = fit page",
    `SkeuomorphPageTurn = ${switchOn}`,
    "ComicBookUI [",
    // applies to PDFs too; Manga Mode is toggled by command in its own groups
    "\tCbxMangaMode = false",
    "]",
    ...fixedPageUI,
    "",
  ].join("\r\n");
}

const BUILD_FILES = ["SumatraPDF.exe", "SumatraPDF.pdb", "libsumatrapdf.dll", "libsumatrapdf.pdb"];

function copyBuild(srcDir: string, dstDir: string): string {
  if (!existsSync(join(srcDir, "SumatraPDF.exe"))) {
    throw new Error(`no SumatraPDF.exe in ${srcDir}`);
  }
  mkdirSync(dstDir, { recursive: true });
  for (const f of BUILD_FILES) {
    const src = join(srcDir, f);
    if (existsSync(src)) {
      copyFileSync(src, join(dstDir, f));
    }
  }
  return join(dstDir, "SumatraPDF.exe");
}

// ---------- capture ----------

// top-right of the work area; needs setProcessDpiAware() so sizes are physical
function windowPos(): { x: number; y: number; dx: number; dy: number } {
  const wa = getWorkArea();
  const dx = Math.min(WINDOW_DX, Math.floor((wa.right - wa.left) / 2));
  const dy = Math.min(WINDOW_DY, wa.bottom - wa.top);
  return { x: wa.right - dx, y: wa.top, dx, dy };
}

type Pic = { w: number; h: number; data: Uint8Array }; // BGRA, top-down

function samePic(a: Pic, b: Pic): boolean {
  return a.w === b.w && a.h === b.h && diffCount(a, b) === 0;
}

// differing pixels, alpha ignored (PrintWindow leaves it undefined)
function diffCount(a: Pic, b: Pic): number {
  if (a.w !== b.w || a.h !== b.h) {
    return Math.max(a.w * a.h, b.w * b.h);
  }
  const d1 = a.data;
  const d2 = b.data;
  let n = 0;
  for (let i = 0; i < d1.length; i += 4) {
    if (d1[i] !== d2[i] || d1[i + 1] !== d2[i + 1] || d1[i + 2] !== d2[i + 2]) {
      n++;
    }
  }
  return n;
}

function capture(canvas: number): Pic {
  const cap = captureWindowPixels(canvas, "client");
  if (!cap) {
    throw new Error("could not capture the canvas");
  }
  return cap;
}

async function captureStable(client: ControlClient, canvas: number): Promise<Pic> {
  const deadline = Date.now() + STABLE_TIMEOUT_MS;
  await client.waitForRenderIdle();
  let prev = capture(canvas);
  let same = 1;
  while (Date.now() < deadline) {
    await sleep(STABLE_INTERVAL_MS);
    const cur = capture(canvas);
    if (samePic(prev, cur)) {
      same++;
      if (same >= STABLE_NEEDED) {
        return cur;
      }
    } else {
      same = 1;
      await client.waitForRenderIdle();
    }
    prev = cur;
  }
  throw new Error(`the canvas did not stop changing within ${STABLE_TIMEOUT_MS / 1000}s`);
}

// ---------- PNG output ----------

function writePng(path: string, w: number, h: number, rgba: Uint8Array): void {
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) {
    raw[y * (w * 4 + 1)] = 0;
    Buffer.from(rgba.buffer, rgba.byteOffset + y * w * 4, w * 4).copy(raw, y * (w * 4 + 1) + 1);
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 6; // RGBA
  writeFileSync(
    path,
    Buffer.concat([
      Buffer.from("89504e470d0a1a0a", "hex"),
      pngChunk("IHDR", ihdr),
      pngChunk("IDAT", deflateSync(raw)),
      pngChunk("IEND", Buffer.alloc(0)),
    ]),
  );
}

function picToRgba(p: Pic): Uint8Array {
  const out = new Uint8Array(p.w * p.h * 4);
  for (let i = 0; i < out.length; i += 4) {
    out[i] = p.data[i + 2]!;
    out[i + 1] = p.data[i + 1]!;
    out[i + 2] = p.data[i]!;
    out[i + 3] = 255;
  }
  return out;
}

// same pixels faded to light grey, differing ones red, outside one of the two
// pictures magenta
function diffRgba(a: Pic, b: Pic): { w: number; h: number; rgba: Uint8Array } {
  const w = Math.max(a.w, b.w);
  const h = Math.max(a.h, b.h);
  const out = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      out[o + 3] = 255;
      if (x >= a.w || y >= a.h || x >= b.w || y >= b.h) {
        out[o] = 255;
        out[o + 1] = 0;
        out[o + 2] = 255;
        continue;
      }
      const ia = (y * a.w + x) * 4;
      const ib = (y * b.w + x) * 4;
      const same = a.data[ia] === b.data[ib] && a.data[ia + 1] === b.data[ib + 1] && a.data[ia + 2] === b.data[ib + 2];
      if (same) {
        const lum = (a.data[ia]! * 29 + a.data[ia + 1]! * 150 + a.data[ia + 2]! * 77) >> 8;
        const v = 200 + (lum * 55) / 255;
        out[o] = v;
        out[o + 1] = v;
        out[o + 2] = v;
      } else {
        out[o] = 255;
        out[o + 1] = 0;
        out[o + 2] = 0;
      }
    }
  }
  return { w, h, rgba: out };
}

// ---------- running one side ----------

type Shot = { name: string; page: number; pic: Pic };

const liveProcs = new Set<Bun.Subprocess>();

function dde(frame: number, cmd: string): void {
  const res = sendCopyDataW(frame, COPYDATA_DDE, cmd);
  if (!res) {
    throw new Error(`SumatraPDF did not take the DDE command ${cmd}`);
  }
}

function resultText(res: unknown[]): { code: number; text: string } {
  return { code: typeof res[0] === "number" ? res[0] : -1, text: String(res[1] ?? "") };
}

async function currentPage(client: ControlClient): Promise<number> {
  const r = resultText(await client.request(ControlCommand.TestCurrentTab));
  const m = /page=(\d+)/.exec(r.text);
  return m ? parseInt(m[1]!, 10) : -1;
}

async function pageCount(client: ControlClient): Promise<number> {
  const r = resultText(await client.request(ControlCommand.TestPageInfo));
  const m = /pages=(\d+)/.exec(r.text);
  if (r.code !== 0 || !m) {
    throw new Error(`TestPageInfo: ${r.text || r.code}`);
  }
  return parseInt(m[1]!, 10);
}

let launchNo = 0;

async function runSide(exe: string, workDir: string, side: string, g: Group, margin: string): Promise<Shot[]> {
  const appdata = join(workDir, `appdata-${++launchNo}-${side}`);
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), settingsText(g.switchOn, margin));
  const p = windowPos();
  const args = [
    "-appdata",
    appdata,
    "-log-to-file",
    join(appdata, "log.txt"),
    "-window-pos",
    `${p.dx}x${p.dy}@${p.x}x${p.y}`,
    "-view",
    g.launchView,
    "-zoom",
    "fit page",
    ...(g.presentation ? ["-presentation"] : []),
    g.doc,
  ];
  return withControlledSumatra(
    exe,
    async (client, proc) => {
      liveProcs.add(proc);
      try {
        const frame = await waitForFrame(proc.pid!);
        if (!frame) {
          throw new Error("SumatraPDF main window did not appear");
        }
        await client.setNotificationsEnabled(false);
        await client.waitForRenderIdle();
        const canvas = findCanvas(frame);
        if (!canvas) {
          throw new Error("no canvas window");
        }
        const n = await pageCount(client);
        const pages = [...new Set([1, Math.ceil(n / 2), n])];
        const shots: Shot[] = [];
        for (const step of g.steps) {
          for (const c of step.cmds) {
            dde(frame, `[${c}]`);
            await client.waitForRenderIdle();
          }
          for (const pg of pages) {
            dde(frame, `[GotoPage("${g.doc}",${pg})]`);
            const pic = await captureStable(client, canvas);
            const label = pg === 1 ? "first" : pg === n ? "last" : "middle";
            shots.push({ name: `${g.name} | ${step.label} | ${label} p${pg}`, page: await currentPage(client), pic });
          }
        }
        return shots;
      } finally {
        liveProcs.delete(proc);
      }
    },
    args,
  );
}

// ---------- main ----------

type Row = { name: string; size: string; pages: string; diff: number; ok: boolean; note: string };

function parseArgs(argv: string[]) {
  const pos: string[] = [];
  const opts = { only: "", presentation: true, out: "", afterMargin: "", saveAll: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i]!;
    if (a === "--only") {
      opts.only = argv[++i] ?? "";
    } else if (a === "--save-all") {
      opts.saveAll = true;
    } else if (a === "--no-presentation") {
      opts.presentation = false;
    } else if (a === "--out") {
      opts.out = argv[++i] ?? "";
    } else if (a === "--after-margin") {
      opts.afterMargin = argv[++i] ?? "";
    } else if (a.startsWith("--")) {
      throw new Error(`unknown option ${a}`);
    } else {
      pos.push(a);
    }
  }
  const before = resolve(ROOT, pos[0] ?? join("out", "dbg64-before-step3"));
  const after = resolve(ROOT, pos[1] ?? join("out", "dbg64"));
  return { before, after, ...opts };
}

function safeName(s: string): string {
  return s.replace(/[^A-Za-z0-9-]+/g, "_").replace(/^_|_$/g, "");
}

function printTable(rows: Row[]): void {
  const w0 = Math.max(4, ...rows.map((r) => r.name.length));
  const w1 = Math.max(4, ...rows.map((r) => r.size.length));
  console.log(`      ${"case".padEnd(w0)}  ${"size".padEnd(w1)}  pages    differing px`);
  for (const r of rows) {
    const mark = r.ok ? "PASS" : "FAIL";
    const note = r.note ? `  ${r.note}` : "";
    console.log(`${mark}  ${r.name.padEnd(w0)}  ${r.size.padEnd(w1)}  ${r.pages.padEnd(7)}  ${r.diff}${note}`);
  }
}

async function main(): Promise<number> {
  const o = parseArgs(process.argv.slice(2));
  // without it Windows hands this process scaled-down captures and sizes
  setProcessDpiAware();
  for (const d of DOCS) {
    if (!existsSync(d.path)) {
      throw new Error(`test shelf file missing: ${d.path} (run bun tests/skeuomorph-test-shelf.ts)`);
    }
  }
  let groups = buildGroups(o.presentation);
  if (o.only) {
    const re = new RegExp(o.only, "i");
    groups = groups.filter((g) => re.test(g.name));
  }
  if (groups.length === 0) {
    throw new Error("no groups selected");
  }

  const stamp = new Date().toISOString().replace(/[-:]/g, "").replace(/\..*/, "").replace("T", "-");
  const workDir = join(tmpdir(), "skeuomorph-pixels", stamp);
  const outDir = o.out ? resolve(o.out) : join(workDir, "pictures");
  mkdirSync(workDir, { recursive: true });
  console.log(`before: ${o.before}`);
  console.log(`after:  ${o.after}`);
  if (o.afterMargin) {
    console.log(`after side: WindowMargin = ${o.afterMargin} (self-test: expect mismatches)`);
  }
  const exeBefore = copyBuild(o.before, join(workDir, "exe-before"));
  const exeAfter = copyBuild(o.after, join(workDir, "exe-after"));

  const rows: Row[] = [];
  const started = Date.now();
  let savedAny = false;
  try {
    for (const g of groups) {
      const t0 = Date.now();
      const a = await runSide(exeBefore, workDir, "before", g, "");
      const b = await runSide(exeAfter, workDir, "after", g, o.afterMargin);
      console.log(`  ${g.name}: ${a.length} screenshots per side, ${((Date.now() - t0) / 1000).toFixed(1)}s`);
      if (a.length !== b.length) {
        throw new Error(`${g.name}: ${a.length} screenshots before, ${b.length} after`);
      }
      for (let i = 0; i < a.length; i++) {
        const sa = a[i]!;
        const sb = b[i]!;
        const n = diffCount(sa.pic, sb.pic);
        const sizeDiff = sa.pic.w !== sb.pic.w || sa.pic.h !== sb.pic.h;
        const row: Row = {
          name: sa.name,
          size: sizeDiff ? `${sa.pic.w}x${sa.pic.h} vs ${sb.pic.w}x${sb.pic.h}` : `${sa.pic.w}x${sa.pic.h}`,
          pages: sa.page === sb.page ? `${sa.page}` : `${sa.page}/${sb.page}`,
          diff: n,
          ok: n === 0 && sa.page === sb.page,
          note: "",
        };
        rows.push(row);
        if (row.ok && !o.saveAll) {
          continue;
        }

        mkdirSync(outDir, { recursive: true });
        const base = join(outDir, safeName(sa.name));
        writePng(`${base}-before.png`, sa.pic.w, sa.pic.h, picToRgba(sa.pic));
        writePng(`${base}-after.png`, sb.pic.w, sb.pic.h, picToRgba(sb.pic));
        savedAny = true;
        if (row.ok) {
          continue;
        }
        const d = diffRgba(sa.pic, sb.pic);
        writePng(`${base}-diff.png`, d.w, d.h, d.rgba);
        row.note = sa.page !== sb.page ? "(landed on different pages)" : "";
      }
    }
  } finally {
    for (const p of liveProcs) {
      await killAndWait(p);
    }
    for (const dir of ["exe-before", "exe-after"]) {
      try {
        rmSync(join(workDir, dir), { recursive: true, force: true });
      } catch {
        console.error(`could not remove ${join(workDir, dir)}`);
      }
    }
  }

  console.log("");
  printTable(rows);
  const failed = rows.filter((r) => !r.ok).length;
  console.log("");
  console.log(`${rows.length} screenshots compared in ${((Date.now() - started) / 1000).toFixed(0)}s`);
  console.log(`logs and settings of every launch: ${workDir}`);
  if (savedAny) {
    console.log(`pictures (before / after, plus diff on a mismatch, red = different): ${outDir}`);
  }
  if (failed > 0) {
    console.log(`skeuomorph-pixels: ${failed} of ${rows.length} differ`);
    return 1;
  }
  console.log(`skeuomorph-pixels: all ${rows.length} match pixel for pixel`);
  return 0;
}

if (import.meta.main) {
  const onSignal = async () => {
    for (const p of liveProcs) {
      await killAndWait(p);
    }
    process.exit(130);
  };
  process.on("SIGINT", onSignal);
  process.on("SIGTERM", onSignal);
  try {
    process.exit(await main());
  } catch (e) {
    for (const p of liveProcs) {
      await killAndWait(p);
    }
    console.error(`\nskeuomorph-pixels: ${(e as Error)?.message ?? e}`);
    process.exit(2);
  }
}

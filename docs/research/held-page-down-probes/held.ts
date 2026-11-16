// probe: does SumatraPDF's own picture (GDI, under the Curl layer) change while Page Down is held?
// bun held.ts <view> <mode: 33|flood|busy33> [holdMs] [repeatMs]; env MAX=1 maximizes, SHOTS=1 screenshots
import { mkdirSync, readFileSync, rmSync, writeFileSync, existsSync } from "node:fs";
import { join } from "node:path";
const WT = "C:/code/sumatrapdf-skeuomorph/.claude/worktrees/agent-aaf5f9c9f6407148c/tests";
process.env.SUMATRA_TEST_EXE =
  process.env.SUMATRA_TEST_EXE ?? "C:\\code\\sumatrapdf-skeuomorph\\out\\dbg64\\SumatraPDF.exe";
const wa = await import(WT + "/win-automation.ts");
const w = await import(WT + "/winapi.ts");
w.setProcessDpiAware();

const view = process.argv[2] ?? "single page";
const mode = process.argv[3] ?? "33";
const HOLD_MS = +(process.argv[4] ?? 2500);
const REPEAT_MS = +(process.argv[5] ?? 33);
const BOOK = process.env.BOOK ?? "C:\\code\\test-shelf\\generated\\numbered-800-pages.pdf";
const tag = process.env.TAG ?? "";
const dir = join(import.meta.dir, "run-" + view.replace(/ /g, "") + "-" + mode + tag);
rmSync(dir, { recursive: true, force: true });
mkdirSync(dir, { recursive: true });
const appdata = join(dir, "appdata");
mkdirSync(appdata);
writeFileSync(
  join(appdata, "SumatraPDF-settings.txt"),
  [
    "Theme = Light",
    "CheckForUpdates = false",
    "RememberOpenedFiles = false",
    "RestoreSession = false",
    "ShowToc = false",
    "ShowStartPage = false",
    "SmoothScroll = false",
    "SkeuomorphPageTurn = true",
    "PageTurnDuration = 700",
    "",
  ].join("\r\n"),
);
const logPath = join(dir, "log.txt");
const { proc, client, frame } = await wa.launchControlled([
  "-appdata",
  appdata,
  "-log-to-file",
  logPath,
  "-view",
  view,
  "-zoom",
  "fit page",
  BOOK,
]);
await client.waitForRenderIdle();
if (process.env.MAX) {
  w.postMessage(frame, 0x0112, 0xf030, 0);
  await w.sleep(800);
  await client.waitForRenderIdle();
}
await w.sleep(1500);
const canvas = wa.findCanvas(frame);
const rc = w.getClientRect(canvas);
const cw = rc.right - rc.left;
const ch = rc.bottom - rc.top;
console.log("canvas", cw, ch, "exe", process.env.SUMATRA_TEST_EXE);

function sig(): string {
  // a band through the page middle of SumatraPDF's own (GDI) picture
  let h = 0;
  const px = w.captureWindowDCRegionPixels(canvas, 0, Math.floor(ch * 0.3), cw, Math.floor(ch * 0.4));
  if (!px) return "none";
  for (let i = 0; i < px.length; i += 12) h = (Math.imul(h, 31) + px[i]) | 0;
  return (h >>> 0).toString(16);
}

const t0 = performance.now();
const events: string[] = [];
const ev = (s: string) => events.push(`${(performance.now() - t0).toFixed(1)} ${s}`);
let last = sig();
ev("sig " + last + " (start)");
let samples = 0;
const shotAt = process.env.SHOTS ? [700, 1500, 2300, 3000] : [];
const poster = Bun.spawn(
  ["bun", join(import.meta.dir, "poster.ts"), String(frame), mode, String(HOLD_MS), String(REPEAT_MS)],
  { stdout: "pipe" },
);
ev("poster started (keydown)");
const posterOut = new Response(poster.stdout).text();
let posterDone = false;
poster.exited.then(() => {
  posterDone = true;
  ev("poster done (keyup)");
});
let after = 0;
while (true) {
  const s = sig();
  samples++;
  if (s !== last) {
    ev("sig " + s);
    last = s;
  }
  const tNow = performance.now() - t0;
  if (shotAt.length && tNow >= shotAt[0]) {
    const at = shotAt.shift()!;
    w.captureScreenRect(w.getWindowRect(canvas), join(dir, `screen-${at}.png`));
    ev(`screen-${at}.png`);
  }
  if (posterDone) {
    if (!after) after = performance.now();
    if (performance.now() - after > 2500) break;
  }
  await w.sleep(1);
}
writeFileSync(join(dir, "events.txt"), events.join("\n") + "\n");
await client.quit().catch(() => {});
await wa.killAndWait(proc);
console.log((await posterOut).trim());
console.log(events.join("\n"));
console.log("samples", samples);
const log = existsSync(logPath) ? readFileSync(logPath, "utf8").split(/\r?\n/) : [];
const turns = log.filter((l) => l.startsWith("skeuomorph: "));
const curls = log.filter((l) => l.startsWith("skeuomorph-curl: "));
const bundles = turns.filter((l) => l.includes("Bundle")).length;
const waits = curls.map((l) => +(/UI waited ([\d.]+)/.exec(l)?.[1] ?? 0));
const meanWait = waits.length ? waits.reduce((a, b) => a + b, 0) / waits.length : 0;
console.log(`turns ${turns.length} (bundles ${bundles}), curls ${curls.length}, mean UI wait ${meanWait.toFixed(1)} ms`);

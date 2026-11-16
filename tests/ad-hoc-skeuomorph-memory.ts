// Skeuomorph Page Turn: is memory returned when the Rough Pages background copy
// closes? (docs/skeuomorph-page-turn-spec.md, section 5.6)
//
// Each cycle: switch on (copy made), hold Page Down, switch off (copy closed),
// Home. Prints the process's private memory after flipping and after closing.
// Printed, not judged. Measure the Release build on an idle PC:
//
//   bun tests/ad-hoc-skeuomorph-memory.ts -exe out/rel64/SumatraPDF.exe [scan|book] [on|off|notoggle] [noflip] [cycles]
//
// off: the switch stays off (SumatraPDF alone). notoggle: on, the copy never
// closes. noflip: the copy is made and closed without drawing.
// Measured 2026-11-16 on the Albertii scan, 20 cycles: the copy's ~250 MB come
// back each time; ~1.8 MB per cycle stays (numbered book: ~0.3 MB). The copy is
// deleted; the rest needs scanned pages drawn, cause unknown.

import { mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { cmdId, runStandalone, tmpPath } from "./util.ts";
import { killAndWait, launchControlled, sendCommand } from "./win-automation.ts";
import { postMessage, sleep, VK_NEXT, WM_KEYDOWN, WM_KEYUP } from "./winapi.ts";

const DOCS: Record<string, string> = {
  scan: "C:\\code\\test-shelf\\Albertii - De re ædificatoria.pdf",
  book: "C:\\code\\test-shelf\\generated\\numbered-800-pages.pdf",
};
const HOLD_MS = 3000;
const SETTLE_MS = 2000;
const CLOSE_WAIT_MS = 2500;
const COPY_WAIT_MS = 1500;
const KEY_REPEAT_MS = 33;
const KEY_REPEAT = 1 << 30;
const VK_HOME = 0x24;
const MB = 1048576;

const argv = process.argv;
const doc = DOCS[argv.find((a) => a in DOCS) ?? "scan"];
const switchOn = !argv.includes("off");
const toggles = switchOn && !argv.includes("notoggle");
const noFlip = argv.includes("noflip");
const cycles = +(argv.find((a) => /^[0-9]+$/.test(a)) ?? 6);

function privateMb(pid: number): number {
  const ps = `(Get-Process -Id ${pid}).PrivateMemorySize64`;
  const out = Bun.spawnSync(["powershell", "-NoProfile", "-Command", ps]).stdout.toString();
  return Math.round(+out.trim() / MB);
}

function key(frame: number, vk: number): void {
  postMessage(frame, WM_KEYDOWN, vk, 0);
  postMessage(frame, WM_KEYUP, vk, 0);
}

async function hold(frame: number, ms: number): Promise<void> {
  postMessage(frame, WM_KEYDOWN, VK_NEXT, 0);
  const end = Date.now() + ms;
  while (Date.now() < end) {
    await sleep(KEY_REPEAT_MS);
    postMessage(frame, WM_KEYDOWN, VK_NEXT, KEY_REPEAT);
  }
  postMessage(frame, WM_KEYUP, VK_NEXT, 0);
}

export async function testit(): Promise<void> {
  const appdata = tmpPath("skeuomorph-memory-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  const settings = [
    "Theme = Light",
    "CheckForUpdates = false",
    "RememberOpenedFiles = false",
    "RestoreSession = false",
    "SmoothScroll = false",
    "DefaultDisplayMode = facing",
    `SkeuomorphPageTurn = ${switchOn}`,
    "",
  ];
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), settings.join("\r\n"));
  const logPath = tmpPath("skeuomorph-memory-log.txt");
  writeFileSync(logPath, "");

  const args = ["-appdata", appdata, "-log-to-file", logPath, "-view", "facing", "-zoom", "fit page", doc];
  const { proc, client, frame } = await launchControlled(args);
  const toggle = cmdId("CmdToggleSkeuomorphPageTurn");
  try {
    await client.setNotificationsEnabled(false);
    await client.waitForRenderIdle();
    await sleep(COPY_WAIT_MS);
    const mode = switchOn ? (toggles ? "cycled" : "on") : "off";
    console.log(`switch ${mode}${noFlip ? ", no flipping" : ""}, ${doc}`);
    console.log(`start: ${privateMb(proc.pid)} MB`);

    for (let i = 1; i <= cycles; i++) {
      if (toggles && i > 1) {
        sendCommand(frame, toggle); // on: copy made
        await sleep(COPY_WAIT_MS);
      }
      if (!noFlip) {
        await hold(frame, HOLD_MS);
      }
      await client.waitForRenderIdle();
      await sleep(SETTLE_MS);
      const flipped = privateMb(proc.pid);

      if (toggles) {
        sendCommand(frame, toggle); // off: copy closed
      }
      await sleep(CLOSE_WAIT_MS);
      const closed = privateMb(proc.pid);
      console.log(`cycle ${i}: flipped ${flipped} MB, closed ${closed} MB`);

      // back to page 1, an instant jump with the switch off
      key(frame, VK_HOME);
      await client.waitForRenderIdle();
    }

    const log = readFileSync(logPath, "utf8").split(/\r?\n/);
    console.log(`background copies made: ${log.filter((l) => l.includes("background copy made")).length}`);
  } finally {
    await killAndWait(proc);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}

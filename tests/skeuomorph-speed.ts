// Skeuomorph Page Turn: Release speed check (docs/skeuomorph-page-turn-spec.md,
// section 1, "Done for a build step" point 7).
//
// The Release build appends one line per Curl to the file named by
// SKEUOMORPH_PERF_LOG:
//   skeuomorph-curl: #3 landed after 702 ms, 42 frames, 0 late, covered 12.8 ms, UI waited 12.1 ms
// A burst of turns at the usual window size (1638x1000) must show no late
// frames; a maximized burst is printed too, not judged.
// Judged only when plugged in: on battery the laptop runs 2-3x slower, so a
// battery run is printed and noted, not judged.
//
// Build Release first, then: bun tests/skeuomorph-speed.ts
// (another exe: bun tests/skeuomorph-speed.ts -exe <path>)

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";

const ROOT = resolve(import.meta.dir, "..");
const REL_EXE = join(ROOT, "out", "rel64", "SumatraPDF.exe");

// util.ts picks the exe at import time: Release unless -exe says otherwise
if (!process.argv.includes("-exe")) {
  process.env.SUMATRA_TEST_EXE = REL_EXE;
}
const { prepareTestEnvironment, runTest, tmpPath } = await import("./util.ts");
const { killAndWait, launchControlled } = await import("./win-automation.ts");
const { postMessage, setForegroundWindow, showWindow, sleep, SW_MAXIMIZE, VK_NEXT, WM_KEYDOWN, WM_KEYUP } =
  await import("./winapi.ts");

const BOOK = "C:\\code\\test-shelf\\generated\\numbered-800-pages.pdf";

const VK_PRIOR = 0x21;
const TURNS_FORWARD = 12;
const TURNS_BACK = 6;
const SETTLE_MS = 400;
const FLIGHT_WAIT_MS = 6000;

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

enum Power {
  Plugged,
  Battery,
  Unknown,
}

// Win32_Battery.BatteryStatus: 1 = on battery, 2 = plugged in
function powerState(): Power {
  const p = Bun.spawnSync(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Battery).BatteryStatus"]);
  const s = p.stdout.toString().trim();
  if (s === "") {
    return Power.Plugged; // no battery: a desktop
  }
  if (s === "1") {
    return Power.Battery;
  }
  return s === "2" ? Power.Plugged : Power.Unknown;
}

const POWER_NAME = ["plugged in", "on battery", "power state unknown"];

// "#3 landed after 702 ms, 42 frames, 0 late, ..."
type Flight = { id: number; landed: boolean; frames: number; late: number; line: string };

function readFlights(path: string): Flight[] {
  const text = existsSync(path) ? readFileSync(path, "utf8") : "";
  const out: Flight[] = [];
  for (const l of text.split(/\r?\n/)) {
    const m = /^skeuomorph-curl: #(\d+) (landed at once|landed|TIMEOUT).* (\d+) frames, (\d+) late/.exec(l);
    if (m) {
      out.push({ id: +m[1], landed: m[2] === "landed", frames: +m[3], late: +m[4], line: l });
    }
  }
  return out;
}

async function waitFlights(path: string, n: number): Promise<void> {
  const deadline = Date.now() + FLIGHT_WAIT_MS;
  while (Date.now() < deadline && readFlights(path).length < n) {
    await sleep(50);
  }
}

type Size = { name: string; args: string[]; maximize: boolean };

// the usual window: the size the no-flash spike measured at (judged);
// maximized: about 4K on the human's screen (printed, not judged; the spike
// saw about one late frame per Curl there)
const USUAL: Size = { name: "1638x1000", args: ["-window-pos", "1638x1000@60x60"], maximize: false };
const MAXIMIZED: Size = { name: "maximized", args: [], maximize: true };

type Burst = { flights: Flight[]; turns: number; frames: number; late: number; notLanded: number };

async function runBurst(size: Size): Promise<Burst> {
  const appdata = tmpPath(`skeuomorph-speed-${size.name}-appdata`);
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);
  const perfLog = tmpPath(`skeuomorph-speed-${size.name}-perf.txt`);
  writeFileSync(perfLog, "");

  const args = [...size.args, "-appdata", appdata, "-view", "facing", "-zoom", "fit page", BOOK];
  const { proc, client, frame } = await launchControlled(args, { env: { SKEUOMORPH_PERF_LOG: perfLog } });
  let turns = 0;
  try {
    if (size.maximize) {
      showWindow(frame, SW_MAXIMIZE);
    }
    setForegroundWindow(frame);
    await client.waitForRenderIdle();
    await sleep(SETTLE_MS);
    const keys = [...Array(TURNS_FORWARD).fill(VK_NEXT), ...Array(TURNS_BACK).fill(VK_PRIOR)];
    for (const vk of keys) {
      postMessage(frame, WM_KEYDOWN, vk, 0);
      postMessage(frame, WM_KEYUP, vk, 0);
      turns++;
      await waitFlights(perfLog, turns);
      await client.waitForRenderIdle();
      await sleep(SETTLE_MS);
    }
  } finally {
    await killAndWait(proc);
  }

  const flights = readFlights(perfLog);
  const frames = flights.reduce((s, f) => s + f.frames, 0);
  const late = flights.reduce((s, f) => s + f.late, 0);
  const notLanded = flights.filter((f) => !f.landed).length;
  console.log(`
${size.name}:`);
  for (const f of flights) {
    console.log(`  ${f.line}`);
  }
  console.log(`  ${flights.length} Curls of ${turns} turns, ${frames} frames, ${late} late`);
  return { flights, turns, frames, late, notLanded };
}

export async function testit(): Promise<void> {
  if (!existsSync(BOOK)) {
    throw new Error(`test shelf missing: ${BOOK} (run bun tests/skeuomorph-test-shelf.ts)`);
  }
  const before = powerState();
  const usual = await runBurst(USUAL);
  const big = await runBurst(MAXIMIZED);
  const after = powerState();
  const power = before === after ? POWER_NAME[before] : `${POWER_NAME[before]}, then ${POWER_NAME[after]}`;
  console.log(`
${power}; maximized (not judged): ${big.late} late of ${big.frames} frames`);

  const fails: string[] = [];
  if (usual.flights.length < usual.turns) {
    fails.push(
      `only ${usual.flights.length} of ${usual.turns} turns logged a Curl (is it a Release build of this fork?)`,
    );
  }
  if (usual.notLanded > 0) {
    fails.push(`${usual.notLanded} Curls did not land normally`);
  }
  if (usual.late > 0) {
    fails.push(`${usual.late} late frames at ${USUAL.name}`);
  }
  if (before !== Power.Plugged || after !== Power.Plugged) {
    console.log("skeuomorph-speed: not judged, plug in and run again (battery runs are only noted)");
    if (fails.length > 0) {
      console.log(`  would have failed: ${fails.join("; ")}`);
    }
    return;
  }
  if (fails.length > 0) {
    throw new Error(`skeuomorph-speed failed: ${fails.join("; ")}`);
  }
  console.log(`skeuomorph-speed: passed, no late frames at ${USUAL.name}`);
}

// no build here: this fork builds with MSBuild (docs/skeuomorph-page-turn-spec.md, section 1)
if (import.meta.main) {
  try {
    prepareTestEnvironment();
    await runTest("skeuomorph-speed", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

// posts a held Page Down to a window, alone in its process so nothing delays it
// bun poster.ts <hwnd> <mode: 33|flood|busy33> <holdMs> <repeatMs>
const WT = "C:/code/sumatrapdf-skeuomorph/.claude/worktrees/agent-aaf5f9c9f6407148c/tests";
const w = await import(WT + "/winapi.ts");
const frame = +process.argv[2];
const mode = process.argv[3];
const holdMs = +process.argv[4];
const repeatMs = +(process.argv[5] ?? 33);
const VK_NEXT = 0x22;
const KEY_REPEAT = 1 << 30;
const t0 = performance.now();
let n = 1;
w.postMessage(frame, w.WM_KEYDOWN, VK_NEXT, 0);
const gaps: number[] = [];
if (mode === "flood") {
  for (let i = 0; i < 40; i++) {
    w.postMessage(frame, w.WM_KEYDOWN, VK_NEXT, KEY_REPEAT);
    n++;
  }
  await Bun.sleep(holdMs);
} else {
  await Bun.sleep(500); // typematic delay
  const end = performance.now() + holdMs;
  let next = performance.now();
  let prev = next;
  while (performance.now() < end) {
    next += repeatMs;
    if (mode === "busy33") {
      while (performance.now() < next) {}
    } else {
      await Bun.sleep(repeatMs);
    }
    const now = performance.now();
    gaps.push(now - prev);
    prev = now;
    w.postMessage(frame, w.WM_KEYDOWN, VK_NEXT, KEY_REPEAT);
    n++;
  }
}
w.postMessage(frame, w.WM_KEYUP, VK_NEXT, 0);
const avg = gaps.length ? gaps.reduce((a, b) => a + b, 0) / gaps.length : 0;
console.log(`POSTER ${n} keydowns in ${(performance.now() - t0).toFixed(0)} ms, mean gap ${avg.toFixed(1)} ms`);

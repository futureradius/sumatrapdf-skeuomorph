// runs held.ts in several setups and prints, per run: paints of SumatraPDF's own
// picture during the hold vs after keyup
import { join } from "node:path";
const H = join(import.meta.dir, "held.ts");
const REL = "C:\\code\\sumatrapdf-skeuomorph\\out\\rel64\\SumatraPDF.exe";
type V = { tag: string; view: string; mode: string; env: Record<string, string>; repeat?: string };
const all: V[] = [
  { tag: "A", view: "facing", mode: "33", env: {} },
  { tag: "A1", view: "single page", mode: "33", env: {} },
  { tag: "B", view: "facing", mode: "busy33", env: { MAX: "1" } },
  { tag: "C", view: "single page", mode: "flood", env: { MAX: "1" } },
  { tag: "D", view: "single page", mode: "busy33", env: { MAX: "1", SUMATRA_TEST_EXE: REL } },
  { tag: "D2", view: "facing", mode: "busy33", env: { MAX: "1", SUMATRA_TEST_EXE: REL } },
  { tag: "E", view: "single page", mode: "busy33", env: { MAX: "1" }, repeat: "70" },
  { tag: "F", view: "facing", mode: "busy33", env: {} },
];
const only = process.argv.slice(2);
for (const v of all) {
  if (only.length && !only.includes(v.tag)) continue;
  const p = Bun.spawn(["bun", H, v.view, v.mode, "2500", v.repeat ?? "33"], {
    env: { ...process.env, TAG: v.tag, ...v.env },
    stdout: "pipe",
    stderr: "pipe",
  });
  const out = await new Response(p.stdout).text();
  await p.exited;
  const lines = out.split(/\r?\n/);
  const t = (re: RegExp) => lines.filter((l) => re.test(l));
  const start = +(t(/poster started/)[0]?.split(" ")[0] ?? 0);
  const end = +(t(/poster done/)[0]?.split(" ")[0] ?? 0);
  const sigs = t(/^\d+\.\d+ sig /)
    .slice(1)
    .map((l) => +l.split(" ")[0]);
  const during = sigs.filter((x) => x > start + 300 && x < end).length; // after the first key's own repaint
  const firstAfter = sigs.find((x) => x >= end);
  console.log(
    `${v.tag} ${v.view} ${v.mode}${v.repeat ? "/" + v.repeat : ""} ${JSON.stringify(v.env.MAX ? "max" : "quarter")} ${v.env.SUMATRA_TEST_EXE ? "rel" : "dbg"}: ` +
      `${t(/^canvas/)[0]?.split(" ").slice(1, 3).join("x")} | ${t(/POSTER/)[0]} | picture changes during hold ${during}, first after keyup +${firstAfter !== undefined ? (firstAfter - end).toFixed(0) : "?"} ms | ${t(/^turns/)[0] ?? ""}`,
  );
}

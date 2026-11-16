// Skeuomorph Page Turn: the hooks in SumatraPDF's own files are all there,
// as docs/skeuomorph-hooks.md lists them. Run it after merging upstream.
//
// 1. Every code block in that file, fenced as "```cpp src/Canvas.cpp", is in
//    the named file: each of its lines, ignoring indentation.
// 2. Every line tagged "skeuomorph: hook" in an original file is in a block,
//    so no hook goes unlisted.
//
// Needs no build: bun tests/skeuomorph-hooks.ts

import { execSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { runTest } from "./util.ts";

const ROOT = join(import.meta.dir, "..");
const LIST = "docs/skeuomorph-hooks.md";
const TAG = "skeuomorph: hook";
// our own code: tags there are not hooks
const OWN = ["src/skeuomorph/", "tests/", "docs/"];

type Block = { path: string; lines: string[] };

function blocks(md: string): Block[] {
  const out: Block[] = [];
  const re = /^```\w+ (\S+)\n([\s\S]*?)^```/gm;
  for (const m of md.matchAll(re)) {
    const lines = m[2]
      .split("\n")
      .map((l) => l.trim())
      .filter((l) => l !== "");
    out.push({ path: m[1], lines });
  }
  return out;
}

function trimmedLines(path: string): Set<string> {
  return new Set(
    readFileSync(join(ROOT, path), "utf8")
      .split(/\r?\n/)
      .map((l) => l.trim()),
  );
}

export async function testit(): Promise<void> {
  const listed = blocks(readFileSync(join(ROOT, LIST), "utf8"));
  const problems: string[] = [];

  for (const b of listed) {
    let have: Set<string>;
    try {
      have = trimmedLines(b.path);
    } catch {
      problems.push(`${b.path}: file missing`);
      continue;
    }
    for (const l of b.lines) {
      if (!have.has(l)) {
        problems.push(`${b.path}: missing  ${l}`);
      }
    }
  }

  const listedLines = new Set(listed.flatMap((b) => b.lines));
  const grep = execSync(`git grep -n "${TAG}" -- src cmd premake5.files.lua`, { cwd: ROOT, encoding: "utf8" });
  let tagged = 0;
  for (const row of grep.split("\n").filter((r) => r !== "")) {
    const [path, lineNo, ...rest] = row.split(":");
    if (OWN.some((o) => path.startsWith(o))) {
      continue;
    }
    tagged++;
    const text = rest.join(":").trim();
    if (!listedLines.has(text)) {
      problems.push(`${path}:${lineNo}: tagged but not in ${LIST}  ${text}`);
    }
  }

  for (const p of problems) {
    console.log(`FAIL ${p}`);
  }
  if (problems.length > 0) {
    throw new Error(`skeuomorph-hooks: ${problems.length} problems`);
  }
  console.log(`skeuomorph-hooks: ${listed.length} blocks and ${tagged} tagged lines all in place`);
}

if (import.meta.main) {
  try {
    await runTest("skeuomorph-hooks", testit);
  } catch (e) {
    console.error(`\n${(e as Error)?.message ?? e}`);
    process.exit(1);
  }
  process.exit(0);
}

// Fills the Skeuomorph Page Turn test shelf (docs/skeuomorph-page-turn-spec.md, "Test shelf").
// The shelf lives outside the repo and is never committed.
//
// Run: bun tests/skeuomorph-test-shelf.ts [shelf-dir] [--force]
//   shelf-dir  default C:\code\test-shelf
//   --force    regenerate the generated books (downloads are never re-fetched once present)
//
// Writes only its own named files:
//   generated\numbered-*.pdf  every page shows its number large, on its own colour
//   downloads\bakemono-shiuchi-hyobanki-rtl.cbz
//   downloads\peter-rabbit.epub
//   README.txt                only if absent or written by this script
// Anything else in the shelf (the human's own scans) is never touched.
//
// Download sources and licences:
// - Comic, right-to-left with double-page spreads: Koikawa Harumachi, "Bakemono Shiuchi Hyōbanki"
//   (妖怪仕内評判記), kibyōshi (illustrated story book, a forerunner of manga), Edo 1779, 2 volumes.
//   Scans: National Diet Library, Japan, https://dl.ndl.go.jp/pid/9892372 (IIIF manifest:
//   https://dl.ndl.go.jp/api/iiif/9892372/manifest.json). Licence: public domain, marked PDM by the
//   NDL (author died 1789). Wikimedia Commons hosts the same pages as PD-old-70, but refuses bun's
//   fetch with HTTP 429, so this script crops the NDL scans itself and packs them into a CBZ.
// - EPUB: Beatrix Potter, "The Tale of Peter Rabbit", Project Gutenberg #14838,
//   https://www.gutenberg.org/ebooks/14838. Licence: public domain (author died 1943);
//   Project Gutenberg License text is inside the book.

import { existsSync, mkdirSync, readFileSync, renameSync, statSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const DEFAULT_SHELF = "C:\\code\\test-shelf";
const GENERATED_DIR = "generated";
const DOWNLOADS_DIR = "downloads";
const README_NAME = "README.txt";
const README_MARKER = "Skeuomorph test shelf";

// a descriptive User-Agent, as public archives ask of scripts
const USER_AGENT = "skeuomorph-test-shelf/1.0 (SumatraPDF fork test script)";
const MIN_DOWNLOAD_BYTES = 100 * 1024;

enum Regen {
  IfMissing,
  Always,
}

enum Outcome {
  Created = "created",
  Skipped = "exists, skipped",
  Failed = "FAILED",
}

interface Result {
  path: string;
  outcome: Outcome;
  note?: string;
}

// ---------------------------------------------------------------------------
// Minimal PDF writer: one filled rect, a frame and text per page, standard fonts.

interface PdfPage {
  w: number; // points
  h: number;
  content: string;
}

const HELVETICA_DIGIT_WIDTH = 0.556; // em, same for every digit in Helvetica and Helvetica-Bold
const HELVETICA_CAP_HEIGHT = 0.718; // em

function num(n: number): string {
  return String(Math.round(n * 100) / 100);
}

function pdfStr(s: string): string {
  return "(" + s.replace(/[\\()]/g, (c) => "\\" + c) + ")";
}

// Serialises objects and computes xref offsets. Everything is 1 byte per char (latin1).
function buildPdf(title: string, pages: PdfPage[]): Buffer {
  const objs: string[] = []; // objs[i] is object i+1
  const CATALOG = 1;
  const PAGES = 2;
  const INFO = 5;
  const firstPage = 6;
  const pageObj = (i: number) => firstPage + i * 2;

  const kids = pages.map((_, i) => `${pageObj(i)} 0 R`).join(" ");
  objs.push(`<< /Type /Catalog /Pages ${PAGES} 0 R >>`);
  objs.push(
    `<< /Type /Pages /Count ${pages.length} /Kids [${kids}] ` + `/Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> >>`,
  );
  objs.push("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  objs.push("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
  objs.push(`<< /Title ${pdfStr(title)} /Producer ${pdfStr("tests/skeuomorph-test-shelf.ts")} >>`);
  for (let i = 0; i < pages.length; i++) {
    const p = pages[i];
    objs.push(
      `<< /Type /Page /Parent ${PAGES} 0 R /MediaBox [0 0 ${num(p.w)} ${num(p.h)}] /Contents ${pageObj(i) + 1} 0 R >>`,
    );
    objs.push(`<< /Length ${p.content.length} >>\nstream\n${p.content}\nendstream`);
  }

  // header's binary comment marks the file as binary for transfer tools
  let out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  const offsets: number[] = [];
  for (let i = 0; i < objs.length; i++) {
    offsets.push(out.length);
    out += `${i + 1} 0 obj\n${objs[i]}\nendobj\n`;
  }

  // each xref entry is exactly 20 bytes: 10-digit offset, 5-digit generation, type, 2-byte EOL
  const xrefStart = out.length;
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    out += String(off).padStart(10, "0") + " 00000 n \n";
  }
  out += `trailer\n<< /Size ${objs.length + 1} /Root ${CATALOG} 0 R /Info ${INFO} 0 R >>\n`;
  out += `startxref\n${xrefStart}\n%%EOF\n`;
  return Buffer.from(out, "latin1");
}

// ---------------------------------------------------------------------------
// Numbered pages

function hslToRgb(h: number, s: number, l: number): [number, number, number] {
  const k = (n: number) => (n + h / 30) % 12;
  const a = s * Math.min(l, 1 - l);
  const f = (n: number) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)));
  return [f(0), f(8), f(4)];
}

// Golden-angle hue steps: neighbouring pages are ~137° apart on the colour wheel,
// so page 5 and page 6 never look alike. Light background keeps dark text readable.
const GOLDEN_ANGLE = 137.508;

function pageColors(n: number): { bg: string; frame: string } {
  const hue = (n * GOLDEN_ANGLE) % 360;
  const rgb = (l: number) =>
    hslToRgb(hue, 0.65, l)
      .map((c) => c.toFixed(3))
      .join(" ");
  return { bg: rgb(0.74), frame: rgb(0.42) };
}

// e.g. n=12, total=40, label="A4 portrait": coloured page, frame, big "12", caption bottom-left
function numberedPage(n: number, total: number, w: number, h: number, label: string): PdfPage {
  const { bg, frame } = pageColors(n);
  const digits = String(n).length;
  const size = Math.min((w * 0.8) / (digits * HELVETICA_DIGIT_WIDTH), (h * 0.6) / HELVETICA_CAP_HEIGHT);
  const x = (w - digits * HELVETICA_DIGIT_WIDTH * size) / 2;
  const y = (h - HELVETICA_CAP_HEIGHT * size) / 2;
  const inset = 12;
  const captionSize = Math.min(14, w / 24);
  const caption = `page ${n} of ${total}` + (label ? `  -  ${label}` : "");

  const content = [
    `q ${bg} rg 0 0 ${num(w)} ${num(h)} re f Q`,
    `q ${frame} RG 6 w ${inset} ${inset} ${num(w - 2 * inset)} ${num(h - 2 * inset)} re S Q`,
    `BT /F2 ${num(size)} Tf 0.08 0.08 0.08 rg ${num(x)} ${num(y)} Td ${pdfStr(String(n))} Tj ET`,
    `BT /F1 ${num(captionSize)} Tf 0.08 0.08 0.08 rg ${inset * 2} ${inset * 2} Td ${pdfStr(caption)} Tj ET`,
  ].join("\n");
  return { w, h, content };
}

const A4 = { w: 595, h: 842 };

function uniformBook(nPages: number): PdfPage[] {
  const pages: PdfPage[] = [];
  for (let n = 1; n <= nPages; n++) {
    pages.push(numberedPage(n, nPages, A4.w, A4.h, ""));
  }
  return pages;
}

// sizes in points (1/72 inch)
const MIXED_SIZES: { w: number; h: number; label: string }[] = [
  { w: 595, h: 842, label: "A4 portrait" },
  { w: 595, h: 842, label: "A4 portrait" },
  { w: 842, h: 595, label: "A4 landscape" },
  { w: 420, h: 595, label: "A5 small" },
  { w: 595, h: 842, label: "A4 portrait" },
  { w: 360, h: 1080, label: "tall 1:3" },
  { w: 1190, h: 842, label: "double-width spread" },
  { w: 595, h: 842, label: "A4 portrait" },
  { w: 298, h: 420, label: "A6 tiny" },
  { w: 612, h: 792, label: "US Letter" },
  { w: 600, h: 600, label: "square" },
  { w: 1190, h: 842, label: "double-width spread" },
  { w: 595, h: 842, label: "A4 portrait" },
];

function mixedBook(): PdfPage[] {
  const total = MIXED_SIZES.length;
  return MIXED_SIZES.map((s, i) => numberedPage(i + 1, total, s.w, s.h, `${s.label} ${s.w} x ${s.h} pt`));
}

interface GeneratedBook {
  name: string;
  pages: () => PdfPage[];
}

const UNIFORM_PAGE_COUNTS = [1, 2, 3, 40, 800];

function generatedBooks(): GeneratedBook[] {
  const books: GeneratedBook[] = UNIFORM_PAGE_COUNTS.map((n) => ({
    name: `numbered-${String(n).padStart(3, "0")}-${n === 1 ? "page" : "pages"}.pdf`,
    pages: () => uniformBook(n),
  }));
  books.push({ name: "numbered-mixed-sizes.pdf", pages: mixedBook });
  return books;
}

// ---------------------------------------------------------------------------
// Minimal zip writer (stored, no compression) for the CBZ

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(data: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  }
  return (c ^ 0xffffffff) >>> 0;
}

const ZIP_LOCAL_SIG = 0x04034b50;
const ZIP_CENTRAL_SIG = 0x02014b50;
const ZIP_END_SIG = 0x06054b50;
const ZIP_VERSION = 20;
const ZIP_STORED = 0;
// fixed timestamp keeps the archive reproducible: 2026-01-01 00:00
const ZIP_DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;

interface ZipEntry {
  name: string; // ASCII
  data: Uint8Array;
}

function buildZip(entries: ZipEntry[]): Buffer {
  const parts: Buffer[] = [];
  const central: Buffer[] = [];
  let offset = 0;

  for (const e of entries) {
    const name = Buffer.from(e.name, "ascii");
    const crc = crc32(e.data);

    const local = Buffer.alloc(30);
    local.writeUInt32LE(ZIP_LOCAL_SIG, 0);
    local.writeUInt16LE(ZIP_VERSION, 4);
    local.writeUInt16LE(0, 6); // flags
    local.writeUInt16LE(ZIP_STORED, 8);
    local.writeUInt16LE(0, 10); // time
    local.writeUInt16LE(ZIP_DOS_DATE, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(e.data.length, 18); // compressed
    local.writeUInt32LE(e.data.length, 22); // uncompressed
    local.writeUInt16LE(name.length, 26);
    local.writeUInt16LE(0, 28); // extra
    parts.push(local, name, Buffer.from(e.data));

    const cen = Buffer.alloc(46);
    cen.writeUInt32LE(ZIP_CENTRAL_SIG, 0);
    cen.writeUInt16LE(ZIP_VERSION, 4); // made by
    cen.writeUInt16LE(ZIP_VERSION, 6); // needed
    cen.writeUInt16LE(0, 8);
    cen.writeUInt16LE(ZIP_STORED, 10);
    cen.writeUInt16LE(0, 12);
    cen.writeUInt16LE(ZIP_DOS_DATE, 14);
    cen.writeUInt32LE(crc, 16);
    cen.writeUInt32LE(e.data.length, 20);
    cen.writeUInt32LE(e.data.length, 24);
    cen.writeUInt16LE(name.length, 28);
    // extra, comment, disk, internal attrs, external attrs: all 0
    cen.writeUInt32LE(offset, 42);
    central.push(cen, name);

    offset += local.length + name.length + e.data.length;
  }

  const centralSize = central.reduce((sum, b) => sum + b.length, 0);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(ZIP_END_SIG, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralSize, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...parts, ...central, end]);
}

// ---------------------------------------------------------------------------
// Downloads

// National Diet Library IIIF image API. Each scan is one opening of the book photographed
// with a ruler and colour chart around it; the server crops and scales for us:
//   {base}/R0000004/pct:x,y,w,h/,2400/0/default.jpg
const NDL_IIIF = "https://dl.ndl.go.jp/api/iiif/9892372/";
const NDL_HEIGHT = 2400; // px per downloaded image; the scans are 3558 px tall

// The rig is fixed, so every interior opening sits in the same place (percent of the scan).
//   +------------------------------------------------+
//   | chart |  left page   |  right page  |   ruler   |
//   +------------------------------------------------+
//          15.3%         50.5% (spine)  85.6%
const BOOK_TOP = 7.6;
const BOOK_HEIGHT = 80.4;
const BOOK_LEFT = 15.3;
const SPINE = 50.5;
const BOOK_RIGHT = 85.6;
// the closed covers are a little wider than a page
const COVER_WIDTH = 38.6;
const FRONT_COVER_LEFT = 14.8;
const BACK_COVER_LEFT = 47.2;

// One entry per opening, in book order. A Japanese book is bound on the right and read
// right to left, so a split opening yields its right page first.
enum Crop {
  FrontCover, // closed book on the left of the scan
  Split, // two single pages: right, then left
  Spread, // the whole opening as one wide image
  BackCover, // closed book on the right of the scan
}

// two volumes: scans 1-9 (vol. 1), 9-17 (vol. 2); spreads chosen where the picture runs across
const COMIC_OPENINGS: Crop[] = [
  Crop.FrontCover, // 1
  Crop.Split,
  Crop.Split,
  Crop.Spread, // 4
  Crop.Split,
  Crop.Spread, // 6
  Crop.Split,
  Crop.Spread, // 8
  Crop.Split, // 9: end of vol. 1, cover of vol. 2
  Crop.Split,
  Crop.Split,
  Crop.Spread, // 12
  Crop.Split,
  Crop.Spread, // 14
  Crop.Split,
  Crop.Split,
  Crop.BackCover, // 17
];

interface ComicPage {
  file: string;
  url: string;
  spread: boolean;
}

function ndlUrl(scan: number, x: number, w: number): string {
  const id = "R" + String(scan).padStart(7, "0");
  return `${NDL_IIIF}${id}/pct:${x},${BOOK_TOP},${num(w)},${BOOK_HEIGHT}/,${NDL_HEIGHT}/0/default.jpg`;
}

// e.g. scan 4 (Spread) -> "06-scan04-spread.jpg"; scan 5 (Split) -> "07-scan05-right.jpg", "08-scan05-left.jpg"
function comicPages(): ComicPage[] {
  const pages: ComicPage[] = [];
  const add = (scan: number, part: string, x: number, w: number, spread = false) => {
    const file = `${String(pages.length + 1).padStart(2, "0")}-scan${String(scan).padStart(2, "0")}-${part}.jpg`;
    pages.push({ file, url: ndlUrl(scan, x, w), spread });
  };
  const pageW = BOOK_RIGHT - SPINE;

  COMIC_OPENINGS.forEach((crop, i) => {
    const scan = i + 1;
    if (crop === Crop.FrontCover) {
      add(scan, "cover", FRONT_COVER_LEFT, COVER_WIDTH);
    } else if (crop === Crop.BackCover) {
      add(scan, "back-cover", BACK_COVER_LEFT, COVER_WIDTH);
    } else if (crop === Crop.Spread) {
      add(scan, "spread", BOOK_LEFT, BOOK_RIGHT - BOOK_LEFT, true);
    } else {
      add(scan, "right", SPINE, pageW);
      add(scan, "left", BOOK_LEFT, SPINE - BOOK_LEFT);
    }
  });
  return pages;
}

const COMIC_PAGES = comicPages();

const COMIC_NAME = "bakemono-shiuchi-hyobanki-rtl.cbz";
const EPUB_NAME = "peter-rabbit.epub";
const EPUB_URL = "https://www.gutenberg.org/cache/epub/14838/pg14838-images-3.epub";

function comicInfoXml(): string {
  const pages = COMIC_PAGES.map((p, i) => {
    const type = i === 0 ? ' Type="FrontCover"' : "";
    const spread = p.spread ? ' DoublePage="true"' : "";
    return `    <Page Image="${i}"${type}${spread}/>`;
  });
  return [
    `<?xml version="1.0" encoding="utf-8"?>`,
    `<ComicInfo>`,
    `  <Title>Bakemono Shiuchi Hyōbanki</Title>`,
    `  <Writer>Koikawa Harumachi</Writer>`,
    `  <Penciller>Koikawa Harumachi</Penciller>`,
    `  <Year>1779</Year>`,
    `  <Summary>Edo-period kibyōshi, read right to left. Public domain. Scans: National Diet Library, Japan.</Summary>`,
    `  <Manga>YesAndRightToLeft</Manga>`,
    `  <Pages>`,
    ...pages,
    `  </Pages>`,
    `</ComicInfo>`,
    ``,
  ].join("\n");
}

const HTTP_TOO_MANY_REQUESTS = 429;
const MAX_ATTEMPTS = 4;
const DEFAULT_RETRY_SEC = 10;

// honours the server's Retry-After on 429
async function fetchPolite(url: string): Promise<Response> {
  for (let attempt = 1; ; attempt++) {
    const res = await fetch(url, { headers: { "User-Agent": USER_AGENT } });
    if (res.status !== HTTP_TOO_MANY_REQUESTS || attempt === MAX_ATTEMPTS) {
      return res;
    }
    await res.arrayBuffer();
    const waitSec = Number(res.headers.get("retry-after")) || DEFAULT_RETRY_SEC;
    console.log(`  rate-limited, waiting ${waitSec} s as the server asks`);
    await Bun.sleep(waitSec * 1000);
  }
}

async function fetchChecked(url: string): Promise<Uint8Array> {
  const res = await fetchPolite(url);
  if (!res.ok) {
    throw new Error(`HTTP ${res.status} for ${url}`);
  }
  const data = new Uint8Array(await res.arrayBuffer());
  if (data.length < MIN_DOWNLOAD_BYTES) {
    throw new Error(`only ${data.length} bytes from ${url}`);
  }
  return data;
}

function isJpeg(d: Uint8Array): boolean {
  return d[0] === 0xff && d[1] === 0xd8;
}

function isZip(d: Uint8Array): boolean {
  return d[0] === 0x50 && d[1] === 0x4b && d[2] === 0x03 && d[3] === 0x04;
}

async function buildComic(): Promise<Buffer> {
  const entries: ZipEntry[] = [];
  for (let i = 0; i < COMIC_PAGES.length; i++) {
    const p = COMIC_PAGES[i];
    const data = await fetchChecked(p.url);
    if (!isJpeg(data)) {
      throw new Error(`not a JPEG: ${p.url}`);
    }
    console.log(`  [${i + 1}/${COMIC_PAGES.length}] ${p.file} ${kb(data.length)}`);
    entries.push({ name: p.file, data });
  }
  entries.push({ name: "ComicInfo.xml", data: Buffer.from(comicInfoXml(), "utf8") });
  return buildZip(entries);
}

async function fetchEpub(): Promise<Buffer> {
  const data = await fetchChecked(EPUB_URL);
  if (!isZip(data)) {
    throw new Error(`not an EPUB (zip): ${EPUB_URL}`);
  }
  return Buffer.from(data);
}

// ---------------------------------------------------------------------------
// Shelf files

function kb(n: number): string {
  return n >= 1024 * 1024 ? `${(n / 1024 / 1024).toFixed(1)} MB` : `${Math.ceil(n / 1024)} KB`;
}

// write to a temp name first so an interrupted run never leaves a half file under the real name
function writeAtomic(path: string, data: Buffer | string) {
  const tmp = path + ".part";
  writeFileSync(tmp, data);
  renameSync(tmp, path);
}

async function makeFile(path: string, regen: Regen, make: () => Promise<Buffer> | Buffer): Promise<Result> {
  if (existsSync(path) && regen === Regen.IfMissing) {
    console.log(`skip ${path} (exists)`);
    return { path, outcome: Outcome.Skipped };
  }

  console.log(`make ${path}`);
  try {
    writeAtomic(path, await make());
  } catch (e) {
    console.log(`  FAILED: ${(e as Error).message}`);
    return { path, outcome: Outcome.Failed, note: (e as Error).message };
  }
  return { path, outcome: Outcome.Created };
}

function readmeText(): string {
  const spreads = COMIC_PAGES.filter((p) => p.spread).map((p) => p.file);
  return `${README_MARKER}
=====================

Made by tests/skeuomorph-test-shelf.ts in the sumatrapdf-skeuomorph repo.
Re-run it any time: it only adds its own missing files and never touches anything else here.
  bun tests/skeuomorph-test-shelf.ts [shelf-dir] [--force]

generated\\
  numbered-001-page.pdf ... numbered-800-pages.pdf
    A4 pages. Each page shows its number large on its own colour; neighbouring
    pages differ strongly in hue. Bottom-left caption: "page N of M".
  numbered-mixed-sizes.pdf
    ${MIXED_SIZES.length} pages: A4 portrait/landscape, A5, A6, tall 1:3, double-width spreads,
    US Letter, square. The caption names each page's size.

downloads\\
  ${COMIC_NAME}
    Koikawa Harumachi, "Bakemono Shiuchi Hyobanki" (1779, 2 volumes), an Edo-period
    kibyoshi (illustrated story book, a forerunner of manga). Read right to left;
    turn on manga mode. ${COMIC_PAGES.length} images in reading order.
    Each library scan shows one opening of the book. Most are cut into two single
    pages (right page first, as read); these are kept whole as double-page spreads:
      ${spreads.join("\n      ")}
    Source: National Diet Library, Japan, https://dl.ndl.go.jp/pid/9892372
    Licence: public domain (marked PDM by the library; author died 1789).

  ${EPUB_NAME}
    Beatrix Potter, "The Tale of Peter Rabbit", Project Gutenberg #14838, with images.
    Source: https://www.gutenberg.org/ebooks/14838
    Licence: public domain (author died 1943); Project Gutenberg License inside the book.
`;
}

// README.txt is ours only if it is absent or starts with our marker
function writeReadme(shelf: string): Result {
  const path = join(shelf, README_NAME);
  const text = readmeText().replace(/\n/g, "\r\n");
  const old = existsSync(path) ? readFileSync(path, "utf8") : null;
  if (old !== null && !old.startsWith(README_MARKER)) {
    console.log(`skip ${path} (not written by this script)`);
    return { path, outcome: Outcome.Skipped, note: "foreign file kept" };
  }
  if (old === text) {
    return { path, outcome: Outcome.Skipped };
  }

  writeAtomic(path, text);
  return { path, outcome: Outcome.Created };
}

function parseArgs(): { shelf: string; regen: Regen } {
  let shelf = DEFAULT_SHELF;
  let regen = Regen.IfMissing;
  for (const a of process.argv.slice(2)) {
    if (a === "--force") {
      regen = Regen.Always;
      continue;
    }
    if (a.startsWith("-")) {
      console.log(`unknown option ${a}\nusage: bun tests/skeuomorph-test-shelf.ts [shelf-dir] [--force]`);
      process.exit(1);
    }
    shelf = a;
  }
  return { shelf, regen };
}

async function main() {
  const { shelf, regen } = parseArgs();
  const genDir = join(shelf, GENERATED_DIR);
  const dlDir = join(shelf, DOWNLOADS_DIR);
  mkdirSync(genDir, { recursive: true });
  mkdirSync(dlDir, { recursive: true });
  console.log(`test shelf: ${shelf}`);

  const results: Result[] = [];
  for (const book of generatedBooks()) {
    const title = book.name.replace(/\.pdf$/, "");
    results.push(await makeFile(join(genDir, book.name), regen, () => buildPdf(title, book.pages())));
  }

  // downloads are fetched once; --force does not re-download
  results.push(await makeFile(join(dlDir, COMIC_NAME), Regen.IfMissing, buildComic));
  results.push(await makeFile(join(dlDir, EPUB_NAME), Regen.IfMissing, fetchEpub));
  results.push(writeReadme(shelf));

  console.log("\nsummary:");
  for (const r of results) {
    const size = existsSync(r.path) ? kb(statSync(r.path).size) : "-";
    const note = r.note ? ` (${r.note})` : "";
    console.log(`  ${r.outcome.padEnd(15)} ${size.padStart(8)}  ${r.path}${note}`);
  }

  const failed = results.filter((r) => r.outcome === Outcome.Failed).length;
  if (failed > 0) {
    console.log(`${failed} failed`);
    process.exit(1);
  }
}

await main();

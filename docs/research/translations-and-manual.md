# Translations and the in-app manual in fork builds

Ticket: #44 (map #36). Researched 2026-11-16. Planning only; no build script was changed.

## Answer

Both can be shipped without the maintainer's secret and without asking anyone:

- **Translations**: apptranslator's public history endpoint returns every translation ever
  submitted, no secret needed. Rebuilt against the fork's current strings it covers German
  1492 of 1493 strings; the one gap is the fork's own "Skeuomorph Page Turn".
- **Manual**: the official website serves every manual page as raw Markdown. As a fallback
  with a clear licence, the fork's own git history holds the manual as of 2026-09-22, when
  it still lived in this GPLv3 repo.

## Translations

### What the build expects

- The build packs `.work/translations.txt` into the embedded archive; when the file is
  missing it packs an empty one (`cmd/pack-embedded-prebuild.cmd:60`, `cmd/helper/embedded.ts:77`).
  An empty file means English only (`src/shared/Translations.cpp`, `SetCurrentLangByCode`).
- Format: two header lines (skipped by `ParseTranslationsTxt`, `src/shared/Translations.cpp:88`),
  then `:<english>` followed by `<lang>:<translation>` lines. Written by
  `writeTranslationsForBinary` (`cmd/trans-dl.ts:1343`), which drops languages missing more
  than `max(180, 35% of strings)`.
- `bun cmd/build.ts` does not run `trans-dl.ts`; it only runs `gen-docs` (`cmd/build.ts:354`).

### Where the secret is and is not needed

`cmd/trans-dl.ts` uses four apptranslator endpoints:

| Endpoint                                     | Secret                     | Use                                                                                  |
| -------------------------------------------- | -------------------------- | ------------------------------------------------------------------------------------ |
| `POST /api/dltransfor`                       | yes (`&secret=`, line 301) | current translations for the posted strings; also marks strings active on the server |
| `GET /api/edittranslation`                   | yes (line 908)             | submit a translation                                                                 |
| `GET /api/dltranshist?app=SumatraPDF&size=0` | **no** (line 551)          | full edit log as CSV: `t,time,user,lang,english,translation`                         |
| `GET /api/transhist?app=..&lang=..`          | **no** (line 802)          | per-language history (JSON)                                                          |

`main()` (line 1406) exits early without the secret, so it never reaches the public
history download, but nothing in the endpoint needs the secret. `dltransfor` is also the
call that writes to the server (it marks strings active), so it is right that a fork does
not call it.

### Measured (2026-11-16)

One anonymous GET of `/api/dltranshist?app=SumatraPDF&size=0` returned HTTP 200, 12 MB,
72 languages, latest edit 2026-11-08. Keeping the last edit per (language, English) and
filtering to the strings `extractStringsToTranslate()` (`cmd/trans-dl.ts:191`) finds in
this fork (1493):

- German: 1492 / 1493 (99.9%). Missing: `Skeuomorph Page Turn` (fork-only, `src/shared/MenuDefs.cpp:356`).
- Last editor of the German strings: 610 human, 844 `ai claude`, 37 `ai auto`, 1 `ai fix`.
  Upstream ships the same AI translations.
- All 72 languages pass the build's completeness threshold.
- Output is line-for-line the same format as the last committed file (e.g. `:&About` →
  `de:&Über` in both). Not yet loaded in a running fork build.

### Snapshots in git history (older, incomplete)

Upstream committed translations until mid-2026; the fork carries that history:

| Commit                                              | File                            | German coverage of today's 1493 strings |
| --------------------------------------------------- | ------------------------------- | --------------------------------------- |
| `d6603d21f^` (2026-07-29, "stop checking in texts") | `translations/translations.txt` | 543 (36%)                               |
| `9dfb2020a^` (2026-10-06)                           | `src/ng/res/translations.txt`   | 751 (50%)                               |

Usable offline, but half the UI would stay English. The history endpoint is better.

### Not checked: extracting from an official build

Official builds embed `translations.txt` and the manual in the `IDR_EMBEDDED_PAK` LzSA
resource. Extraction would need resource extraction plus an LzSA unpacker (the repo has
only the packer, `MakeLZSA.exe`). More work than the endpoint gives; keep it as a last resort.

### Licence

SumatraPDF is GPLv3 (`COPYING`; GitHub reports `GPL-3.0`). Translations were committed to
the GPLv3 repo for years and ship inside the GPLv3 binaries; `docs/TRANSLATORS` sends
contributors to apptranslator.org/app/SumatraPDF. The fork reuses them on the same terms.
apptranslator's source is not on GitHub under kjk's account and the site states no API
terms; the endpoint is anonymous and is the one upstream's own script calls without
credentials. Be polite: download once per build at most (better: cache), never call the
write endpoints.

## Manual

### What the build expects

`cmd/gen-docs.ts` reads `../hack/webapps/sumatra-website/www/docs` (`websiteDocsDir`,
`cmd/util.ts:5`), walks the `.md` links from `SumatraPDF-documentation.md`, and writes
`.work/docs` with the static files from this repo's `docs/` (`kManualStaticFiles`).
Without the directory it skips (line 233: "CI has no (private) sumatra-website checkout").
`gen-pdf-scripts` likewise needs `www/docs/scripts`.

### Sources

1. **Website, raw Markdown (current).** Anonymous GETs on 2026-11-16, all HTTP 200
   `text/markdown`:
   `https://www.sumatrapdfreader.org/docs/SumatraPDF-documentation.md` (6.8 KB, 118 `.md` links),
   `.../docs/Commands.md` (54 KB), and
   `.../docs/SumatraPDF-all-docs-for-llm-ai.md` (1 MB, 117 pages concatenated, each
   preceded by `<!-- Name.md -->`). Images are already CDN URLs (commit `6a09e126a`
   "Serve docs images from R2 CDN"), so no image download is needed.
2. **Git history (frozen, clear licence).** `docs/md/` at `b47280dbf^` (2026-09-22, the
   commit before "Move user docs to sumatra-website repo") has 85 files including
   `SumatraPDF-documentation.md`. It is GPLv3 like the rest of the repo, but misses two
   months of docs.
3. **The website repo.** Not public: `sumatrapdfreader/sumatra-website`, `kjk/sumatra-website`
   and `kjk/hack` all return 404; `gen-docs.ts` calls the checkout private.

### Licence

The website states no licence. The same pages were GPLv3 in this repo until September
2026 and ship embedded in GPLv3 binaries. For a personal build this does not matter; for
redistributed fork builds, source 2 is unambiguous and source 1 is the same author's
text published for anyone to read.

## Recommendation

One new script, e.g. `cmd/fork-assets.ts`, run by `cmd/build.ts` before `genDocsForBuild()`
only when `.work/translations.txt` is empty and no website checkout exists:

```
fork-assets
  translations:
    GET apptranslator /api/dltranshist?app=SumatraPDF&size=0    (cache in .work/, reuse if < 1 day old)
    reuse trans-dl.ts: extractStringsToTranslate(), parseCsv(), writeTranslationsForBinary()
    overlay fork strings from a committed file, e.g. translations/fork.txt
        :Skeuomorph Page Turn
        de:Skeuomorphes Umblättern
    -> .work/translations.txt
  manual:
    GET www.sumatrapdfreader.org/docs/SumatraPDF-documentation.md
    follow its .md links (same walk as gen-docs collectPages), GET each, plus scripts/ if needed
    -> .work/website-docs/
    point gen-docs at it (websiteDocsDir override, e.g. env SUMATRA_DOCS_DIR)
    offline fallback: git archive b47280dbf^ docs/md
```

Changes this implies, all in `cmd/` (no `src/` change): export a few helpers from
`trans-dl.ts`, make `websiteDocsDir` overridable, add the script and a call in `build.ts`
(and `ng-build.ts` if ng is built). Fork-only strings and fork manual pages (Page Turn)
need their own overlay either way; keep them in the repo so they survive upstream merges.

Open checks for the build ticket: load the rebuilt file in a running build with UI
language German; confirm `gen-docs` accepts the crawled pages (link validation, `Commands.md`
format) and the `scripts/` directory layout on the website.

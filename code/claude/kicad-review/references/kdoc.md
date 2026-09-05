# kdoc.py - datasheet search

| command | use |
| --- | --- |
| `grep PATTERN [-d NAME]` | search every indexed doc, or only names containing NAME; reports `doc:page:line` with `>>match<<` |
| `grep PATTERN --count` | hit counts per doc/page only. Use to pick a page cheaply. |
| `near A B` | pages where both patterns appear |
| `page DOC N` | prints a path to the page image; then `view` it |
| `text DOC N` | dump one page's text |
| `toc DOC` / `list` | heading per page / what is indexed |

`page` and `text` also accept the doc via `-d`, the same way `grep` does, so
`kdoc.py -d tps25947 page 4` and `kdoc.py page tps25947 4` are equivalent. They
also accept the page token exactly as `grep` prints it, so a hit labelled
`bq25798:chunk6:l11` can be opened with `kdoc.py text chunk6 -d bq25798` - copy the
label across without stripping the `chunk`/`p` prefix by hand.

Search is line-wrap insensitive by default (whitespace collapsed before matching),
so a phrase broken across two PDF lines still hits; `--raw` when column layout
matters. Invalid regex is treated as a literal. Other flags: `-C N`, `-m N`,
`--case`, `--dpi N`, `--force`, `--ocr` (index only). Cache in `~/.cache/kdoc`
(`KDOC_CACHE` overrides).

`-d` matching is punctuation-insensitive: the slug for `lm61460-q1.pdf` is stored
as `lm61460_q1`, but `-d lm61460-q1`, `-d "lm61460 q1"` and `-d lm61460q1` all
resolve to it. Copy the name straight off the filename; don't hand-convert hyphens.

Prefer plain strings over regex when grepping - the index is line-wrap normalised
and a regex tuned to the raw layout will miss.

## Why it exists

Project-uploaded "PDFs" are not always real PDFs. kdoc sniffs content, not
extension, and handles three cases behind a `.pdf` name plus
`.xlsx`/`.csv`/`.txt`/`.md`/`.net`:

- zip container of per-page `N.txt` + `N.jpeg` + `manifest.json` (`pdftotext` fails
  on these) - tag `img` in `list`
- a real PDF (`%PDF-` header) - text via one `pdftotext -layout` call for the whole
  file (pages split on form-feed), page images rendered on demand with `pdftoppm` -
  tag `render`. `pdftotext` decodes CID / Identity-H TrueType fonts, so an ordinary
  datasheet reads with no font parsing; if grep comes back empty the doc has the
  text, the pattern is wrong (try shorter, or `--raw`).
- scraped text with no PDF structure at all, e.g. a vendor product-page dump (seen
  with some TI datasheets in this project) - text-only, no page images ever exist -
  tag `text`. `list` flags which docs are this case.

## Scanned / image-only pages

A real PDF page that yields no extractable text is image-only (a scanned doc, or a
figure-only page). Those pages are counted and the doc is tagged `scan N/M` in
`list`; grep on such a doc says so and points at `--ocr` rather than silently
returning nothing. `kdoc.py index <file> --ocr` rasterises just the blank pages at
300 dpi and runs `tesseract` over them, writing the recovered text back into the
cache (needs `tesseract` on PATH; without it the pages stay blank and it says so).
Re-running `index --ocr` on an already-cached scan doc re-extracts it; a plain
`index` keeps the cache.

## Finding the files

With the cache empty, `kdoc` auto-indexes `/mnt/project`, `/mnt/user-data/uploads`
**and the working directory**, plus anything in `KDOC_DIRS` (colon separated).
Outside the uploads sandbox those `/mnt` paths do not exist. Auto-indexing is
budgeted - 32 MB per file, 40 files, 128 MB extracted - because a real folder holds
textbooks and video next to the datasheet; it says when it stopped. Point it at what
you actually want instead: `kdoc.py index ~/Downloads/parsnip.pdf`, or a directory.
An explicit `index` has no budget and reports per-file failures rather than dying on
the first unreadable archive. Run `index` once per doc, then grep.

`grep`/`text`/`toc` work identically on all three types. Only `page` differs: it
returns an image for `img`/`render` docs and a clear "no image possible" message for
`text` docs (get the content with `text` instead, or re-upload the real PDF if a
figure is actually needed). Scraped docs have no real page breaks, so hits are
labelled `chunkN` (a 200-line slice) instead of `pN` - **never cite a chunk number
as the datasheet's printed page number.**

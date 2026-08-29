---
name: part-search
description: Look up and choose electronic components on LCSC, JLCPCB and DigiKey with part.py. Does parametric selection (e.g. a ceramic cap 4.7uF-100uF rated >=25V, cheapest first), finds cheaper drop-in alternates for a part on the board, reports JLCPCB Basic vs Extended and $3/line assembly fees, runs one-shot board sourcing triage, and returns pricing ladders, stock, MOQ and a verified datasheet URL. Use this whenever the user asks about a specific component, an LCSC C-number, an MPN, a distributor price, whether something is in stock, what a part costs at some quantity, what the datasheet link is, asks to compare two parts, asks to pick or find a part meeting electrical constraints, asks whether a part is JLC Basic, or asks to price or source a whole board. Use it instead of web search for anything part-shaped, and never hand-write a scraping script for LCSC - the working endpoints are already recorded here and the obvious ones are blocked.
---

# Distributor part search and selection

`scripts/part.py` is stdlib-only Python 3. No install, no API key needed for LCSC or JLC.

```bash
P=<this-skill>/scripts/part.py
python3 $P selftest
python3 $P pick MLCC --cap 4.7u..100u --volt '>=25' --pkg 0805 --diel X7R,X5R
```

## Pick the command by what is being asked

| the question | command |
| --- | --- |
| "find me a cap 4.7-100 uF, >=25 V, cheapest" | `pick` |
| "is there something cheaper than C45783" | `alt C45783` |
| "what does C18164413 cost / is it stocked" | `show` |
| "is this part JLC Basic", "what are my assembly fees" | `jlc` |
| "datasheet link for X" | `ds` |
| "X vs Y" | `compare` |
| "what does this whole board cost" | `bom` |
| "source this board" / "what's blocking assembly" | `check` - `bom` + `jlc` + missing-code triage in one call |
| bare MPN, no constraints, just find it | `search` |

`search` is keyword-only and ranks badly on parametric queries. `search '22uF 25V X7R 1206'`
returns electrolytics and tantalums in the top hits, because LCSC has no category
filter. **For anything with electrical constraints, use `pick`, not `search`.**

## Run selftest first in a new session

LCSC has no public API. `part.py` uses undocumented endpoints that can start
returning 403/404 without notice. `selftest` reports in one line which providers are
alive, so you never debug endpoints by hand or rebuild a scraper.

If a provider shows DEAD, the endpoint moved. The constants are at the top of
`part.py`. Do not write a replacement script; fix the constant.

Endpoint status as recorded (probed 2026-08-18):

| endpoint | status |
| --- | --- |
| `wmsc.lcsc.com/ftps/wm/product/detail` | works, detail by C-code, full parameters |
| `easyeda.com/api/eda/product/search` (POST, form-encoded) | works, keyword search, `pageSize` up to 200 |
| `jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList` (POST, JSON) | works, no auth, 200 rows/call **with parsed attributes** |
| `wmsc.lcsc.com/wmsc/product/detail` | dead, 404 JSON |
| `wmsc.lcsc.com/ftps/wm/search/global` | blocked, Akamai Access Denied |
| `wmsc.lcsc.com/ftps/wm/{search/product,product/search,product/list,catalog/list}` | dead, "static resource unavailable" |
| `cart.jlcpcb.com/.../selectSmtComponentList` | superseded by the `jlcpcb.com/api/...` path above |

### LCSC cannot filter or sort server-side. Do not go looking again.

Probed exhaustively against the easyeda search endpoint: `sortField`, `sortOrder`,
`orderBy`, `catalogId`, `paramNameValueMap`, `attributes`, `paramList`, `filters`,
`paramValueList`, `selectedParams`, `paramMap`, `attributeFilter`, `params`,
`searchParam`, `stockFlag`. Every one either changed nothing or returned `total=0`.
Its search rows carry **no parameters at all** - only mpn, number, manufacturer,
package, stock, price, url. `needAggs=true` returns facet lists but they cannot be
sent back as filters.

This is why `pick` works the way it does: LCSC candidates must be fetched by
`product/detail` one at a time to learn their parameters. That is cached and
parallel, so it is fast, but it is why there is a `--pool` cap.

## Commands

| command | use |
| --- | --- |
| `pick 'MLCC' --cap 4.7u..100u --volt '>=25'` | parametric selection, sorted, filtered |
| `alt C45783` | cheaper/stocked/Basic equivalents of a part already on the board |
| `jlc board.net` | Basic vs Extended per part + total $3/line assembly fees |
| `show C18164413` | ladder, stock, MOQ, multiple, params, category, verified datasheet |
| `ds C42409135` | datasheet URL only, with pass/fail per candidate |
| `compare C1525 C60474` | side-by-side, differing parameters only |
| `search 'TPS61033'` | keyword search. Add `--instock`. |
| `bom board.net --qty 5` | prices a whole KiCad netlist by its LCSC Part property |
| `check board.net --qty 5` | one-shot sourcing triage: `bom` pricing + `jlc` Basic/Extended, one table, one pass over the netlist |
| `selftest` | which providers and the FX rate source work right now |

`show`, `ds`, `compare` and `alt` accept a C-code, a bare MPN, or a pasted LCSC URL.
`show` with several SKUs prints one verbose block per part by default; `--table`
switches to one compact row per part (sku/mpn/mfr/pkg/stock/price/desc) for
comparing candidates without shell paste/grep, `--json` for machine-readable output.
Exit codes: 0 ok, 1 nothing found.

## pick

```bash
part.py pick MLCC --cap 4.7u..100u --volt '>=25' --pkg 0805 --diel X7R,X5R --qty 100
part.py pick MLCC --cap 10u --volt '>=25' --pkg 0805 --basic      # no $3 line fee
part.py pick 'schottky diode' --vr '>=40' --ifwd '>=1' --pkg SOD-123
part.py pick 'ferrite bead' --w 'Impedance@Frequency=~600' --irms '>=2'
```

### Constraint syntax

Every `--flag` and every `--w NAME=SPEC` takes the same spec grammar:

| spec | meaning |
| --- | --- |
| `22uF` | equal (numeric, so `22uF` never matches `2.2uF`) |
| `4.7u..100u` | inclusive range |
| `>=25` `<=50` `>1k` `<10` | comparison |
| `X7R,X5R` | any-of; numeric-aware, so `25` matches `25V` |
| `~ceramic` | case-insensitive substring |
| `!X5R` | negated |

Values parse as engineering notation: `4u7`, `100nF`, `4R7`, `10k`, `±20%`, `25V`
all work, and `10V~35V` takes the first figure.

### Attribute shorthands

`--cap --res --ind --volt --pkg --diel --tol --current --power --freq --temp --type
--dcr --esr --vr --vf --ifwd --ir --vds --rdson --isat --irms`

These fuzzy-resolve onto whatever LCSC actually calls the parameter, deterministically
(exact alias, then prefix, then containment; ties break shortest-then-alphabetical, so
resolution never depends on parameter ordering). `pick` prints a `resolved:` line
whenever a shorthand mapped to a differently-named attribute, e.g.

```
resolved: ifwd -> Current - Rectified;  vr -> Voltage - DC Reverse (Vr) (Max)
```

**Check that line.** If a shorthand resolved to the wrong attribute, use `--w` with the
verbatim LCSC name instead: `--w 'Voltage - DC Reverse (Vr)=>=40'`.

### --fields is the cheap discovery pass

When you do not know what a category's attributes are called, run the same query with
`--fields`. It lists the attribute names and their commonest values across the
candidate pool and skips filtering entirely. One call, small output. Do this before
guessing at flag names for an unfamiliar part type.

```
part.py pick 'schottky diode' --pkg SOD-123 --fields
  Current - Rectified        1A, 3A, 5A, 2A, 10A
  Voltage - DC Reverse (Vr)  100V, 50V, 40V
  Voltage - Forward(Vf@If)   450mV@1A, 850mV@3A
```

### Why pick fans out into several queries

LCSC ranks by keyword relevance only. A single query for a *range* would never surface
the mid-range values, and a single query with a `>=25V` filter fills the candidate pool
with cheap 6.3/10/16 V parts that then all fail the filter, returning nothing. So `pick`
expands a range over E6 preferred values (`--e12` to widen) and over the standard
voltage series, and issues one query per combination, capped by `--maxq` (default 14).
The `pick:` header line shows the queries it actually ran.

If a result set comes back empty, `pick` prints which constraint rejected how many
candidates, e.g. `every candidate failed on: volt(131), diel(20)`. That tells you which
one to loosen.

### pick flags

`--sort price|stock|cap|volt` (default price), `--qty N` (default 100, MOQ/multiple
applied), `-n N` rows (default 8), `--basic`, `--source lcsc|jlc|both` (default lcsc),
`--anystock`, `--fields`, `--e12`, `--pool N` LCSC parts to detail (default 240),
`--maxq N` sub-queries, `--jobs N` threads, `--nojlc`, `--json`, `--fresh`.

## JLCPCB Basic vs Extended

LCSC has no concept of "Basic" - it is a JLC assembly-library field. So:

- `--basic` pools straight from JLC's base library (server-side filter, authoritative,
  ~1 call per keyword, and much faster than filtering LCSC results).
- Without `--basic`, `pick` annotates only the rows it finally displays, one lookup per
  C-code. The `jlc` column shows `BASIC`, `ext` or `-`.
- Do not infer Basic/Extended from a JLC *keyword* search: its relevance ranking drops
  parts that do exist in the library, which shows up as a false `-`.

```bash
part.py jlc /mnt/project/meshtastic.net       # whole board, with refdes
part.py jlc C45783 C1525 C2650956
```

For "source this whole board" or "what's blocking assembly", use `check board.net`
instead of running `bom` then `jlc` separately - one netlist parse, one table with
both LCSC price and JLC Basic/Extended per line, plus the components with no LCSC
code at all (the ones actually blocking assembly, not just costing more).

`jlc` on a `.net` file parses the `LCSC Part` property via kicad-review's `knet.py`.
It must not regex for `C\d+` on a netlist - that matches capacitor refdes like `C108`.

## Prices: two catalogs, never add them together

**`show`, `search`, `compare`, `bom`, and `pick --source lcsc` report LCSC retail
prices. `jlc` and `pick --basic` report JLCPCB assembly-catalog prices.** They are
different numbers for the same part. Say which one you are quoting.

Prices display in one currency, CAD by default. LCSC prices arrive in USD and are
converted with a daily FX rate (open.er-api.com, frankfurter.dev fallback, cached
24 h); the native USD figure is kept in parentheses and a one-line rate provenance note
prints once per run. A trailing `!` on a price means FX was unreachable and the native
currency is shown unconverted - never add a `!` price to a converted one.
`--currency USD` (or EUR etc.) switches everything.

`bom` totals exclude shipping, tax, PCB fabrication and assembly. Say so when quoting a
per-board cost. Extended line fees are `jlc`'s job, not `bom`'s.

## What the tool does that a hand-rolled script will not

- **Datasheet links are verified, not returned on faith.** Ranged GET, checks HTTP
  status, content type and the `%PDF` magic bytes. A dead link or an HTML login page is
  reported as BROKEN with the reason, then it falls through to the next candidate and
  finally the product page. Manufacturer-hosted links with expiring `?ts=` tokens are
  exactly what this catches.
- **Quantity pricing respects MOQ and order multiple.** Asking for 3 of a part with
  MOQ 5 / multiple 5 returns "buy 5" and says it rounded up.
- **Numeric comparison is numeric.** Value normalisation drops the decimal point, so a
  text match would treat `2.2uF` and `22uF` as identical. Anything that parses as a
  number is compared as a number and never falls through to the text branch.
- **Disk cache**, 24 h TTL, in `~/.cache/partsearch`. Repeat lookups across sessions are
  free; a warm `pick` is ~1 s against a ~10-30 s cold one. `--fresh` when stock matters.
- **`bom` honours DNP and `exclude_from_bom`**, and separately lists placed parts that
  carry no LCSC number at all, which is what blocks JLCPCB assembly.

## Reading the output

Stock is authoritative from the detail endpoint; the search index agrees with it, so a
0 in search results is a real 0, not a missing field.

`pick` shows `unit@N` and `ext` (= unit x the actual buy quantity after MOQ rounding).

## DigiKey setup

Optional. LCSC and JLC work with no configuration. For DigiKey, get free credentials at
developer.digikey.com (create an app, Production, Product Information V4), then:

```bash
export DIGIKEY_CLIENT_ID=...
export DIGIKEY_CLIENT_SECRET=...
```

or write `~/.config/partsearch/config.json` as
`{"digikey_client_id": "...", "digikey_client_secret": "..."}`.

Defaults are `--site CA --currency CAD`. The OAuth2 token is cached with its expiry.
DigiKey participates in `search`/`show` only, not in `pick`.

The DigiKey response normaliser (`_dk_norm`) has not been exercised against a live v4
response. If `selftest` shows auth OK but search DEAD, the field mapping there is the
place to look, not the request code.

## kcap.py: MLCC effective-density + voltage-stress-life comparator

`scripts/kcap.py` answers "higher voltage rating or higher nominal capacitance?"
objectively - minimising capacitor volume while keeping *effective* uF (after DC-bias
derating, density-driven dielectric thinning, temperature and aging) and wear-out
life in spec. Fully offline by default (fitted formulas); imports this skill's own
`part.py` client automatically when present for LCSC C-number resolution and a live
density-ceiling refinement, but never requires it.

```bash
python3 $skill/scripts/kcap.py compare '10u/25V/X5R/0805' '10u/25V/X7R/1206' --vop 8.4 --temp 45
python3 $skill/scripts/kcap.py compare C19666 C1791 --vop 5.0        # LCSC C-numbers work too
python3 $skill/scripts/kcap.py solve MLCC --need 8uF --vop 8.4 --temp 45   # smallest/cheapest that clears it
```

A spec string is slash-separated and order-independent: `CAP/VOLT/DIEL/PKG`, e.g.
`10u/25V/X5R/0805` or `22nF/50V/C0G/0603`.

`--vop` is required (the actual operating voltage the part will see - never assume
Vrated). Full flag reference: `python3 $skill/scripts/kcap.py --help`.

Model: `eff_C = C_nom * bias * densitySeverity * tempPenalty * agingPenalty`, reported
as retained %, effective uF, uF/mm^3, uF/mm^2, % of density ceiling, and a relative
voltage-stress life verdict. Case L/W confirmed against manufacturer dimension tables;
H is the middle of KEMET's documented per-case thickness-code range (varies with
capacitance/layer count within one case code - override with `--dim-a`/`--dim-b`
`L,W,H` for a specific real SKU when it matters). Voltage-stress life covers
TDDB/insulation-resistance wear-out only, **not** flex cracking, thermal cycling or
moisture, which dominate real field failures - `L_ref`/`T_ref`/`Ea` are an assumed
illustrative reference point, not a datasheet-certified figure; override with
`--lref-hours`/`--tref`/`--ea` if a manufacturer publishes real endurance numbers.

Verified on parsnip:
- X7R beat X5R on effective uF at every bulk position checked.
- Density traps: 10uF/25V in 0603, and 10uF/50V in 0805, both lose 35-45% to bias at
  a typical ~50% Vrated operating point - use 0805/1210 instead.
- A smaller case usually still wins on uF/mm^3 despite steeper derating, unless run
  near rated voltage.
- At 2.5x or more Vrated/Vop, voltage-stress life is never the binding constraint
  (2.5x margin gives ~15.6x relative life, matching `n=3`).

For a capacitor question that comes up mid netlist review, use this instead of
hand-computing derating - see also `kicad-review`'s own note about it.

## Related

`kicad-review` provides `knet.py`, whose netlist parser `bom` and `jlc` import when both
skills are installed. Without it, `bom` falls back to regex and `jlc` refuses `.net`
input rather than risk matching refdes.

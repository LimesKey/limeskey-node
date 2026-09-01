# pick - parametric selection

```bash
part.py pick MLCC --cap 4.7u..100u --volt '>=25' --pkg 0805 --diel X7R,X5R --qty 100
part.py pick MLCC --cap 10u --volt '>=25' --pkg 0805 --basic      # no $3 line fee
part.py pick 'schottky diode' --vr '>=40' --ifwd '>=1' --pkg SOD-123
part.py pick 'ferrite bead' --w 'Impedance@Frequency=~600' --irms '>=2'
```

## Constraint syntax

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

## Attribute shorthands

`--cap --res --ind --volt --pkg --diel --tol --current --power --freq --temp --type
--dcr --esr --vr --vf --ifwd --ir --vds --rdson --isat --irms`

These fuzzy-resolve onto whatever LCSC actually calls the parameter, deterministically
(exact alias, then prefix, then containment; ties break shortest-then-alphabetical, so
resolution never depends on parameter ordering). `pick` prints a `resolved:` line
whenever a shorthand mapped to a differently-named attribute:

```
resolved: ifwd -> Current - Rectified;  vr -> Voltage - DC Reverse (Vr) (Max)
```

**Check that line.** If a shorthand resolved to the wrong attribute, use `--w` with
the verbatim LCSC name instead: `--w 'Voltage - DC Reverse (Vr)=>=40'`.

## --fields is the cheap discovery pass

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

## Why pick fans out into several queries

LCSC ranks by keyword relevance only. A single query for a *range* would never surface
the mid-range values, and a single query with a `>=25V` filter fills the candidate pool
with cheap 6.3/10/16 V parts that then all fail the filter, returning nothing. So `pick`
expands a range over E6 preferred values (`--e12` to widen) and over the standard
voltage series, and issues one query per combination, capped by `--maxq` (default 14).
The `pick:` header line shows the queries it actually ran.

If a result set comes back empty, `pick` prints which constraint rejected how many
candidates, e.g. `every candidate failed on: volt(131), diel(20)`. That tells you which
one to loosen.

## Flags

`--sort price|stock|cap|volt` (default price), `--qty N` (default 100, MOQ/multiple
applied), `-n N` rows (default 8), `--basic`, `--source lcsc|jlc|both` (default lcsc),
`--anystock`, `--fields`, `--e12`, `--pool N` LCSC parts to detail (default 240),
`--maxq N` sub-queries, `--jobs N` threads, `--nojlc`, `--json`, `--fresh`.

`--pool` exists because LCSC cannot filter server-side: candidates must be fetched by
`product/detail` one at a time to learn their parameters. That is cached and parallel,
so it is fast, but it is bounded. See [endpoints.md](endpoints.md).

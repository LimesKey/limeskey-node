# knet.py - netlist connectivity, values, ERC, BOM

`knet.py FILE <cmd>` - file first, then subcommand.

| command | use |
| --- | --- |
| `summary` | comps, sheets, rails, DNP list, largest nets. Run first on an unfamiliar board. |
| `around REF...` | **highest value per call.** Pins, types, nets, position, every part one hop away. Use instead of `comp` + several `pin` calls. |
| `check` | 16 rule-based findings, grouped ERROR/WARN/INFO. Exit 2 if any ERROR. Three or more findings sharing a message fold into one line, `tail [N]: refs` - `[49]` means 49 separate findings, not one. |
| `check --since old.net` | only findings NEW vs an older export, plus a fixed/unchanged tally. |
| `draw REF\|NET [-d N] [-o x.svg] [--spec]` | KiCad-style schematic from the netlist; `--spec` gives the editable ksch source. |
| `notes` | schematic text notes by sheet - designer intent that exists nowhere in the netlist. |
| `rails` | each power rail: what feeds it, total decoupling, loads. |
| `divider REF.PIN\|NET` | resistor-divider trip voltage from netlist resistor values, worst case from tolerance if stated. See below. |
| `diff old.net` | what changed vs an earlier export (rename-safe: compares neighbour sets). |
| `comp REF...` | pin table with net, node count, supply rail. |
| `net PATTERN` | exact name, then sheet-local name, then regex. |
| `pin U2.9` | net at a pin plus everything else on it. |
| `find PATTERN` | searches refs, values, footprints, properties, pin functions, net names. |
| `walk REF[.PIN] -d N` | expand outward through 2-pin passives. |
| `path A B` | shortest electrical path between two parts. |
| `bom [--sheet PATH]` | grouped, DNP separated, flags parts with no LCSC number. `--sheet /Root/Rails/` scopes to one hierarchical sheet and everything nested under it. |
| `unconnected` | floating pins, `[NC flag]` vs `[NO NC flag]` with the sidecar, plus single-node nets. |
| `sheets` | components per hierarchical sheet. |

## Reading `around`

One line per pin, then its peers indented under it. The pin-type column is omitted
when the symbol types every pin `unspecified` (easyeda2kicad does), so a missing
type means unknown, never `passive`. A net already shown on an earlier pin of the
same part prints bare, or `(peers listed at pin N)` - that is two pins on one net,
not a different net.

## `divider`

`REF.PIN` accepts the pin's declared NAME (`U5.OVLO`, or the raw KiCad-escaped
`U14.EN{slash}UVLO`) as well as its number. Handles a plain 2-resistor divider
automatically. For TI's 3-resistor UVLO/OVLO string
(`IN->R1->EN/UVLO->R2->OVLO->R3->GND`, every TPS2594x/hot-swap part) it auto-detects
the shape and reports VIN_UV/VIN_OV once given `--vth <threshold from the
datasheet>`; add `--vth-tol <%>`, since the IC's own threshold accuracy usually
dominates the resistor tolerance. Use it for any FB/OVLO/UVLO/ADC-sense divider
instead of tracing it by hand across several `pin`/`net` calls.

## Flags

`--through R,L,FB,F,FL,JP` (pass-through prefixes; add `C`/`D` to trace through caps
or diodes), `--fanout N` (nets above N nodes are rails, default 8), `--as-drawn`
(traverse DNP parts too; **default treats DNP as open circuits**),
`--rail '+5V=5,VCC_RF=3.3'`, `--peer-max N`, `-o FILE`, `--spec`, `--theme`,
`--json`, `--only`/`--skip RULE,RULE`, `--rules` for the rule legend,
`--no-suppress` (for `check`), `--sheet PATH` (for `bom`), `--vth V` /
`--vth-tol PCT` (for a 3-resistor `divider`).

`walk` caps a rail's load list at 10 with a `+N more (rail net, N total)` tail once
a net is a named rail or above `--fanout`. The count in the tail stays accurate even
when not every load is printed.

## Project config

`knet.json` next to the netlist persists board defaults:

```json
{"rails": {"+BATT": 8.4}, "fanout": 8, "suppress": ["RFSTUB:GPS_ANT", "OCNOPULL:U9"]}
```

Precedence: defaults < knet.json < CLI.

`suppress` mutes `check` findings already confirmed not to be bugs, so they stop
costing tokens on every future review instead of restating "don't re-flag this"
each session. An entry is `"RULE"` (mute the whole rule) or `"RULE:TOKEN"` (mute
only findings naming that ref or net - a confirmed false positive on one net must
not hide a real one elsewhere). `check` reports how many it muted; verify the count
looks right, and use `--no-suppress` after a big rewire to see everything again.

## check rules

`FLOATPWR NCDRIVEN SOLO CONTEND DOMAIN DECOUPLE OCNOPULL I2CPULL PASSONLY RFSTUB
DNPPATH CAPRATING NOVALUE GNDISLAND CLAMPRATING PARPIN`. `--rules` for one-line
descriptions. With the sidecar, FLOATPWR downgrades to WARN when the schematic
carries an explicit NC flag.

- **`DOMAIN`** is the useful one: it infers each net's pulled-to voltage from series
  resistors and ferrites to named rails and compares against each IC's supply rail,
  catching pull-ups to 5 V on 3.3 V logic.
- **`PARPIN`** catches a paralleled pad left floating: two or more pins on the same
  part sharing the same declared pin NAME (e.g. multiple BAT pins on a charger IC)
  where one is wired to a real net and a sibling is not. Matches on pin name rather
  than pin type, so it still fires when the symbol types the dangling pin `passive`
  rather than `power_in` - the case that let a real floating BAT pin slip past
  FLOATPWR.
- **`CLAMPRATING`** parses a TVS's standoff voltage from a recognised part-number
  series (SMF/SMAJ/SMBJ/SMCJ/SM6T/SM8S/P6KE/1.5KE/P4KE) and flags it against the
  named rail it sits on between rail and GND - narrow by design (only those series,
  no margin math beyond "not below") to avoid a confidently wrong number. The rail
  figure is name-derived or `--rail`/knet.json, not the regulator's true analog
  output, so a finding here can understate the real gap; it will not overstate it.
- **`GNDISLAND`** catches a different shape of bug than FLOATPWR: pins named like
  ground (GND, AGND, VSS, ...) that ARE wired to each other but never reach the
  board's real GND net - a merge that silently didn't happen. `is_gnd()` recognises
  these by pin name even on an auto-generated net (`Net-(U7-GND-Pad1)`), so DECOUPLE
  no longer misfires on them as "missing a bypass cap".

**These are heuristics. Confirm against the datasheet before calling anything a
bug.** Worked example: `U7.5 (FB)` on a TPS61033 sat on the same net as `VIN` -
looks like a broken feedback loop, but the datasheet says that is the documented
fixed-5.0 V configuration. The correct move was `kdoc.py grep 'FB' -d tps` first.

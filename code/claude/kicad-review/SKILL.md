---
name: kicad-review
description: Query and review KiCad netlists (.net), schematics (.kicad_sch), board placement (.kicad_pcb) and datasheets, and draw schematics: knet.py, kpcb.py, ksch.py, kdoc.py. Use for any question touching a KiCad schematic, netlist, ERC, BOM, footprint, pin, net, connectivity, decoupling or power rail ("what does U2 pin 9 connect to"), and for board layout, placement, floorplan, footprint position, courtyard, edge clearance, mounting hole or keepout ("is U8 too close to the antenna"). `kpcb.py FILE ic U13` also RECOMMENDS where a regulator's caps, inductor and feedback divider go, with a diagram: use it for "where do these go", "lay out this buck/LDO", "minimise the switching loop". Never answer connectivity or geometry from memory or by eyeballing a schematic or board image; exports go stale within a session. Use it too for ANY request to draw or show a circuit, even one not on the board yet: ksch.py draws real symbols from a text spec and hand-written SVG is never right.
---

# KiCad netlist review, placement review and schematic drawing

Four stdlib-only tools in this skill's `scripts/`. Nothing to install.

```bash
K=<skill>/scripts/knet.py   P=<skill>/scripts/kpcb.py
D=<skill>/scripts/kdoc.py   S=<skill>/scripts/ksch.py
```

Which file answers which question:

| question | file | tool |
| --- | --- | --- |
| what is wired to what, values, ERC | `.net` | `knet.py` |
| where is it on the board, does it fit, does it clash | `.kicad_pcb` | `kpcb.py` |
| what does the part's datasheet say | PDFs | `kdoc.py` |
| show me the circuit | - | `ksch.py` |

`kpcb.py` needs `knet.py` beside it (shared S-expression parser and finding
formatter) but does **not** need the `.net` - pads in a `.kicad_pcb` carry their
own net names and pin functions.

Find the `.net` first: usually `/mnt/project/*.net` or `/mnt/user-data/uploads/*.net`.
`kdoc.py` auto-indexes those directories on its first search. If `*.kicad_sch` files
sit beside the `.net`, knet parses them as a sidecar (no_connect flags, text notes,
symbol positions), self-validating and degrading to netlist-only if the geometry
does not check out.

## Order of operations

1. **Connectivity, values, footprints, part numbers -> `knet.py`.** The netlist is
   the only authoritative record of what is wired to what.
2. **Position, clearance, floorplan -> `kpcb.py`.** The board file is the only
   record of where anything actually is. Never infer geometry from a screenshot.
3. **Design rules, absolute maximums, recommended values -> `kdoc.py grep`.**
4. **Page image -> only when the drawing itself must be read.** `kdoc.py page DOC N`
   prints a path, then `view` it.

Never derive connectivity from a schematic plot: extracted text is spatially
scrambled. The plot only tells you which sheet a symbol lives on.

---

# Drawing (ksch.py)

**Never hand-write SVG for a circuit, and never build one in the visualizer.**
Write a short spec, pipe it to `ksch.py`, `present_files` the `.svg`. A spec is ~20
short lines where hand-drawn SVG is hundreds, and the result looks like KiCad
instead of like boxes with text in them.

| situation | do this |
| --- | --- |
| the circuit is on the board | `knet.py FILE draw U8 -d 2 -o out.svg` - laid out from the netlist |
| ...and needs tidying | `knet.py FILE draw U8 --spec > x.ksch`, edit, `ksch.py render x.ksch -o out.svg` |
| the circuit is proposed / from a datasheet | write the spec by hand (below) |

## Spec language

One statement per line, `#` comments. Grid unit = 2.54 mm = 100 mil, **+y is down**.
Coordinates are integers. **Two-pin parts are 2 units long and the coordinate you
give is pin 1**, so `r R1 4,2 v` puts pin 1 at (4,2) and pin 2 at (4,4).

```
title  <text>
ic   REF x,y NAME [w=N] [h=N] [p=N] [flat] [dnp] L:1=EN,2=IN R:6=OUT T:5=VCC B:8=GND
conn REF x,y NAME R:1=VBUS,2=GND               connector / one-sided box
r|c|cp|l|fb|d|ds|dz|led|tvs|sw|fuse|xtal|bat|jp|tp|ant  REF x,y [ORIENT] [value] [dnp]
nmos|pmos|npn|pnp  REF x,y [l] [value]
wire  A.1 B.2 [12,4 ...]             orthogonal route; extra x,y are waypoints
net   NAME [@x=N|@y=N] A.1 B.2 ...   named net; @x/@y gives it a trunk column/row
gnd   A.2 [earth]                    pwr  +3V3 A.1
label NAME A.1     hlabel NAME A.1 [in|out]     glabel NAME A.1
nc    U1.7         note x,y <text>              group x,y w,h <text>
```

- `ic` anchor is the **top-left of the body**; pins run down each side at 2-unit
  pitch in the order listed, and `L:1=EN,,3=RST` leaves a blank slot to keep
  alignment. Height and width are automatic unless you set `h=`/`w=`.
- ORIENT: `v` (default, pin 1 top) `h` (pin 1 left) `vr` (pin 1 bottom) `hr` (pin 1
  right). Transistors: gate/base left by default, `l` mirrors, anchor is the gate or
  base pin, drain/collector down-side of the channel; p-channel and PNP are drawn
  source/emitter up, the way they are actually used.
- Address pins by number or name: `U8.6`, `U8.OUT`, `Q1.g`, `D1.k`, `R1.2`.
- Junction dots, routing and text placement are automatic. A wire always leaves a
  pin the way the pin faces, so it never dives back through its own body.

**Worked example** - one bash call, then `present_files`:

```bash
python3 $S render -o /mnt/user-data/outputs/efuse.svg - <<'EOF'
title U8 GNSS eFuse
ic U8 12,2 TPS259474L L:1=EN/UVLO,2=OVLO R:6=OUT T:5=IN B:8=GND
r R40 4,3 v 100k
r R41 4,9 v 150k
c C40 7,3 v 1uF
pwr +3V3 U8.5
pwr +3V3 R40.1
gnd U8.8
gnd R41.2
gnd C40.2
wire R40.2 R41.1
wire R40.2 U8.1
wire C40.1 7,1 U8.5
label 3V3_GNSS U8.6
nc U8.2
EOF
```

## Getting it right first time

- `ksch.py syms` prints every symbol with its pins and anchor; read it once per
  session if you need a part you have not drawn before. `ksch.py help` prints the
  full language.
- **`--net board.net` is the accuracy feature.** It fills IC pin names and values
  from the netlist, so `ic U8 10,4 @` alone draws the whole part correctly, and it
  verifies every drawn connection against the real board: a wire the board does not
  have, a pin number the part does not have, or a label sitting on the wrong net all
  come back as errors. Use it for any diagram of a circuit that exists.
- Errors name the line and say what was expected. Fix and re-run; do not fall back
  to hand-written SVG.
- The tool also warns about overlapping bodies, wires crossing a body, and pins
  drawn but left unwired.
- Flags: `--theme kicad|mono|dark`, `--px N` (22), `--us` (zigzag resistors),
  `--grid`, `--frame`, `--quiet`. Exit 3 = spec error, 2 = verify found an error.
- Layout habits that avoid rework: 5-6 units between columns, 3 between rows, inputs
  left, outputs right, supplies up, grounds down. Put a `pwr`/`gnd` symbol on every
  rail pin instead of running long wires, and use a `label` instead of dragging a
  wire across the sheet.

---

# knet.py

| command | use |
| --- | --- |
| `summary` | comps, sheets, rails, DNP list, largest nets. Run first on an unfamiliar board. |
| `around REF...` | **highest value per call.** Pins, types, nets, position, every part one hop away. Use instead of `comp` + several `pin` calls. |
| `check` | 14 rule-based findings, grouped ERROR/WARN/INFO. Exit 2 if any ERROR. Three or more findings sharing a message fold into one line, `tail [N]: refs` - `[49]` means 49 separate findings, not one. |
| `check --since old.net` | only findings NEW vs an older export, plus a fixed/unchanged tally. |
| `draw REF\|NET [-d N] [-o x.svg] [--spec]` | KiCad-style schematic from the netlist; `--spec` gives the editable ksch source. |
| `notes` | schematic text notes by sheet - designer intent that exists nowhere in the netlist. |
| `rails` | each power rail: what feeds it, total decoupling, loads. |
| `divider REF.PIN\|NET` | resistor-divider trip voltage computed directly from netlist resistor values, worst-case from resistor tolerance if stated. `REF.PIN` accepts the pin's declared NAME (e.g. `U5.OVLO`, or the raw KiCad-escaped `U14.EN{slash}UVLO`) as well as its number. Handles a plain 2-resistor divider automatically; for TI's 3-resistor UVLO/OVLO string (`IN->R1->EN/UVLO->R2->OVLO->R3->GND`, every TPS2594x/hot-swap part) it auto-detects the shape and reports VIN_UV/VIN_OV once given `--vth <threshold from the datasheet>` (add `--vth-tol <%>` - the IC's own threshold accuracy usually dominates the resistor tolerance). Use for any FB/OVLO/UVLO/ADC-sense divider instead of tracing it by hand across several `pin`/`net` calls. |
| `diff old.net` | what changed vs an earlier export (rename-safe: compares neighbour sets). |
| `comp REF...` | pin table with net, node count, supply rail. |
| `net PATTERN` | exact name, then sheet-local name, then regex. |
| `pin U2.9` | net at a pin plus everything else on it. |
| `find PATTERN` | searches refs, values, footprints, properties, pin functions, net names. |
| `walk REF[.PIN] -d N` | expand outward through 2-pin passives. |
| `path A B` | shortest electrical path between two parts. |
| `bom [--sheet PATH]` | grouped, DNP separated, flags parts with no LCSC number. Board-wide by default; `--sheet /Root/Rails/` scopes to one hierarchical sheet and everything nested under it (reuses `sheets`' own grouping). |
| `unconnected` | floating pins, `[NC flag]` vs `[NO NC flag]` with the sidecar, plus single-node nets. |
| `sheets` | components per hierarchical sheet. |

Reading `around`: one line per pin, then its peers indented under it. The pin-type
column is omitted when the symbol types every pin `unspecified` (easyeda2kicad
does), so a missing type means unknown, never `passive`. A net already shown on an
earlier pin of the same part prints bare, or `(peers listed at pin N)` - that is
two pins on one net, not a different net.

Flags: `--through R,L,FB,F,FL,JP` (pass-through prefixes; add `C`/`D` to trace
through caps or diodes), `--fanout N` (nets above N nodes are rails, default 8),
`--as-drawn` (traverse DNP parts too; **default treats DNP as open circuits**),
`--rail '+5V=5,VCC_RF=3.3'`, `--peer-max N`, `-o FILE`, `--spec`, `--theme`,
`--json`, `--only`/`--skip RULE,RULE`, `--rules` for the rule legend,
`--no-suppress` (for `check`: ignore knet.json's `suppress` list), `--sheet PATH`
(for `bom`), `--vth V` / `--vth-tol PCT` (for a 3-resistor `divider`).

`walk` caps a rail's load list at 10 with a `+N more (rail net, N total)` tail once
a net is a named rail or above `--fanout` - it always printed "showing loads only"
but used to dump every node regardless; one call on a big GND net could run to
hundreds of lines. The count in the tail stays accurate even when not every load
is printed.

Project config: `knet.json` next to the netlist persists board defaults, e.g.
`{"rails": {"+BATT": 8.4}, "fanout": 8, "suppress": ["RFSTUB:GPS_ANT", "OCNOPULL:U9"]}`.
Precedence: defaults < knet.json < CLI.

`suppress` mutes `check` findings already confirmed not to be bugs, so they stop
costing tokens on every future review instead of you having to restate "don't
re-flag this" each session. Entry is `"RULE"` (mute the whole rule) or
`"RULE:TOKEN"` (mute only findings naming that ref or net - a confirmed false
positive on one net must not hide a real one elsewhere). `check` reports how
many it muted; verify the count looks right, and use `--no-suppress` after a
big rewire to see everything again with fresh eyes.

Exit codes: 0 clean, 1 not found, 2 `check`/`draw` found an ERROR, 3 bad file.

### check rules

`FLOATPWR NCDRIVEN SOLO CONTEND DOMAIN DECOUPLE OCNOPULL I2CPULL PASSONLY RFSTUB
DNPPATH CAPRATING NOVALUE GNDISLAND CLAMPRATING PARPIN`. `--rules` for one-line
descriptions. With the sidecar, FLOATPWR downgrades to WARN when the schematic
carries an explicit NC flag.

`PARPIN` catches a paralleled pad left floating: two or more pins on the same
part sharing the same declared pin NAME (e.g. multiple BAT pins on a
battery-charger IC) where one is wired to a real net and a sibling is not.
Matches on pin name rather than pin type, so it still fires when the symbol
types the dangling pin `passive` rather than `power_in` - the case that let a
real floating BAT pin slip past FLOATPWR.

`CLAMPRATING` parses a TVS's standoff voltage from a recognised part-number
series (SMF/SMAJ/SMBJ/SMCJ/SM6T/SM8S/P6KE/1.5KE/P4KE) and flags it against the
named rail it sits on between rail and GND - narrow by design (only those
series, no margin math beyond "not below") to avoid a confidently wrong number.
The rail figure is name-derived or `--rail`/knet.json, not the regulator's true
analog output, so a finding here can understate the real gap - it will not
over-state it.

`DOMAIN` is the useful one: it infers each net's pulled-to voltage from series
resistors and ferrites to named rails and compares against each IC's supply rail,
catching pull-ups to 5 V on 3.3 V logic.

`GNDISLAND` catches a different shape of bug than FLOATPWR: pins named like
ground (GND, AGND, VSS, ...) that ARE wired to each other but never reach the
board's real GND net - a merge that silently didn't happen, e.g. a connector or
IC whose ground pins only tie to one another. `is_gnd()` recognises these by
pin name even on an auto-generated net (`Net-(U7-GND-Pad1)`), so DECOUPLE no
longer misfires on them as "missing a bypass cap" - GNDISLAND is the finding
that actually says what's wrong.

**These are heuristics. Confirm against the datasheet before calling anything a
bug.** Worked example: `U7.5 (FB)` on a TPS61033 sat on the same net as `VIN` -
looks like a broken feedback loop, but the datasheet says that is the documented
fixed-5.0 V configuration. The correct move was `kdoc.py grep 'FB' -d tps` first.

# kpcb.py

Placement review, from `parsnip.kicad_pcb` alone. No routing, no DRC, no 3D
bodies - everything here is checkable the moment a footprint is dropped.

| command | use |
| --- | --- |
| `summary` | outline size, stackup, zones, how much of each sheet is placed, biggest parts. Run first. |
| `check` | 9 rule-based findings, grouped ERROR/WARN/INFO. Exit 2 if any ERROR. |
| `where REF...` | **highest value per call.** Position, rotation, courtyard, edge distance, class, nets, and every neighbour within `-r`. Use instead of eyeballing coordinates. |
| `where X,Y -r N` | same, around a bare coordinate - "is there room here". |
| `map [--side f\|b] [--cols N]` | ASCII occupancy map, one letter per schematic sheet. Shows the floorplan and the free space in ~50 lines. |
| `sheet [PATH]` | per-sheet placed/left counts, bounding box, spread, and parts that drifted from their block. |
| `unplaced` | what is still parked off the outline, ref-ranged by sheet, biggest first. |
| `ic REF` | **where the passives should go**, not just what is wrong. Suggested x,y and rotation for the input caps, inductor, output caps, feedback divider, boot and bias caps, each with the rule behind it, plus an ASCII picture. `ic` alone lists the parts worth asking about. |
| `span` | nets ranked by how far apart their placed pads sit. The one routing-quality number that exists before routing does. |
| `sync [board.net]` | **run this first.** Board vs netlist: same parts, footprints, values, DNP flags and net on every pad. Finds a `.net` beside the board automatically. |
| `review [board.net]` | one call for a fresh session: sync, summary, check, longest nets, then the specific next calls worth making. |

Flags: `-r N` (neighbour radius, 5), `--max N` (cap per rule / per neighbour
list, 12), `--cols N` / `--side f|b` (for `map`), `--only`/`--skip RULE,RULE`,
`--rules`, `--json`, `--no-suppress`, and one threshold per rule:
`--edge 0.5` `--hole 1.5` `--conn 10` `--rf 8` `--therm 8` `--bypass 3`
`--clear 0` `--fb 4` `--gap 0.25` `--tol 1` `--span 0` (all mm; `--span 0` means
half the board diagonal), plus `--fanout 8`. For `ic`: `--anchor REF`,
`--cin REF,REF`, `--cout REF,REF`, `--ncin 3`, `--ncout 3`, `--assoc 6`.

Project config: `kpcb.json` next to the board, same shape and precedence as
`knet.json` - `{"edge": 0.3, "bypass": 4.0, "suppress": ["OVERLAP:BT1", "UNPLACED"]}`.
Use `suppress` for the placements you have already confirmed are deliberate
(thermistors under a cell, a module antenna overhanging the edge) so they stop
costing tokens on every review.

Exit codes: 0 clean, 1 not found, 2 `check`/`sync` found an ERROR, 3 not a board file.

### `sync` - is the board even the circuit you drew

`kpcb.py board.kicad_pcb sync` (the `.net` beside the board is found on its own).
**Nothing else here means anything if this fails.** A board that was never
re-synced after a schematic change carries pad net names that look perfectly
valid, so neither the board file nor any check on it can tell you it is stale -
only the netlist can. Rules: `SYNCPART SYNCFP SYNCVAL SYNCDNP SYNCNET`.

Clean output is two lines (`IN SYNC`), so it is cheap to run every time. A
`SYNCNET` error means re-run KiCad's *Update PCB from Schematic* before reading
any other finding. `SYNCVAL`/`SYNCDNP` alone are annotation drift: worth fixing,
but the connectivity is still right and placement findings still hold.

`unconnected-*` pseudo-nets get a `_1` suffix on the board and not in the
netlist; that is normalised away rather than reported as 439 differences.

### `review` - the whole placement picture in one call

`kpcb.py board.kicad_pcb review` runs sync, `summary`, `check` and the top of
`span`, then prints a **next** block naming the exact calls worth making on
*this* board - `ic` on the unplaced regulators, `where` on whichever refs appear
in the most ERROR findings, `map` if much is still unplaced. That last block is
the point: it is the reasoning a session would otherwise have to do itself from
a summary it has not read yet. If the board is out of sync it says STOP and
explains that everything below is about a different circuit.

### check rules

`OVERLAP EDGECLR HOLECLR CONNACC RFNOISE THERMAL BYPASS NETSPAN NOCRTYD
UNPLACED`. `--rules` for the legend.

- `OVERLAP` compares courtyard **bounding boxes**, not their true outlines, so a
  rotated or L-shaped part reports a slightly larger box than it occupies -
  conservative, never permissive. Parts on opposite sides only collide where a
  drilled barrel actually lands in the other's area; a back-side battery holder
  sitting over front-side 0402s is not a finding.
- `EDGECLR` is an ERROR when the courtyard crosses the outline, a WARN when it
  is merely inside `--edge`. An edge-mount part (SMA, U.FL, USB-C, `EdgeMount`
  in the footprint name) crossing the edge downgrades to INFO - that is what it
  is for.
- `HOLECLR` keepout radius is the hole's own pad/drill radius plus `--hole`.
  It also catches a mounting hole placed outside the board entirely.
- `CONNACC` has two halves: a connector further than `--conn` from any edge, and
  a connector whose cable-exit corridor (its courtyard swept straight out to the
  nearest edge) is blocked by another part. The corridor is axis-aligned only -
  a diagonal exit is not modelled, and neither is component height.
- `RFNOISE` needs a switching node. Those are found by pin NAME (`SW`, `LX`,
  `PH`, ...) plus, as a fallback for a switcher whose pin is unnamed, the nets of
  a power inductor (>=1uH **and** >=6 mm2 of courtyard, which is what separates a
  buck inductor from a 470nH 0603 antenna match). Named power rails are excluded,
  or every load on +3V3 would look noisy.
- `THERMAL` pairs heat sources (power inductors, switching/charger/eFuse/LDO
  parts) against heat-sensitive ones (crystals, oscillators, battery cells, the
  GPS module). Thermistors are deliberately **not** sensitive - sitting next to
  something hot is their job. Cross-side pairs are reported and labelled: heat
  couples through the board.
- `BYPASS` measures each supply pin to the nearest **placed** cap on the same
  net. It stays quiet when no cap on that net is placed yet, so it does not
  shout through the whole placement stage. Matched on pin NAME, never on
  `pintype`: easyeda2kicad types nearly every pin `passive`, so a type-based
  test would silently check nothing - the same trap `PARPIN` documents in knet.
- `NETSPAN` is the only rule that looks at wiring: a **fully placed** non-rail
  net whose pads sit further apart than `--span` (default half the board
  diagonal). Fully placed only, because a net still waiting on parts will move
  and flagging it now is noise that clears itself. Rails are excluded by node
  count **and** by name - a rail's span is the board, and dumping GND's 245
  nodes is exactly the failure the folding exists to prevent. `span` is the
  detailed view of the same data, ranked and capped.
- `UNPLACED` prints **one folded line per sheet**, never one per part.
- Two things that used to fire on a half-placed board and no longer do:
  a mounting hole sitting in the parked pile is INFO, not an ERROR about being
  off the board (an error still, if it is out there on its own); and an RF
  module whose antenna end overhangs the outline is INFO with a note to check
  the keepout, because that is how a WROOM or an E22 is meant to be placed.

**These are geometric heuristics.** They know nothing about component height,
the enclosure, keep-out zones you have not drawn, or your assembler's rules.
Confirm anything that matters against KiCad's own DRC and the mechanical
drawing.

### `ic REF` - where the passives go

The only command here that answers "where should this go" instead of "what is
wrong". One call per regulator, ~35 lines, no reasoning required from the
caller. Run `ic` with no ref first to see which parts it can advise on.

Nothing is templated per part number: pins are classified by NAME (`VIN` `PGND`
`SW` `FB` `BOOT` `VCC/BIAS` `OUT`, with a blank pin sitting on GND counted as a
ground pin), and every position comes out of the real pad coordinates in the
board file. So a part it has never seen works, and a part whose pins are unnamed
degrades to "nothing positionable found" rather than to a confident wrong answer.

What it places, and the rule each one comes from:

| role | where it is put | why |
| --- | --- | --- |
| input caps | straddling the **tightest VIN/PGND pad pair**, on the outside face of the package, smallest value innermost, extra caps stacked outward | that pad pair carries the high-di/dt loop; a cap across it *is* the small loop |
| inductor | hard against the SW pad(s), body pointing away from the package. Two switch nodes (a 4-switch buck-boost like the BQ25798) -> it bridges them instead | SW-pad-to-inductor is the other edge of the same loop, and the SW node is the antenna, so it stays short |
| output caps | a bank across the output node at the inductor's far pad, largest first | load current flows through them; a line of caps end to end would put the last one 25 mm downstream |
| feedback divider | out the far side of the package from SW, FB pad facing the IC | FB is the high-impedance node; it must not run past the SW pad or under the inductor |
| boot / bias caps | against the pins they serve | small loops of their own |
| a linear reg or load switch | no inductor, so the output cap straddles OUT/GND the same way the input cap straddles IN/GND | same rule, no switching node |
| a plain IC (no VIN pin) | supply pins found via the same name table `BYPASS` uses, caps straddling supply/GND | bypass placement is the loop rule at a smaller scale |

Rotations are snapped to 90 degrees. A pin pair on a diagonal would otherwise
ask for a 165.3-degree part, and the few degrees of loop cost nothing.

**`--anchor` is what makes it usable mid-placement.** The big parts land first,
so the inductor is usually down while the regulator is still in the parked pile.
Given that, "L4 is 90 mm from its slot" is true and useless - the inductor is
not what should move. So when the IC is unplaced and one of its passives is not,
`ic` hangs the whole layout off that part and reports **where the IC itself
goes**. `--anchor REF` picks a different one, `--anchor none` turns it off.
Translation only: if the anchor also needs turning, it says so and asks you to
rotate the IC in KiCad and re-run, because rotating it moves every pad and
guessing at that would be worse than saying it.

The `now` column compares against what is already placed: `OK` (within `--tol`),
`4.8 mm / 90 deg off`, or `unplaced`. It also reports parts standing in a
suggested slot, feedback parts within `--fb` of a switching node, and the
current input-loop length against the suggested one.

**The honest limit is which caps belong to which regulator.** On a rail like
`VSYS` (20+ caps here) nothing in a `.kicad_pcb` says which cap the schematic
drew next to which pin. `ic` ranks by schematic sheet and by refdes proximity to
the IC's own private-net parts, prefers one cap of each distinct value so
"smallest innermost" means something, takes `--ncin`/`--ncout` of them, and
**says in the output that it guessed**. Two regulators on one rail can be
offered the same cap. Check it against the schematic and pass `--cin`/`--cout`
once - that is the one number worth a second call.

### Output is capped, on purpose

A 439-footprint board can produce thousands of pair findings. Three caps keep a
review cheap: findings sharing a shape fold into one `tail [N]: refs` line, each
rule stops after `--max` lines with a `+N more (N total)` tail, and `where`
caps its neighbour list the same way. **The counts in the tails stay accurate
even when not every item is printed** - `+90 more OVERLAP line(s) (102 total)`
means 102 real findings. Widen deliberately with `--only RULE` or `--max`,
never by default.

# kdoc.py

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
`bq25798:chunk6:l11` can be opened with `kdoc.py text chunk6 -d bq25798` -
copy the label across without stripping the `chunk`/`p` prefix by hand.

Search is line-wrap insensitive by default (whitespace collapsed before matching),
so a phrase broken across two PDF lines still hits; `--raw` when column layout
matters. Invalid regex is treated as a literal. Other flags: `-C N`, `-m N`,
`--case`, `--dpi N`, `--force`. Cache in `~/.cache/kdoc` (`KDOC_CACHE` overrides).

Why it exists: project-uploaded "PDFs" are not always real PDFs. kdoc sniffs
content, not extension, and handles three cases behind a `.pdf` name plus
`.xlsx`/`.csv`/`.txt`/`.md`/`.net`:
- zip container of per-page `N.txt` + `N.jpeg` + `manifest.json` (`pdftotext`
  fails on these) - tag `img` in `list`
- a real PDF (`%PDF-` header) - text via `pdftotext -layout`, page images
  rendered on demand with `pdftoppm` - tag `render`
- scraped text with no PDF structure at all, e.g. a vendor product-page dump
  (seen with some TI datasheets in this project) - text-only, no page images
  ever exist - tag `text`. `list` flags which docs are this case.

**Finding the files.** With the cache empty, `kdoc` auto-indexes `/mnt/project`,
`/mnt/user-data/uploads` **and the working directory**, plus anything in
`KDOC_DIRS` (colon separated). Outside the uploads sandbox those `/mnt` paths do
not exist, and it used to index nothing and say nothing, so `grep` came back
empty on a PDF sitting in the same folder. Auto-indexing is budgeted - 32 MB per
file, 40 files, 128 MB extracted - because a real folder holds textbooks and
video next to the datasheet; it says when it stopped. Point it at what you
actually want instead: `kdoc.py index ~/Downloads/parsnip.pdf`, or a directory.
An explicit `index` has no budget and reports per-file failures rather than
dying on the first unreadable archive.

`grep`/`text`/`toc` work identically on all three. Only `page` differs: it
returns an image for `img`/`render` docs and a clear "no image possible" message
for `text` docs (get the content with `text` instead, or re-upload the real PDF
if a figure is actually needed). Scraped docs have no real page breaks, so hits
are labelled `chunkN` (a 200-line slice) instead of `pN` - never cite a chunk
number as the datasheet's printed page number.

# Facts that are easy to get wrong

- **The `.net` is re-exported constantly.** Re-run the tool. Do not trust a value,
  ref or net name quoted earlier in the conversation or in a summary.
- **DNP parts are open circuits by default.** A DNP 0 R in series means the path
  does not exist on the built board; `walk`/`path`/`draw` stop there. `--as-drawn`
  restores schematic-as-drawn traversal; DNP parts still render, dashed.
- **Ref prefix matching must use the full alphabetic prefix.** `JP` is not `J`.
- **Rails are terminal in traversal.** A path through GND or +3V3 is not a path.
- **Floating pins rarely show as "absent from all nets".** KiCad emits pseudo-nets
  named `unconnected-(REF-PIN-PadN)`. A **named** single-node net is usually a real
  bug (a label going nowhere); `unconnected-*` is an intentional NC or a forgotten
  pin - the sidecar NC annotation tells you which.
- **A part is pass-through if it has exactly two connected pins**, rails included.
- KiCad escapes text: `{slash}` is `/`, `~{FLT}` is an overbar. Drawings decode
  both; netlist queries use the raw escaped name.
- **Mid-placement, most of the BOM is parked in a pile beside the board.** Every
  kpcb check ignores anything whose centre is outside the outline, or the pile
  would drown the findings. `placed 135 of 439` in a header is progress, not an
  error. A part "not found" by `where` may simply not exist on the board yet.
- **`.kicad_pcb` y grows downward** and rotation is counter-clockwise on screen,
  so a footprint's child geometry transforms as `x + lx*cos - ...`, not the
  textbook rotation. `where` already prints board coordinates; do not re-derive
  them from `(at ...)` by hand.
- **A `.kicad_pcb` pad carries its own net name and pin function**, so kpcb needs
  no `.net` export. That also means the board can be out of date with the
  schematic: if kpcb and knet disagree about a net, the board has not been
  re-synced from the schematic.

# Recipes

**"Review this board's layout"** - one call: `kpcb.py FILE review`. It ends by
naming the next calls to make, so there is nothing to work out first.

**"Did the board get updated from the schematic?"** `kpcb.py FILE sync`. Two
lines when clean. Run it before quoting any placement finding.

**"Review this placement / where should X go?"** `kpcb.py FILE summary`, then
`check`, then `map` to see the free space, then `where REF` on anything the
check named. `sheet` tells you which blocks are still scattered. Say plainly
that these are geometric heuristics.

**"Is there room for X here?"** `kpcb.py FILE where 130,60 -r 10`. One call.

**"Where do I put the caps around U13 / how do I lay out this buck?"**
`kpcb.py FILE ic U13`. One call gives positions, rotations, the reason for each
and a picture. Do not reason it out from pad coordinates by hand, and do not
answer it from the datasheet's layout figure alone - the figure does not know
where this board's inductor already is.

**"Which nets are stretched across the board?"** `kpcb.py FILE span`. Answers
the routing question that is answerable before routing exists.

**"Review this board."** `summary`, `check`, `around` each flagged IC, `divider`
on any FB/OVLO/UVLO net that check or around surfaces, then `kdoc.py grep` the
datasheet rule for anything that looks wrong. Say plainly which findings are
unconfirmed heuristics.

**"Where does X connect / is X right?"** `around X`. One call.

**"What voltage does this divider set / is this OVLO threshold right?"**
`divider REF.PIN` (the IC pin, e.g. `U5.OVLO`) or `divider NET` directly - one
call gives nominal and worst-case trip voltage from the actual resistor values
and tolerances, instead of `pin`/`net`/`comp` calls plus doing the math by hand.

**"Draw / show me / how do I wire ..."** On the board: `knet.py FILE draw X -d 2 -o
/mnt/user-data/outputs/x.svg`. Not on the board yet: hand-write a ksch spec, adding
`--net FILE` if part of it exists. Then `present_files`.

**"What changed?"** `diff old.net` for wiring, `check --since old.net` for findings.

**"What does the datasheet say about Y?"** `kdoc.py grep 'Y' --count` for the page,
then grep with context, or `page` + `view` for figures.

**Self-test after editing a tool:**
```bash
python3 $K references/selftest.net check          # 4 error, 6 warn, 1 info
python3 $K references/selftest.net walk /DANGLE   # stops at DNP R2
python3 $K references/selftest.net draw U1 -d 2 -o /tmp/t.svg
python3 $K references/selftest.net divider /VSENSE  # 1.6667 V nominal, 1.6445-1.6890 V worst case
python3 $S render -o /tmp/t2.svg references/selftest.ksch   # no ERROR lines
python3 $P references/selftest.kicad_pcb check    # 4 error, 7 warn, 2 info; exit 2
python3 $P references/selftest.kicad_pcb summary  # 40 x 30 mm, placed 16 of 20
python3 $P references/selftest.kicad_pcb map      # 40x30 grid, no '*' outside a part
python3 $P references/selftest.kicad_pcb span     # 1 net (NB, 0.5 mm); rails excluded
python3 $P references/selftest.kicad_pcb sheet    # /Test/ 16 placed, /Test/Spare/ 4 left
python3 $P references/selftest.kicad_pcb sync references/selftest.net
                                      # 23 error, 7 warn; exit 2. The two fixtures
                                      # are deliberately different circuits, so this
                                      # is the negative test: all five SYNC rules fire
                                      # at once. `sync` on a real board+net that match
                                      # prints IN SYNC in two lines.
python3 $P references/selftest.kicad_pcb ic       # "no regulator-shaped part found"
```
The fixture has no regulator, so `ic` is exercised against the real board
instead: `kpcb.py board.kicad_pcb ic` must list the switchers, and `ic <a buck>`
must name an input cap, an inductor and a feedback part and print a diagram
whose IC body is visible inside the frame. A blank-looking frame means the
anchor shift was applied to the slots but not to the IC's own geometry.

`selftest.kicad_pcb` is a 40x30 mm fixture carrying one deliberate fault per
rule, including both `EDGECLR` severities (R4 crosses, R5 is merely close), both
`CONNACC` halves (J1 buried, J3's exit blocked by C2) and both `OVERLAP` cases
(R1/R2 same side, TP1's drill under U1 from the back). If a rule stops firing on
it, that rule is dead - kpcb's thresholds are loose enough that a clean board
reports nothing, which looks identical to a broken check.

# Related

Part sourcing (LCSC/DigiKey pricing, stock, verified datasheet URLs) is the
separate `part-search` skill. `part.py bom board.net` reuses this skill's parser
when both are installed. If a capacitor question comes up mid-review - "higher
voltage rating or higher nominal capacitance", DC-bias derating, effective uF in
a given footprint, voltage-stress life - that skill's `kcap.py` answers it
directly instead of hand-computing derating from the datasheet curve.

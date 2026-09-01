# kpcb.py - placement review

Placement review from the `.kicad_pcb` alone. No routing, no DRC, no 3D bodies -
everything here is checkable the moment a footprint is dropped.

| command | use |
| --- | --- |
| `summary` | outline size, stackup, zones, how much of each sheet is placed, biggest parts. Run first. |
| `check` | 10 rule-based findings, grouped ERROR/WARN/INFO. Exit 2 if any ERROR. |
| `where REF...` | **highest value per call.** Position, rotation, courtyard, edge distance, class, nets, and every neighbour within `-r`. Use instead of eyeballing coordinates. |
| `where X,Y -r N` | same, around a bare coordinate - "is there room here". |
| `map [--side f\|b] [--cols N]` | ASCII occupancy map, one letter per schematic sheet. Shows the floorplan and the free space in ~50 lines. |
| `sheet [PATH]` | per-sheet placed/left counts, bounding box, spread, and parts that drifted from their block. |
| `unplaced` | what is still parked off the outline, ref-ranged by sheet, biggest first. |
| `ic REF` | **where the passives should go**, not just what is wrong. See below. `ic` alone lists the parts worth asking about. |
| `span` | nets ranked by how far apart their placed pads sit. The one routing-quality number that exists before routing does. |
| `sync [board.net]` | **run this first.** Board vs netlist: same parts, footprints, values, DNP flags and net on every pad. Finds a `.net` beside the board automatically. |
| `review [board.net]` | one call for a fresh session: sync, summary, check, longest nets, then the specific next calls worth making. |

## Flags

`-r N` (neighbour radius, 5), `--max N` (cap per rule / per neighbour list, 12),
`--cols N` / `--side f|b` (for `map`), `--only`/`--skip RULE,RULE`, `--rules`,
`--json`, `--no-suppress`, and one threshold per rule: `--edge 0.5` `--hole 1.5`
`--conn 10` `--rf 8` `--therm 8` `--bypass 3` `--clear 0` `--fb 4` `--gap 0.25`
`--tol 1` `--span 0` (all mm; `--span 0` means half the board diagonal), plus
`--fanout 8`. For `ic`: `--anchor REF`, `--cin REF,REF`, `--cout REF,REF`,
`--ncin 3`, `--ncout 3`, `--assoc 6`.

## Project config

`kpcb.json` next to the board, same shape and precedence as `knet.json`:

```json
{"edge": 0.3, "bypass": 4.0, "suppress": ["OVERLAP:BT1", "UNPLACED"]}
```

Use `suppress` for placements already confirmed deliberate (thermistors under a
cell, a module antenna overhanging the edge) so they stop costing tokens on every
review.

## `sync` - is the board even the circuit you drew

`kpcb.py board.kicad_pcb sync` (the `.net` beside the board is found on its own).
**Nothing else here means anything if this fails.** A board that was never
re-synced after a schematic change carries pad net names that look perfectly valid,
so neither the board file nor any check on it can tell you it is stale - only the
netlist can. Rules: `SYNCPART SYNCFP SYNCVAL SYNCDNP SYNCNET`.

Clean output is two lines (`IN SYNC`), so it is cheap to run every time. A `SYNCNET`
error means re-run KiCad's *Update PCB from Schematic* before reading any other
finding. `SYNCVAL`/`SYNCDNP` alone are annotation drift: worth fixing, but the
connectivity is still right and placement findings still hold.

`unconnected-*` pseudo-nets get a `_1` suffix on the board and not in the netlist;
that is normalised away rather than reported as 439 differences.

## `review` - the whole placement picture in one call

Runs sync, `summary`, `check` and the top of `span`, then prints a **next** block
naming the exact calls worth making on *this* board - `ic` on the unplaced
regulators, `where` on whichever refs appear in the most ERROR findings, `map` if
much is still unplaced. That last block is the point: it is the reasoning a session
would otherwise have to do itself from a summary it has not read yet. If the board
is out of sync it says STOP and explains that everything below is about a different
circuit.

## check rules

`OVERLAP EDGECLR HOLECLR CONNACC RFNOISE THERMAL BYPASS NETSPAN NOCRTYD UNPLACED`.
`--rules` for the legend.

- **`OVERLAP`** compares courtyard **bounding boxes**, not their true outlines, so a
  rotated or L-shaped part reports a slightly larger box than it occupies -
  conservative, never permissive. Parts on opposite sides only collide where a
  drilled barrel actually lands in the other's area; a back-side battery holder
  sitting over front-side 0402s is not a finding.
- **`EDGECLR`** is an ERROR when the courtyard crosses the outline, a WARN when it
  is merely inside `--edge`. An edge-mount part (SMA, U.FL, USB-C, `EdgeMount` in
  the footprint name) crossing the edge downgrades to INFO - that is what it is for.
- **`HOLECLR`** keepout radius is the hole's own pad/drill radius plus `--hole`. It
  also catches a mounting hole placed outside the board entirely.
- **`CONNACC`** has two halves: a connector further than `--conn` from any edge, and
  a connector whose cable-exit corridor (its courtyard swept straight out to the
  nearest edge) is blocked by another part. The corridor is axis-aligned only - a
  diagonal exit is not modelled, and neither is component height.
- **`RFNOISE`** needs a switching node. Those are found by pin NAME (`SW`, `LX`,
  `PH`, ...) plus, as a fallback for a switcher whose pin is unnamed, the nets of a
  power inductor (>=1uH **and** >=6 mm2 of courtyard, which is what separates a buck
  inductor from a 470nH 0603 antenna match). Named power rails are excluded, or
  every load on +3V3 would look noisy.
- **`THERMAL`** pairs heat sources (power inductors, switching/charger/eFuse/LDO
  parts) against heat-sensitive ones (crystals, oscillators, battery cells, the GPS
  module). Thermistors are deliberately **not** sensitive - sitting next to
  something hot is their job. Cross-side pairs are reported and labelled: heat
  couples through the board.
- **`BYPASS`** measures each supply pin to the nearest **placed** cap on the same
  net. It stays quiet when no cap on that net is placed yet. Matched on pin NAME,
  never on `pintype`: easyeda2kicad types nearly every pin `passive`, so a
  type-based test would silently check nothing - the same trap `PARPIN` documents
  in knet.
- **`NETSPAN`** is the only rule that looks at wiring: a **fully placed** non-rail
  net whose pads sit further apart than `--span` (default half the board diagonal).
  Fully placed only, because a net still waiting on parts will move. Rails are
  excluded by node count **and** by name. `span` is the detailed view of the same
  data, ranked and capped.
- **`UNPLACED`** prints **one folded line per sheet**, never one per part.
- Two things that used to fire on a half-placed board and no longer do: a mounting
  hole sitting in the parked pile is INFO, not an ERROR about being off the board
  (an error still, if it is out there on its own); and an RF module whose antenna
  end overhangs the outline is INFO with a note to check the keepout, because that
  is how a WROOM or an E22 is meant to be placed.

**These are geometric heuristics.** They know nothing about component height, the
enclosure, keep-out zones you have not drawn, or your assembler's rules. Confirm
anything that matters against KiCad's own DRC and the mechanical drawing.

## `ic REF` - where the passives go

The only command here that answers "where should this go" instead of "what is
wrong". One call per regulator, ~35 lines, no reasoning required from the caller.
Run `ic` with no ref first to see which parts it can advise on.

Nothing is templated per part number: pins are classified by NAME (`VIN` `PGND`
`SW` `FB` `BOOT` `VCC/BIAS` `OUT`, with a blank pin sitting on GND counted as a
ground pin), and every position comes out of the real pad coordinates in the board
file. So a part it has never seen works, and a part whose pins are unnamed degrades
to "nothing positionable found" rather than to a confident wrong answer.

| role | where it is put | why |
| --- | --- | --- |
| input caps | straddling the **tightest VIN/PGND pad pair**, on the outside face of the package, smallest value innermost, extra caps stacked outward | that pad pair carries the high-di/dt loop; a cap across it *is* the small loop |
| inductor | hard against the SW pad(s), body pointing away from the package. Two switch nodes (a 4-switch buck-boost like the BQ25798) -> it bridges them instead | SW-pad-to-inductor is the other edge of the same loop, and the SW node is the antenna |
| output caps | a bank across the output node at the inductor's far pad, largest first | load current flows through them; a line of caps end to end would put the last one 25 mm downstream |
| feedback divider | out the far side of the package from SW, FB pad facing the IC | FB is the high-impedance node; it must not run past the SW pad or under the inductor |
| boot / bias caps | against the pins they serve | small loops of their own |
| a linear reg or load switch | no inductor, so the output cap straddles OUT/GND the same way the input cap straddles IN/GND | same rule, no switching node |
| a plain IC (no VIN pin) | supply pins found via the same name table `BYPASS` uses, caps straddling supply/GND | bypass placement is the loop rule at a smaller scale |

Rotations are snapped to 90 degrees. A pin pair on a diagonal would otherwise ask
for a 165.3-degree part, and the few degrees of loop cost nothing.

**`--anchor` is what makes it usable mid-placement.** The big parts land first, so
the inductor is usually down while the regulator is still in the parked pile. Given
that, "L4 is 90 mm from its slot" is true and useless - the inductor is not what
should move. So when the IC is unplaced and one of its passives is not, `ic` hangs
the whole layout off that part and reports **where the IC itself goes**.
`--anchor REF` picks a different one, `--anchor none` turns it off. Translation
only: if the anchor also needs turning, it says so and asks you to rotate the IC in
KiCad and re-run, because rotating it moves every pad.

The `now` column compares against what is already placed: `OK` (within `--tol`),
`4.8 mm / 90 deg off`, or `unplaced`. It also reports parts standing in a suggested
slot, feedback parts within `--fb` of a switching node, and the current input-loop
length against the suggested one.

**The honest limit is which caps belong to which regulator.** On a rail like `VSYS`
(20+ caps here) nothing in a `.kicad_pcb` says which cap the schematic drew next to
which pin. `ic` ranks by schematic sheet and by refdes proximity to the IC's own
private-net parts, prefers one cap of each distinct value so "smallest innermost"
means something, takes `--ncin`/`--ncout` of them, and **says in the output that it
guessed**. Two regulators on one rail can be offered the same cap. Check it against
the schematic and pass `--cin`/`--cout` once - that is the one number worth a
second call.

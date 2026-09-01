# Recipes

**"Review this board's layout"** - one call: `kpcb.py FILE review`. It ends by
naming the next calls to make, so there is nothing to work out first.

**"Did the board get updated from the schematic?"** `kpcb.py FILE sync`. Two lines
when clean. Run it before quoting any placement finding.

**"Review this placement / where should X go?"** `kpcb.py FILE summary`, then
`check`, then `map` to see the free space, then `where REF` on anything the check
named. `sheet` tells you which blocks are still scattered. Say plainly that these
are geometric heuristics.

**"Is there room for X here?"** `kpcb.py FILE where 130,60 -r 10`. One call.

**"Where do I put the caps around U13 / how do I lay out this buck?"**
`kpcb.py FILE ic U13`. One call gives positions, rotations, the reason for each and
a picture. Do not reason it out from pad coordinates by hand, and do not answer it
from the datasheet's layout figure alone - the figure does not know where this
board's inductor already is.

**"Which nets are stretched across the board?"** `kpcb.py FILE span`. Answers the
routing question that is answerable before routing exists.

**"Review this board."** `summary`, `check`, `around` each flagged IC, `divider` on
any FB/OVLO/UVLO net that check or around surfaces, then `kdoc.py grep` the
datasheet rule for anything that looks wrong. Say plainly which findings are
unconfirmed heuristics.

**"Where does X connect / is X right?"** `around X`. One call.

**"What voltage does this divider set / is this OVLO threshold right?"**
`divider REF.PIN` (the IC pin, e.g. `U5.OVLO`) or `divider NET` directly - one call
gives nominal and worst-case trip voltage from the actual resistor values and
tolerances, instead of `pin`/`net`/`comp` calls plus doing the math by hand.

**"Draw / show me / how do I wire ..."** On the board:
`knet.py FILE draw X -d 2 -o out.svg`. Not on the board yet: hand-write a ksch spec,
adding `--net FILE` if part of it exists. Then `present_files`.

**"What changed?"** `diff old.net` for wiring, `check --since old.net` for findings.

**"What does the datasheet say about Y?"** `kdoc.py grep 'Y' --count` for the page,
then grep with context, or `page` + `view` for figures.

# Self-test after editing a tool

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

The fixture has no regulator, so `ic` is exercised against the real board instead:
`kpcb.py board.kicad_pcb ic` must list the switchers, and `ic <a buck>` must name an
input cap, an inductor and a feedback part and print a diagram whose IC body is
visible inside the frame. A blank-looking frame means the anchor shift was applied
to the slots but not to the IC's own geometry.

`selftest.kicad_pcb` is a 40x30 mm fixture carrying one deliberate fault per rule,
including both `EDGECLR` severities (R4 crosses, R5 is merely close), both `CONNACC`
halves (J1 buried, J3's exit blocked by C2) and both `OVERLAP` cases (R1/R2 same
side, TP1's drill under U1 from the back). If a rule stops firing on it, that rule is
dead - kpcb's thresholds are loose enough that a clean board reports nothing, which
looks identical to a broken check.

**After editing SKILL.md or any reference file**, re-run the self-test above and
confirm the counts still match. They are the only thing that distinguishes a working
rule from a silently dead one.

#!/usr/bin/env python3
"""Merge the netlists of several root schematics into one .net.

parsnip keeps battery.kicad_sch and usb_interface.kicad_sch as their own roots
rather than as hierarchical sheets. kicad-cli exports one root at a time, so no
single export ever sees the whole board. This runs an export per root and
splices the results into one netlist that knet.py and kpcb.py can read.

    kmerge.py OUT.net ROOT.kicad_sch [ROOT2.kicad_sch ...]

Net scoping follows what kicad-cli already encodes in the name:

    GND, +3V3, I2C_HOST_SDA   no leading slash -> power symbol or global label,
                              shared across roots, so nodes merge by name
    /EN, /Rails/5V_RAW        leading slash -> local to that root's hierarchy,
                              so it is renamed /<root>/EN to avoid a false merge
    Net-(C6-Pad2)             auto-generated from a refdes, unique board-wide

Refdes collisions between roots are a hard error: two parts with one designator
cannot both reach the board.
"""

import argparse
import os
import subprocess
import sys
import tempfile

# ---------------------------------------------------------------- s-expressions


def parse(text):
    """Return a nested list. Atoms stay strings; quoted strings keep a \0 marker
    so re-serialising can tell "1" (a string) from 1 (a bare token)."""
    out, stack, i, n = [], [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "(":
            new = []
            (stack[-1] if stack else out).append(new)
            stack.append(new)
            i += 1
        elif c == ")":
            stack.pop()
            i += 1
        elif c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    buf.append(text[j + 1])
                    j += 2
                else:
                    buf.append(text[j])
                    j += 1
            (stack[-1] if stack else out).append("\0" + "".join(buf))
            i = j + 1
        elif c.isspace():
            i += 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()\"":
                j += 1
            (stack[-1] if stack else out).append(text[i:j])
            i = j
    return out[0] if len(out) == 1 else out


def dump(node, indent=0):
    pad = "  " * indent
    if isinstance(node, str):
        if node.startswith("\0"):
            return '"%s"' % node[1:].replace("\\", "\\\\").replace('"', '\\"')
        return node
    if not node:
        return "()"
    head = node[0]
    simple = all(isinstance(x, str) for x in node)
    if simple:
        return "(" + " ".join(dump(x) for x in node) + ")"
    parts = [dump(head)]
    for child in node[1:]:
        parts.append("\n" + pad + "  " + dump(child, indent + 1))
    return "(" + " ".join(parts[:1]) + "".join(parts[1:]) + "\n" + pad + ")"


def kids(node, tag):
    return [c for c in node if isinstance(c, list) and c and c[0] == tag]


def kid(node, tag):
    got = kids(node, tag)
    return got[0] if got else None


def val(node, tag):
    """First value of (tag value), with the quoted-string marker stripped."""
    c = kid(node, tag)
    if not c or len(c) < 2 or not isinstance(c[1], str):
        return None
    return c[1][1:] if c[1].startswith("\0") else c[1]


def q(s):
    return "\0" + s


# ---------------------------------------------------------------- export


def export(root, workdir):
    out = os.path.join(workdir, os.path.basename(root) + ".net")
    r = subprocess.run(
        ["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", out, root],
        capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        sys.exit("kicad-cli failed on %s:\n%s%s" % (root, r.stdout, r.stderr))
    return parse(open(out).read())


# ---------------------------------------------------------------- merge


def scope(name, prefix):
    """Rename a hierarchy-local net so two roots cannot collide on it.

    The primary root keeps prefix "/" and so is untouched; a secondary root's
    "/EN" becomes "/Charger/EN"."""
    if prefix == "/" or not name.startswith("/"):
        return name
    return prefix.rstrip("/") + name


def merge(roots):
    comps, libparts, libraries = [], {}, {}
    nets = {}          # merged name -> list of node nodes
    order = []         # merged name, first-seen order
    design_sheets = []  # (name, tstamps) in page order
    seen_ref = {}
    stats = []

    for idx, (label, root) in enumerate(roots):
        stem = label
        prefix = "/" if idx == 0 else "/%s/" % label
        with tempfile.TemporaryDirectory() as td:
            top = export(root, td)

        # design sheet records: what knet's summary groups by
        for sh in kids(kid(top, "design") or [], "sheet"):
            nm = val(sh, "name") or "/"
            design_sheets.append((prefix.rstrip("/") + nm if prefix != "/" else nm,
                                  val(sh, "tstamps") or "/"))

        block = kid(top, "components")
        n_c = 0
        for comp in (kids(block, "comp") if block else []):
            ref = val(comp, "ref")
            if ref in seen_ref:
                sys.exit("refdes collision: %s is in both %s and %s"
                         % (ref, seen_ref[ref], stem))
            seen_ref[ref] = stem
            # record which root it came from, so `sheets` stays meaningful
            sheetpath = kid(comp, "sheetpath")
            if sheetpath is not None and prefix != "/":
                names = kid(sheetpath, "names")
                if names and len(names) > 1 and isinstance(names[1], str):
                    cur = names[1][1:] if names[1].startswith("\0") else names[1]
                    names[1] = q(prefix.rstrip("/") + cur)
            comps.append(comp)
            n_c += 1

        for lp in kids(kid(top, "libparts") or [], "libpart"):
            libparts.setdefault((val(lp, "lib"), val(lp, "part")), lp)
        for lb in kids(kid(top, "libraries") or [], "library"):
            libraries.setdefault(val(lb, "logical"), lb)

        n_n = 0
        for net in kids(kid(top, "nets") or [], "net"):
            name = scope(val(net, "name") or "", prefix)
            if name not in nets:
                nets[name] = []
                order.append(name)
            nets[name].extend(kids(net, "node"))
            n_n += 1
        stats.append((stem, n_c, n_n))

    merged_nets = ["nets"]
    for code, name in enumerate(order, 1):
        merged_nets.append(["net", ["code", q(str(code))], ["name", q(name)]] + nets[name])

    design = ["design", ["source", q(os.path.abspath(roots[0][1]))],
              ["tool", q("kmerge.py")]]
    for n, (nm, ts) in enumerate(design_sheets, 1):
        design.append(["sheet", ["number", q(str(n))], ["name", q(nm)],
                       ["tstamps", q(ts)]])

    top = ["export", ["version", q("E")],
           design,
           ["components"] + comps,
           ["libparts"] + list(libparts.values()),
           ["libraries"] + list(libraries.values()),
           merged_nets]
    return top, stats, len(comps), len(order)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("roots", nargs="+", metavar="[NAME=]ROOT.kicad_sch",
                    help="first root is primary and keeps path '/'; each later "
                         "root is scoped under NAME (default: its filename stem)")
    a = ap.parse_args()

    roots = []
    for spec in a.roots:
        label, _, path = spec.rpartition("=")
        if not path:
            path = spec
        if not os.path.exists(path):
            sys.exit("no such schematic: %s" % path)
        roots.append((label or os.path.splitext(os.path.basename(path))[0], path))

    top, stats, n_comp, n_net = merge(roots)
    with open(a.out, "w") as fh:
        fh.write(dump(top) + "\n")

    for stem, c, n in stats:
        print("  %-22s %4d comps  %4d nets" % (stem, c, n))
    print("  %-22s %4d comps  %4d nets  -> %s" % ("MERGED", n_comp, n_net, a.out))


if __name__ == "__main__":
    main()

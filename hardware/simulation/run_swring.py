#!/usr/bin/env python3
"""SW-node ringing with layout parasitics, with and without the R27/C20
snubber, at 1 A and 3 A.  Writes results_swring.md.  Run: python3 run_swring.py"""
import re
import subprocess
import sys

KEYS = ["vsw_max", "vsw_min", "vpin_max", "vdrev_max", "tr_10", "tr_90", "ring_pp", "psn"]
rows = []
for ltot, (lvin, lsw, ldk, lgnd) in ((30, ("8n", "6n", "8n", "8n")), (50, ("12n", "10n", "15n", "13n")),
                                      (70, ("16n", "14n", "22n", "18n"))):
    for iload in ("1", "3"):
        for sn in ("none", "10R+470p"):
            p = dict(lvin=lvin, lsw=lsw, ldk=ldk, lgnd=lgnd, iload=iload,
                     rsn="10" if sn != "none" else "1G", csn="470p" if sn != "none" else "1f", lsn="25n",
                     title=f"L~{ltot}nH I={iload}A snubber={sn}")
            open("_sw.cir", "w").write(open("swring.cir.tpl").read().format(**p))
            out = subprocess.run([sys.executable, "ngrun.py", "_sw.cir"], capture_output=True, text=True).stdout
            v = {k: float(m.group(1)) if (m := re.search(r"^%s\s*=\s*([-+0-9.eE]+)" % k, out, re.M)) else float("nan")
                 for k in KEYS}
            print(ltot, iload, sn, v, flush=True)
            rows.append((ltot, iload, sn, v))
with open("results_swring.md", "w") as f:
    f.write("| hot-loop L (nH) | I(L) (A) | snubber | V(SW) peak (V) | V(SW) min at turn-off (V) | VIN pin peak (V) | "
            "diode reverse peak (V) | rise 10-90% (ns) | ringing pp after the edge (V) | snubber loss (W) |\n|"
            + "---|" * 10 + "\n")
    for ltot, iload, sn, v in rows:
        f.write("| %d | %s | %s | %.1f | %.2f | %.1f | %.1f | %.1f | %.1f | %.3f |\n" % (
            ltot, iload, sn, v["vsw_max"], v["vsw_min"], v["vpin_max"], v["vdrev_max"],
            (v["tr_90"] - v["tr_10"]) * 1e9, v["ring_pp"], v["psn"]))

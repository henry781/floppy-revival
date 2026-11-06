#!/usr/bin/env python3
"""Snubber robustness: snubber loop inductance 25/50 nH and a 60 V diode
(SB560, Cj ~ 300 pF assumed vs 470 pF) at the worst hot-loop case.
Writes results_swring2.md.  Run: python3 run_swring2.py"""
import re
import subprocess
import sys

KEYS = ["vsw_max", "vdrev_max", "ring_pp", "psn"]
rows = []
L70 = dict(lvin="16n", lsw="14n", ldk="22n", lgnd="18n")
L50 = dict(lvin="12n", lsw="10n", ldk="15n", lgnd="13n")
for lname, lp in (("50", L50), ("70", L70)):
    for iload in ("1", "3"):
        for lsn in ("25n", "50n"):
            for diode, cj in (("SB540-like", "470E-12"), ("SB560-like", "300E-12")):
                p = dict(lp, iload=iload, rsn="10", csn="470p", lsn=lsn, title="")
                deck = open("swring.cir.tpl").read().format(**p).replace("CJO=470E-12", "CJO=" + cj)
                deck = deck.replace("tran 0.1n 12u 7u 0.1n", "tran 0.05n 12u 7u 0.05n")
                open("_sw2.cir", "w").write(deck)
                out = subprocess.run([sys.executable, "ngrun.py", "_sw2.cir"], capture_output=True, text=True).stdout
                v = {k: float(m.group(1)) if (m := re.search(r"^%s\s*=\s*([-+0-9.eE]+)" % k, out, re.M))
                     else float("nan") for k in KEYS}
                rows.append((lname, iload, lsn, diode, v))
                print(rows[-1], flush=True)
with open("results_swring2.md", "w") as f:
    f.write("| hot-loop L (nH) | I(L) (A) | snubber loop L | diode | V(SW) peak (V) | diode reverse peak (V) | "
            "ringing pp (V) | snubber loss (W) |\n|---|---|---|---|---|---|---|---|\n")
    for l, i, ls, d, v in rows:
        f.write("| %s | %s | %s | %s | %.1f | %.1f | %.1f | %.3f |\n" % (l, i, ls, d, v["vsw_max"], v["vdrev_max"],
                                                                     v["ring_pp"], v["psn"]))

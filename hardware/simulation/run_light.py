#!/usr/bin/env python3
"""Light load on the fixed regulators: the drive idles at 0.03-0.04 A on 12V
with the spindle off (TEAC FD-55GFR spec), below TI's 100 mA test
condition.  Constant load from t=0, 120 ms runs; ripple and average over
100-120 ms, compared with 80-90 ms to prove the output has settled (the
first version of this study read the start-up decay as ripple).  Writes
results_light.md.  Run: python3 run_light.py"""
import re
import subprocess
import sys

from run_rails import R12, R5

CTRL = """.control
tran 20n 120m 0 20n
meas tran rip_j5 pp v(j5) from=100m to=120m
meas tran rip_co pp v(out1) from=100m to=120m
meas tran vj5 avg v(j5) from=100m to=120m
meas tran vj5_early avg v(j5) from=80m to=90m
.endc"""


def main():
    rows = []
    for rail, base, loads in (("12V", R12, ("0.02", "0.04", "0.1")), ("5V", R5, ("0.05", "0.2"))):
        for il in loads:
            p = dict(base, vin="19.5", ilo=il, ihi=il, title=f"{rail} light {il}A")
            deck = re.sub(r"\.control.*\.endc", CTRL, open("rail.cir.tpl").read().format(**p), flags=re.S)
            open("_light.cir", "w").write(deck)
            out = subprocess.run([sys.executable, "ngrun.py", "_light.cir"], capture_output=True, text=True).stdout
            v = {k: float(x) for k, x in re.findall(r"^(\w+)\s*=\s*([-+0-9.eE]+)", out, re.M)}
            rows.append((rail, il, v.get("rip_j5", float("nan")), v.get("rip_co", float("nan")),
                         v.get("vj5", float("nan")), v.get("vj5_early", float("nan"))))
            print(rows[-1], flush=True)
    with open("results_light.md", "w") as f:
        f.write("Fixed regulators, VIN 19.5V, constant load, window 100-120 ms (run_light.py)\n\n"
                "| Rail | Load (A) | ripple J5 (mVpp) | ripple Cout (mVpp) | V(J5) 100-120 ms | V(J5) 80-90 ms |\n"
                "|---|---|---|---|---|---|\n")
        for r in rows:
            f.write("| %s | %s | %.2f | %.1f | %.4f | %.4f |\n" % (r[0], r[1], r[2] * 1e3, r[3] * 1e3, r[4], r[5]))


if __name__ == "__main__":
    main()

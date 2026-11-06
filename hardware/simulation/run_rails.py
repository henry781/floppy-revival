#!/usr/bin/env python3
"""Rail simulations (12V and 5V) over input voltage and load cases.
Writes results_rails.md.  Run: python3 run_rails.py"""
import re
import subprocess
import sys

R12 = dict(gm1="1.54m", rfb2="22.19k", ron="0.12", ilim="7", cout="470u", esro="0.030", esrf="0.080", rtrk="0.030")
R5 = dict(gm1="0.673m", rfb2="7.79k", ron="0.15", ilim="4.5", cout="470u", esro="0.056", esrf="0.130", rtrk="0.030")
CASES = []
for v in ("18", "19.5", "21"):
    CASES.append(("12V", f"VIN {v}V, idle 0.22A -> spindle 1.0A", dict(R12, vin=v, ilo="0.22", ihi="1.0")))
CASES += [("12V", "VIN 19.5V, idle 0.22A -> design margin 2.9A", dict(R12, vin="19.5", ilo="0.22", ihi="2.9")),
          ("12V", "VIN 19.5V, light 0.05A -> 0.22A", dict(R12, vin="19.5", ilo="0.05", ihi="0.22"))]
for v in ("18", "19.5", "21"):
    CASES.append(("5V", f"VIN {v}V, 0.38A -> peak 0.46A", dict(R5, vin=v, ilo="0.38", ihi="0.46")))
CASES += [("5V", "VIN 19.5V, 0.38A -> design margin 2.0A", dict(R5, vin="19.5", ilo="0.38", ihi="2.0")),
          ("5V", "VIN 19.5V, light 0.05A -> 0.38A", dict(R5, vin="19.5", ilo="0.05", ihi="0.38"))]
KEYS = ["vj5", "rip_j5", "rip_co", "vmin_step", "vj5_hi", "rip_j5_hi", "vmax_rel", "il_startup", "il_hi", "vmax_start"]


def run(params, title):
    deck = open("rail.cir.tpl").read().format(title=title, **params)
    open("_rail.cir", "w").write(deck)
    out = subprocess.run([sys.executable, "ngrun.py", "_rail.cir"], capture_output=True, text=True).stdout
    vals = {}
    for k in KEYS:
        m = re.search(r"^%s\s*=\s*([-+0-9.eE]+)" % k, out, re.M)
        vals[k] = float(m.group(1)) if m else float("nan")
    return vals


def main():
    rows = []
    for rail, title, p in CASES:
        v = run(p, f"{rail} {title}")
        print(rail, title, v, flush=True)
        rows.append((rail, title, v))
    with open("results_rails.md", "w") as f:
        f.write("| Rail | Case | V(J5) idle | ripple J5 idle (mVpp) | ripple Cout (mVpp) | min V(J5) after step | "
                "V(J5) loaded | ripple J5 loaded (mVpp) | max V(J5) on release | I(L) peak start-up (A) | "
                "I(L) peak loaded (A) | max V(J5) start-up |\n|" + "---|" * 12 + "\n")
        for rail, title, v in rows:
            f.write("| %s | %s | %.3f | %.2f | %.1f | %.3f | %.3f | %.2f | %.3f | %.2f | %.2f | %.3f |\n" % (
                rail, title, v["vj5"], v["rip_j5"] * 1e3, v["rip_co"] * 1e3, v["vmin_step"], v["vj5_hi"],
                v["rip_j5_hi"] * 1e3, v["vmax_rel"], v["il_startup"], v["il_hi"], v["vmax_start"]))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Switch-off then restart: how long until the switched bus is below the
regulators' UVLO (7.9V), so that a re-enable starts through Q1's ramp (soft)
instead of a hard ON/OFF enable into a charged bulk (12.66V / 5.68V
overshoot with the fixed parts, enable.cir.tpl).  Averaged chain model,
GP18 low at 300 ms, R26 4k7 vs 2k2.  Writes results_off.md.
Run: python3 run_off.py"""
import re
import subprocess
import sys

import run_chain as rc

MEAS = """meas tran t_q1_off when vgs=-3 rise=1
meas tran t_bus_uvlo when v(v19sw)=7.9 fall=1 from=300m
meas tran t_bus_5 when v(v19sw)=5 fall=1 from=300m
meas tran t12_off when v(j51)=1 fall=1 from=300m
meas tran t5_off when v(j54)=0.5 fall=1 from=300m
meas tran vsw_1s find v(v19sw) at=1.3
meas tran vsw_3s find v(v19sw) at=3.3"""


def main():
    rows = ["| R26 | Q1 off (Vgs > -3V) | 12V < 1V | 5V < 0.5V | bus at +1 s | bus at +3 s | bus < 7.9V (UVLO) | bus < 5V |",
            "|---|---|---|---|---|---|---|---|"]
    for r26, tstop in (("4.7k", "7"), ("2.2k", "4")):
        p = dict(rc.BASE, avg=1, tplug="0", tplug_e="1u", ton="1m", ton_e="1.001m", toff="300m", toff_e="300.001m",
                 tstop=tstop, t_end_a="290m", meas=MEAS)
        tpl = open("chain_avg.cir.tpl").read()
        assert "R26 v19sw 0 4.7k" in tpl
        open("_off.cir", "w").write(tpl.replace("R26 v19sw 0 4.7k", "R26 v19sw 0 " + r26).format(title="", **p))
        out = subprocess.run([sys.executable, "ngrun.py", "_off.cir"], capture_output=True, text=True).stdout
        v = {k: float(x) for k, x in re.findall(r"^(\w+)\s*=\s*([-+0-9.eE]+)", out, re.M)}
        g = lambda k, off=0.3: ("%.2f s" % (v[k] - off)) if k in v else "-"
        rows.append("| %s | %s | %s | %s | %.2f V | %.2f V | %s | %s |" % (
            r26, g("t_q1_off"), g("t12_off"), g("t5_off"), v.get("vsw_1s", float("nan")),
            v.get("vsw_3s", float("nan")), g("t_bus_uvlo"), g("t_bus_5")))
        print(rows[-1], flush=True)
    open("results_off.md", "w").write("Switch-off at 300 ms, times after GP18 low (run_off.py)\n\n" + "\n".join(rows) + "\n")


if __name__ == "__main__":
    main()

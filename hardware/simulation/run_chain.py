#!/usr/bin/env python3
"""Full drive power chain: switch-on (inrush, ramp, Q1 stress, rail start-up),
switch-off (time to a dead drive) and adapter hot-plug with GP18 low.
Writes results_chain.md.  Run: python3 run_chain.py"""
import re
import subprocess
import sys

BASE = dict(c11="10u", c22="1u", rload12="54.5", rload5="13.2")   # idle drive: 0.22 A / 0.38 A
ON = """meas tran iad_pk max iad from={ton} to={tstop}
meas tran t10 when v(v19sw)=1.95 rise=1
meas tran t90 when v(v19sw)=17.55 rise=1
meas tran p_q1_pk max pq1 from={ton} to={tstop}
meas tran e_q1 integ pq1 from={ton} to={tstop}
meas tran il1_pk max i(L1) from={ton} to={tstop}
meas tran il3_pk max i(L3) from={ton} to={tstop}
meas tran il5_pk max i(L5) from={ton} to={tstop}
meas tran t12 when v(j51)=11.4 rise=1
meas tran t5 when v(j54)=4.75 rise=1
meas tran v12_end avg v(j51) from={t_end_a} to={tstop}
meas tran v5_end avg v(j54) from={t_end_a} to={tstop}
meas tran v12_max max v(j51) from={ton} to={tstop}
meas tran v5_max max v(j54) from={ton} to={tstop}
meas tran vgs_min min vgs from={ton} to={tstop}
meas tran t_on when vgs=-3.5 fall=1"""
OFF = """meas tran vsw_on find v(v19sw) at=299m
meas tran t_sw_off when v(v19sw)=1 fall=1 from=300m
meas tran t12_off when v(j51)=1 fall=1 from=300m
meas tran t5_off when v(j54)=0.5 fall=1 from=300m
meas tran id_q1_after max iad from=1.2 to={tstop}
meas tran t_q1_off when vgs=-3 rise=1
meas tran vsw_at_1s find v(v19sw) at=1.3
meas tran id_q1_end find iad at=1.45
meas tran v12_end2 find v(j51) at=1.45
meas tran v5_end2 find v(j54) at=1.45"""
PLUG = """meas tran vgs_plug_min min vgs from={tplug} to={tstop}
meas tran il5_plug max i(L5) from={tplug_s} to={tstop}
meas tran vsw_plug max v(v19sw) from={tplug} to={tstop}
meas tran vj4_pk max v(j4) from={tplug} to={tstop}
meas tran vf_pk max v(v19f) from={tplug} to={tstop}
meas tran drven_max max v(drven) from={tplug} to={tstop}
meas tran v12_plug max v(j51) from={tplug} to={tstop}
meas tran v5_plug max v(j54) from={tplug} to={tstop}"""
CASES = [
    ("Switch-on: adapter present, GP18 raised at 5 ms (idle drive)",
     dict(BASE, tplug="0", tplug_e="1u", ton="5m", ton_e="5.001m", toff="1", toff_e="1.001", tstop="200m",
          t_end_a="190m", meas=ON)),
    ("Switch-on, X7R DC-bias derated gate caps (C11 6uF, C22 0.7uF)",
     dict(BASE, c11="6u", c22="0.7u", tplug="0", tplug_e="1u", ton="5m", ton_e="5.001m", toff="1", toff_e="1.001",
          tstop="200m", t_end_a="190m", meas=ON)),
    ("Switch-off: GP18 driven low at 300 ms after a normal start",
     dict(BASE, avg=1, tplug="0", tplug_e="1u", ton="1m", ton_e="1.001m", toff="300m", toff_e="300.001m",
          tstop="1.5", t_end_a="290m", meas=OFF)),
    ("Hot-plug: adapter plugged at 1 ms with GP18 low (Q1 must stay off)",
     dict(BASE, tplug="1m", tplug_e="1.001m", tplug_s="1.0005m", ton="10", ton_e="10.001", toff="20", toff_e="20.001",
          tstop="20m", t_end_a="15m", meas=PLUG)),
]


def run(p, tpl=None):
    p = dict(p)
    p["meas"] = p["meas"].format(**p)
    tpl = tpl or ("chain_avg.cir.tpl" if p.get("avg") else "chain.cir.tpl")
    open("_chain.cir", "w").write(open(tpl).read().format(title="", **p))
    out = subprocess.run([sys.executable, "ngrun.py", "_chain.cir"], capture_output=True, text=True).stdout
    return dict(re.findall(r"^(\w+)\s*=\s*([-+0-9.eE]+)", out, re.M)), out


def main(cases=CASES, tpl=None, results="results_chain.md"):
    with open(results, "w") as f:
        for title, p in cases:
            vals, out = run(p, tpl)
            print(title, vals, flush=True)
            if not vals:
                print(out[-2000:])
            f.write("### %s\n\n" % title)
            for k, v in vals.items():
                f.write("- %s = %s\n" % (k, v))
            f.write("\n")


if __name__ == "__main__":
    main()

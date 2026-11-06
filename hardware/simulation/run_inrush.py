#!/usr/bin/env python3
"""Switch-on current profile for two gate networks: time above an adapter's
rating.  Writes results_inrush.md.  Run: python3 run_inrush.py"""
import re
import subprocess
import sys

src = open("run_chain.py").read()
ns = {}
exec(src.split("\ndef run(p):")[0], ns)
base = [c for c in ns["CASES"] if c[0].startswith("Switch-on: adapter")][0][1]
rows = []
for c11, c22, tstop in (("4.7u", "470n", "100m"), ("10u", "1u", "160m")):
    p = dict(base, c11=c11, c22=c22, tstop=tstop, t_end_a="%gm" % (float(tstop[:-1]) - 5))
    p["meas"] = """meas tran iad_pk max iad from=5m to={tstop}
meas tran t_ramp10 when v(v19sw)=1.95 rise=1
meas tran t_ramp90 when v(v19sw)=17.55 rise=1
meas tran p_q1_pk max pq1 from=5m to={tstop}
meas tran e_q1 integ pq1 from=5m to={tstop}
let over231 = iad > 2.31
let over334 = iad > 3.34
meas tran t_over_231 integ over231 from=5m to={tstop}
meas tran t_over_334 integ over334 from=5m to={tstop}
meas tran t12 when v(j51)=11.4 rise=1
meas tran v12_max max v(j51) from=5m to={tstop}
meas tran v5_max max v(j54) from=5m to={tstop}""".format(tstop=tstop)
    open("_inr.cir", "w").write(open("chain.cir.tpl").read().format(title="", **p))
    out = subprocess.run([sys.executable, "ngrun.py", "_inr.cir"], capture_output=True, text=True).stdout
    v = dict(re.findall(r"^(\w+)\s*=\s*([-+0-9.eE]+)", out, re.M))
    rows.append((c11, c22, v))
    print(c11, c22, v, flush=True)
with open("results_inrush.md", "w") as f:
    for c11, c22, v in rows:
        f.write("### C11 %s, C22 %s\n\n" % (c11, c22) + "".join("- %s = %s\n" % kv for kv in v.items()) + "\n")

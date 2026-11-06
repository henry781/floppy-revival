#!/usr/bin/env python3
"""Averaged CCM small-signal loop gain of the LM267x-ADJ rails.

TI's error amplifier and compensation exactly as in models/lm267x_sw.lib, PWM
ramp 0.6..3.2V, the output network as built (470uF, 4.7uH + 220uF post
filter, soft-start network on FB), loop broken at the divider top (voltage
injection into the 470uF node).  Prints fc, phase margin, gain margin and
the minimum phase below fc (conditional stability) for the nominal design
and its corners: ESR spread (polymer 10m .. cold x10), GM1 -30%, VIN 18-21V,
load 0.3-2.9A.  Writes results_loop.md.  Run: python3 run_loop.py"""
import math, subprocess, sys
DECK = """* averaged loop: {title}
.model DCL D
.model D4148 D(IS=2.52n RS=0.568 N=1.752 CJO=4p M=0.4 TT=20n BV=100)
Vin vin 0 {vin}
Vref refv 0 1.215
Gm1 0 n15 fb refv {gm1}
Bgm2 0 n17 I = v(refv,n15) * v(gm2)
Bgm2v gm2 0 V = max(0.48m, min(1.48m, v(vin) < 10 ? 1.48m : (v(vin) < 20 ? 1.48m - (v(vin)-10)*0.0807m : 0.673m - (v(vin)-20)*0.00965m)))
R3 refv n16 10k
R4 n15 n16 2k
Lc n16 refv 20m
R5 n17 n18 15k
C1 n18 0 10n
C6 n17 n18 105p
R10 n17 0 300k
D1 n17 r5v DCL
D2 vcl n17 DCL
Vr5 r5v 0 5
Vcl vcl 0 0.9
Berr err 0 V = 0.8*v(n17)
Bd d 0 V = min(0.91, max(0, (v(err)-0.6)/2.6))
Bsw swa 0 V = v(d)*(v(vin) - {ron}*i(Vil)) - (1-v(d))*0.45
Vil swa swb 0
Lo swb l1o 33u
RL1 l1o out1 24.5m
COUT out1 co {cout}
RCO co 0 {esro}
{post}
Iload j5 0 {iload}
Vinj y out1 AC 1
RTOP y fb {rtop}
CFF y fb {cff}
RBOT fb 0 {rbot}
{ss}
.control
set units=degrees
op
print v(out1) v(j5) v(d)
ac dec 200 10 1meg
let T = -v(out1)/v(y)
wrdata _loop.dat vdb(T) vp(T)
.endc
.end
"""
POST = "L2 out1 l2o 4.7u\nRL2 l2o j5 0.053\nCF j5 cf 220u\nRCF cf 0 {esrf}"
NOPOST = "Rj out1 j5 1m"
SS = "CSS out1 m 1u\nDSS m fb D4148\nDRS 0 m D4148\nRDS m 0 10k"
B12 = dict(cff="1f", vin="19.5", gm1="0.673m", ron="0.12", cout="470u", esro="0.030", esrf="0.080", iload="1.0", rtop="9.09k", rbot="1.02k", post=POST, ss=SS)
B5 = dict(cff="1f", vin="19.5", gm1="0.673m", ron="0.15", cout="470u", esro="0.056", esrf="0.130", iload="0.42", rtop="3.16k", rbot="1.0k", post=POST, ss=SS)
B12 = dict(B12, cff="2.2n")
B5 = dict(B5, rtop="4.02k", rbot="1.28k")
CASES = [
    ("12V without C_FF (first proposal)", dict(B12, cff="1f")),
    ("12V without C_FF, ESR 10m", dict(B12, cff="1f", esro="0.010")),
    ("12V C_FF 2.2n, nominal 1A", B12),
    ("12V C_FF 2.2n, 0.3A", dict(B12, iload="0.3")),
    ("12V C_FF 2.2n, 2.9A", dict(B12, iload="2.9")),
    ("12V C_FF 2.2n, VIN 18", dict(B12, vin="18")),
    ("12V C_FF 2.2n, VIN 21", dict(B12, vin="21")),
    ("12V C_FF 2.2n, ESR 10m (polymer)", dict(B12, esro="0.010")),
    ("12V C_FF 2.2n, 2x470uF / 15m", dict(B12, cout="940u", esro="0.015")),
    ("12V C_FF 2.2n, ESR 90m (cold x3)", dict(B12, esro="0.090")),
    ("12V C_FF 2.2n, ESR 300m (cold x10), post 0.8", dict(B12, esro="0.300", esrf="0.8")),
    ("12V C_FF 2.2n, GM1 -30%, ESR 10m", dict(B12, gm1="0.47m", esro="0.010")),
    ("12V C_FF 2.2n, GM1 +30%", dict(B12, gm1="0.875m")),
    ("12V C_FF 2.2n, -20% C, ESR 90m", dict(B12, cout="376u", esro="0.090")),
    ("12V C_FF 4.7n, ESR 300m (cold x10)", dict(B12, cff="4.7n", esro="0.300", esrf="0.8")),
    ("5V nominal 0.42A (4.02k/1.28k)", B5),
    ("5V 2A", dict(B5, iload="2.0")),
    ("5V ESR 10m", dict(B5, esro="0.010")),
    ("5V ESR 280m (cold x5)", dict(B5, esro="0.280", esrf="0.65")),
    ("5V GM1 -30%", dict(B5, gm1="0.47m")),
    ("5V GM1 +50%", dict(B5, gm1="1.0m")),
]


def analyse():
    f, db, ph = [], [], []
    for line in open("_loop.dat"):
        p = line.split()
        if len(p) < 4: continue
        f.append(float(p[0])); db.append(float(p[1])); ph.append(float(p[3]))
    # unwrap phase
    u = [ph[0]]
    for i in range(1, len(ph)):
        d = ph[i] - ph[i-1]
        if d > 180: d -= 360
        if d < -180: d += 360
        u.append(u[-1] + d)
    # reference: at 10 Hz the loop is an integrator-ish; expect about -90.. choose branch so low-f phase is in (-180, 0]
    off = 0
    while u[0] + off > 0: off -= 360
    while u[0] + off <= -180: off += 360
    u = [x + off for x in u]
    fc = pm = None
    for i in range(1, len(db)):
        if db[i-1] >= 0 > db[i]:
            t = db[i-1] / (db[i-1] - db[i]); fc = f[i-1] * (f[i]/f[i-1])**t; pm = 180 + (u[i-1] + t*(u[i]-u[i-1])); break
    gm = f180 = None
    for i in range(1, len(u)):
        if u[i-1] > -180 >= u[i]:
            t = (u[i-1] + 180) / (u[i-1] - u[i]); f180 = f[i-1] * (f[i]/f[i-1])**t; gm = -(db[i-1] + t*(db[i]-db[i-1])); break
    # worst (minimum) phase above the first crossover among points with gain > -6dB (conditional-stability check)
    pmin = min((u[i] for i in range(len(f)) if fc and f[i] <= fc), default=None)
    return fc, pm, f180, gm, pmin, min(u)
rows = ["| case | Vout | fc (kHz) | PM (deg) | f(-180) (kHz) | GM (dB) | min phase below fc |",
        "|---|---|---|---|---|---|---|"]
print("\n".join(rows))
for title, p in CASES:
    open("_loop.cir", "w").write(DECK.format(title=title, **p).replace("{esrf}", p["esrf"]))
    out = subprocess.run([sys.executable, "ngrun.py", "_loop.cir"], capture_output=True, text=True).stdout
    vo = [l for l in out.splitlines() if l.startswith("v(out1)")]
    try:
        fc, pm, f180, gm, pmin, umin = analyse()
    except Exception as e:
        print(title, "FAILED", e, out[-800:]); continue
    g = lambda x, s=1e-3: ("%.2f" % (x*s)) if x is not None else "-"
    rows.append("| %s | %s | %s | %s | %s | %s | %s |" % (title, vo[0].split("=")[1].strip()[:6] if vo else "?", g(fc),
                                                         g(pm, 1), g(f180), g(gm, 1), g(pmin, 1)))
    print(rows[-1], flush=True)
open("results_loop.md", "w").write("Averaged loop gain, LM267x-ADJ rails (run_loop.py)\n\n" + "\n".join(rows) + "\n")

* Rev C drive power chain: adapter -> F1 -> Q1 (C11 G-S, C22 G-D) -> C8 -> L5 -> bulk -> U3/U4 -> post-filters -> J5
* {title}
.include models/lm267x_sw.lib
.include models/irf9540n.spi
.model BC547B NPN(IS=1.8E-14 ISE=5E-14 NF=0.9955 NE=1.46 BF=400 BR=35.5 IKF=0.14 IKR=0.03 ISC=1.72E-13 NC=1.27
+ NR=1.005 RB=0.56 RE=0.6 RC=0.25 VAF=80 VAR=12.5 CJE=13E-12 TF=6.4E-10 CJC=4E-12 TR=5E-8 VJC=0.54 MJC=0.33)
.model DSCH D(BV=40 CJO=470E-12 EG=0.69 IBV=1E-6 IS=9E-6 M=0.8 N=1.442 RS=60E-3 VJ=1 XTI=2)
.model SWPLUG SW(VT=0.5 VH=0.1 RON=0.01 ROFF=100MEG)
* adapter (19.5V, ~50 mohm) plugged at t_plug through ~2 m of cable
VAD ad 0 19.5
RAD ad ad1 0.05
SPLUG ad1 ad2 plug 0 SWPLUG
VPLUG plug 0 PWL(0 0 {tplug} 0 {tplug_e} 1)
LCAB ad2 ad3 1.5u
RCAB ad3 j4 0.05
CJ4 j4 0 1n
RF1 j4 v19f 0.06
* D3 P6KE27A input TVS (VBR 25.7-28.4V, clamp 37.5V at 16A)
D3 0 v19f DTVS27
.model DTVS27 D(IS=1e-12 N=1.5 RS=0.6 BV=27 IBV=1m CJO=2.5n VJ=0.75 M=0.33)
* Q1 high side (drain = 19V5_SW, source = 19V5_F), gate network
XQ1 v19sw q1g v19f irf9540n
R24 q1g v19f 47k
C11 q1g v19f {c11}
C22 q1g v19sw {c22}
R25 q1g q2c 22k
XQ2 q2c q2b 0 BC547B
.subckt BC547B c b e
Q c b e BC547B
.ends
R22 gp18 q2b 10k
* regulator ON/OFF (DRV_EN): R29 1k from GP18, R30 4k7 pull-down, C23 10nF, internal pull-ups 2 x 20uA
R29 gp18 drven 1k
R30 drven 0 4.7k
C23 drven 0 10n
IPU 0 drven 40u
R23 q2b 0 100k
VGP gp18 0 PWL(0 0 {ton} 0 {ton_e} 3.3 {toff} 3.3 {toff_e} 0)
C8 v19sw c8 100u
RC8 c8 0 0.15
R26 v19sw 0 4.7k
RL5 v19sw l5a 0.023
L5 l5a vb 4.7u
C12 vb c12 470u
RC12 c12 0 0.03
C16 vb c16 470u
RC16 c16 0 0.03
C13 vb c13 1u
RC13 c13 0 5m
C17 vb c17 1u
RC17 c17 0 5m
* regulators as their input power (idle drive: 12V 0.22A, 5V 0.38A, ~90%/85% efficiency):
* 12V regulates above ~13.5V, tracks 0.91*Vin in dropout, both stop below UVLO 7.9V
* smooth enables (ON/OFF at 1.4V, UVLO at 7.9V) so the solver never meets a step
Ben en 0 V = 0.5*(1 + tanh((v(drven) - 1.4)/0.05)) * 0.5*(1 + tanh((v(vb) - 7.9)/0.2))
B12 vb 0 I = v(en) * min(2.93, (0.91*v(vb))**2/54.5/0.9) / max(v(vb), 1)
B5 vb 0 I = v(en) * 2.24 / max(v(vb), 1)
B12o j51 0 V = v(en) * min(12, 0.91*v(vb))
B5o j54 0 V = v(en) * min(5, 0.91*v(vb))
.options method=gear reltol=1e-3 abstol=1e-9 vntol=1e-6 itl4=300
.control
tran 1u {tstop} 0 20u
let vgs = v(q1g) - v(v19f)
let iad = -i(VAD)
let pq1 = (v(v19f) - v(v19sw)) * iad
{meas}
.endc
.end

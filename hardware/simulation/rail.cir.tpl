* Rev C drive rail: LM267x (switching model, TI compensation) -> L -> Cout -> post-filter -> J5
* {title}
.include models/lm267x_sw.lib
VIN in0 0 PWL(0 0 2m {vin})
RL5 in0 in1 0.023
L5 in1 vin 4.7u
CBULK vin cb 470u
RCB cb 0 0.03
CCER vin cc 1u
RCC cc 0 5m
X1 vin out1 sw 0 LM267X_SW PARAMS: GM1={gm1} RFB2={rfb2} RON={ron} ILIM={ilim}
DCATCH 0 sw DSCH
.model DSCH D(BV=40 CJO=470E-12 EG=0.69 IBV=1E-6 IS=9E-6 M=0.8 N=1.442 RS=60E-3 VJ=1 XTI=2)
L1 sw l1o 33u
RL1 l1o out1 24.5m
COUT out1 co {cout}
RCO co coe {esro}
LCO coe 0 5n
L2 out1 l2o 4.7u
RL2 l2o j5r 23m
RTRK j5r j5 {rtrk}
CF j5 cf 220u
RCF cf cfe {esrf}
LCF cfe 0 5n
CHF j5 0 100n
ILOAD j5 0 PWL(0 0 8m 0 8.01m {ilo} 20m {ilo} 20.001m {ihi} 26m {ihi} 26.001m {ilo})
.options method=gear reltol=1e-3 abstol=1e-9 vntol=1e-6 itl4=200
.control
tran 10n 30m 0 20n
meas tran vj5 avg v(j5) from=17m to=19.9m
meas tran rip_j5 pp v(j5) from=17m to=19.9m
meas tran rip_co pp v(out1) from=17m to=19.9m
meas tran vmin_step min v(j5) from=20m to=23m
meas tran vj5_hi avg v(j5) from=24m to=25.9m
meas tran rip_j5_hi pp v(j5) from=24m to=25.9m
meas tran vmax_rel max v(j5) from=26m to=29m
meas tran il_startup max i(L1) from=0 to=8m
meas tran il_hi max i(L1) from=20m to=26m
meas tran vmax_start max v(j5) from=0 to=8m
.endc
.end

* Rail start by ON/OFF with the input already at {vin}V (Q1 as automatic limiter): {title}
.include models/lm267x_sw.lib
VIN vin 0 {vin}
CBULK vin cb 470u
RCB cb 0 0.03
VON on 0 PWL(0 0 2m 0 2.0001m 2.7)
X1 vin out1 sw 0 on LM267X_SWEN PARAMS: GM1={gm1} RFB2={rfb2} RON={ron} ILIM={ilim}
DCATCH 0 sw DSCH
.model DSCH D(BV=60 CJO=300E-12 EG=0.69 IBV=1E-6 IS=9E-6 M=0.8 N=1.442 RS=60E-3 VJ=1 XTI=2)
L1 sw l1o 33u
RL1 l1o out1 24.5m
COUT out1 co 470u
RCO co 0 {esro}
L2 out1 l2o 4.7u
RL2 l2o j5 0.053
CF j5 cf 220u
RCF cf 0 {esrf}
RLOAD j5 0 {rload}
.options method=gear reltol=1e-3 abstol=1e-9 vntol=1e-6 itl4=200
.control
tran 10n 14m 0 20n
meas tran il_pk max i(L1) from=2m to=14m
meas tran vmax max v(j5) from=2m to=14m
meas tran t_reg when v(j5)={vreg} rise=1
meas tran vend avg v(j5) from=12m to=14m
.endc
.end

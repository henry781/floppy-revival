* SW node ringing at the switch turn-on (diode turn-off), {title}
.include models/lm2678_fet.lib
.model DSCH D(BV=40 CJO=470E-12 EG=0.69 IBV=1E-6 IS=9E-6 M=0.8 N=1.442 RS=60E-3 VJ=1 XTI=2)
VIN vbulk 0 19.5
* bulk (C12) to the ceramic through the 19V5_BUCK track
LBULK vbulk vcer 20n
CCER vcer cc 1u
RCC cc lcc 5m
LCC lcc gnd_c 2n
* hot loop: ceramic -> VIN pin (track + TO-220 lead), switch, SW pin -> diode, diode anode -> ceramic GND (B.Cu tie)
LVIN vcer vpin {lvin}
XSW vpin gate swpin NATIONALFET5A_0
* gate drive as in TI's model (10V on, through the FET's internal 50 ohm), 2 ns edges
VG gate swpin PULSE(0 10 1u 2n 2n 2.6u 3.8u)
LSW swpin swd {lsw}
DCATCH dk swd DSCH
LDK gnd_d dk {ldk}
LGND gnd_d gnd_c {lgnd}
RGNDREF gnd_c 0 1m
* inductor current (L1) as a source during the edge
IL swd gnd_c {iload}
* optional snubber R27 + C20 across the diode, with its own loop inductance
RSN swd snr {rsn}
CSN snr snc {csn}
LSN snc gnd_d {lsn}
.options method=gear reltol=1e-4 abstol=1e-10 vntol=1e-6 itl4=500
.control
tran 0.1n 12u 7u 0.1n
let vsw = v(swd) - v(gnd_c)
let vpin = v(vpin) - v(gnd_c)
let vdrev = v(swd) - v(dk)
let psnv = i(lsn) * i(lsn) * {rsn}
meas tran vsw_max max vsw from=8.5u to=9.3u
meas tran vsw_min min vsw from=10.9u to=11.6u
meas tran vpin_max max vpin from=8.5u to=9.3u
meas tran vdrev_max max vdrev from=8.5u to=9.3u
meas tran tr_10 when vsw=2 rise=1 from=8.5u
meas tran tr_90 when vsw=17.5 rise=1 from=8.5u
meas tran ring_pp pp vsw from=8.75u to=8.9u
meas tran psn avg psnv from=7.6u to=11.4u
.endc
.end

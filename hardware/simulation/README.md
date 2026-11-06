# Power stage simulations

ngspice decks used to size the drive supply (19.5 V in, 12 V and 5 V out).
The power stage was designed by simulation and calculation before it was
built, so most of the power numbers in `docs/hardware.md` come from here.
They are model results, some from simplified models (see "Model limits"),
and are cross-checked on the built board where a measurement exists.

`ngrun.py` loads the libngspice that ships with KiCad, so nothing else has
to be installed. The path in it is the macOS one; change `LIB` and
`SPICE_LIB_DIR` on Linux or Windows. Run any `run_*.py` from this folder;
each one writes its `results_*.md`.

## Models

| File | In the repo | What it is |
|---|---|---|
| `models/lm267x_sw.lib` | yes | Switching model of the LM2678 / LM2676 written for ngspice. It keeps the error amplifier, internal compensation, Vin-dependent GM2, clamps, RC oscillator and switch Rds(on) of TI's LM2678 PSpice model (SNVM880). Forced-off time from the datasheet's 91 % maximum duty, UVLO 7.9 V, smooth current clamp (LM2678 7 A, LM2676 4.5 A typical). |
| `models/lm2678_fet.lib` | no | TI's internal power switch subcircuit (`NATIONALFET5A_0`), used only by the ringing study. Download the LM2678 PSpice transient model from <https://www.ti.com/product/LM2678> and copy its `.SUBCKT NATIONALFET5A_0` ... `.ENDS` block into this file. |
| `models/irf9540n.spi` | no | IRF9540N model (Symmetry Design Systems / International Rectifier), used by the switch-on and switch-off decks. Its licence does not allow redistribution. A copy is in the KiCad-Spice-Library, <https://github.com/kicad-spice-library/KiCad-Spice-Library>. |

TI's model is not used directly because it does not converge in ngspice
at the first switching edge (discontinuous IF sources); the same
behaviour was rebuilt with smooth functions.

## Model limits

- TI publishes no LM2676 model. The 5 V rail uses the same core with the
  LM2676-5.0 transconductance (GM1 0.673 mS), its internal divider and a
  0.15 ohm Rds(on). This is an assumption.
- The current limit is a smooth clamp: peak currents at the limit are
  right, the dissipation during a limit event is not.
- Leakage is not modelled. On the built board the 5 V rail with no load
  at all creeps up to about 6.1 V within seconds, which the simulation
  could not show. R33 (470 ohm minimum load) was added for that reason.

## Studies

| Study | Files | Result |
|---|---|---|
| Ripple and load steps, both rails, VIN 18 to 21 V | `rail.cir.tpl`, `run_rails.py` | Ripple at J5 under 1.5 mVpp (targets 50 / 20 mV). 12 V, 0.22 to 2.9 A step: minimum 11.57 V. 5 V, 0.38 to 2 A step: 4.73 V. |
| Light load (spindle off, 20 to 50 mA) | `run_light.py` | Under 1 mVpp at J5 once settled. Measure at least three time constants after the last event, or the window shows the start-up decay. |
| Input pi filter (Middlebrook criterion) | `filter.cir` | Filter output impedance peaks at 0.30 ohm, against at least 6.5 ohm converter input impedance. |
| Adapter plugged straight into the input bank, no switch | `plug_noq1.cir` | Why the input is switched and soft-started at all. |
| Switch-on, both converters running | `chain.cir.tpl`, `run_chain.py` | With C11 4.7 uF / C22 470 nF: 2.95 A adapter peak, 16 ms ramp. Adapter hot-plug with GP18 low: D3 clamps the cable ring to 30 V, Q1 Vgs stays at -2.7 V, the regulators never start. |
| Softer gate network (values on the board) | `run_inrush.py` | C11 10 uF / C22 1 uF: 1.86 A adapter peak, 31 ms ramp, rails up 66 ms after GP18. |
| Switch-off | `chain_avg.cir.tpl`, `run_off.py` | Q1 alone leaves the bus at the 7.9 V lockout with 5 V still on and about 2.5 W in Q1. Driving the regulators' ON/OFF pins stops the rails at once; R26 bleeds the input below lockout in 2.6 s. |
| Enable into a charged input | `enable.cir.tpl` | The fixed regulators have no soft start: 12.66 V / 5.68 V peak. Avoided by the 3.5 s off time the firmware enforces before re-enabling. |
| Switch-node ringing, 30 to 70 nH hot loop | `swring.cir.tpl`, `run_swring.py`, `run_swring2.py` | Without snubber the catch diode sees 31.5 to 40.4 V reverse; with 10 ohm + 470 pF, 20 to 35 V and no ringing. Hence the snubbers and the 60 V diodes. |
| Loop gain (averaged small-signal model) | `run_loop.py` | Fixed LM2678-12: 65 degrees phase margin. The script also covers the adjustable LM2678-ADJ that was evaluated and not used (33 degrees without a feed-forward capacitor). |

Notes on ngspice: `meas` cannot take `v(a,b)` or `par()`, use `let`;
inside `.control`, `let x = a > 5` is a shell redirect, not a comparison;
discontinuous B-sources and very stiff current limits stall the time
step, use tanh or sqrt smoothing.

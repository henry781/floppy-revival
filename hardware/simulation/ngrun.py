#!/usr/bin/env python3
"""Run an ngspice deck with the libngspice bundled in KiCad (no ngspice install
needed).  The deck's .control block writes its results (wrdata/print); this
only loads the library, sources the deck and relays ngspice's output.

  python3 ngrun.py deck.cir"""
import ctypes
import os
import sys

LIB = "/Applications/KiCad/KiCad.app/Contents/PlugIns/sim/libngspice.dylib"
os.environ.setdefault("SPICE_LIB_DIR", "/Applications/KiCad/KiCad.app/Contents/SharedSupport/ngspice")
CB_STR = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
CB_EXIT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_bool, ctypes.c_bool, ctypes.c_int, ctypes.c_void_p)
CB_BG = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_bool, ctypes.c_int, ctypes.c_void_p)


def _out(msg, ident, user):
    line = msg.decode(errors="replace")
    for p in ("stdout ", "stderr "):
        if line.startswith(p):
            line = line[len(p):]
    if line.strip():
        print(line)
    return 0


def _quiet(*a):
    return 0


def main(deck):
    ng = ctypes.CDLL(LIB)
    keep = [CB_STR(_out), CB_STR(_quiet), CB_EXIT(_quiet), CB_BG(_quiet)]
    ng.ngSpice_Init(keep[0], keep[1], keep[2], None, None, keep[3], None)
    os.chdir(os.path.dirname(os.path.abspath(deck)))
    # PSpice compatibility (TI models: VALUE={IF()}, TABLE, LIMIT) and all
    # .lib/.inc paths relative to the deck
    cm = os.path.join(os.path.dirname(LIB), "ngspice")
    pre = ["codemodel " + os.path.join(cm, f) for f in ("analog.cm", "digital.cm", "spice2poly.cm", "table.cm",
                                                       "xtradev.cm", "xtraevt.cm")]
    for c in pre + ["set ngbehavior=psa", "set num_threads=4", "source " + os.path.basename(deck)]:
        ng.ngSpice_Command(c.encode())


if __name__ == "__main__":
    main(sys.argv[1])

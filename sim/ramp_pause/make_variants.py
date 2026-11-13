# SPDX-License-Identifier: GPL-2.0-or-later
# Writes the comparison versions of rtl/vectrex.vhd for the RAMP pause test (the file itself is not changed):
#   before       the core before the fix (13 Nov 2026), rebuilt exactly: the CB2 delay, RAMP delay and RAMP release
#                counter step on every 24 MHz clock, pause or not, and cpu_phase runs on during a pause outside
#                PiTrex mode
#   before_free  the same, with cpu_phase running on during a pause as in normal 6809 mode (ptx_enable = '0')
#   free         the fixed core but with cpu_phase running on during a pause (the 24 MHz paths fixed, the phase not)
# The test drives the VIA through the PiTrex port, timed by the CPU's E enables just as a real 6809's bus cycles are,
# so the _free versions show what normal 6809 mode does with the same stream. The fixed core itself ("cur") needs no
# _free version: it freezes the phase in both modes.
# usage: make_variants.py rtl/vectrex.vhd OUTDIR
import sys, os

src = open(sys.argv[1], newline='').read()
nl = '\r\n' if '\r\n' in src else '\n'
out = sys.argv[2]

def swap(text, old, new):
    old = old.replace('\n', nl); new = new.replace('\n', nl)
    assert text.count(old) == 1, "not found exactly once:\n" + old
    return text.replace(old, new)

T = '\t'
BLOCK = [
    "cb2_delay_q <= cb2_delay_q(cb2_delay_q'high - 1 downto 0) & via_cb2_o;",
    "",
    "ramp_delay_q <= ramp_delay_q(ramp_delay_q'high - 1 downto 0) & via_pb_o(7);",
    "if ramp_delay_q(ramp_delay_q'high) = '0' then",
    T + "ramp_integrator_n <= '0';",
    T + "ramp_release_q    <= ramp_release_clocks;",
    "elsif ramp_release_q /= 0 then",
    T + "ramp_release_q <= ramp_release_q - 1;",
    "else",
    T + "ramp_integrator_n <= '1';",
    "end if;",
]
def lines(ls, tabs=0):
    return ''.join((T * tabs + l if l else l) + '\n' for l in ls)

# the fix, piece by piece: (fixed core text, the text before the fix)
FIX = [
    (lines(["signal clken_12            : std_logic;",
            "signal clken_24            : std_logic := '1';"]),
     lines(["signal clken_12            : std_logic;"])),
    (lines(["-- Both CB2 blanking edges: 1 us [24 clocks at 24 MHz].",
            "-- These 24 MHz paths step on clken_24, so a pause holds them where they are, in step with the VIA, the",
            "-- DAC/ZERO delay buffer and the integrators (clken_12), and they resume unchanged."]),
     lines(["-- Both CB2 blanking edges: 1 us [24 clocks at 24 MHz]."])),
    (lines(["if clken_24 = '1' then"], 2) + lines(BLOCK, 3) + lines(["end if;"], 2),
     lines(BLOCK, 2)),
    (lines(["-- A pause (manual or OSD pause, PiTrex command underrun) freezes the phase too, so the",
            "-- beam/integrator enables keep their exact 8-per-E-cycle pattern and their alignment with the",
            "-- 24 MHz RAMP/CB2 paths across a pause of any length. A free-running phase could lose or gain",
            "-- one 12 MHz tick per pause.",
            "if pause = '0' then"], 2),
     lines(["-- B8: in PiTrex mode a pause (command underrun, manual pause) freezes the phase too, so",
            "-- the beam/integrator enables keep their exact 8-per-E-cycle pattern across the pause.",
            "-- Otherwise the free-running phase could lose or gain one 12 MHz tick per pause.",
            "if pause = '0' or ptx_enable = '0' then"], 2)),
    (lines(["-- registered like clken_12 and cpu_en, so all three stop and restart on the same clock",
            "clken_24  <= not pause;"], 2),
     ""),
]
before = src
for fixed, old in FIX:
    before = swap(before, fixed, old)

PH = lines(["cpu_phase <= cpu_phase + 1;"], 3)
FREE = lines(["if true then"], 2) + PH
free = swap(src, lines(["if pause = '0' then"], 2) + PH, FREE)
before_free = swap(before, lines(["if pause = '0' or ptx_enable = '0' then"], 2) + PH, FREE)

os.makedirs(out, exist_ok=True)
for name, text in (("before", before), ("before_free", before_free), ("free", free)):
    with open(os.path.join(out, "vectrex_%s.vhd" % name), "w", newline='') as f:
        f.write(text)
print("variants written")

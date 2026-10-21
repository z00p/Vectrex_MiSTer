# Gyrocks for PiTrex on MiSTer

Carsten Wartmann's [Gyrocks](https://github.com/callimero/Gyrocks) (GPL-2), as included in the
PiTrex tree, with these additions (marked in `gyrocks.c`; reproducible with
`tools/patch_gyrocks.py` against the PiTrex copy):
- the ship takes damage from rocks and enemies, with 3 lives and a short invulnerability after a hit;
- vector explosions, plus PSG sound for shots and explosions;
- game over, with button 1 to restart, and a ships counter.

Controls: stick moves, **button 4** fires.

The original README follows.

---

# Gyrocks
A Vector Game For Oscilloscopes


<p align="center">
  <img src="https://raw.githubusercontent.com/callimero/Gyrocks/master/Bilder/GyrocksTest.jpg?raw=true" width="250"/>
</p>

Largely build onto Hudson Trammells teensyv/v.st project. v.st is a work from Trammell Hudson, License GPL


Modified version of https://github.com/osresearch/vst since I did never get the scopeclock to work.

Gyrocks is a 2,5D Game on the oscilloscope which uses many code from v.st. 

Carsten Wartmann, cw@blenderbuch.de

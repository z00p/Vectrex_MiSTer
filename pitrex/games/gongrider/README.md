# Gong Rider

**Gong Rider** by Roger Boesch, the PiTrex version, heavily inspired by Master of the Lamps
(Activision, 1985). Source: <https://rogerboesch.games/learn/gong-rider/>. MIT licence (see
`LICENSE`); `gongrider.c` here is the upstream file, unchanged. It runs on the MiSTer through
the same shim as the other PiTrex programs.

The release ships `gongrider` pre-built, with an original overlay (`art/GongRider.art`; no
artwork from the game is used). `make_overlay.py` generates the overlay picture
(`GongRider.png`); see its header for turning it into a `.art` file with the Overlay Tools.

## Controls
The stick steers the carpet down the tunnel and strikes the gongs in the gong chamber (the
stick direction that points at a gong). The face buttons strike the gongs too, laid out as a diamond: on an Xbox-style pad Y is the
top gong, B right, A bottom and X left. (The MiSTer passes B, A, Y, X on as Vectrex buttons 1-4,
so tools/build_gongrider.sh remaps the gong section on a copy of the source.)

## Building (developers)
```
tools/build_gongrider.sh
```

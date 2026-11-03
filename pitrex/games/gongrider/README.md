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
stick direction that points at a gong). The face buttons strike the gongs too, laid out as a
diamond: the top button strikes the top gong, the left button the left gong, and so on.
(tools/build_gongrider.sh remaps the gong section's Vectrex buttons 1-4 on a copy of the source
to match a MiSTer pad: 1 bottom, 2 left, 3 right, 4 top.)

## Building (developers)
```
tools/build_gongrider.sh
```

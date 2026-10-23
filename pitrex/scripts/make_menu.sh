#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the MiSTer main-menu entries for PiTrex and UVMC2 programs (run on the MiSTer).
#   "/media/fat/_PiTrex & UVMC2"/<Title>.mgl   what the menu shows
#   /media/fat/games/Vectrex/PiTrex/<prog>.ptx   a small launch script the MGL hands the core
# Picking an entry loads the PiTrex core, its overlay (if there is one) and the .ptx;
# pitrexd (started at boot by PiTrex_Setup) sees the pick and runs the .ptx.
# Run again after adding programs or overlays.
FAT=/media/fat
PITREX=$FAT/pitrex
MENU="$FAT/_PiTrex & UVMC2"
PTX=$FAT/games/Vectrex/PiTrex
OVL=$FAT/games/Vectrex/Overlays
CORE=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_*.rbf 2>/dev/null | sort -V | tail -1)
[ -n "$CORE" ] || CORE=$(ls -1 $FAT/_Unstable/Vectrex_PiTrex_B*.rbf 2>/dev/null | sort -V | tail -1)
[ -n "$CORE" ] || { echo "No PiTrex core in _Unstable"; exit 1; }
RBF=${CORE#$FAT/}; RBF=${RBF%.rbf}
mkdir -p "$MENU" "$PTX"
# the core is now Vectrex-PiTrex_*.rbf: drop the older Vectrex_PiTrex_B* files once one is there
case "$CORE" in */Vectrex-PiTrex_*) rm -f $FAT/_Unstable/Vectrex_PiTrex_B*.rbf ;; esac
python3 - <<'PYEOF'
# PiTrex Mode = OSD status bit 36 (byte 4, bit 4). Our core loads as "Vectrex-PiTrex" (the MGL's
# setname), so its settings live in their own file; clear the bit older releases set in the
# official core's VECTREX_v1.CFG.
def setbit(path, on):
    try:
        b = bytearray(open(path, "rb").read())
    except OSError:
        if not on:
            return
        b = bytearray()
    b += bytes(max(0, 16 - len(b)))
    new = (b[4] | 0x10) if on else (b[4] & ~0x10)
    if new != b[4]:
        b[4] = new
        open(path, "wb").write(b)
setbit("/media/fat/config/Vectrex-PiTrex_v1.CFG", True)
setbit("/media/fat/config/VECTREX_v1.CFG", False)
PYEOF
# games start from this folder now: remove the per-game Scripts entries B6 and older installed
rm -f $FAT/Scripts/PiTrex_Cube_demo.sh $FAT/Scripts/PiTrex_Gyrocks.sh $FAT/Scripts/PiTrex_Selftest.sh \
	$FAT/Scripts/PiTrex_VecBreak.sh $FAT/Scripts/PiTrex_VecDrive.sh $FAT/Scripts/PiTrex_VecDrive_v2.sh \
	$FAT/Scripts/UVMC2_Hello.sh $FAT/Scripts/UVMC2_TacScan.sh
rm -f "$MENU"/*.mgl "$PTX"/*.ptx

# title | program | environment for the program (UVMC2 games need theirs)
while IFS='|' read -r title prog env; do
	[ -x "$PITREX/$prog" ] || continue
	{
		echo '#!/bin/bash'
		[ -n "$env" ] && echo "export $env"
		echo "exec $PITREX/$prog"
	} > "$PTX/$prog.ptx"
	chmod +x "$PTX/$prog.ptx"
	ovl=$(find "$OVL" -maxdepth 1 -iname "$prog.art" 2>/dev/null | head -1)
	{
		echo "<mistergamedescription>"
		echo "  <rbf>$RBF</rbf>"
		echo "  <setname>Vectrex-PiTrex</setname>"
		[ -n "$ovl" ] && echo "  <file delay=\"1\" type=\"f\" index=\"2\" path=\"$ovl\"/>"
		echo "  <file delay=\"2\" type=\"f\" index=\"3\" path=\"$PTX/$prog.ptx\"/>"
		echo "</mistergamedescription>"
	} > "$MENU/$title.mgl"
	echo "$title${ovl:+ (overlay $(basename "$ovl"))}"
done <<'EOF'
VecDrive|vecdrive|
VecBreak|vecbreak|
Gyrocks|gyrocks|
Gong Rider|gongrider|
Cube demo|cube|
PiTrex Selftest|ptx_selftest|
UVMC2 Hello|hello_uvmc2|
UVMC2 Playroom|playroom|
UVMC2 FX demo|fx_demo|
UVMC2 Tac Scan|tacscan|UVMC2_DIR=/media/fat/pitrex/uvmc2/tacscan UVMC2_MUTE=0,1,2 AAE_ROMZIP=$(ls /media/fat/games/mame/tacscan.zip /media/fat/pitrex/uvmc2/tacscan/tacscan.zip 2>/dev/null | head -1)
EOF

# a running pitrexd keeps the code it started with: restart it so it knows the core name
if ps | grep -v grep | grep -q pitrexd.sh; then
	for p in $(ps | grep -v grep | grep pitrexd.sh | awk '{print $1}'); do kill "$p"; done
	nohup "$PITREX/pitrexd.sh" > /dev/null 2>&1 &
fi

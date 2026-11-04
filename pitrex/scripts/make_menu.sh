#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build the MiSTer main-menu entries for PiTrex and UVMC2 programs (run on the MiSTer).
#   "/media/fat/_PiTrex & UVMC2"/<Title>.mgl   what the menu shows
#   /media/fat/games/Vectrex-PiTrex/PiTrex/<prog>.ptx   a small launch script the MGL hands the core
# Picking an entry loads the PiTrex core, its overlay (if there is one) and the .ptx;
# pitrexd (started at boot by PiTrex_Setup) sees the pick and runs the .ptx.
# The entries are plain MGLs (undated core name, paths relative to games/Vectrex-PiTrex, like any MiSTer game MGL): they keep
# working after a core update, and anyone can write one for a game. Copy one and change the path, for example
#   <file delay="2" type="f" index="3" path="UVMC2/Punchout.um2"/>
# "UVMC2 Games" loads the core alone: pick a game in its OSD.
FAT=/media/fat
PITREX=$FAT/pitrex
MENU="$FAT/_PiTrex & UVMC2"
PTX=$FAT/games/Vectrex-PiTrex/PiTrex
OVL=$FAT/games/Vectrex/Overlays
OVLNEW=$FAT/games/Vectrex-PiTrex/Overlays
# standard MiSTer naming from B8: Vectrex-PiTrex_YYYYMMDD.rbf, in _Console from 1.0 (the alphas and betas used _Unstable;
# older releases: Vectrex-PiTrex_B<n>_<date>.rbf)
CORE=$(ls -1 $FAT/_Console/Vectrex-PiTrex_[0-9]*.rbf 2>/dev/null | sort | tail -1)
[ -n "$CORE" ] || CORE=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_[0-9]*.rbf 2>/dev/null | sort | tail -1)
[ -n "$CORE" ] || CORE=$(ls -1 $FAT/_Unstable/Vectrex-PiTrex_*.rbf 2>/dev/null | sort -V | tail -1)
[ -n "$CORE" ] || CORE=$(ls -1 $FAT/_Unstable/Vectrex_PiTrex_B*.rbf 2>/dev/null | sort -V | tail -1)
[ -n "$CORE" ] || { echo "No PiTrex core in _Console or _Unstable"; exit 1; }
RBF=$(basename "$(dirname "$CORE")")/Vectrex-PiTrex
echo "Menu entries load the newest $RBF"
mkdir -p "$MENU" "$PTX" "$OVLNEW"
# keep only the newest core, as the MiSTer updaters do: once a dated one is in _Console, the PiTrex cores in _Unstable
# and the older dated ones in _Console go
case "$CORE" in
	$FAT/_Console/Vectrex-PiTrex_[0-9]*)
		rm -f $FAT/_Unstable/Vectrex_PiTrex_B*.rbf $FAT/_Unstable/Vectrex-PiTrex_*.rbf
		for f in $FAT/_Console/Vectrex-PiTrex_[0-9]*.rbf; do [ "$f" = "$CORE" ] || rm -f "$f"; done ;;
	*/Vectrex-PiTrex_[0-9]*) rm -f $FAT/_Unstable/Vectrex_PiTrex_B*.rbf $FAT/_Unstable/Vectrex-PiTrex_B*.rbf ;;
	*/Vectrex-PiTrex_*) rm -f $FAT/_Unstable/Vectrex_PiTrex_B*.rbf ;;
esac
python3 - <<'PYEOF'
# PiTrex Mode = OSD status bit 117 (byte 14, bit 5). Our core loads as "Vectrex-PiTrex" (the MGL's
# setname), so its settings live in their own file, Vectrex-PiTrex_v2.CFG since B13 (Videodr0me's
# VECTREX_v2 layout). Clear the bit older releases set (status bit 36, byte 4 bit 4) in the
# official core's VECTREX_v1.CFG.
def setbit(path, i, m, on):
    try:
        b = bytearray(open(path, "rb").read())
    except OSError:
        if not on:
            return
        b = bytearray()
    b += bytes(max(0, 16 - len(b)))
    new = (b[i] | m) if on else (b[i] & ~m)
    if new != b[i]:
        b[i] = new
        open(path, "wb").write(b)
setbit("/media/fat/config/Vectrex-PiTrex_v2.CFG", 14, 0x20, True)
setbit("/media/fat/config/VECTREX_v1.CFG", 4, 0x10, False)
# Open Matte is our core's default: View Area = status[65] = byte 8 bit 1, in our own settings file only. Once: a later choice in the OSD stands.
# (The mark has a _v2 name since B13's new settings file, so installs that had Open Matte in the old file get it again.)
import os
MARK = "/media/fat/pitrex/.open_matte_default_v2"
if not os.path.exists(MARK):
    path = "/media/fat/config/Vectrex-PiTrex_v2.CFG"
    try:
        b = bytearray(open(path, "rb").read())
        b += bytes(max(0, 16 - len(b)))
        b[8] |= 0x02
        open(path, "wb").write(b)
        open(MARK, "w").close()
    except OSError as e:
        print("Open Matte default not set:", e)
PYEOF
# games start from this folder now: remove the per-game Scripts entries B6 and older installed
rm -f $FAT/Scripts/PiTrex_Cube_demo.sh $FAT/Scripts/PiTrex_Gyrocks.sh $FAT/Scripts/PiTrex_Selftest.sh \
	$FAT/Scripts/PiTrex_VecBreak.sh $FAT/Scripts/PiTrex_VecDrive.sh $FAT/Scripts/PiTrex_VecDrive_v2.sh \
	$FAT/Scripts/UVMC2_Hello.sh $FAT/Scripts/UVMC2_TacScan.sh
rm -f "$MENU"/*.mgl "$PTX"/*.ptx "$FAT"/games/Vectrex/PiTrex/*.ptx
rmdir "$FAT/games/Vectrex/PiTrex" 2>/dev/null
mkdir -p "$FAT/games/Vectrex-PiTrex/UVMC2" "$PITREX/uvmc2"

# Scripts > PiTrex_Make_UVMC2_Card: a tool, not a game, so it sits next to Setup, Update, Stop and Uninstall
# (they print to the Scripts console; a main-menu entry would load the core and show nothing)
if [ -f "$PITREX/make_card.sh" ]; then
	cat > "$FAT/Scripts/PiTrex_Make_UVMC2_Card.sh" <<EOF
#!/bin/bash
# Build the UVMC2 card image $PITREX/uvmc2/sd.img from the files in $PITREX/uvmc2/card.
# Written by make_menu.sh (PiTrex_Setup); removed by PiTrex_Uninstall.
exec bash $PITREX/make_card.sh
EOF
	chmod +x "$FAT/Scripts/PiTrex_Make_UVMC2_Card.sh"
	echo "Scripts > PiTrex_Make_UVMC2_Card"
fi

# core only: the games are picked in the OSD
{
	echo "<mistergamedescription>"
	echo "  <rbf>$RBF</rbf>"
	echo "</mistergamedescription>"
} > "$MENU/UVMC2 Games.mgl"
echo "UVMC2 Games (core only; pick in the OSD)"

# Tac/Scan sound: convert MAME's tacscan samples zip (games/mame/samples) once
TS_ZIP=$FAT/games/mame/samples/tacscan.zip
TS_PCM=$PITREX/uvmc2/tacscan/TACSCAN.PCM
if [ -f "$TS_ZIP" ] && { [ ! -f "$TS_PCM" ] || [ "$TS_ZIP" -nt "$TS_PCM" ]; }; then
	mkdir -p "$PITREX/uvmc2/tacscan"
	echo "Converting the Tac/Scan samples (about 20 seconds)..."
	python3 "$PITREX/ts_samples.py" "$TS_ZIP" "$TS_PCM"
fi

# title | program | environment for the program (UVMC2 games need theirs)
while IFS='|' read -r title prog env; do
	[ -x "$PITREX/$prog" ] || continue
	{
		echo '#!/bin/bash'
		[ -n "$env" ] && echo "export $env"
		[ "$prog" = tacscan ] && echo '[ -f /media/fat/pitrex/uvmc2/tacscan/TACSCAN.PCM ] || [ ! -f /media/fat/games/mame/samples/tacscan.zip ] || python3 /media/fat/pitrex/ts_samples.py /media/fat/games/mame/samples/tacscan.zip /media/fat/pitrex/uvmc2/tacscan/TACSCAN.PCM >&2'
		echo "exec $PITREX/$prog"
	} > "$PTX/$prog.ptx"
	chmod +x "$PTX/$prog.ptx"
	ovl=$(find "$OVL" -maxdepth 1 -iname "$prog.art" 2>/dev/null | head -1)
	[ -n "$ovl" ] && cp -f "$ovl" "$OVLNEW/"
	{
		echo "<mistergamedescription>"
		echo "  <rbf>$RBF</rbf>"
		[ -n "$ovl" ] && echo "  <file delay=\"1\" type=\"f\" index=\"2\" path=\"Overlays/${ovl##*/}\"/>"
		echo "  <file delay=\"2\" type=\"f\" index=\"3\" path=\"PiTrex/$prog.ptx\"/>"
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

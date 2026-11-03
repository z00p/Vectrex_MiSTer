#!/bin/sh
# Build Roger Boesch's Gong Rider for the MiSTer (run in WSL).
#   tools/build_gongrider.sh [path/to/gongrider.c]     -> build/arm/gongrider
# One MiSTer change, made on a copy: the gong section maps the Vectrex's buttons so a pad's
# face-button diamond matches the gongs. The MiSTer passes B, A, Y, X on as buttons 1-4, and
# its names are SNES positions (X top, A right, B bottom, Y left), so 1 = bottom gong,
# 2 = right, 3 = left, 4 = top (3 Nov: the 24 Oct map read the names as Xbox positions and
# put every button one gong round). The stick is unchanged.
set -e
cd "$(dirname "$0")/.."
src=${1:-games/gongrider/gongrider.c}
[ -f "$src" ] || src=upstream/gongrider/src/gong-rider-pitrex/src/gongrider.c
mkdir -p build/gongrider
python3 -I - "$src" build/gongrider/gongrider.c <<'EOF'
import sys
s = open(sys.argv[1]).read()
pairs = [("(buttons & 0x01) return 0;", "(buttons & 0x01) return 2;"),
         ("(buttons & 0x02) return 1;", "(buttons & 0x02) return 1;"),
         ("(buttons & 0x04) return 2;", "(buttons & 0x04) return 3;"),
         ("(buttons & 0x08) return 3;", "(buttons & 0x08) return 0;")]
for old, _ in pairs:
    assert s.count(old) == 1, "gongrider.c: gong button line not found: " + old
for i, (old, _) in enumerate(pairs):
    s = s.replace(old, "@%d@" % i)
for i, (_, new) in enumerate(pairs):
    s = s.replace("@%d@" % i, new)
open(sys.argv[2], "w").write(s)
EOF
EXTRA_CFLAGS="-Wno-error=implicit-function-declaration" sh tools/build_prog.sh arm gongrider build/gongrider/gongrider.c

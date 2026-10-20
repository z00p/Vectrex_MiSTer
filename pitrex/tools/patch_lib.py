# SPDX-License-Identifier: GPL-2.0-or-later
"""Copy the PiTrex library from upstream/ into build/lib/ and patch it for MiSTer.

The upstream clone is never modified. Each patch inserts an early-out under
#ifdef PITREX_MISTER at the top of one function, so the change is easy to read
and the original code is still there for the Pi build.

Run from anywhere:  python -I tools/patch_lib.py
"""
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UP = ROOT / "upstream" / "pitrex" / "pitrex"
OUT = ROOT / "build" / "lib"

# function name -> code inserted at the top of its body (under PITREX_MISTER)
PATCHES = {
    "delayNano": "ptx_advance_ns(n); return;",
    "v_delayCycles": "ptx_advance_cycles(n); return;",
    "v_delayCyclesQuarter": "ptx_advance_ns(167); return;",
    "v_delayCyclesEighth": "ptx_advance_ns(83); return;",
    "v_delayCycles1point5": "ptx_advance_ns(1000); return;",
    "setMarkStart": "markStartHi = 0; markStartLo = ptx_now_us(); return;",
    "waitMarkEnd": "ptx_wait_until_cycle(((uint64_t)(markStartLo + offsetMicro)) * 3 / 2); return;",
    "waitFullMicro": "{ int ticks = (int)(ptx_now_us() - markStartLo + 1);"
                     " ptx_wait_until_cycle(((uint64_t)ptx_now_us() + 1) * 3 / 2); return ticks; }",
    "waitMarkEndMark": "ptx_wait_until_cycle(((uint64_t)(markStartLo + offsetMicro)) * 3 / 2);"
                       " markStartHi = 0; markStartLo = ptx_now_us(); return;",
    "v_millis": "return ptx_now_us() / 1000;",
    "v_micros": "return ptx_now_us();",
    "disableLinuxInterrupts": "return;",
    "enableLinuxInterrupts": "return;",
    "v_WaitRecal": "ptx_frame();",          # no return: frame marker, then the real work
}


def patch_function(src: str, name: str, code: str) -> str:
    # definition at column 0: return type, name, '(' ... ')' then '{' (same or next line)
    pat = re.compile(r"^[A-Za-z_][\w \t\*]*\b" + re.escape(name) + r"\s*\([^;{]*\)\s*\{", re.M)
    m = pat.search(src)
    if not m:
        sys.exit(f"patch_lib: definition of {name}() not found")
    insert = f"\n#ifdef PITREX_MISTER\n  {code}\n#endif\n"
    return src[: m.end()] + insert + src[m.end():]


def main() -> None:
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "pitrex").mkdir(parents=True)
    (OUT / "vectrex").mkdir(parents=True)
    for f in ("pitrexio-gpio.h", "bcm2835.c", "bcm2835.h", "via6522.h"):
        shutil.copy2(UP / "pitrex" / f, OUT / "pitrex" / f)
    for f in (UP / "vectrex").iterdir():
        if f.suffix in (".c", ".h", ".i"):
            shutil.copy2(f, OUT / "vectrex" / f.name)
    for d in ("baremetal",):                       # headers some files include
        if (UP / d).exists():
            shutil.copytree(UP / d, OUT / d, ignore=shutil.ignore_patterns("*.o", "*.a"))

    vi = OUT / "vectrex" / "vectrexInterface.c"
    src = vi.read_text(encoding="latin-1")
    hook = '#include "vectrexInterface.h"\n'
    if hook not in src:
        sys.exit("patch_lib: include hook not found")
    src = src.replace(hook, hook + '#ifdef PITREX_MISTER\n#include "ptx.h"\n#endif\n', 1)
    for name, code in PATCHES.items():
        src = patch_function(src, name, code)
    vi.write_text(src, encoding="latin-1", newline="\n")
    print(f"patch_lib: {len(PATCHES)} functions patched -> {vi.relative_to(ROOT)}")


if __name__ == "__main__":
    main()

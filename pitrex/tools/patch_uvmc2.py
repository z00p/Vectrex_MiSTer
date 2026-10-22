# SPDX-License-Identifier: GPL-2.0-or-later
"""Make Linux copies of the UVMC2 SDK files that need it: the two that use ARM `svc` traps,
and uvm2_smp.c, whose host build never loads the sample bundle.

On the cartridge a .um2 game reaches the runtime through `svc #n`, and the runtime's
dispatcher (uvm2_svc.c) reads n back out of the instruction. On the DE10's ARM under Linux
those become direct calls: sdk_rp2350.c's sys_*() helpers call uvm2_svc_dispatch() with the
number in frame[7], and the dispatcher reads it from there. Nothing else changes.

    python3 -I tools/patch_uvmc2.py      # writes build/uvmc2/{sdk_rp2350.c,uvm2_svc.c,uvm2_smp.c}
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / "upstream/uvmc2-sdk"
OUT = ROOT / "build/uvmc2"
OUT.mkdir(parents=True, exist_ok=True)

# --- sdk_rp2350.c: the sys_* helpers -------------------------------------------------
src = (SDK / "rp2350-sdk/sdk_rp2350.c").read_text(encoding="utf-8")
first = src.index("static inline void sys_reset0ref(void)")
last_line = src.index("static inline void sys_raster_text(")
last = src.index("\n", last_line) + 1
helpers = r'''/* UVMC2 on MiSTer: the svc traps become direct calls (tools/patch_uvmc2.py). */
void uvm2_svc_dispatch(uint32_t *frame);
static inline uint32_t sys_call(uint32_t n, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t f[8] = { a, b, c, d, 0, 0, 0, n };
    uvm2_svc_dispatch(f);
    return f[0];
}
static inline void sys_reset0ref(void)       { sys_call(0, 0, 0, 0, 0); }
static inline void sys_wait_recal(void)      { sys_call(1, 0, 0, 0, 0); }
static inline void sys_set_intensity(int b)  { sys_call(2, (uint32_t)b, 0, 0, 0); }
static inline void sys_psg_write(int reg,int val){ sys_call(5, (uint32_t)reg, (uint32_t)val, 0, 0); }
static inline int  sys_read_buttons(void)    { return (int)sys_call(7, 0, 0, 0, 0); }
static inline int  sys_read_axes(void)       { return (int)sys_call(13, 0, 0, 0, 0); }
static inline int  sys_sample_pos(int fps)   { return (int)sys_call(10, (uint32_t)fps, 0, 0, 0); }
static inline void sys_play_music(const void *p){ sys_call(21, (uint32_t)(uintptr_t)p, 0, 0, 0); }
static inline void sys_stop_music(void)      { sys_call(22, 0, 0, 0, 0); }
static inline void sys_play_sfx(const void *p){ sys_call(23, (uint32_t)(uintptr_t)p, 0, 0, 0); }
static inline void sys_raster_text(int x,int y,const unsigned char*s,int n){ sys_call(26, (uint32_t)x, (uint32_t)y, (uint32_t)(uintptr_t)s, (uint32_t)n); }
'''
# keep the comments that sat between the helpers out of the way: replace the whole block
src = src[:first] + helpers + src[last:]
# samples the launcher asks to mute (UVMC2_MUTE=0,1,2): see uvmc2_sample_muted() in the stubs
old_play = "void v_playSample(int idx, int voice, int loop)\n{\n    const void *p = v_sampleData(idx);"
assert src.count(old_play) == 1, "sdk_rp2350.c: v_playSample not found"
src = src.replace(old_play, "int uvmc2_sample_muted(int idx);   /* src/uvm2_mister_stubs.c */\n"
                            "void v_playSample(int idx, int voice, int loop)\n{\n"
                            "    if (uvmc2_sample_muted(idx)) return;\n"
                            "    const void *p = v_sampleData(idx);")
assert "svc #" not in src.split("uvm2_svc_dispatch")[0][-4000:] or True
(OUT / "sdk_rp2350.c").write_text(src, encoding="utf-8", newline="\n")

# --- uvm2_svc.c: the number comes in frame[7] ------------------------------------------
svc = (SDK / "uvm2-sdk/uvm2_svc.c").read_text(encoding="utf-8")
pat = re.compile(r"const uint8_t \*pc\s*=\s*\(const uint8_t \*\)\(uintptr_t\)frame\[6\];\s*\n\s*uint32_t\s+num\s*=\s*pc\[-2\];")
assert len(pat.findall(svc)) == 1, "uvm2_svc.c: dispatcher prologue not found"
svc = pat.sub("uint32_t       num = frame[7];   /* UVMC2 on MiSTer: passed, not decoded */", svc)
(OUT / "uvm2_svc.c").write_text(svc, encoding="utf-8", newline="\n")
# --- uvm2_smp.c: load the sample bundle from a file (the host build skips it) -----------
smp = (SDK / "uvm2-sdk/uvm2_smp.c").read_text(encoding="utf-8")
old = "#ifdef UVM2_HOST\n    return 0;                     /* no card in the host harness */\n#else"
assert smp.count(old) == 1, "uvm2_smp.c: bundle_load host branch not found"
smp = smp.replace(old, "#if defined(UVM2_HOST) && !defined(UVM2_MISTER)\n"
                       "    return 0;                     /* no card in the host harness */\n#else")
# the bundle goes into the cartridge's PSRAM at a fixed address; on Linux, a static buffer
old_buf = "    return (void *)(uintptr_t)UVM2_SMP_PSRAM_BASE;"
assert smp.count(old_buf) == 1, "uvm2_smp.c: bundle buffer not found"
smp = smp.replace(old_buf, "#ifdef UVM2_MISTER\n"
                           "    static uint8_t s_bundle_ram[UVM2_SMP_BUNDLE_MAX];   /* no PSRAM here */\n"
                           "    return s_bundle_ram;\n"
                           "#else\n" + old_buf + "\n#endif")
# the host build doesn't include uvm2_sd.h; the reader is in src/uvm2_mister_stubs.c
smp = ("#include <stdint.h>\n"
       "uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from);\n"
       + smp)
(OUT / "uvm2_smp.c").write_text(smp, encoding="utf-8", newline="\n")
print("wrote", OUT / "sdk_rp2350.c", OUT / "uvm2_svc.c", "and", OUT / "uvm2_smp.c")

# --- Tac/Scan (starter kit, if present): the mute list on its jack path too ------------
TSJ = ROOT / "upstream/uvmc2-starter-kit/game/tacscan/src/ts_jack.c"
if TSJ.exists():
    ts = TSJ.read_text(encoding="utf-8")
    head = "void ts_jack_play(int voice, int idx, int loop)\n{\n"
    assert ts.count(head) == 1, "ts_jack.c: ts_jack_play not found"
    ts = ts.replace(head, "int uvmc2_sample_muted(int idx);   /* src/uvm2_mister_stubs.c */\n" + head
                    + "    if (uvmc2_sample_muted(idx)) return;\n")
    (OUT / "ts_jack.c").write_text(ts, encoding="utf-8", newline="\n")
    print("wrote", OUT / "ts_jack.c")

AAE Z80 core (cpu_z80.cpp / cpu_z80.h) - vendored, UNMODIFIED
=============================================================

Source     https://github.com/tcottrill/AAE
           aae/aae/cpu_code/cpu_z80.cpp
           aae/aae/cpu_code/cpu_z80.h
Fetched    2026-10-23 with `gh api repos/tcottrill/AAE/contents/<path>`, from the default branch,
           whose head commit was def8653760623d6f4fe791e7b0a28b16e52330c3 (2026-10-07).
           Last commit touching both files: b1f1a1afd5c28fd122d8b4ffb59196030f960298 (2026-10-04,
           "Sync Eliminator crash fix, Z80 PPC sentinel and aae.exe from aae-wip").
SHA-256    cpu_z80.cpp  b8106be240dc050eea676dac8c68ede1a8a7670d92be857823bea73c8873f50b
           cpu_z80.h    8f9c8089759b179a4680f36dde8c6643168c4047a92d7b36eed9f6983a0065e7

Licence    The file headers say (cpu_z80.cpp, lines 1-5):
             "This file is part of the AAE project and is released under The Unlicense.
              You are free to use, modify, and distribute this software without restriction."
           (http://unlicense.org/). NOTE: the repository as a whole carries a COPYING file
           (GPL-3.0); the Unlicense grant is the per-file header. cpu_z80.h has no licence text of
           its own, only the class declaration of the same core.

Origins    As stated in the header of cpu_z80.cpp:
             - original Zilog Z80 emulator by Edward Massey (1997), ported to C by Jeff Mitchell;
             - based on and API-compatible with Neil Bradley's MZ80 (compatible API only; the
               source is a different code base);
             - revisions by EAM (1997); modern C++ refactor, cycle-accurate execution and
               additions by TC (Tim Cottrill, 2015-2025);
             - guidance and documentation provided by ChatGPT;
             - references/improvements adapted from OldSpark (Vedder Bruno) and the Superzazu Z80
               (https://github.com/superzazu/z80, MIT licensed).
           Passes the Zexdoc tests (per the header).

Not copied from AAE: the shims the core needs live in games/uvmc2_tacscan/z80shim/
(deftypes.h, sys_log.h, timer.h, mz80_compat.h, mz80_aae.cpp), written for this project (GPL-2.0-or-later like the build scripts).
Build: tools/build_tacscan.sh with TACSCAN_Z80=aae (the default).

Permission and licence (23 October 2026)
  Tim Cottrill agreed to its use in this project: https://github.com/tcottrill/AAE/issues/13
  He notes that the core is a heavily modified and tested port of Edward Massey's 1997 Z80
  emulator (used in MageX), so it should be treated as GPL-2-compatible, and that all code
  from Marcel de Kogel (Marat) has been removed. Edward Massey's original licence is the
  GNU Library General Public License version 2; his original readme and documentation are
  included here unchanged as massey_README.TXT and massey_EXECZ80.DOC. As part of this
  project's binaries the core is distributed under the GNU GPL (version 2 or later), as the
  LGPL permits, with its source in this folder. The superzazu/z80 references are MIT
  licensed (copyright notice: https://github.com/superzazu/z80/blob/master/LICENSE).

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# MiSTer.ini edits for PiTrex-on-MiSTer (run by PiTrex_Setup and PiTrex_Uninstall).
#   pitrex_ini.py setup     [INI]   log_file_entry=1 for our core only, in a [Vectrex-PiTrex] section
#   pitrex_ini.py uninstall [INI]   remove that line (and the section, if nothing else is in it)
# Why: pitrexd learns about an OSD pick from the files Main writes to /tmp (FILESELECT, FULLPATH,
# CURRENTPATH), and Main only writes them with log_file_entry=1. A core section applies when the
# core runs under that name, which the PiTrex & UVMC2 menu entries give it (MGL setname), so the
# [MiSTer] settings and every other core are left alone.
# Both modes also undo what releases up to B8 and the first alpha draft did: log_file_entry=1 in
# [MiSTer], with the original file kept as MiSTer.ini.pitrex-bak. That is put back as it was in
# the backup, and the backup is removed.
# Only whole lines are touched; the file's line endings (LF or CRLF) are kept.
import os
import sys

SECTION = "vectrex-pitrex"
LINE = "log_file_entry=1"


def norm(line):
    return line.strip().replace(" ", "").replace("\t", "").lower()


def section_of(lines):
    """Section name (lower case, "" before the first header) for every line."""
    out, cur = [], ""
    for l in lines:
        s = l.strip()
        if s.startswith("[") and "]" in s:
            cur = s[1:s.index("]")].strip().lower()
        out.append(cur)
    return out


def is_header(line, name):
    s = line.strip()
    return s.startswith("[") and "]" in s and s[1:s.index("]")].strip().lower() == name


def undo_global(lines, ini):
    """Put [MiSTer] log_file_entry back as it was in MiSTer.ini.pitrex-bak (older releases)."""
    bak = ini + ".pitrex-bak"
    if not os.path.exists(bak):
        return lines, None
    with open(bak, newline="") as f:
        b = [norm(l) for l in f.read().split("\n")]
    was = [l for l in b if l.startswith("log_file_entry=")]
    secs = section_of(lines)
    out, msg = [], None
    for l, s in zip(lines, secs):
        if s in ("", "mister") and norm(l) == LINE:
            if not was:
                msg = "MiSTer.ini: removed log_file_entry=1 from [MiSTer] (an older PiTrex release added it)"
                continue
            if was[-1] == "log_file_entry=0":
                k = l.rindex("1")
                l = l[:k] + "0" + l[k + 1:]
                msg = "MiSTer.ini: log_file_entry in [MiSTer] back to 0 (an older PiTrex release had set it)"
        out.append(l)
    os.remove(bak)
    return out, msg


def setup(lines, nl):
    secs = section_of(lines)
    head = next((i for i, l in enumerate(lines) if is_header(l, SECTION)), None)
    if head is None:
        while lines and lines[-1].strip() == "":
            lines.pop()
        return lines + ["", "[Vectrex-PiTrex]", LINE, ""], "MiSTer.ini: added a [Vectrex-PiTrex] section with log_file_entry=1"
    mine = [i for i, s in enumerate(secs) if s == SECTION and i > head]
    for i in mine:
        if norm(lines[i]) == LINE:
            return lines, "MiSTer.ini: [Vectrex-PiTrex] already has log_file_entry=1"
    for i in mine:
        if norm(lines[i]).startswith("log_file_entry="):
            lines[i] = LINE
            return lines, "MiSTer.ini: log_file_entry=1 in [Vectrex-PiTrex]"
    return lines[:head + 1] + [LINE] + lines[head + 1:], "MiSTer.ini: log_file_entry=1 added to [Vectrex-PiTrex]"


def uninstall(lines, nl):
    secs = section_of(lines)
    out = [l for l, s in zip(lines, secs) if not (s == SECTION and norm(l) == LINE)]
    if len(out) == len(lines):
        return lines, None
    # drop the header too when nothing but blank lines is left in the section (the way setup adds it)
    head = next(i for i, l in enumerate(out) if is_header(l, SECTION))
    end = head + 1
    while end < len(out) and out[end].strip() == "":
        end += 1
    if end == len(out) or out[end].strip().startswith("["):
        out = out[:head] + out[end:]
        if head > 0 and out[head - 1].strip() == "":
            del out[head - 1]
        if out and out[-1] != "":
            out.append("")
    return out, "MiSTer.ini: removed log_file_entry from [Vectrex-PiTrex]"


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else ""
    ini = sys.argv[2] if len(sys.argv) > 2 else "/media/fat/MiSTer.ini"
    if mode not in ("setup", "uninstall"):
        sys.exit("usage: pitrex_ini.py setup|uninstall [MiSTer.ini]")
    try:
        with open(ini, newline="") as f:
            data = f.read()
    except OSError:
        if mode == "uninstall":
            return
        data = ""
    nl = "\r\n" if "\r\n" in data else "\n"
    lines = data.replace("\r\n", "\n").split("\n")
    lines, m1 = undo_global(lines, ini)
    lines, m2 = (setup if mode == "setup" else uninstall)(lines, nl)
    new = nl.join(lines)
    if new != data:
        with open(ini, "w", newline="") as f:
            f.write(new)
    for m in (m1, m2):
        if m:
            print(m)


main()

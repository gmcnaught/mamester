#!/usr/bin/env python3
"""
Generate one MiSTer .mgl shortcut per MAMESTer romset, serving both entry points.

    <mistergamedescription>
        <rbf>_Other/MAMESTer_20260801</rbf>
        <file delay="2" type="s" index="0" path="roms/gng.zip"/>
    </mistergamedescription>

The files live in /media/fat/_MAMESTer (a top-level _-prefixed directory becomes
an entry in MiSTer's main menu), and games/MAMESTer/Games is symlinked to it so
the core's own OSD picker reaches the same files:

  From the MAIN MENU   selecting one loads the core AND mounts the romset, so
                       games sit in the menu next to every other system's titles.
  From the CORE picker selecting one mounts the .mgl itself, and
                       games/MAMESTer/game_lib.sh reads the romset path out of
                       the XML.

Why not point the picker straight at the romsets: it cannot select one.
Main_MiSTer's browser fakes every .zip into a directory and descends into it
(file_io.cpp ScanDirectory, suppressed only by SCANO_NOZIP, which a core cannot
request), so a romset can be browsed but never picked.

The XML path is relative to the core's home directory — device-observed:
Main_MiSTer joins it onto games/MAMESTer before storing the result in
MAMESTer.s0. The 2-second delay is measured from core load; the manager anchors
its "is this pick new?" test on /tmp/CORENAME's mtime so it cannot miss an early
mount.

Titles come from `mame -listfull` on the device, so entries read "Ghosts'n
Goblins (World? set 1)" rather than "gng".

SPACE: this device's exFAT allocates 128 KB per file, so each shortcut costs
128 KB of card whatever its contents — ~120 MB for 940 games. Generate a curated
subset unless you really want the whole romset in the menus.

Usage:
  tools/make-shortcuts.py --match "gng,1943,mk"     just these setnames
  tools/make-shortcuts.py --match "kof*"            glob on setname or title
  tools/make-shortcuts.py --all                     every romset present
  tools/make-shortcuts.py --list                    show what would be written
  tools/make-shortcuts.py --clean                   remove the generated set
"""

import argparse
import fnmatch
import re
import shlex
import subprocess
import sys

DEFAULT_HOST = "192.168.20.81"
HOMEDIR = "/media/fat/games/MAMESTer"   # core setname dir = OSD browser home
GAMEDIR = "/media/fat/games/mame"       # emulator + romsets
OTHERDIR = "/media/fat/_Other"
# A top-level _-prefixed directory becomes an entry in MiSTer's main menu.
MGLDIR = "/media/fat/_MAMESTer"
# The core's picker opens at HOMEDIR, so it reaches the same files through this.
PICKER_LINK = f"{HOMEDIR}/Games"

MGL = """<mistergamedescription>
\t<rbf>_Other/{rbf}</rbf>
\t<file delay="2" type="s" index="0" path="roms/{zip}"/>
</mistergamedescription>
"""

# MiSTer's browser shows the filename; keep it to characters exFAT accepts.
UNSAFE = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def ssh(host, command, check=True):
    proc = subprocess.run(
        ["ssh", "-o", "ConnectTimeout=10", f"root@{host}", command],
        capture_output=True, text=True,
    )
    if check and proc.returncode != 0:
        sys.exit(f"ssh failed: {command}\n{proc.stderr.strip()}")
    return proc.stdout


def romsets(host):
    """Setnames present on the device, as {setname: zip filename}."""
    out = ssh(host, f"ls -1 {shlex.quote(GAMEDIR)}/roms/*.zip 2>/dev/null")
    found = {}
    for line in out.splitlines():
        name = line.rsplit("/", 1)[-1]
        if name.lower().endswith(".zip"):
            found[name[:-4]] = name
    return found


def titles(host):
    """setname -> description, from mame's own driver table."""
    out = ssh(host, f"cd {shlex.quote(GAMEDIR)} && ./mame -listfull 2>/dev/null",
              check=False)
    table = {}
    for line in out.splitlines():
        m = re.match(r'^(\S+)\s+"(.*)"\s*$', line)
        if m:
            table[m.group(1)] = m.group(2)
    return table


def safe_name(text):
    return UNSAFE.sub("-", text).strip().rstrip(".")


def write_files(host, files):
    """Write {remote_path: content} in one ssh round trip.

    One round trip per file is minutes across 900 games; a single heredoc script
    is seconds.
    """
    script = [f"cat > {shlex.quote(path)} <<'MAMEOF'\n{body}MAMEOF"
              for path, body in files.items()]
    ssh(host, "\n".join(script))


def main():
    ap = argparse.ArgumentParser(
        description="Generate MAMESTer .mgl game shortcuts.",
        epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--match", help="comma-separated setnames or globs "
                                    "(matched against setname and title)")
    ap.add_argument("--all", action="store_true", help="every romset present")
    ap.add_argument("--list", action="store_true", help="show, do not write")
    ap.add_argument("--clean", action="store_true",
                    help=f"remove {MGLDIR} and the picker symlink, then exit")
    ap.add_argument("--rbf", help="RBF basename without .rbf "
                                  "(default: newest MAMESTer_* on the device)")
    args = ap.parse_args()

    if args.clean:
        ssh(args.host, f"rm -rf {shlex.quote(MGLDIR)}; "
                       f"rm -f {shlex.quote(PICKER_LINK)}")
        print(f"removed {MGLDIR} and {PICKER_LINK}")
        return 0

    if not (args.all or args.match):
        ap.error("choose --match <names> or --all (see --help for the space cost)")

    rbf = args.rbf or ssh(
        args.host, f"ls -1 {OTHERDIR}/MAMESTer_*.rbf | tail -1").strip()
    if not rbf:
        sys.exit(f"no MAMESTer_*.rbf found in {OTHERDIR} — deploy a core first")
    rbf = rbf.rsplit("/", 1)[-1]
    if rbf.endswith(".rbf"):
        rbf = rbf[:-4]

    present = romsets(args.host)
    if not present:
        sys.exit(f"no romsets in {GAMEDIR}/roms")
    table = titles(args.host)

    if args.all:
        chosen = sorted(present)
    else:
        # Exact setname or an explicit glob — no implicit trailing "*", so
        # "1943" means 1943 and "1943*" also brings in 1943kai/1943mii. Titles
        # are matched too, so --match "*ghosts*" works.
        patterns = [p.strip() for p in args.match.split(",") if p.strip()]

        def hit(setname, pattern):
            return (fnmatch.fnmatch(setname, pattern)
                    or fnmatch.fnmatch(table.get(setname, "").lower(), pattern.lower()))

        chosen = sorted(s for s in present if any(hit(s, p) for p in patterns))
        for p in [p for p in patterns if not any(hit(s, p) for s in chosen)]:
            print(f"  no romset matched '{p}'", file=sys.stderr)

    if not chosen:
        sys.exit("nothing matched")

    print(f"{len(chosen)} games -> {MGLDIR}  (rbf {rbf})")
    if args.list:
        for s in chosen:
            print(f"    {s:<12} {table.get(s, '(no title)')}")
        return 0

    ssh(args.host, f"mkdir -p {shlex.quote(MGLDIR)}")
    write_files(args.host, {
        f"{MGLDIR}/{safe_name(table.get(s) or s)}.mgl":
            MGL.format(rbf=rbf, zip=present[s])
        for s in chosen
    })
    n = ssh(args.host, f"ls -1 {shlex.quote(MGLDIR)}/*.mgl | wc -l").strip()

    # The core's picker opens at HOMEDIR and cannot see _MAMESTer, so bridge the
    # two with a symlink rather than a second copy of every file. Main_MiSTer's
    # browser stat()s DT_LNK entries and shows them as directories.
    ssh(args.host, f"test -e {shlex.quote(PICKER_LINK)} || "
                   f"ln -s {shlex.quote(MGLDIR)} {shlex.quote(PICKER_LINK)}")

    print(f"wrote {n} shortcuts")
    print(f"  MiSTer main menu   _MAMESTer")
    print(f"  core 'Load Game'   Games/  (-> {MGLDIR})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Aggregate a MISTER-PROF sampler dump into a per-function flat profile.

The sampler (tools/mame-frontend/mister-backend/mister_profile.cpp) prints raw
file offsets because the device has no symbols. This maps them back through the
UNSTRIPPED build and any device libraries you point it at.

    tools/build-mame.sh STRIP=true all          # keep the symbol table
    scp vendor/mame4all-pi/mame root@DEV:/media/fat/games/mame/mame-prof
    ssh root@DEV 'cd /media/fat/games/mame && SDL_VIDEODRIVER=dummy \\
        MISTER_PROFILE=1000 MISTER_BENCH_FRAMES=600 \\
        ./mame-prof 720 -rompath roms -nothrottle' 2>prof.txt
    tools/symbolize-prof.py prof.txt

Device shared libraries are resolved too if a copy is available:

    tools/symbolize-prof.py prof.txt --lib libc-2.31.so=/tmp/libc-2.31.so
    tools/symbolize-prof.py prof.txt --pull root@192.168.20.81   # fetch them

Only the PCs the sampler dumped (MISTER_PROFILE_TOP, default 400) are covered,
so the per-function figures are a lower bound; the MISTER-PROF-MOD totals are
complete and are printed as the check against that.
"""

import argparse
import bisect
import os
import re
import subprocess
import sys
from collections import defaultdict

PC_RE = re.compile(r"^MISTER-PROF-PC (\d+)\s+([\d.]+)%\s+0x([0-9a-f]+)\s+(.+)$")
MOD_RE = re.compile(r"^MISTER-PROF-MOD (\d+)\s+([\d.]+)%\s+(.+)$")
HDR_RE = re.compile(r"^MISTER-PROF samples=(\d+) dropped=(\d+) hz=(\d+)")
NM_RE = re.compile(r"^([0-9a-f]+)\s+[tTwWvV]\s+(.+)$")


def load_segments(path):
    """[(file_offset, filesz, vaddr)] for each PT_LOAD, from the ELF headers.

    The sampler reports file offsets (that is what /proc/self/maps gives it), but
    nm reports virtual addresses. Those are equal for a shared library and for a
    PIE, and differ by the load base for a non-PIE executable — so translate
    rather than assume.
    """
    import struct
    with open(path, "rb") as f:
        data = f.read(64)
        if data[:4] != b"\x7fELF" or data[4] != 1:      # ELF32 only (armhf)
            return []
        little = data[5] == 1
        end = "<" if little else ">"
        e_phoff, = struct.unpack_from(end + "I", data, 28)
        e_phentsize, e_phnum = struct.unpack_from(end + "HH", data, 42)
        f.seek(e_phoff)
        raw = f.read(e_phentsize * e_phnum)
    segs = []
    for i in range(e_phnum):
        p = raw[i * e_phentsize:(i + 1) * e_phentsize]
        p_type, p_offset, p_vaddr, _paddr, p_filesz = struct.unpack_from(end + "5I", p, 0)
        if p_type == 1 and p_filesz:                     # PT_LOAD
            segs.append((p_offset, p_filesz, p_vaddr))
    return segs


def file_offset_to_vaddr(segs, off):
    for p_offset, p_filesz, p_vaddr in segs:
        if p_offset <= off < p_offset + p_filesz:
            return off - p_offset + p_vaddr
    return off


def symbol_table(path):
    """Sorted (address, name) for every text symbol, static ones included."""
    out = subprocess.run(["nm", "-n", "--defined-only", path],
                         capture_output=True, text=True)
    syms = []
    for line in out.stdout.splitlines():
        m = NM_RE.match(line.strip())
        if m:
            syms.append((int(m.group(1), 16), m.group(2)))
    if not syms:   # stripped: fall back to the dynamic table
        out = subprocess.run(["nm", "-n", "-D", "--defined-only", path],
                             capture_output=True, text=True)
        for line in out.stdout.splitlines():
            m = NM_RE.match(line.strip())
            if m:
                syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    return syms


def lookup(syms, addr):
    if not syms:
        return None
    i = bisect.bisect_right([s[0] for s in syms], addr) - 1
    return syms[i][1] if i >= 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump", help="file holding the MISTER-PROF lines (stderr)")
    ap.add_argument("--elf", default=None,
                    help="unstripped mame ELF (default: vendor/mame4all-pi/mame)")
    ap.add_argument("--lib", action="append", default=[],
                    help="basename=path for a device library")
    ap.add_argument("--pull", metavar="USER@HOST",
                    help="scp the device libraries named in the dump into "
                         "--cache and symbolize them too")
    ap.add_argument("--cache", default=".prof-libs", help="where --pull stores libs")
    ap.add_argument("--top", type=int, default=30, help="functions to print")
    args = ap.parse_args()

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    elf = args.elf or os.path.join(repo, "vendor/mame4all-pi/mame")

    text = open(args.dump, encoding="utf-8", errors="replace").read().splitlines()
    header = next((HDR_RE.match(l) for l in text if HDR_RE.match(l)), None)
    mods = [(int(m.group(1)), float(m.group(2)), m.group(3))
            for m in (MOD_RE.match(l) for l in text) if m]
    pcs = [(int(m.group(1)), int(m.group(3), 16), m.group(4))
           for m in (PC_RE.match(l) for l in text) if m]
    if not pcs:
        sys.exit("no MISTER-PROF-PC lines in %s" % args.dump)

    # Which binary answers for each module path in the dump.
    binaries = {}
    for _, _, path in mods:
        base = os.path.basename(path)
        if base.startswith("mame"):
            binaries[path] = elf
    for spec in args.lib:
        base, _, p = spec.partition("=")
        for _, _, path in mods:
            if os.path.basename(path) == base:
                binaries[path] = p
    if args.pull:
        os.makedirs(args.cache, exist_ok=True)
        for _, _, path in mods:
            if path in binaries or not path.startswith("/"):
                continue
            local = os.path.join(args.cache, os.path.basename(path))
            if not os.path.exists(local):
                r = subprocess.run(["scp", "-q", "%s:%s" % (args.pull, path), local])
                if r.returncode != 0:
                    continue
            binaries[path] = local

    tables = {p: symbol_table(b) for p, b in binaries.items() if os.path.exists(b)}
    segments = {p: load_segments(b) for p, b in binaries.items() if os.path.exists(b)}

    if header:
        print("samples=%s dropped=%s hz=%s" % header.groups())
    print("\n== per module (complete) ==")
    for count, pct, path in sorted(mods, reverse=True):
        print("  %6.2f%%  %8d  %s" % (pct, count, path))

    dumped = sum(c for c, _, _ in pcs)
    total = int(header.group(1)) if header else dumped
    per_fn = defaultdict(int)
    for count, off, path in pcs:
        vaddr = file_offset_to_vaddr(segments.get(path, []), off)
        name = lookup(tables.get(path, []), vaddr)
        if name is None:
            name = "%s+0x%x" % (os.path.basename(path), off)
        per_fn["%s  [%s]" % (name, os.path.basename(path))] += count

    print("\n== per function (over the %d dumped PCs = %.1f%% of all samples) =="
          % (len(pcs), 100.0 * dumped / total if total else 0.0))
    for name, count in sorted(per_fn.items(), key=lambda kv: -kv[1])[:args.top]:
        print("  %6.2f%%  %8d  %s" % (100.0 * count / total, count, name))


if __name__ == "__main__":
    main()

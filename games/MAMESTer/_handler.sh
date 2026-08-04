#!/bin/bash
#
# MAMESTer auto-launch handler — invoked by MiSTer's Master_Daemon (Frontier)
# when the MAMESTer FPGA core loads: the daemon watches /tmp/CORENAME and routes
# a loaded core by name (from the RBF's CONF_STR setname) to
# games/<setname>/_handler.sh.
#
# TWO DIRECTORIES:
#   HOMEDIR  /media/fat/games/MAMESTer — this directory. Its name MUST equal the
#            CONF_STR setname exactly (fpga/MAME.sv), because that is how the
#            daemon finds this file and where MiSTer's OSD file browser opens.
#            It holds the launch harness only.
#   GAMEDIR  /media/fat/games/mame — the emulator and its data: the mame binary,
#            mame.cfg, roms/, opts/, and the cfg/nvram/hi/inp/snap state
#            directories. MAME chdir()s to its own binary's directory at startup,
#            so this is the cwd its relative config paths resolve against.
#
# This handler does the per-launch setup and then execs game_manager.sh, which
# owns the game lifecycle: idle until a romset is picked from the OSD, then
# launch / switch / stop MAME. `exec` keeps this PID, so the daemon's kill_child
# signals the manager directly and the manager's TERM trap stops MAME.
#
# WHY A HANDLER, NOT A `main=` WRAPPER: a `main=` binary REPLACES MiSTer's
# main() and inherits its obligations to the core — notably the scheduler's
# per-iteration `while (!is_fpga_ready(1)) fpga_wait_to_reset();` guard. The
# sibling ports measured the wrapper causing frame-1 wedges (maldita 3/5 vs
# stock-main 0/5). Stock MiSTer main keeps running and keeps its FPGA-readiness
# contract; the emulator is an ordinary child process.
#
# TRADEOFF: OSD Reset and a joystick-SHM bridge are NOT available on this path
# (they need the wrapper). Defer the wrapper unless/until it satisfies main()'s
# contract (readiness before spawn).

HOMEDIR="/media/fat/games/MAMESTer"
GAMEDIR="/media/fat/games/mame"
LOGDIR="/media/fat/logs/MAMESTer"

cd "$HOMEDIR" || exit 1

# Drop the stale OSD pick FIRST, before anything slow. Master_Daemon already
# clears .s0 on a core transition, but Main_MiSTer restores a saved SC0 mount at
# core load (user_io.cpp, the "%s.s%c" block), so the file can be back by the
# time we run, and entering the core should always wait for a fresh pick.
#
# This has to happen within the first second: an MGL shortcut mounts its file on
# a delay measured from core load (delay="2"), and deleting .s0 after that write
# would throw away a pick the user just made. The manager's core-load-mtime
# baseline covers the residual race in the other direction.
rm -f /media/fat/config/MAMESTer.s0 2>/dev/null

# Own every directory the emulator needs, so a fresh install works with no
# manual setup.
mkdir -p "$LOGDIR" "$GAMEDIR/roms" "$GAMEDIR/opts" "$GAMEDIR/samples" \
         "$GAMEDIR/cfg" "$GAMEDIR/nvram" "$GAMEDIR/hi" "$GAMEDIR/inp" \
         "$GAMEDIR/snap" 2>/dev/null

# The .mgl shortcuts' romset paths are resolved against HOMEDIR (that is what
# Main_MiSTer does with a mount path), so the romsets must be reachable from
# here.
if [ ! -e "$HOMEDIR/roms" ]; then
    ln -s "$GAMEDIR/roms" "$HOMEDIR/roms" 2>/dev/null
fi

# Keep the OSD picker one keypress from the game list. MiSTer starts a mount
# browser at the directory of the last mounted file, which after any .mgl launch
# is the romset directory rather than HOMEDIR — so "Games" is linked from both.
# The romset listing itself is never selectable: the browser fakes every .zip
# into a directory, whatever the CONF_STR extension filter says.
for _link in "$HOMEDIR/Games" "$GAMEDIR/roms/Games"; do
    [ -e "$_link" ] || ln -s /media/fat/_MAMESTer "$_link" 2>/dev/null
done

# Write-combining for the present path. Without this the DDR framebuffer window
# maps Strongly-Ordered — ARM's phys_mem_access_prot() gives any /dev/mem mmap
# of a fabric address pgprot_noncached(), whatever O_SYNC says — and the frame
# memcpy runs at 89.5 MB/s (4.19 ms at 512x384) instead of merging into bursts.
# Restricted to this core's own 4 MB window rather than left wide open.
#
# ENTIRELY OPTIONAL. The module is built out-of-tree against a specific MiSTer
# kernel (tools/mister/mem_wc/README.md), so a MiSTer update invalidates its
# vermagic and insmod starts failing here. nv_present.c falls back to /dev/mem
# on its own, so that must cost frame rate and nothing else — hence no error
# check and no exit path. `dmesg | grep mem_wc` says whether it took.
if [ -f "$GAMEDIR/mem_wc.ko" ] && [ ! -e /dev/mem_wc ]; then
    insmod "$GAMEDIR/mem_wc.ko" phys_base=0x3A000000 phys_size=0x00400000 \
        2>/dev/null
fi

# Rotate the previous manager log, and cap the per-game log history so
# /media/fat/logs/MAMESTer cannot grow without bound (one .log + one .prev per
# romset ever launched — with a 2000-driver romset that adds up).
mv -f "$LOGDIR/MAMESTer.log" "$LOGDIR/MAMESTer.prev.log" 2>/dev/null
ls -t "$LOGDIR"/*.log.prev 2>/dev/null | tail -n +21 | xargs -r rm -f

# Singleton guard — a survivor from a previous session (a manual SSH bench run,
# a crashed manager, or two daemon instances each spawning a handler, which is a
# real observed failure on the sibling ports) keeps writing the DDR framebuffer
# and fights this instance for it. Reap by name: there is no unrelated binary
# called "mame" on this device.
# NOTE: busybox has no pkill — killall is the tool. MAME ignores SIGTERM.
if ps w | grep -q "[m]ame "; then
    echo "MAMESTer handler: reaping a pre-existing mame before launch" >&2
    killall -9 mame 2>/dev/null
    sleep 1
fi

# FPGA settle after core load, before anything touches the DDR framebuffer. The
# sibling ports use 1-2 s; a fabric access issued before the core is up can wedge
# the arbiter. Cheap insurance on a path that only runs at core load.
sleep 2

echo "MAMESTer handler: CORENAME='$(cat /tmp/CORENAME 2>/dev/null)' home=$HOMEDIR game=$GAMEDIR" \
    > "$LOGDIR/MAMESTer.log"

export HOMEDIR GAMEDIR
exec ./game_manager.sh >> "$LOGDIR/MAMESTer.log" 2>&1
